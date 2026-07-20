#include "TestCommon.h"
#include "TestAllocationCounter.h"

#include "dsp/DynamicProductionRuntime.h"

#include <cmath>
#include <functional>

// =============================================================================
// Phase 12 hard tests for the Service-mediated safe retune path
// (DynamicProductionRuntime::configurePackagesIfNeeded/advanceStateEditAuditions).
//
// These exercise the click-safety property directly and independently of any
// assumption about per-sample coefficient ramping (there is none in
// DynamicHotBranchEngine): editing an already-audible State's package must
// never produce an abrupt output discontinuity, must never allocate, and must
// never leave a stale/incorrect package in place once settled.
// =============================================================================

namespace
{
    constexpr double kSR = 48000.0;

    DynamicGlobalBase makeGlobalBase (double sampleRate)
    {
        DynamicGlobalBase gb;
        gb.globalBaseDelayMs = 0.0f;
        gb.polarityInvert = false;
        gb.crossoverEnabled = true;
        gb.crossoverHz = 150.0f;
        gb.allpassEnabled = true;
        gb.globalAllpassFreqHz = 100.0f;
        gb.globalAllpassQ = 0.7f;
        gb.allpassStages = 2;
        gb.delayInterpolationIndex = 0;
        gb.learnedSampleRate = sampleRate;
        return gb;
    }

    DynamicMatchCalibration makeGenerousCalibration()
    {
        DynamicMatchCalibration c;
        c.valid = true;
        c.absoluteDistanceThreshold = 0.9f;
        c.ambiguityMargin = 0.05f;
        return c;
    }

    DynamicFingerprintPrototype makeSyntheticPrototype (float seed)
    {
        DynamicFingerprintPrototype p;
        p.valid = true;
        p.featureCount = (uint8_t) DynamicStateMapContract::kMaxFingerprintFeatures;
        for (int i = 0; i < DynamicStateMapContract::kMaxFingerprintFeatures; ++i)
            p.features[(size_t) i] = std::clamp (std::sin (seed + 0.3f * (float) i), -0.99f, 0.99f);
        return p;
    }

    DynamicStateMap makeSingleStateMap (const DynamicFingerprintPrototype& fp, uint64_t id,
                                        double sampleRate, float delayDelta, float freq, float q)
    {
        DynamicStateMap map = makeEmptyDynamicStateMap();
        map.valid = true;
        map.globalBase = makeGlobalBase (sampleRate);
        map.calibration = makeGenerousCalibration();

        DynamicState s;
        s.occupied = true;
        s.stableStateId = id;
        s.fingerprint = fp;
        s.hasLearnedPackage = true;
        s.learnedPackage.delayDeltaMs = delayDelta;
        s.learnedPackage.allpassFreqHz = freq;
        s.learnedPackage.allpassQ = q;
        s.origin = DynamicStateOrigin::Auto;
        s.evidence = DynamicStateEvidence::Stable;
        s.enabled = true;
        s.hitCount = 6;
        s.repeatability = 0.9f;
        s.ambiguity = 0.1f;

        map.states[0] = s;
        map.nextStateId = id + 1;
        return map;
    }

    // Deterministic bass tone with periodic kick transients, matching the
    // pattern already used in DynamicProductionRuntimeTests.cpp.
    void makeFixture (std::vector<float>& bass, std::vector<float>& kick,
                      int totalSamples, double sr, int periodSamples)
    {
        bass.assign ((size_t) totalSamples, 0.0f);
        kick.assign ((size_t) totalSamples, 0.0f);
        const double twoPi = juce::MathConstants<double>::twoPi;
        for (int i = 0; i < totalSamples; ++i)
            bass[(size_t) i] = 0.5f * (float) std::sin (twoPi * 55.0 * (double) i / sr);

        const int burst = (int) (sr * 0.02);
        for (int start = periodSamples; start < totalSamples; start += periodSamples)
            for (int j = 0; j < burst && start + j < totalSamples; ++j)
            {
                const double env = std::exp (-(double) j / (sr * 0.003));
                kick[(size_t) (start + j)] += (float) (0.9 * env * std::sin (twoPi * 65.0 * (double) j / sr));
            }
    }

    // Exactly one kick burst near the start, then nothing for the remainder
    // - for tests that need a State recognized (so a fingerprint can be
    // extracted) but genuinely unreferenced/silent for a long, uninterrupted
    // stretch afterward.
    void makeSingleKickFixture (std::vector<float>& bass, std::vector<float>& kick,
                                int totalSamples, double sr, int kickAtSample)
    {
        bass.assign ((size_t) totalSamples, 0.0f);
        kick.assign ((size_t) totalSamples, 0.0f);
        const double twoPi = juce::MathConstants<double>::twoPi;
        for (int i = 0; i < totalSamples; ++i)
            bass[(size_t) i] = 0.5f * (float) std::sin (twoPi * 55.0 * (double) i / sr);

        const int burst = (int) (sr * 0.02);
        for (int j = 0; j < burst && kickAtSample + j < totalSamples; ++j)
        {
            const double env = std::exp (-(double) j / (sr * 0.003));
            kick[(size_t) (kickAtSample + j)] += (float) (0.9 * env * std::sin (twoPi * 65.0 * (double) j / sr));
        }
    }

    DynamicFingerprintPrototype fingerprintOfFirstKick (const std::vector<float>& bass,
                                                        const std::vector<float>& kick, double sr)
    {
        TransientDetector detector;
        detector.prepare (sr);
        detector.setThreshold (1.0e-7f);
        detector.setMinimumEnergyGate (1.0e-8f);
        detector.setAttackReleaseMs (2.0f, 60.0f);
        detector.setTriggerRatio (1.35f);
        detector.setHoldoffMs (90.0f);

        int64_t trigger = -1;
        for (int i = 0; i < (int) kick.size(); ++i)
            if (detector.processSample (kick[(size_t) i]))
            {
                trigger = i;
                break;
            }
        if (trigger < 0)
            return {};

        DynamicFingerprintObservation obs;
        if (! extractDynamicFingerprintOffline (bass.data(), kick.data(), (int) kick.size(),
                                                sr, trigger, obs))
            return {};
        return obs.fingerprint.toPrototype();
    }

    // Drives the runtime one block at a time. `onBlockBoundary` is called
    // after each block with the block index, so a test can inject a live edit
    // (activateMap) or a transport event mid-stream. Returns the concatenated
    // channel-0 output and the maximum |sample[i] - sample[i-1]| observed -
    // a numeric click detector: this synthetic 55 Hz/0.5-amplitude tone has a
    // theoretical max per-sample slope on the order of 1e-3, so any abrupt
    // coefficient/tap jump against stale filter memory produces an easily
    // distinguishable spike.
    struct DriveResult
    {
        std::vector<float> output;
        float maxAdjacentDelta = 0.0f;
        bool allFinite = true;
    };

    DriveResult driveWithHook (DynamicProductionRuntime& runtime,
                              const std::vector<float>& bass,
                              const std::vector<float>& kick,
                              int channels, int blockSize, double strength,
                              const std::function<void (int, DynamicProductionRuntime&)>& onBlockBoundary)
    {
        DriveResult result;
        result.output.reserve (bass.size());
        const int total = (int) bass.size();
        juce::AudioBuffer<float> in (channels, blockSize);
        juce::AudioBuffer<float> outBuf (channels, blockSize);
        float previousSample = 0.0f;
        bool havePrevious = false;
        int blockIndex = 0;
        for (int offset = 0; offset < total; offset += blockSize, ++blockIndex)
        {
            const int n = std::min (blockSize, total - offset);
            for (int ch = 0; ch < channels; ++ch)
                for (int i = 0; i < n; ++i)
                    in.setSample (ch, i, bass[(size_t) (offset + i)]);
            const bool ok = runtime.process (in, bass.data() + offset,
                                             kick.data() + offset, true, strength, outBuf, n);
            if (! ok)
                result.allFinite = false;
            for (int i = 0; i < n; ++i)
            {
                const float sample = outBuf.getSample (0, i);
                if (! std::isfinite (sample))
                    result.allFinite = false;
                if (havePrevious)
                    result.maxAdjacentDelta = std::max (result.maxAdjacentDelta, std::abs (sample - previousSample));
                previousSample = sample;
                havePrevious = true;
                result.output.push_back (sample);
            }
            if (onBlockBoundary)
                onBlockBoundary (blockIndex, runtime);
        }
        return result;
    }
}

class DynamicStateEditAuditionTests : public juce::UnitTest
{
public:
    DynamicStateEditAuditionTests()
        : juce::UnitTest ("DynamicStateEditAudition", "Dynamic State Map") {}

    void runTest() override
    {
        beginTest ("Editing an already-audible State's Delay/Frequency/Q produces no click");
        {
            const int blockSize = 256;
            const int channels = 2;
            std::vector<float> bass, kick;
            makeFixture (bass, kick, (int) (kSR * 6.0), kSR, (int) (kSR * 0.4));
            const auto fp = fingerprintOfFirstKick (bass, kick, kSR);
            expect (fp.valid, "fixture must produce a valid fingerprint to test against");

            const uint64_t id = 42;
            auto initialMap = makeSingleStateMap (fp, id, kSR, 1.0f, 80.0f, 1.2f);
            auto editedMap = makeSingleStateMap (fp, id, kSR, -2.5f, 150.0f, 0.5f);

            DynamicProductionRuntime runtime;
            expect (runtime.prepare (kSR, blockSize, channels));
            runtime.activateMap (initialMap);

            bool editApplied = false;
            const auto result = driveWithHook (runtime, bass, kick, channels, blockSize, 1.0,
                [&] (int blockIndex, DynamicProductionRuntime& rt)
                {
                    // Apply the edit only after the State has had time to be
                    // recognized and become audible at least once.
                    if (! editApplied && blockIndex * blockSize > (int) (kSR * 1.0))
                    {
                        rt.activateMap (editedMap);
                        editApplied = true;
                    }
                });

            expect (editApplied, "test must actually reach the point where the edit is injected");
            expect (result.allFinite, "no NaN/Inf anywhere in the output");
            expectLessThan (result.maxAdjacentDelta, 0.05f,
                            "no abrupt single-sample discontinuity around the live edit");
        }

        beginTest ("Rapid repeated edits (every ~30 ms) settle on the newest value, no click, no allocation");
        {
            const int blockSize = 256;
            const int channels = 2;
            std::vector<float> bass, kick;
            makeFixture (bass, kick, (int) (kSR * 6.0), kSR, (int) (kSR * 0.35));
            const auto fp = fingerprintOfFirstKick (bass, kick, kSR);
            expect (fp.valid);

            const uint64_t id = 7;
            auto initialMap = makeSingleStateMap (fp, id, kSR, 0.0f, 100.0f, 0.7f);

            DynamicProductionRuntime runtime;
            expect (runtime.prepare (kSR, blockSize, channels));
            runtime.activateMap (initialMap);

            int editCount = 0;
            const int editEveryBlocks = (int) std::ceil (kSR * 0.03 / blockSize); // ~30 ms
            DynamicStateMap lastEdit = initialMap;

            // Pre-allocate every buffer used by the drive loop, and reserve
            // the output vector, BEFORE the allocation counter starts
            // tracking: only the actual runtime.process()/activateMap()
            // calls below - the production audio-thread/edit-publish path -
            // should be measured, not this test harness's own setup.
            const int total = (int) bass.size();
            juce::AudioBuffer<float> in (channels, blockSize);
            juce::AudioBuffer<float> outBuf (channels, blockSize);
            std::vector<float> output;
            output.reserve ((size_t) total);
            bool allFinite = true;
            float maxAdjacentDelta = 0.0f;
            float previousSample = 0.0f;
            bool havePrevious = false;

            ScopedTestAllocationCounter allocationCounter;
            int blockIndex = 0;
            for (int offset = 0; offset < total; offset += blockSize, ++blockIndex)
            {
                const int n = std::min (blockSize, total - offset);
                for (int ch = 0; ch < channels; ++ch)
                    for (int i = 0; i < n; ++i)
                        in.setSample (ch, i, bass[(size_t) (offset + i)]);
                if (! runtime.process (in, bass.data() + offset, kick.data() + offset, true, 1.0, outBuf, n))
                    allFinite = false;
                for (int i = 0; i < n; ++i)
                {
                    const float sample = outBuf.getSample (0, i);
                    if (! std::isfinite (sample))
                        allFinite = false;
                    if (havePrevious)
                        maxAdjacentDelta = std::max (maxAdjacentDelta, std::abs (sample - previousSample));
                    previousSample = sample;
                    havePrevious = true;
                    output.push_back (sample);
                }

                if (blockIndex > (int) (kSR * 1.0 / blockSize) && blockIndex % editEveryBlocks == 0
                    && editCount < 10)
                {
                    const float delay = -2.0f + 0.4f * (float) editCount;
                    const float freq = 60.0f + 10.0f * (float) editCount;
                    lastEdit = makeSingleStateMap (fp, id, kSR, delay, freq, 0.6f);
                    runtime.activateMap (lastEdit);
                    ++editCount;
                }
            }
            const auto allocationSnapshot = allocationCounter.snapshot();

            expect (editCount == 10, "test must actually inject the full rapid-edit sequence");
            expect (allFinite);
            expectLessThan (maxAdjacentDelta, 0.05f, "rapid edits still never click");
            expectEquals ((int) allocationSnapshot.count, 0,
                         "no heap allocation anywhere during rapid live edits");
        }

        beginTest ("Edit while unreferenced (Global-only recognized State) commits directly, no click");
        {
            const int blockSize = 256;
            const int channels = 2;
            std::vector<float> bass, kick;
            // Exactly one kick at t=0.2s (so a fingerprint can be extracted
            // and the State recognized once), then a guaranteed 2.8s+ of
            // silence with no further kicks at all - the State is
            // recognized but never actively selected/audible during or
            // after the edit.
            makeSingleKickFixture (bass, kick, (int) (kSR * 3.0), kSR, (int) (kSR * 0.2));
            const auto fp = fingerprintOfFirstKick (bass, kick, kSR);
            expect (fp.valid);

            const uint64_t id = 3;
            auto initialMap = makeSingleStateMap (fp, id, kSR, 0.5f, 90.0f, 0.9f);
            auto editedMap = makeSingleStateMap (fp, id, kSR, 1.5f, 120.0f, 1.1f);

            DynamicProductionRuntime runtime;
            expect (runtime.prepare (kSR, blockSize, channels));
            runtime.activateMap (initialMap);

            bool editApplied = false;
            const auto result = driveWithHook (runtime, bass, kick, channels, blockSize, 1.0,
                [&] (int blockIndex, DynamicProductionRuntime& rt)
                {
                    // Well after the one kick at 0.2s has fully settled/faded.
                    if (! editApplied && blockIndex * blockSize > (int) (kSR * 1.0))
                    {
                        rt.activateMap (editedMap);
                        editApplied = true;
                    }
                });

            expect (editApplied);
            expect (result.allFinite);
            expectLessThan (result.maxAdjacentDelta, 0.05f);
        }

        beginTest ("Edit survives a loop-wrap transport reset immediately after injection");
        {
            const int blockSize = 256;
            const int channels = 2;
            std::vector<float> bass, kick;
            makeFixture (bass, kick, (int) (kSR * 6.0), kSR, (int) (kSR * 0.4));
            const auto fp = fingerprintOfFirstKick (bass, kick, kSR);
            expect (fp.valid);

            const uint64_t id = 9;
            auto initialMap = makeSingleStateMap (fp, id, kSR, 1.0f, 80.0f, 1.2f);
            auto editedMap = makeSingleStateMap (fp, id, kSR, -1.0f, 130.0f, 0.6f);

            DynamicProductionRuntime runtime;
            expect (runtime.prepare (kSR, blockSize, channels));
            runtime.activateMap (initialMap);

            bool editApplied = false;
            bool loopWrapped = false;
            const auto result = driveWithHook (runtime, bass, kick, channels, blockSize, 1.0,
                [&] (int blockIndex, DynamicProductionRuntime& rt)
                {
                    if (! editApplied && blockIndex * blockSize > (int) (kSR * 1.0))
                    {
                        rt.activateMap (editedMap);
                        editApplied = true;
                        return;
                    }
                    if (editApplied && ! loopWrapped && blockIndex * blockSize > (int) (kSR * 1.2))
                    {
                        rt.notifyTransportReset (DynamicProductionTransportReason::LoopWrap);
                        loopWrapped = true;
                    }
                });

            expect (editApplied);
            expect (loopWrapped);
            expect (result.allFinite, "loop wrap immediately after an in-flight edit stays finite");
            expectLessThan (result.maxAdjacentDelta, 0.05f, "loop wrap does not itself introduce a click");
        }

        beginTest ("Seek immediately after an edit safely falls back without corrupting later output");
        {
            const int blockSize = 256;
            const int channels = 2;
            std::vector<float> bass, kick;
            makeFixture (bass, kick, (int) (kSR * 6.0), kSR, (int) (kSR * 0.4));
            const auto fp = fingerprintOfFirstKick (bass, kick, kSR);
            expect (fp.valid);

            const uint64_t id = 11;
            auto initialMap = makeSingleStateMap (fp, id, kSR, 1.0f, 80.0f, 1.2f);
            auto editedMap = makeSingleStateMap (fp, id, kSR, 2.0f, 95.0f, 1.4f);

            DynamicProductionRuntime runtime;
            expect (runtime.prepare (kSR, blockSize, channels));
            runtime.activateMap (initialMap);

            // A Seek is a deliberate transport jump: the frozen contract
            // explicitly allows an abrupt reset back to Global at that
            // boundary, and afterward a fresh confident match triggers a
            // perfectly ordinary Global<->State/Service crossfade (already
            // proven click-safe by DynamicReleaseGateTests.cpp's own
            // localizedTransitionScore metric, which correctly tolerates the
            // inherent phase difference between two differently-processed
            // branches during a smooth linear gain fade). A raw adjacent-
            // sample delta cannot distinguish that legitimate, expected
            // transient from a genuine coefficient-swap-on-a-live-branch
            // click, so this test only asserts click-safety up to the seek
            // (proving the *edit itself*, the thing Section 12 actually
            // governs, never clicked before the reset) and finiteness
            // afterward - not zero-click across the reset/reacquisition
            // window, which is a different, already-covered property.
            bool editApplied = false;
            bool sought = false;
            size_t seekSample = 0;
            std::function<void (int, DynamicProductionRuntime&)> onBlock =
                [&] (int blockIndex, DynamicProductionRuntime& rt)
                {
                    if (! editApplied && blockIndex * blockSize > (int) (kSR * 1.0))
                    {
                        rt.activateMap (editedMap);
                        editApplied = true;
                        return;
                    }
                    if (editApplied && ! sought && blockIndex * blockSize > (int) (kSR * 1.05))
                    {
                        rt.notifyTransportReset (DynamicProductionTransportReason::Seek);
                        sought = true;
                        seekSample = (size_t) ((blockIndex + 1) * blockSize);
                    }
                };
            const auto result = driveWithHook (runtime, bass, kick, channels, blockSize, 1.0, onBlock);

            expect (editApplied);
            expect (sought);
            expect (result.allFinite);

            float maxDeltaBeforeSeek = 0.0f;
            for (size_t i = 1; i < seekSample && i < result.output.size(); ++i)
                maxDeltaBeforeSeek = std::max (maxDeltaBeforeSeek,
                                               std::abs (result.output[i] - result.output[i - 1]));
            expectLessThan (maxDeltaBeforeSeek, 0.05f,
                            "the edit itself, before any transport reset, never clicks");
        }

        beginTest ("Edit at 96 kHz and 192 kHz remains click-free");
        {
            for (const double sr : { 96000.0, 192000.0 })
            {
                const int blockSize = 512;
                const int channels = 2;
                std::vector<float> bass, kick;
                makeFixture (bass, kick, (int) (sr * 6.0), sr, (int) (sr * 0.4));
                const auto fp = fingerprintOfFirstKick (bass, kick, sr);
                expect (fp.valid);

                const uint64_t id = 55;
                auto initialMap = makeSingleStateMap (fp, id, sr, 1.0f, 80.0f, 1.2f);
                auto editedMap = makeSingleStateMap (fp, id, sr, -1.5f, 140.0f, 0.5f);

                DynamicProductionRuntime runtime;
                expect (runtime.prepare (sr, blockSize, channels));
                runtime.activateMap (initialMap);

                bool editApplied = false;
                const auto result = driveWithHook (runtime, bass, kick, channels, blockSize, 1.0,
                    [&] (int blockIndex, DynamicProductionRuntime& rt)
                    {
                        if (! editApplied && blockIndex * blockSize > (int) (sr * 1.0))
                        {
                            rt.activateMap (editedMap);
                            editApplied = true;
                        }
                    });

                expect (editApplied);
                expect (result.allFinite);
                expectLessThan (result.maxAdjacentDelta, 0.05f);
            }
        }

        beginTest ("Deterministic replay: identical input and edit sequence produce identical output");
        {
            const int blockSize = 256;
            const int channels = 2;
            std::vector<float> bass, kick;
            makeFixture (bass, kick, (int) (kSR * 4.0), kSR, (int) (kSR * 0.4));
            const auto fp = fingerprintOfFirstKick (bass, kick, kSR);
            expect (fp.valid);

            const uint64_t id = 21;
            auto initialMap = makeSingleStateMap (fp, id, kSR, 1.0f, 80.0f, 1.2f);
            auto editedMap = makeSingleStateMap (fp, id, kSR, -0.5f, 110.0f, 0.8f);

            auto runOnce = [&] ()
            {
                DynamicProductionRuntime runtime;
                runtime.prepare (kSR, blockSize, channels);
                runtime.activateMap (initialMap);
                bool editApplied = false;
                return driveWithHook (runtime, bass, kick, channels, blockSize, 1.0,
                    [&] (int blockIndex, DynamicProductionRuntime& rt)
                    {
                        if (! editApplied && blockIndex * blockSize > (int) (kSR * 1.0))
                        {
                            rt.activateMap (editedMap);
                            editApplied = true;
                        }
                    }).output;
            };

            const auto first = runOnce();
            const auto second = runOnce();
            expect (first.size() == second.size());
            bool identical = true;
            for (size_t i = 0; i < first.size(); ++i)
                if (first[i] != second[i])
                {
                    identical = false;
                    break;
                }
            expect (identical, "replaying the identical input/edit sequence is fully deterministic");
        }

        beginTest ("Self-heal: reactivating an unchanged map reconfigures a persistent slot the engine cleared out-of-band");
        {
            // Regression coverage for a real bug this phase introduced and
            // fixed: configurePackagesIfNeeded()'s "content unchanged, skip
            // reconfiguration" cache only recorded what IT last wrote, so it
            // could not detect the engine's own slot having been cleared by
            // something else - reactivating the identical map (same
            // generation-worthy content) must still notice the divergence
            // and repair it, not silently trust the stale cache.
            const int blockSize = 32;
            const int channels = 1;
            std::vector<float> bass, kick;
            makeFixture (bass, kick, (int) (kSR * 4.0), kSR, (int) (kSR * 0.4));
            const auto fp = fingerprintOfFirstKick (bass, kick, kSR);
            expect (fp.valid);

            const uint64_t id = 501;
            auto map = makeSingleStateMap (fp, id, kSR, 1.0f, 90.0f, 1.0f);

            DynamicProductionRuntime runtime;
            expect (runtime.prepare (kSR, blockSize, channels));
            runtime.activateMap (map);

            juce::AudioBuffer<float> in (channels, blockSize);
            juce::AudioBuffer<float> out (channels, blockSize);
            const int total = ((int) bass.size() / blockSize) * blockSize;
            auto renderTo = [&] (int upToSample)
            {
                for (int offset = 0; offset + blockSize <= upToSample; offset += blockSize)
                {
                    for (int i = 0; i < blockSize; ++i)
                        in.setSample (0, i, bass[(size_t) (offset + i)]);
                    runtime.process (in, bass.data() + offset, kick.data() + offset, true, 1.0, out, blockSize);
                }
            };

            // Warm the persistent slot through several kicks (natural
            // cold-start -> Service -> State progression).
            renderTo (total / 2);
            expect (runtime.getEngineForTesting().getStateInfo (0).active,
                    "setup: the persistent slot must actually be configured before this test can mean anything");

            // Simulate an out-of-band engine-level clear (the same technique
            // the pre-existing DynamicReleaseGateTests.cpp "Global -> Service
            // and Service -> State" test uses) and confirm the precondition
            // this bug depended on: the identity is still referenced via
            // Service at this exact moment, even though the persistent slot
            // itself is not.
            runtime.getEngineForTesting().clearStateSlot (0);
            expect (! runtime.getEngineForTesting().getStateInfo (0).active,
                    "setup: the slot is genuinely cleared, not just re-warmed by the next render");

            // Reactivating the SAME map (identical resolved package content,
            // only a new generation number) is the production self-heal
            // trigger.
            runtime.activateMap (map);
            renderTo (total);

            expect (runtime.getEngineForTesting().getStateInfo (0).active,
                    "self-heal: the persistent slot is reconfigured, not left silently cleared, "
                    "even though nothing about the desired package content changed");
        }

        beginTest ("Direct commit is gated on the specific slot's liveness, not the identity's Service binding");
        {
            // Regression coverage for the exact fix: the gating check must
            // be scheduler.isStateSlotReferenced(slot) (this slot only), not
            // scheduler.isSemanticStateReferenced(id) (any branch, including
            // Service) - otherwise a Service binding for the same identity
            // would wrongly be treated as proof the persistent slot itself
            // is unsafe to touch, permanently blocking the self-heal above
            // whenever the identity keeps being confidently matched.
            const int blockSize = 32;
            const int channels = 1;
            std::vector<float> bass, kick;
            makeFixture (bass, kick, (int) (kSR * 4.0), kSR, (int) (kSR * 0.4));
            const auto fp = fingerprintOfFirstKick (bass, kick, kSR);
            expect (fp.valid);

            const uint64_t id = 502;
            auto map = makeSingleStateMap (fp, id, kSR, 1.0f, 90.0f, 1.0f);

            DynamicProductionRuntime runtime;
            expect (runtime.prepare (kSR, blockSize, channels));
            runtime.activateMap (map);

            juce::AudioBuffer<float> in (channels, blockSize);
            juce::AudioBuffer<float> out (channels, blockSize);
            const int total = ((int) bass.size() / blockSize) * blockSize;
            auto renderTo = [&] (int upToSample)
            {
                for (int offset = 0; offset + blockSize <= upToSample; offset += blockSize)
                {
                    for (int i = 0; i < blockSize; ++i)
                        in.setSample (0, i, bass[(size_t) (offset + i)]);
                    runtime.process (in, bass.data() + offset, kick.data() + offset, true, 1.0, out, blockSize);
                }
            };

            renderTo (total / 2);
            expect (runtime.getEngineForTesting().getStateInfo (0).active);

            runtime.getEngineForTesting().clearStateSlot (0);
            // Render past at least one full kick period so a fresh confident
            // match actually occurs and Service gets (re)bound to this
            // identity now that the persistent slot can no longer serve it.
            renderTo (total / 2 + (int) (kSR * 0.4) + 2000);

            // The identity must now be live via Service specifically (the
            // precondition this fix depends on), while the persistent slot
            // itself remains the one that is not.
            // Note: DynamicHotBranchEngine always stores 0 as Service's own
            // internal stableStateId (Phase 5); the semantic binding is
            // DynamicProductionRuntime's own bookkeeping.
            expect (runtime.getServiceBindingValid() && runtime.getServiceBoundStableStateId() == id,
                    "setup: Service, not the persistent slot, is currently serving this identity");
            expect (! runtime.getEngineForTesting().getStateInfo (0).active,
                    "setup: the persistent slot is still genuinely cleared at this point");

            runtime.activateMap (map);
            renderTo (total);

            expect (runtime.getEngineForTesting().getStateInfo (0).active,
                    "the persistent slot is still reconfigured directly - Service holding the same "
                    "identity must not be mistaken for the slot itself being unsafe to touch");
        }
    }
};

static DynamicStateEditAuditionTests dynamicStateEditAuditionTests;
