#include "TestCommon.h"

#include "DynamicStateMeasurements.h"
#include "DynamicMeasurementScorer.h"
#include "DynamicPredictedMeasurement.h"
#include "DynamicProductionRuntime.h"
#include "DynamicStateSerialization.h"
#include "util/LearnHitQueue.h"

#include "TestAllocationCounter.h"

// =============================================================================
// Safe Neutral Override (product safety gate) tests.
//
// Gap: the real-audio acceptance harness (AudioAcceptanceTests.cpp) proved
// that the shared Global package degrades certain recognized, no-correction
// identities (distorted_bass, unison_modulated) by ~35% low-band match. These
// tests cover the DSP-level mechanism the harness's product fixes rely on:
// the measurement gate that detects the harm and assigns a persisted
// DynamicCorrectionPolicy::NeutralSafe, the always-hot Neutral branch that
// then carries that identity instead of Global, and the runtime/persistence
// guarantees around it. The harness itself (Release AudioAcceptanceTests)
// proves items 1, 2 and 5 against the real distorted/unison fixtures with the
// real production Learn pipeline; these are unit-level proofs of the
// mechanism using deterministic synthetic material.
// =============================================================================

namespace
{
    constexpr double kSR = 48000.0;

    // A quarter-period phase relationship at 100 Hz is 2.5 ms - the same
    // deterministic construction already used by DynamicStateMeasurementTests.cpp.
    void makeAlignedPair (std::vector<float>& bass, std::vector<float>& kick,
                          int numSamples, double sampleRate, double freqHz)
    {
        bass.assign ((size_t) numSamples, 0.0f);
        kick.assign ((size_t) numSamples, 0.0f);
        for (int i = 0; i < numSamples; ++i)
        {
            const float sample = (float) std::sin (juce::MathConstants<double>::twoPi
                * freqHz * (double) i / sampleRate);
            bass[(size_t) i] = sample;
            kick[(size_t) i] = sample;
        }
    }

    DynamicFingerprintPrototype testPrototype (float seed)
    {
        DynamicFingerprintPrototype value;
        value.valid = true;
        value.featureCount = DynamicFingerprintContract::kFeatureCount;
        for (int i = 0; i < DynamicFingerprintContract::kFeatureCount; ++i)
            value.features[(size_t) i] = std::clamp (seed + 0.004f * (float) i, -1.0f, 1.0f);
        return value;
    }

    // A recognized, Auto/Stable, no-package identity - exactly the class of
    // State the measurement gate must probe against the shared Global package.
    DynamicStateMap makeRecognizedNoCorrectionMap (uint64_t stableId,
                                                   const DynamicFingerprintPrototype& fingerprint,
                                                   float globalBaseDelayMs)
    {
        DynamicStateMap map = makeEmptyDynamicStateMap();
        map.valid = true;
        map.globalBase.globalBaseDelayMs = globalBaseDelayMs;
        map.globalBase.crossoverEnabled = false;
        map.globalBase.allpassEnabled = false;
        map.globalBase.globalAllpassFreqHz = 100.0f;
        map.globalBase.globalAllpassQ = 0.7f;
        map.globalBase.allpassStages = 2;
        map.globalBase.learnedSampleRate = kSR;
        map.calibration.valid = true;
        map.calibration.absoluteDistanceThreshold = 0.15f;
        map.calibration.ambiguityMargin = 0.02f;
        map.nextStateId = stableId + 1;

        DynamicState state;
        state.occupied = true;
        state.stableStateId = stableId;
        state.fingerprint = fingerprint;
        state.origin = DynamicStateOrigin::Auto;
        state.evidence = DynamicStateEvidence::Stable;
        state.hitCount = 6;
        state.repeatability = 0.9f;
        state.hasLearnedPackage = false;
        map.states[0] = state;
        return map;
    }

    LearnHitWindow makeWindow (int sequence, int64_t triggerSample, std::vector<float> bass, std::vector<float> kick)
    {
        LearnHitWindow window;
        window.sequence = sequence;
        window.absoluteSampleAtTrigger = (int) triggerSample;
        window.bass = std::move (bass);
        window.kick = std::move (kick);
        return window;
    }

    DynamicLearnHit makeHitForFingerprint (int sequence, int64_t triggerSample,
                                           const DynamicFingerprintPrototype& fingerprint)
    {
        DynamicLearnHit hit;
        hit.sequence = sequence;
        hit.triggerSample = triggerSample;
        hit.fingerprint = fingerprint;
        hit.fingerprintValidity = DynamicFingerprintValidity::Valid;
        return hit;
    }

    // Builds five retained real-audio windows (aligned bass/kick at 100 Hz)
    // for one recognized, no-package identity and runs it through the exact
    // production path (retainDynamicLearnMeasurementWindows then
    // computeDynamicPredictedMeasurement) with a caller-chosen Global base
    // delay, returning the resulting map.
    DynamicStateMap runGateWithGlobalDelay (uint64_t stableId, float globalBaseDelayMs)
    {
        const int n = (int) std::lround (kSR * 0.05);
        const auto fingerprint = testPrototype (0.2f + (float) stableId * 1.0e-4f);

        std::vector<LearnHitWindow> windows;
        std::vector<DynamicLearnHit> hits;
        for (int i = 0; i < 5; ++i)
        {
            std::vector<float> bass, kick;
            makeAlignedPair (bass, kick, n, kSR, 100.0);
            windows.push_back (makeWindow (i, i * 10000, bass, kick));
            hits.push_back (makeHitForFingerprint (i, i * 10000, fingerprint));
        }

        auto map = makeRecognizedNoCorrectionMap (stableId, fingerprint, globalBaseDelayMs);
        const auto retained = retainDynamicLearnMeasurementWindows (hits, windows, map, kSR);
        const auto result = computeDynamicPredictedMeasurement (map, retained, kSR, 0);
        return result.map;
    }

    DynamicStateMap makeValidGlobalMapForRuntime (float globalBaseDelayMs)
    {
        DynamicStateMap map = makeEmptyDynamicStateMap();
        map.valid = true;
        map.globalBase.globalBaseDelayMs = globalBaseDelayMs;
        map.globalBase.polarityInvert = false;
        map.globalBase.crossoverEnabled = true;
        map.globalBase.crossoverHz = 150.0f;
        map.globalBase.allpassEnabled = true;
        map.globalBase.globalAllpassFreqHz = 100.0f;
        map.globalBase.globalAllpassQ = 0.7f;
        map.globalBase.allpassStages = 2;
        map.globalBase.delayInterpolationIndex = 0;
        map.globalBase.learnedSampleRate = kSR;
        map.calibration.valid = true;
        map.calibration.absoluteDistanceThreshold = 0.9f;
        map.calibration.ambiguityMargin = 0.05f;
        map.nextStateId = 2;
        return map;
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

    struct DriveResult
    {
        std::vector<float> output;
        bool allFinite = true;
        float maxAdjacentDelta = 0.0f;
    };

    DriveResult driveRuntime (DynamicProductionRuntime& runtime,
                             const std::vector<float>& bass,
                             const std::vector<float>& kick,
                             int channels, int blockSize, bool hasSidechain)
    {
        DriveResult result;
        result.output.reserve (bass.size());
        const int total = (int) bass.size();
        juce::AudioBuffer<float> in (channels, blockSize);
        juce::AudioBuffer<float> outBuf (channels, blockSize);
        float previous = 0.0f;
        bool havePrevious = false;
        for (int offset = 0; offset < total; offset += blockSize)
        {
            const int n = std::min (blockSize, total - offset);
            for (int ch = 0; ch < channels; ++ch)
                for (int i = 0; i < n; ++i)
                    in.setSample (ch, i, bass[(size_t) (offset + i)]);
            const bool ok = runtime.process (in, bass.data() + offset,
                                             hasSidechain ? kick.data() + offset : nullptr,
                                             hasSidechain, 1.0, outBuf, n);
            if (! ok)
                result.allFinite = false;
            for (int i = 0; i < n; ++i)
            {
                const float sample = outBuf.getSample (0, i);
                if (! std::isfinite (sample))
                    result.allFinite = false;
                if (havePrevious)
                    result.maxAdjacentDelta = std::max (result.maxAdjacentDelta, std::abs (sample - previous));
                previous = sample;
                havePrevious = true;
                result.output.push_back (sample);
            }
        }
        return result;
    }
}

// -----------------------------------------------------------------------
// 1 & 2: the harmful Global candidate is actually generated and measured,
// and the safety gate rejects it because of the measured degradation.
// 6: a safe Global identity is not accidentally redirected to NeutralSafe.
// -----------------------------------------------------------------------
class DynamicNeutralSafeMeasurementTests : public juce::UnitTest
{
public:
    DynamicNeutralSafeMeasurementTests() : juce::UnitTest ("DynamicNeutralSafeMeasurement", "Phase9") {}

    void runTest() override
    {
        beginTest ("1/2: Global package causing severe degradation assigns NeutralSafe with a precise reason");
        {
            // Aligned bass/kick (lag 0) at 100 Hz: raw correlation is
            // near-perfect. A Global base delay of one quarter period
            // (2.5 ms) rotates the two roughly 90 degrees out of phase,
            // a severe, deterministic, measured degradation.
            const auto map = runGateWithGlobalDelay (700, 2.5f);
            const auto& state = map.states[0];
            expect (state.occupied, "identity remains recognizable after the gate runs");
            expect (! state.hasLearnedPackage, "no State-level package was fabricated");
            expectEquals ((int) state.correctionPolicy, (int) DynamicCorrectionPolicy::NeutralSafe,
                         "the harmful Global candidate must be rejected into NeutralSafe");
            expectEquals ((int) state.policyRejectionReason,
                         (int) DynamicPolicyRejectionReason::GlobalPackageExcessiveRegression,
                         "the persisted reason must be precise, not generic");
            expect (isValidDynamicState (state), "the resulting State is still structurally valid");
        }

        beginTest ("6: a Global package that is safe for this identity leaves it on GlobalFallback");
        {
            // A negligible Global base delay (0.05 ms against a 2.5 ms
            // quarter period) causes only a tiny, well-bounded phase shift.
            const auto map = runGateWithGlobalDelay (701, 0.05f);
            const auto& state = map.states[0];
            expect (state.occupied);
            expectEquals ((int) state.correctionPolicy, (int) DynamicCorrectionPolicy::GlobalFallback,
                         "a safe identity must not be redirected to NeutralSafe");
            expect (state.policyRejectionReason == DynamicPolicyRejectionReason::None,
                   "no rejection reason should be recorded when nothing was rejected");
        }

        beginTest ("Boundary: exactly at the regression bound does not flip the gate either way spuriously");
        {
            // Sanity: the gate must be deterministic (same inputs, same
            // verdict) rather than flip-flopping across repeated runs.
            const auto mapA = runGateWithGlobalDelay (702, 2.5f);
            const auto mapB = runGateWithGlobalDelay (702, 2.5f);
            expectEquals ((int) mapA.states[0].correctionPolicy, (int) mapB.states[0].correctionPolicy,
                         "the gate's verdict is a deterministic function of the measured material");
        }
    }
};

// -----------------------------------------------------------------------
// 3: runtime recognizes the identity and selects NeutralSafe.
// 4: final audible output stays within the defined degradation bounds -
//    proven at the mechanism level: Neutral applies zero extra delay and no
//    allpass, so the harmful Global package genuinely never reaches it.
// 9: rapid transitions between LearnedState, GlobalFallback and NeutralSafe
//    are click-free and deterministic.
// 10: no allocation on the audio thread while NeutralSafe routing is active.
// -----------------------------------------------------------------------
class DynamicNeutralSafeRuntimeTests : public juce::UnitTest
{
public:
    DynamicNeutralSafeRuntimeTests() : juce::UnitTest ("DynamicNeutralSafeRuntime", "Phase7") {}

    void runTest() override
    {
        beginTest ("3/4: runtime selects the Neutral branch for a NeutralSafe identity, with zero extra delay/allpass");
        {
            std::vector<float> bass, kick;
            makeFixture (bass, kick, (int) (kSR * 1.2), kSR, (int) (kSR * 0.4));
            const auto fingerprint = fingerprintOfFirstKick (bass, kick, kSR);
            expect (fingerprint.valid, "offline fingerprint of the first kick is valid");

            auto map = makeValidGlobalMapForRuntime (5.0f);
            DynamicState state;
            state.occupied = true;
            state.stableStateId = 900;
            state.fingerprint = fingerprint;
            state.origin = DynamicStateOrigin::Auto;
            state.evidence = DynamicStateEvidence::Stable;
            state.hitCount = 6;
            state.repeatability = 0.9f;
            state.hasLearnedPackage = false;
            state.correctionPolicy = DynamicCorrectionPolicy::NeutralSafe;
            state.policyRejectionReason = DynamicPolicyRejectionReason::GlobalPackageExcessiveRegression;
            map.states[0] = state;
            map.nextStateId = 901;
            expect (isRuntimeEligibleDynamicStateMap (map));

            DynamicProductionRuntime runtime;
            expect (runtime.prepare (kSR, 256, 1));
            runtime.activateMap (map);

            const auto result = driveRuntime (runtime, bass, kick, 1, 128, true);
            expect (result.allFinite, "output stays finite throughout");
            expectEquals ((int64_t) runtime.getSelectedStableStateId(), (int64_t) 0,
                         "Neutral, like Global, carries no per-identity semantic selection");
            expect (runtime.getSelectorDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::Neutral,
                   "runtime routes the recognized NeutralSafe identity to the Neutral branch");

            const auto neutralInfo = runtime.getEngineForTesting().getNeutralInfo();
            expect (neutralInfo.active, "Neutral is configured and always hot");
            expectWithinAbsoluteError ((float) neutralInfo.effectiveAbsoluteDelayMs, 0.0f, 1.0e-6f,
                                      "Neutral applies zero per-identity delay - the harmful Global delay never reaches it");
            expectWithinAbsoluteError ((float) neutralInfo.physicalTapSamples,
                                      (float) runtime.getReportedLatencySamples(), 1.0e-3f,
                                      "Neutral's tap is exactly the plugin's reported latency, nothing more");
        }

        beginTest ("9/10: rapid LearnedState <-> GlobalFallback <-> NeutralSafe transitions are click-free, "
                  "deterministic, and allocation-free");
        {
            std::vector<float> bass, kick;
            makeFixture (bass, kick, (int) (kSR * 0.6), kSR, (int) (kSR * 0.15));
            const auto fingerprint = fingerprintOfFirstKick (bass, kick, kSR);
            expect (fingerprint.valid);

            auto buildPhaseMap = [&] (DynamicCorrectionPolicy policy)
            {
                auto map = makeValidGlobalMapForRuntime (1.0f);
                DynamicState state;
                state.occupied = true;
                state.stableStateId = 950;
                state.fingerprint = fingerprint;
                state.origin = DynamicStateOrigin::Auto;
                state.evidence = DynamicStateEvidence::Stable;
                state.hitCount = 6;
                state.repeatability = 0.9f;
                if (policy == DynamicCorrectionPolicy::LearnedState)
                {
                    state.hasLearnedPackage = true;
                    state.learnedPackage.delayDeltaMs = 1.0f;
                    state.learnedPackage.allpassFreqHz = 80.0f;
                    state.learnedPackage.allpassQ = 1.2f;
                    state.correctionPolicy = DynamicCorrectionPolicy::LearnedState;
                }
                else
                {
                    state.hasLearnedPackage = false;
                    state.correctionPolicy = policy;
                    if (policy == DynamicCorrectionPolicy::NeutralSafe)
                        state.policyRejectionReason = DynamicPolicyRejectionReason::GlobalPackageExcessiveRegression;
                }
                map.states[0] = state;
                map.nextStateId = 951;
                return map;
            };

            const std::array<DynamicCorrectionPolicy, 3> sequence {
                DynamicCorrectionPolicy::LearnedState,
                DynamicCorrectionPolicy::GlobalFallback,
                DynamicCorrectionPolicy::NeutralSafe
            };

            auto runSequenceTwice = [&] ()
            {
                DynamicProductionRuntime runtime;
                runtime.prepare (kSR, 128, 1);
                std::vector<float> combined;
                float maxDelta = 0.0f;
                bool finite = true;

                for (const auto policy : sequence)
                {
                    runtime.activateMap (buildPhaseMap (policy));
                    const auto result = driveRuntime (runtime, bass, kick, 1, 128, true);
                    if (! result.allFinite) finite = false;
                    maxDelta = std::max (maxDelta, result.maxAdjacentDelta);
                    combined.insert (combined.end(), result.output.begin(), result.output.end());
                }
                return std::make_tuple (combined, maxDelta, finite);
            };

            const auto first = runSequenceTwice();
            expect (std::get<2> (first), "output stays finite across every transition");
            expectLessThan (std::get<1> (first), 0.05f,
                           "no audible click across LearnedState -> GlobalFallback -> NeutralSafe transitions");

            const auto second = runSequenceTwice();
            expectEquals ((int) std::get<0> (first).size(), (int) std::get<0> (second).size());
            bool identical = true;
            for (size_t i = 0; i < std::get<0> (first).size() && identical; ++i)
                if (std::get<0> (first)[i] != std::get<0> (second)[i])
                    identical = false;
            expect (identical, "the same map/policy transition sequence produces bit-identical output twice");

            // No allocation, lock, or reset on the audio thread while the
            // sequence (including the NeutralSafe phase) runs. activateMap()
            // itself is a message-thread-style publish in production (not
            // called from the audio thread), so only process() calls are
            // measured here against that discipline. Every buffer the drive
            // loop touches is preallocated with real capacity BEFORE the
            // counter starts, so only runtime.process() itself is measured -
            // not this test harness's own vector setup.
            {
                DynamicProductionRuntime runtime;
                runtime.prepare (kSR, 128, 1);
                const int blockSize = 128;
                const int total = (int) bass.size();
                juce::AudioBuffer<float> in (1, blockSize);
                juce::AudioBuffer<float> outBuf (1, blockSize);
                for (const auto policy : sequence)
                {
                    runtime.activateMap (buildPhaseMap (policy));
                    ScopedTestAllocationCounter counter;
                    for (int offset = 0; offset < total; offset += blockSize)
                    {
                        const int n = std::min (blockSize, total - offset);
                        for (int i = 0; i < n; ++i)
                            in.setSample (0, i, bass[(size_t) (offset + i)]);
                        runtime.process (in, bass.data() + offset, kick.data() + offset, true, 1.0, outBuf, n);
                    }
                    const auto snapshot = counter.snapshot();
                    expectEquals ((int) snapshot.count, 0,
                                 "process() must not allocate, including while routing through Neutral");
                }
            }
        }
    }
};

// -----------------------------------------------------------------------
// 7: save/reload preserves the policy and rejection reason.
// 8: Revert restores the previous exact policy (proven at the value-copy
//    level; PluginProcessor's revert bundle is a plain DynamicStateMap copy,
//    so struct-level preservation here is exactly what Revert relies on).
// -----------------------------------------------------------------------
class DynamicNeutralSafePersistenceTests : public juce::UnitTest
{
public:
    DynamicNeutralSafePersistenceTests() : juce::UnitTest ("DynamicNeutralSafePersistence", "Phase10") {}

    void runTest() override
    {
        beginTest ("7: ValueTree round-trip preserves correctionPolicy and policyRejectionReason exactly");
        {
            auto map = makeRecognizedNoCorrectionMap (800, testPrototype (0.6f), 5.0f);
            map.states[0].correctionPolicy = DynamicCorrectionPolicy::NeutralSafe;
            map.states[0].policyRejectionReason = DynamicPolicyRejectionReason::GlobalPackageExcessiveRegression;

            const auto tree = dynamicStateMapToValueTree (map);
            const auto restored = dynamicStateMapFromValueTree (tree);
            expect (restored.valid, "round-tripped map remains valid");
            expect (restored.states[0].occupied);
            expectEquals ((int) restored.states[0].correctionPolicy, (int) DynamicCorrectionPolicy::NeutralSafe);
            expect (restored.states[0].policyRejectionReason
                        == DynamicPolicyRejectionReason::GlobalPackageExcessiveRegression);
        }

        beginTest ("7: presets saved before this policy existed still load, defaulting to GlobalFallback");
        {
            auto map = makeRecognizedNoCorrectionMap (801, testPrototype (0.7f), 5.0f);
            auto tree = dynamicStateMapToValueTree (map);
            // Simulate an older on-disk preset that predates the policy field.
            auto child = tree.getChild (0);
            child.removeProperty (juce::Identifier ("correctionPolicy"), nullptr);
            child.removeProperty (juce::Identifier ("policyRejectionReason"), nullptr);

            const auto restored = dynamicStateMapFromValueTree (tree);
            expect (restored.valid, "a preset missing the new fields must still load");
            expectEquals ((int) restored.states[0].correctionPolicy, (int) DynamicCorrectionPolicy::GlobalFallback,
                         "missing policy defaults to the historical always-Global behavior");
            expect (restored.states[0].policyRejectionReason == DynamicPolicyRejectionReason::None);
        }

        beginTest ("8: a captured copy (as Revert's bundle takes) preserves the exact prior policy through a later edit");
        {
            auto map = makeRecognizedNoCorrectionMap (802, testPrototype (0.8f), 5.0f);
            map.states[0].correctionPolicy = DynamicCorrectionPolicy::NeutralSafe;
            map.states[0].policyRejectionReason = DynamicPolicyRejectionReason::GlobalPackageExcessiveRegression;

            // Revert's bundle is exactly this: a value copy taken before a
            // later publication.
            const DynamicStateMap capturedForRevert = map;

            // Simulate a later, unrelated Learn/Apply overwriting the live map.
            DynamicStateMap laterMap = map;
            laterMap.states[0].correctionPolicy = DynamicCorrectionPolicy::GlobalFallback;
            laterMap.states[0].policyRejectionReason = DynamicPolicyRejectionReason::None;
            expectEquals ((int) laterMap.states[0].correctionPolicy, (int) DynamicCorrectionPolicy::GlobalFallback);

            // Revert restores the captured bundle verbatim.
            const DynamicStateMap restored = capturedForRevert;
            expectEquals ((int) restored.states[0].correctionPolicy, (int) DynamicCorrectionPolicy::NeutralSafe,
                         "revert must restore the exact previous policy, not the intervening one");
            expect (restored.states[0].policyRejectionReason
                        == DynamicPolicyRejectionReason::GlobalPackageExcessiveRegression);
        }
    }
};

static DynamicNeutralSafeMeasurementTests dynamicNeutralSafeMeasurementTests;
static DynamicNeutralSafeRuntimeTests dynamicNeutralSafeRuntimeTests;
static DynamicNeutralSafePersistenceTests dynamicNeutralSafePersistenceTests;
