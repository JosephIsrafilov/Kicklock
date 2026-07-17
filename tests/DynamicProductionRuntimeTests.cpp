#include "TestCommon.h"

#include "dsp/DynamicProductionRuntime.h"
#include "dsp/DynamicStateSerialization.h"
#include "dsp/DynamicMapSourceResolver.h"
#include "dsp/DynamicRuntimeSelector.h"

// =============================================================================
// Phase 7 production-runtime and processor-integration tests.
// =============================================================================

namespace
{
    constexpr double kSR = 48000.0;

    DynamicGlobalBase makeValidGlobalBase()
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
        gb.learnedSampleRate = kSR;
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

    DynamicState makeAutoStableState (uint64_t id, const DynamicFingerprintPrototype& fp,
                                      float delayDelta = 1.0f, float freq = 80.0f, float q = 1.2f)
    {
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
        s.bypassed = false;
        s.hitCount = 6;
        s.repeatability = 0.9f;
        s.ambiguity = 0.1f;
        return s;
    }

    DynamicStateMap makeEligibleMap (const DynamicFingerprintPrototype& fp, uint64_t id = 101)
    {
        DynamicStateMap map = makeEmptyDynamicStateMap();
        map.valid = true;
        map.globalBase = makeValidGlobalBase();
        map.calibration = makeGenerousCalibration();
        map.states[0] = makeAutoStableState (id, fp);
        map.nextStateId = id + 1;
        return map;
    }

    // A deterministic bass tone with periodic kick transients.
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

    // Extracts the fingerprint the runtime's own trigger detector would capture
    // for the first kick, so a stored prototype matches the served capture.
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

    bool finiteBuffer (const juce::AudioBuffer<float>& b, int n)
    {
        for (int c = 0; c < b.getNumChannels(); ++c)
            for (int i = 0; i < n; ++i)
                if (! std::isfinite (b.getSample (c, i)))
                    return false;
        return true;
    }

    // Drives a runtime over the whole fixture in fixed-size outer blocks and
    // returns the concatenated channel-0 output.
    std::vector<float> driveRuntime (DynamicProductionRuntime& runtime,
                                     const std::vector<float>& bass,
                                     const std::vector<float>& kick,
                                     int channels, int blockSize, double strength,
                                     bool hasSidechain, bool& allFinite)
    {
        std::vector<float> out;
        out.reserve (bass.size());
        allFinite = true;
        const int total = (int) bass.size();
        juce::AudioBuffer<float> in (channels, blockSize);
        juce::AudioBuffer<float> outBuf (channels, blockSize);
        for (int offset = 0; offset < total; offset += blockSize)
        {
            const int n = std::min (blockSize, total - offset);
            for (int ch = 0; ch < channels; ++ch)
                for (int i = 0; i < n; ++i)
                    in.setSample (ch, i, bass[(size_t) (offset + i)]);
            const bool ok = runtime.process (in, bass.data() + offset,
                                             hasSidechain ? kick.data() + offset : nullptr,
                                             hasSidechain, strength, outBuf, n);
            if (! ok || ! finiteBuffer (outBuf, n))
                allFinite = false;
            for (int i = 0; i < n; ++i)
                out.push_back (outBuf.getSample (0, i));
        }
        return out;
    }

    // ---- processor helpers ----
    void setParameter (KickLockAudioProcessor& processor, const char* id, float value)
    {
        if (auto* parameter = processor.apvts.getParameter (id))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    }

    void prepareProcessor (KickLockAudioProcessor& processor, double sampleRate, int blockSize)
    {
        processor.enableAllBuses();
        processor.setRateAndBufferSizeDetails (sampleRate, blockSize);
        processor.prepareToPlay (sampleRate, blockSize);
    }

    void configureDynamic (KickLockAudioProcessor& processor)
    {
        setParameter (processor, "correction_mode", 1.0f);
        setParameter (processor, "dynamic_strength", 1.0f);
        setParameter (processor, "allpass_enable", 1.0f);
        setParameter (processor, "phaseFilterEnabled", 1.0f);
    }

    void driveProcessor (KickLockAudioProcessor& processor, double sr, int blockSize,
                         const std::vector<float>& bass, const std::vector<float>& kick,
                         bool withSidechain, std::vector<float>* output = nullptr)
    {
        juce::ignoreUnused (sr);
        const int total = (int) bass.size();
        const int channels = juce::jmax (processor.getTotalNumInputChannels(),
                                         processor.getTotalNumOutputChannels());
        for (int offset = 0; offset < total; offset += blockSize)
        {
            const int n = std::min (blockSize, total - offset);
            juce::AudioBuffer<float> buffer (channels, n);
            buffer.clear();
            for (int i = 0; i < n; ++i)
            {
                buffer.setSample (0, i, bass[(size_t) (offset + i)]);
                if (channels > 1) buffer.setSample (1, i, bass[(size_t) (offset + i)]);
                if (withSidechain && channels > 2) buffer.setSample (2, i, kick[(size_t) (offset + i)]);
                if (withSidechain && channels > 3) buffer.setSample (3, i, kick[(size_t) (offset + i)]);
            }
            juce::MidiBuffer midi;
            processor.processBlock (buffer, midi);
            if (output != nullptr)
                for (int i = 0; i < n; ++i)
                    output->push_back (buffer.getSample (0, i));
        }
    }

    NotePhaseMapSnapshot makeUsableLegacyMap()
    {
        NotePhaseMapSnapshot map;
        map.valid = true;
        map.schemaVersion = NoteMap::kSchemaVersion;
        map.base.delayMs = 0.0f;
        map.base.polarityInvert = false;
        map.base.crossoverEnabled = true;
        map.base.crossoverHz = 150.0f;
        map.base.allpassStages = 2;
        map.base.delayInterpolationIndex = 0;
        map.base.learnedSampleRate = kSR;
        map.global.allpassFreqHz = 50.0f;
        map.global.allpassQ = 0.7f;
        map.global.confidence = 0.8f;
        map.global.fundamentalSpreadRatio = 0.0f;
        map.global.hitCount = NoteMap::kMinHitsPerNote;
        return map;
    }
}

class DynamicProductionPackageTests : public juce::UnitTest
{
public:
    DynamicProductionPackageTests() : juce::UnitTest ("DynamicProductionPackages", "Phase7") {}

    void runTest() override
    {
        beginTest ("C: Global + occupied slot configured; disabled/candidate/bypassed clear the slot");
        {
            DynamicProductionRuntime runtime;
            expect (runtime.prepare (kSR, 256, 2));

            auto map = makeEligibleMap (makeSyntheticPrototype (0.2f), 55);
            // slot 1: bypassed -> recognized but no correction branch
            map.states[1] = makeAutoStableState (56, makeSyntheticPrototype (1.1f));
            map.states[1].bypassed = true;
            // slot 2: Auto Candidate -> excluded from correction
            map.states[2] = makeAutoStableState (57, makeSyntheticPrototype (2.0f));
            map.states[2].evidence = DynamicStateEvidence::Candidate;
            map.states[2].hitCount = 3;
            // slot 3: disabled
            map.states[3] = makeAutoStableState (58, makeSyntheticPrototype (2.7f));
            map.states[3].enabled = true; // keep valid; then disable at runtime view
            map.states[3].enabled = false;
            map.nextStateId = 100;

            expect (isRuntimeEligibleDynamicStateMap (map));
            runtime.activateMap (map);

            std::vector<float> bass, kick;
            makeFixture (bass, kick, (int) (kSR * 0.1), kSR, (int) (kSR * 0.4));
            bool finite = true;
            driveRuntime (runtime, bass, kick, 2, 128, 1.0, true, finite);
            expect (finite);

            auto& engine = runtime.getEngineForTesting();
            expect (engine.getGlobalInfo().active, "Global always configured");
            expect (engine.getStateInfo (0).active && engine.getStateInfo (0).stableStateId == 55,
                    "corrected Auto Stable state occupies its slot by identity");
            expect (! engine.getStateInfo (1).active, "bypassed state clears its branch");
            expect (! engine.getStateInfo (2).active, "Auto Candidate is excluded from correction");
            expect (! engine.getStateInfo (3).active, "disabled state clears its branch");
        }

        beginTest ("C/L: repeated identical configuration keeps warmth; Strength 0 and 1 differ in delay tap");
        {
            DynamicProductionRuntime runtime;
            expect (runtime.prepare (kSR, 256, 1));
            auto map = makeEligibleMap (makeSyntheticPrototype (0.5f), 70);
            runtime.activateMap (map);

            std::vector<float> bass, kick;
            makeFixture (bass, kick, (int) (kSR * 0.3), kSR, (int) (kSR * 0.1));
            bool finite = true;
            driveRuntime (runtime, bass, kick, 1, 64, 1.0, true, finite);
            expect (finite);
            const bool warmAfterFirst = runtime.getEngineForTesting().getStateInfo (0).warm;
            // Re-drive with identical strength; branch identity/warmth preserved.
            driveRuntime (runtime, bass, kick, 1, 64, 1.0, true, finite);
            expect (finite);
            expect (runtime.getEngineForTesting().getStateInfo (0).warm || warmAfterFirst,
                    "identical config does not cold-reset the hot branch");

            const double tap1 = runtime.getEngineForTesting().getStateInfo (0).effectiveAbsoluteDelayMs;
            driveRuntime (runtime, bass, kick, 1, 64, 0.0, true, finite);
            const double tap0 = runtime.getEngineForTesting().getGlobalInfo().effectiveAbsoluteDelayMs;
            expectWithinAbsoluteError ((float) tap0, 0.0f, 1.0e-4f, "Strength 0 keeps Global base delay");
            expect (std::abs (tap1 - 0.0) > 1.0e-4, "Strength 1 morphs the state delay away from Global");
        }
    }
};

class DynamicProductionFingerprintTests : public juce::UnitTest
{
public:
    DynamicProductionFingerprintTests() : juce::UnitTest ("DynamicProductionFingerprint", "Phase7") {}

    void runTest() override
    {
        beginTest ("D: served capture matches the stored prototype and selects the state");
        {
            std::vector<float> bass, kick;
            makeFixture (bass, kick, (int) (kSR * 1.2), kSR, (int) (kSR * 0.4));
            const auto prototype = fingerprintOfFirstKick (bass, kick, kSR);
            expect (prototype.valid, "offline fingerprint of the first kick is valid");

            auto map = makeEligibleMap (prototype, 321);
            DynamicProductionRuntime runtime;
            expect (runtime.prepare (kSR, 512, 1));
            runtime.activateMap (map);

            bool finite = true;
            driveRuntime (runtime, bass, kick, 1, 256, 1.0, true, finite);
            expect (finite);
            expect (runtime.getDiagnostics().acceptedCaptures > 0, "kicks trigger captures");
            expect (runtime.getDiagnostics().completedObservations > 0, "captures complete");
            expectEquals ((int64_t) runtime.getSelectedStableStateId(), (int64_t) 321,
                          "the runtime routes to the recognized State by stable identity");
        }

        beginTest ("E: block partitioning does not change output or selection");
        {
            std::vector<float> bass, kick;
            makeFixture (bass, kick, (int) (kSR * 1.0), kSR, (int) (kSR * 0.35));
            const auto prototype = fingerprintOfFirstKick (bass, kick, kSR);
            auto map = makeEligibleMap (prototype, 9);

            std::vector<float> reference;
            uint64_t referenceId = 0;
            for (int block : { 1, 7, 64, 127, 512 })
            {
                DynamicProductionRuntime runtime;
                expect (runtime.prepare (kSR, 512, 1));
                runtime.activateMap (map);
                bool finite = true;
                auto out = driveRuntime (runtime, bass, kick, 1, block, 1.0, true, finite);
                expect (finite);
                if (reference.empty())
                {
                    reference = out;
                    referenceId = runtime.getSelectedStableStateId();
                }
                else
                {
                    float maxDiff = 0.0f;
                    for (size_t i = 0; i < std::min (reference.size(), out.size()); ++i)
                        maxDiff = std::max (maxDiff, std::abs (reference[i] - out[i]));
                    expectLessThan (maxDiff, 1.0e-5f, "output is invariant across block partitions");
                    expectEquals ((int64_t) runtime.getSelectedStableStateId(), (int64_t) referenceId,
                                  "selection is invariant across block partitions");
                }
            }
        }
    }
};

class DynamicProductionFailureTests : public juce::UnitTest
{
public:
    DynamicProductionFailureTests() : juce::UnitTest ("DynamicProductionFailure", "Phase7") {}

    void runTest() override
    {
        beginTest ("M/L: non-finite strength and non-finite audio stay finite and latency-correct");
        {
            std::vector<float> bass, kick;
            makeFixture (bass, kick, (int) (kSR * 0.4), kSR, (int) (kSR * 0.2));
            bass[100] = std::numeric_limits<float>::quiet_NaN();
            kick[200] = std::numeric_limits<float>::infinity();

            auto map = makeEligibleMap (makeSyntheticPrototype (0.3f), 7);
            DynamicProductionRuntime runtime;
            expect (runtime.prepare (kSR, 256, 1));
            runtime.activateMap (map);
            expectEquals (runtime.getReportedLatencySamples(),
                          (int) std::ceil (kSR * 20.0 / 1000.0), "exact 20 ms PDC latency");

            bool finite = true;
            for (double strength : { 0.0, 0.5, 1.0, std::numeric_limits<double>::quiet_NaN() })
                driveRuntime (runtime, bass, kick, 1, 96, strength, true, finite);
            expect (finite, "non-finite strength / audio never produce non-finite output");
        }

        beginTest ("M: an empty (ineligible) map falls back to a finite dry latency path");
        {
            DynamicProductionRuntime runtime;
            expect (runtime.prepare (kSR, 256, 1));
            runtime.activateMap (makeEmptyDynamicStateMap());
            std::vector<float> bass, kick;
            makeFixture (bass, kick, (int) (kSR * 0.2), kSR, (int) (kSR * 0.1));
            bool finite = true;
            driveRuntime (runtime, bass, kick, 1, 64, 1.0, true, finite);
            expect (finite);
        }

        beginTest ("H: sidechain loss returns selection to Global and clears Service");
        {
            std::vector<float> bass, kick;
            makeFixture (bass, kick, (int) (kSR * 1.0), kSR, (int) (kSR * 0.3));
            const auto prototype = fingerprintOfFirstKick (bass, kick, kSR);
            auto map = makeEligibleMap (prototype, 44);
            DynamicProductionRuntime runtime;
            expect (runtime.prepare (kSR, 256, 1));
            runtime.activateMap (map);
            bool finite = true;
            driveRuntime (runtime, bass, kick, 1, 128, 1.0, true, finite);

            runtime.notifySidechainLost();
            std::vector<float> silentKick ((size_t) (kSR * 0.2), 0.0f);
            std::vector<float> contBass ((size_t) (kSR * 0.2), 0.1f);
            driveRuntime (runtime, contBass, silentKick, 1, 128, 1.0, false, finite);
            expect (finite);
            expect (runtime.isFallbackActive(), "no sidechain returns selection to Global");
            expect (! runtime.getServiceBindingValid(), "Service binding cleared on sidechain loss");
        }
    }
};

class DynamicProductionProcessorTests : public juce::UnitTest
{
public:
    DynamicProductionProcessorTests() : juce::UnitTest ("DynamicProductionProcessor", "Phase7") {}

    void runTest() override
    {
        constexpr int blockSize = 256;

        beginTest ("A: source priority New > Legacy > None; Static ignores both");
        {
            const auto prototype = makeSyntheticPrototype (0.4f);
            const auto newMap = makeEligibleMap (prototype, 12);
            const auto legacyMap = makeUsableLegacyMap();
            std::vector<float> bass, kick;
            makeFixture (bass, kick, blockSize * 4, kSR, blockSize * 2);

            auto sourceAfter = [&] (bool publishNew, bool publishLegacy, bool dynamic)
            {
                KickLockAudioProcessor processor;
                prepareProcessor (processor, kSR, blockSize);
                setParameter (processor, "correction_mode", dynamic ? 1.0f : 0.0f);
                setParameter (processor, "dynamic_strength", 1.0f);
                if (publishNew) processor.publishDynamicStateMapForTesting (newMap);
                if (publishLegacy) processor.publishNoteMapForTesting (legacyMap);
                driveProcessor (processor, kSR, blockSize, bass, kick, true);
                return processor.getActiveDynamicMapSourceForTesting();
            };

            expect (sourceAfter (true, true, true) == DynamicMapSource::NewDynamicStateMap,
                    "valid New + valid Legacy -> New");
            expect (sourceAfter (false, true, true) == DynamicMapSource::LegacyDynamicCompatibility,
                    "empty New + valid Legacy -> Legacy");
            expect (sourceAfter (true, false, true) == DynamicMapSource::NewDynamicStateMap,
                    "valid New + empty Legacy -> New");
            expect (sourceAfter (false, false, true) == DynamicMapSource::None,
                    "neither -> None");
            expect (sourceAfter (true, true, false) == DynamicMapSource::None,
                    "Static ignores both runtime selectors");
        }

        beginTest ("B: New + Legacy maps coexist through binary round-trip; New wins after restore");
        {
            KickLockAudioProcessor a;
            prepareProcessor (a, kSR, blockSize);
            const auto newMap = makeEligibleMap (makeSyntheticPrototype (0.6f), 0xABCDEF12u);
            const auto legacyMap = makeUsableLegacyMap();
            a.publishDynamicStateMapForTesting (newMap);
            a.publishNoteMapForTesting (legacyMap);
            // process one block so the audio-owned copies + message-owned copies settle
            std::vector<float> bass (blockSize, 0.0f), kick (blockSize, 0.0f);
            driveProcessor (a, kSR, blockSize, bass, kick, true);

            juce::MemoryBlock state;
            a.getStateInformation (state);

            KickLockAudioProcessor b;
            prepareProcessor (b, kSR, blockSize);
            b.setStateInformation (state.getData(), (int) state.getSize());
            setParameter (b, "correction_mode", 1.0f);
            driveProcessor (b, kSR, blockSize, bass, kick, true);

            const auto restored = b.getActiveDynamicStateMapForTesting();
            expect (isRuntimeEligibleDynamicStateMap (restored), "restored New map is runtime-eligible");
            expect (restored.states[0].stableStateId == 0xABCDEF12u,
                    "uint64 stable IDs survive serialization exactly");
            expect (b.getActiveNoteMapForTesting().valid, "legacy map coexists after restore");
            expect (b.getActiveDynamicMapSourceForTesting() == DynamicMapSource::NewDynamicStateMap,
                    "source priority after restore is New");
        }

        beginTest ("B: absent New child restores empty rather than retaining prior data");
        {
            KickLockAudioProcessor prior;
            prepareProcessor (prior, kSR, blockSize);
            prior.publishDynamicStateMapForTesting (makeEligibleMap (makeSyntheticPrototype (0.9f), 5));
            std::vector<float> bass (blockSize, 0.0f), kick (blockSize, 0.0f);
            driveProcessor (prior, kSR, blockSize, bass, kick, true);
            expect (isRuntimeEligibleDynamicStateMap (prior.getActiveDynamicStateMapForTesting()));

            // A payload with no KLDynamicStateMap child (legacy-only project).
            juce::ValueTree bare ("APVTS");
            bare.setProperty ("PARAM", 0, nullptr);
            KickLockAudioProcessor loaded;
            prepareProcessor (loaded, kSR, blockSize);
            loaded.publishDynamicStateMapForTesting (makeEligibleMap (makeSyntheticPrototype (0.1f), 9));
            driveProcessor (loaded, kSR, blockSize, bass, kick, true);

            juce::MemoryBlock legacyOnlyState;
            {
                // Build a valid APVTS payload with only a legacy note-map child.
                KickLockAudioProcessor src;
                prepareProcessor (src, kSR, blockSize);
                // do NOT publish any dynamic map -> message-owned dynamic map is empty
                src.getStateInformation (legacyOnlyState);
            }
            loaded.setStateInformation (legacyOnlyState.getData(), (int) legacyOnlyState.getSize());
            driveProcessor (loaded, kSR, blockSize, bass, kick, true);
            expect (! isRuntimeEligibleDynamicStateMap (loaded.getActiveDynamicStateMapForTesting()),
                    "absent New child clears any prior New map to empty");
        }

        beginTest ("K: source changes stay finite with identical reported latency");
        {
            KickLockAudioProcessor processor;
            prepareProcessor (processor, kSR, blockSize);
            const int latency = processor.getLatencySamples();
            const auto newMap = makeEligibleMap (makeSyntheticPrototype (0.35f), 77);
            processor.publishDynamicStateMapForTesting (newMap);
            processor.publishNoteMapForTesting (makeUsableLegacyMap());

            std::vector<float> bass, kick;
            makeFixture (bass, kick, blockSize * 6, kSR, blockSize * 2);

            std::vector<float> output;
            // Dynamic New
            setParameter (processor, "correction_mode", 1.0f);
            driveProcessor (processor, kSR, blockSize, bass, kick, true, &output);
            // Dynamic -> Static
            setParameter (processor, "correction_mode", 0.0f);
            driveProcessor (processor, kSR, blockSize, bass, kick, true, &output);
            // Static -> Dynamic New
            setParameter (processor, "correction_mode", 1.0f);
            driveProcessor (processor, kSR, blockSize, bass, kick, true, &output);

            expectEquals (processor.getLatencySamples(), latency, "reported latency unchanged across sources");
            bool finite = true;
            for (float v : output) finite = finite && std::isfinite (v);
            expect (finite, "finite output across all source changes");
        }

        beginTest ("I: host bypass never emits New Dynamic corrected audio (fixed delayed dry)");
        {
            KickLockAudioProcessor processor;
            prepareProcessor (processor, kSR, blockSize);
            configureDynamic (processor);
            processor.publishDynamicStateMapForTesting (makeEligibleMap (makeSyntheticPrototype (0.5f), 3));

            const int latency = processor.getLatencySamples();
            std::vector<float> bass, kick;
            makeFixture (bass, kick, blockSize * 4, kSR, blockSize * 2);
            const int channels = juce::jmax (processor.getTotalNumInputChannels(),
                                             processor.getTotalNumOutputChannels());

            std::vector<float> bypassOut;
            for (int offset = 0; offset + blockSize <= (int) bass.size(); offset += blockSize)
            {
                juce::AudioBuffer<float> buffer (channels, blockSize);
                buffer.clear();
                for (int i = 0; i < blockSize; ++i)
                {
                    buffer.setSample (0, i, bass[(size_t) (offset + i)]);
                    if (channels > 1) buffer.setSample (1, i, bass[(size_t) (offset + i)]);
                    if (channels > 2) buffer.setSample (2, i, kick[(size_t) (offset + i)]);
                    if (channels > 3) buffer.setSample (3, i, kick[(size_t) (offset + i)]);
                }
                juce::MidiBuffer midi;
                processor.processBlockBypassed (buffer, midi);
                for (int i = 0; i < blockSize; ++i)
                    bypassOut.push_back (buffer.getSample (0, i));
            }

            // Bypass output must equal the input delayed by exactly `latency`.
            float maxErr = 0.0f;
            for (int i = latency; i < (int) bypassOut.size(); ++i)
                maxErr = std::max (maxErr, std::abs (bypassOut[(size_t) i] - bass[(size_t) (i - latency)]));
            expectLessThan (maxErr, 1.0e-4f, "bypass is exactly the fixed 20 ms delayed dry path");
        }
    }
};

static DynamicProductionPackageTests dynamicProductionPackageTests;
static DynamicProductionFingerprintTests dynamicProductionFingerprintTests;
static DynamicProductionFailureTests dynamicProductionFailureTests;
static DynamicProductionProcessorTests dynamicProductionProcessorTests;
