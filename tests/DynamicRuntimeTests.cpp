#include "TestCommon.h"
#include "dsp/DynamicRuntimeSelector.h"

namespace
{
    RuntimeBaseSettings matchingBase()
    {
        RuntimeBaseSettings base;
        base.delayMs = 1.0f;
        base.polarityInvert = true;
        base.crossoverEnabled = true;
        base.crossoverHz = 140.0f;
        base.allpassStages = 3;
        base.delayInterpolationIndex = 1;
        return base;
    }

    NotePhaseMapSnapshot makeMap()
    {
        NotePhaseMapSnapshot map;
        map.valid = true;
        map.schemaVersion = NoteMap::kSchemaVersion;
        const auto base = matchingBase();
        map.base.delayMs = base.delayMs;
        map.base.polarityInvert = base.polarityInvert;
        map.base.crossoverEnabled = base.crossoverEnabled;
        map.base.crossoverHz = base.crossoverHz;
        map.base.allpassStages = base.allpassStages;
        map.base.delayInterpolationIndex = base.delayInterpolationIndex;
        map.base.learnedSampleRate = 48000.0;
        map.global.allpassFreqHz = 50.0f;
        map.global.allpassQ = 0.7f;
        map.global.confidence = 0.8f;
        map.global.fundamentalSpreadRatio = 0.0f;
        map.global.hitCount = NoteMap::kMinHitsPerNote;

        auto& note = map.notes[(size_t) NotePhaseMapSnapshot::indexForMidi (33)];
        note.learned = true;
        note.fundamentalHz = NoteQuantizer::midiToHz (33);
        note.allpassFreqHz = 100.0f;
        note.allpassQ = 1.5f;
        note.delayMs = 2.5f;
        note.polarityInvert = false;
        note.timingConfidence = 0.8f;
        note.timingSpreadMs = 0.03f;
        note.confidence = 0.8f;
        note.fundamentalSpreadRatio = 0.05f;
        note.hitCount = NoteMap::kMinHitsPerNote;
        note.rotatorHelps = true;
        return map;
    }

    DynamicRuntimeSelection select (const NotePhaseMapSnapshot& map, float hz,
                                    float strength, DynamicNoteState& state,
                                    RuntimeBaseSettings base = matchingBase())
    {
        return selectDynamicRuntime (map, base, 73.0f, 1.1f, hz, strength,
                                     256, 12000, state);
    }

    bool finiteBuffer (const juce::AudioBuffer<float>& b)
    {
        for (int c = 0; c < b.getNumChannels(); ++c)
            for (int i = 0; i < b.getNumSamples(); ++i)
                if (! std::isfinite (b.getSample (c, i)))
                    return false;
        return true;
    }

    void setParameter (KickLockAudioProcessor& processor, const char* id, float value)
    {
        if (auto* parameter = processor.apvts.getParameter (id))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    }

    void configureProcessor (KickLockAudioProcessor& processor, bool dynamic,
                             bool phaseEnabled = true, bool pitchFollow = false)
    {
        setParameter (processor, "delay_ms", 1.0f);
        setParameter (processor, "delayMs", 1.0f);
        setParameter (processor, "polarity_invert", 1.0f);
        setParameter (processor, "polarityInvert", 1.0f);
        setParameter (processor, "crossover_enable", 1.0f);
        setParameter (processor, "crossover_freq", 140.0f);
        setParameter (processor, "rotatorStages", 1.0f);
        setParameter (processor, "delayInterp", 1.0f);
        setParameter (processor, "allpass_enable", phaseEnabled ? 1.0f : 0.0f);
        setParameter (processor, "phaseFilterEnabled", phaseEnabled ? 1.0f : 0.0f);
        setParameter (processor, "allpass_freq", 73.0f);
        setParameter (processor, "rotatorFreq", 73.0f);
        setParameter (processor, "rotatorQ", 1.1f);
        setParameter (processor, "correction_mode", dynamic ? 1.0f : 0.0f);
        setParameter (processor, "dynamic_strength", 1.0f);
        setParameter (processor, "pitch_track", pitchFollow ? 1.0f : 0.0f);
    }

    void prepareProcessor (KickLockAudioProcessor& processor, double sampleRate, int blockSize)
    {
        processor.enableAllBuses();
        processor.setRateAndBufferSizeDetails (sampleRate, blockSize);
        processor.prepareToPlay (sampleRate, blockSize);
    }

    void processTone (KickLockAudioProcessor& processor, double sampleRate, int blockSize,
                      int startSample, int totalSamples, bool silence,
                      std::vector<float>* output = nullptr)
    {
        for (int offset = 0; offset < totalSamples; offset += blockSize)
        {
            const int n = std::min (blockSize, totalSamples - offset);
            juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                         processor.getTotalNumOutputChannels()), n);
            buffer.clear();
            for (int i = 0; i < n; ++i)
            {
                const double t = (double) (startSample + offset + i) / sampleRate;
                const float bass = silence ? 0.0f : 0.55f * (float) std::sin (kTwoPi * 55.0 * t);
                const float kick = silence ? 0.0f : 0.20f * (float) std::sin (kTwoPi * 60.0 * t);
                buffer.setSample (0, i, bass);
                if (buffer.getNumChannels() > 1) buffer.setSample (1, i, bass);
                if (buffer.getNumChannels() > 2) buffer.setSample (2, i, kick);
                if (buffer.getNumChannels() > 3) buffer.setSample (3, i, kick);
            }
            juce::MidiBuffer midi;
            processor.processBlock (buffer, midi);
            if (output != nullptr)
                for (int i = 0; i < n; ++i)
                    output->push_back (buffer.getSample (0, i));
        }
    }

    bool allFinite (const std::vector<float>& values)
    {
        return std::all_of (values.begin(), values.end(), [] (float value) { return std::isfinite (value); });
    }

    float maxAbsDifference (const std::vector<float>& a, const std::vector<float>& b)
    {
        float maximum = 0.0f;
        for (size_t i = 0; i < std::min (a.size(), b.size()); ++i)
            maximum = std::max (maximum, std::abs (a[i] - b[i]));
        return maximum;
    }

    float maxAdjacentDerivative (const std::vector<float>& values, int begin, int end)
    {
        float maximum = 0.0f;
        begin = std::max (1, begin);
        end = std::min ((int) values.size(), end);
        for (int i = begin; i < end; ++i)
            maximum = std::max (maximum, std::abs (values[(size_t) i] - values[(size_t) i - 1]));
        return maximum;
    }

    MultibandPhaseCore::Params coreParams (float frequency, float q, float smoothing)
    {
        MultibandPhaseCore::Params params;
        params.crossoverEnabled = false;
        params.allpassEnabled = true;
        params.allpassFreqHz = frequency;
        params.allpassQ = q;
        params.allpassSmoothingSeconds = smoothing;
        return params;
    }

    std::vector<float> renderCoreAutomation (double sampleRate, int blockSize,
                                             const MultibandPhaseCore::Params& before,
                                             const MultibandPhaseCore::Params& middle,
                                             const MultibandPhaseCore::Params& after,
                                             int firstChange, int secondChange = -1)
    {
        const int total = (int) std::round (sampleRate * 1.05);
        MultibandPhaseCore core;
        core.prepare (sampleRate, blockSize, 1, 20.0f);
        juce::AudioBuffer<float> main (1, blockSize), sidechain (1, blockSize);
        std::vector<float> result;
        result.reserve ((size_t) total);

        for (int offset = 0; offset < total; offset += blockSize)
        {
            const int n = std::min (blockSize, total - offset);
            main.setSize (1, n, false, false, true);
            sidechain.setSize (1, n, false, false, true);
            const auto& params = secondChange >= 0 && offset >= secondChange ? after
                               : offset >= firstChange ? middle : before;
            for (int i = 0; i < n; ++i)
            {
                const double t = (double) (offset + i) / sampleRate;
                main.setSample (0, i, 0.50f * (float) std::sin (kTwoPi * 55.0 * t)
                                      + 0.18f * (float) std::sin (kTwoPi * 91.0 * t + 0.2));
            }
            core.process (main, sidechain, params, n);
            for (int i = 0; i < n; ++i)
                result.push_back (main.getSample (0, i));
        }
        return result;
    }
}

class DynamicRuntimeSelectorTests : public juce::UnitTest
{
public:
    DynamicRuntimeSelectorTests() : juce::UnitTest ("DynamicRuntimeSelector", "Phase4") {}

    void runTest() override
    {
        beginTest ("T11: learned note, unknown note, and tracker zero source selection");
        {
            const auto map = makeMap();
            DynamicNoteState state;
            const auto learned = select (map, NoteQuantizer::midiToHz (33), 1.0f, state);
            expect (learned.mapUsable);
            expect (learned.usingLearnedNote);
            expect (! learned.fallbackActive);
            expectEquals (learned.selectedMidi, 33);
            expectWithinAbsoluteError (learned.targetFreqHz, 100.0f, 1.0e-5f);
            expectWithinAbsoluteError (learned.targetQ, 1.5f, 1.0e-5f);
            expectWithinAbsoluteError (learned.targetDelayMs, 1.0f, 1.0e-5f,
                                       "delay stays pinned to current regardless of the learned note");
            expect (learned.targetPolarityInvert,
                    "polarity stays pinned to current regardless of the learned note");

            const auto unknown = select (map, NoteQuantizer::midiToHz (34), 1.0f, state);
            expect (unknown.mapUsable && unknown.fallbackActive && ! unknown.usingLearnedNote);
            expectEquals (unknown.selectedMidi, 34);
            expectWithinAbsoluteError (unknown.targetFreqHz, 100.0f, 1.0e-5f,
                                       "weak tracking holds the last stable entry briefly");
            expect (unknown.targetDelayMs == 1.0f,
                    "held fallback delay stays pinned to current");
            expect (unknown.targetPolarityInvert,
                    "held fallback polarity stays pinned to current");

            const auto zero = select (map, 0.0f, 1.0f, state);
            expect (zero.fallbackActive);
            expectEquals (zero.selectedMidi, -1);
        }

        beginTest ("T11: unlearned, low-confidence, no-help, and out-of-range notes fall back");
        {
            for (int variant = 0; variant < 4; ++variant)
            {
                auto map = makeMap();
                DynamicNoteState state;
                float hz = NoteQuantizer::midiToHz (33);
                if (variant == 0)
                    map.notes[(size_t) NotePhaseMapSnapshot::indexForMidi (33)].learned = false;
                if (variant == 1)
                    map.notes[(size_t) NotePhaseMapSnapshot::indexForMidi (33)].confidence = 0.34f;
                if (variant == 2)
                    map.notes[(size_t) NotePhaseMapSnapshot::indexForMidi (33)].rotatorHelps = false;
                if (variant == 3)
                    hz = NoteQuantizer::midiToHz (69);
                const auto result = select (map, hz, 1.0f, state);
                expect (result.fallbackActive && ! result.usingLearnedNote);
                expectWithinAbsoluteError (result.targetFreqHz, 50.0f, 1.0e-5f);
            }
        }

        beginTest ("T11: empty, corrupt, invalid-global, and stale maps use manual targets");
        {
            for (int variant = 0; variant < 4; ++variant)
            {
                auto map = makeMap();
                auto base = matchingBase();
                if (variant == 0) map.valid = false;
                if (variant == 1) map.schemaVersion = 999;
                if (variant == 2) map.global.allpassFreqHz = std::numeric_limits<float>::quiet_NaN();
                if (variant == 3) base.delayMs += 0.051f;
                DynamicNoteState state;
                const auto result = select (map, NoteQuantizer::midiToHz (33), 1.0f, state, base);
                expect (result.fallbackActive);
                expect (result.mapStale == (variant == 3));
                expectWithinAbsoluteError (result.targetFreqHz, 73.0f, 1.0e-5f);
                expectWithinAbsoluteError (result.targetQ, 1.1f, 1.0e-5f);
                expectEquals (state.lastMidi, -1);
            }
        }

        beginTest ("T12: Strength uses log-frequency and linear-Q interpolation");
        {
            const auto map = makeMap();
            float previous = 0.0f;
            for (float strength : { 0.0f, 0.25f, 0.5f, 0.75f, 1.0f })
            {
                DynamicNoteState state;
                const auto result = select (map, NoteQuantizer::midiToHz (33), strength, state);
                const float expectedF = std::exp (std::log (50.0f)
                                                 + (std::log (100.0f) - std::log (50.0f)) * strength);
                expectWithinAbsoluteError (result.targetFreqHz, expectedF, 1.0e-5f);
                expectWithinAbsoluteError (result.targetQ, 0.7f + 0.8f * strength, 1.0e-5f);
                expectWithinAbsoluteError (result.targetDelayMs, 1.0f, 1.0e-5f,
                                           "delay must not move with dynamicStrength");
                expect (result.targetPolarityInvert,
                        "polarity must not flip with dynamicStrength");
                expectGreaterOrEqual (result.targetFreqHz, previous);
                previous = result.targetFreqHz;
            }
            DynamicNoteState state;
            const auto clamped = select (map, NoteQuantizer::midiToHz (33), 5.0f, state);
            expectWithinAbsoluteError (clamped.targetFreqHz, 100.0f, 1.0e-5f);
        }

        beginTest ("T12: Delay and polarity remain global for learned and fallback notes");
        {
            const auto current = matchingBase();
            const auto map = makeMap();
            for (float strength : { 0.0f, 0.5f, 1.0f })
            {
                DynamicNoteState state;
                const auto result = select (map, NoteQuantizer::midiToHz (33), strength, state, current);
                expect (result.targetDelayMs == current.delayMs,
                        "learned-note delay must exactly match current");
                expect (result.targetPolarityInvert == current.polarityInvert,
                        "learned-note polarity must exactly match current");
            }

            auto fallbackMap = makeMap();
            fallbackMap.notes[(size_t) NotePhaseMapSnapshot::indexForMidi (33)].timingConfidence = 0.0f;
            DynamicNoteState state;
            const auto fallback = select (fallbackMap, NoteQuantizer::midiToHz (33), 0.5f, state, current);
            expect (fallback.fallbackActive && ! fallback.usingLearnedNote);
            expect (fallback.targetDelayMs == current.delayMs,
                    "fallback delay must exactly match current");
            expect (fallback.targetPolarityInvert == current.polarityInvert,
                    "fallback polarity must exactly match current");
        }

        beginTest ("T13: context tolerance is inclusive and sample rate is metadata");
        {
            const auto map = makeMap();
            auto base = matchingBase();
            base.delayMs += 0.05f;
            expect (mapContextMatchesCurrentParameters (map, base));
            base.delayMs += 0.001f;
            expect (! mapContextMatchesCurrentParameters (map, base));
            base = matchingBase(); base.crossoverHz += 1.0f;
            expect (mapContextMatchesCurrentParameters (map, base));
            base.crossoverHz += 0.01f;
            expect (! mapContextMatchesCurrentParameters (map, base));
            base = matchingBase(); base.polarityInvert = false;
            expect (! mapContextMatchesCurrentParameters (map, base));
            base = matchingBase(); base.crossoverEnabled = false;
            expect (! mapContextMatchesCurrentParameters (map, base));
            base = matchingBase(); base.allpassStages = 2;
            expect (! mapContextMatchesCurrentParameters (map, base));
            base = matchingBase(); base.delayInterpolationIndex = 0;
            expect (! mapContextMatchesCurrentParameters (map, base));
        }

        beginTest ("Hysteresis survives short dropout and resets after silence or map reset");
        {
            const auto map = makeMap();
            DynamicNoteState state;
            expectEquals (select (map, NoteQuantizer::midiToHz (33), 1.0f, state).selectedMidi, 33);
            const float within = NoteQuantizer::midiToHz (33) * std::pow (2.0f, 0.60f / 12.0f);
            expectEquals (select (map, within, 1.0f, state).selectedMidi, 33);
            const float outside = NoteQuantizer::midiToHz (33) * std::pow (2.0f, 0.61f / 12.0f);
            expectEquals (select (map, outside, 1.0f, state).selectedMidi, 34);
            select (map, 0.0f, 1.0f, state);
            expectEquals (state.lastMidi, 34, "short dropout retains the held note");
            for (int i = 0; i < 50; ++i) select (map, 0.0f, 1.0f, state);
            expectEquals (state.lastMidi, -1, "250 ms silence clears the held note");
            state.lastMidi = 33;
            auto staleBase = matchingBase(); staleBase.delayMs += 1.0f;
            select (map, NoteQuantizer::midiToHz (33), 1.0f, state, staleBase);
            expectEquals (state.lastMidi, -1);
        }
    }
};

class DynamicSmoothingTests : public juce::UnitTest
{
public:
    DynamicSmoothingTests() : juce::UnitTest ("DynamicSmoothing", "Phase4") {}

    void runTest() override
    {
        beginTest ("T14: Static keeps 30 ms and Dynamic uses 70 ms without invalid output");
        {
            for (double sr : { 44100.0, 48000.0, 96000.0 })
            for (int block : { 32, 256, 2048 })
            {
                MultibandPhaseCore core;
                core.prepare (sr, block, 1, 20.0f);
                MultibandPhaseCore::Params params;
                params.crossoverEnabled = false;
                params.allpassEnabled = true;
                juce::AudioBuffer<float> main (1, block), sidechain (1, block);
                for (int i = 0; i < block; ++i)
                    main.setSample (0, i, 0.6f * std::sin ((float) i * 0.03f));
                core.process (main, sidechain, params, block);
                expectWithinAbsoluteError (core.getAllpassSmoothingSecondsForTesting(), 0.030f, 1.0e-6f);
                params.allpassSmoothingSeconds = 0.070f;
                params.allpassFreqHz = 100.0f;
                params.allpassQ = 1.5f;
                core.process (main, sidechain, params, block);
                expectWithinAbsoluteError (core.getAllpassSmoothingSecondsForTesting(), 0.070f, 1.0e-6f);
                expect (finiteBuffer (main), "finite output at sr/block fixture");
            }
        }
    }
};

class DynamicProcessorIntegrationTests : public juce::UnitTest
{
public:
    DynamicProcessorIntegrationTests() : juce::UnitTest ("DynamicProcessorIntegration", "Phase4") {}

    void runTest() override
    {
        constexpr double sampleRate = 48000.0;
        constexpr int blockSize = 256;
        constexpr int warmSamples = 48000;

        beginTest ("T11: matching map is consumed, latest queue snapshot wins, and statuses publish");
        {
            KickLockAudioProcessor processor;
            prepareProcessor (processor, sampleRate, blockSize);
            configureProcessor (processor, true);
            auto first = makeMap();
            auto latest = makeMap();
            latest.global.allpassFreqHz = 63.0f;
            latest.notes[(size_t) NotePhaseMapSnapshot::indexForMidi (33)].allpassFreqHz = 130.0f;
            expect (processor.publishNoteMapForTesting (first));
            expect (processor.publishNoteMapForTesting (latest));
            std::vector<float> output;
            processTone (processor, sampleRate, blockSize, 0, warmSamples, false, &output);

            expect (processor.getActiveNoteMapForTesting().valid);
            expectWithinAbsoluteError (processor.getActiveNoteMapForTesting().global.allpassFreqHz, 63.0f, 1.0e-6f);
            expectEquals (processor.activeMidiNote.load(), 33);
            expect (! processor.dynamicFallbackActive.load());
            expect (! processor.dynamicMapStale.load());
            expect (allFinite (output));
        }

        beginTest ("T11: Static is unchanged by an active map; Dynamic ignores Pitch Follow while Static honours it");
        {
            auto render = [&] (bool dynamic, bool pitchFollow, bool publishMap)
            {
                KickLockAudioProcessor processor;
                prepareProcessor (processor, sampleRate, blockSize);
                configureProcessor (processor, dynamic, true, pitchFollow);
                if (publishMap) expect (processor.publishNoteMapForTesting (makeMap()));
                std::vector<float> output;
                processTone (processor, sampleRate, blockSize, 0, warmSamples, false, &output);
                return output;
            };

            const auto staticWithoutMap = render (false, false, false);
            const auto staticWithMap = render (false, false, true);
            expectLessThan (maxAbsDifference (staticWithoutMap, staticWithMap), 1.0e-6f);

            const auto dynamicFollowOff = render (true, false, true);
            const auto dynamicFollowOn = render (true, true, true);
            expectLessThan (maxAbsDifference (dynamicFollowOff, dynamicFollowOn), 1.0e-6f);

            const auto staticFollowOff = render (false, false, false);
            const auto staticFollowOn = render (false, true, false);
            expectGreaterThan (maxAbsDifference (staticFollowOff, staticFollowOn), 1.0e-4f);
        }

        beginTest ("T11: disabled phase filter, empty map, and stale map publish exact fallback states");
        {
            KickLockAudioProcessor disabled;
            prepareProcessor (disabled, sampleRate, blockSize);
            configureProcessor (disabled, true, false);
            expect (disabled.publishNoteMapForTesting (makeMap()));
            processTone (disabled, sampleRate, blockSize, 0, blockSize * 3, false);
            expect (! disabled.dynamicFallbackActive.load());
            expect (! disabled.dynamicMapStale.load());
            expectEquals (disabled.activeMidiNote.load(), -1);

            KickLockAudioProcessor empty;
            prepareProcessor (empty, sampleRate, blockSize);
            configureProcessor (empty, true);
            processTone (empty, sampleRate, blockSize, 0, warmSamples, false);
            expect (empty.dynamicFallbackActive.load());
            expect (! empty.dynamicMapStale.load());
            expectEquals (empty.activeMidiNote.load(), -1);

            KickLockAudioProcessor stale;
            prepareProcessor (stale, sampleRate, blockSize);
            configureProcessor (stale, true);
            expect (stale.publishNoteMapForTesting (makeMap()));
            setParameter (stale, "delay_ms", 1.2f);
            setParameter (stale, "delayMs", 1.2f);
            processTone (stale, sampleRate, blockSize, 0, warmSamples, false);
            expect (stale.dynamicFallbackActive.load());
            expect (stale.dynamicMapStale.load());
            expectEquals (stale.activeMidiNote.load(), -1);
        }

        beginTest ("T11/T13: active map survives prepareToPlay at a new host sample rate");
        {
            KickLockAudioProcessor processor;
            prepareProcessor (processor, sampleRate, blockSize);
            configureProcessor (processor, true);
            auto map = makeMap();
            map.base.learnedSampleRate = 44100.0;
            expect (processor.publishNoteMapForTesting (map));
            processTone (processor, sampleRate, blockSize, 0, warmSamples, false);
            expect (processor.getActiveNoteMapForTesting().valid);
            expect (! processor.dynamicMapStale.load());

            constexpr double reprepareRate = 96000.0;
            processor.setRateAndBufferSizeDetails (reprepareRate, blockSize);
            processor.prepareToPlay (reprepareRate, blockSize);
            expect (processor.getActiveNoteMapForTesting().valid);
            expectWithinAbsoluteError ((float) processor.getActiveNoteMapForTesting().base.learnedSampleRate,
                                       44100.0f, 1.0e-3f);
            std::vector<float> output;
            processTone (processor, reprepareRate, blockSize, 0, (int) reprepareRate, false, &output);
            expect (! processor.dynamicMapStale.load());
            expect (allFinite (output));
        }

        beginTest ("T11: replacing an active map resets the held-note state before stale fallback");
        {
            KickLockAudioProcessor processor;
            prepareProcessor (processor, sampleRate, blockSize);
            configureProcessor (processor, true);
            expect (processor.publishNoteMapForTesting (makeMap()));
            processTone (processor, sampleRate, blockSize, 0, warmSamples, false);
            expectEquals (processor.getDynamicLastMidiForTesting(), 33);

            auto replacement = makeMap();
            replacement.base.delayMs += 1.0f;
            expect (processor.publishNoteMapForTesting (replacement));
            processTone (processor, sampleRate, blockSize, warmSamples, blockSize, false);
            expectEquals (processor.getDynamicLastMidiForTesting(), -1);
            expect (processor.dynamicMapStale.load());
        }
    }
};

class DynamicTransitionTests : public juce::UnitTest
{
public:
    DynamicTransitionTests() : juce::UnitTest ("DynamicTransitions", "Phase4") {}

    void runTest() override
    {
        beginTest ("T14: smoother duration preserves current and target F/Q exactly");
        {
            MultibandPhaseCore core;
            core.prepare (48000.0, 256, 1, 20.0f);
            juce::AudioBuffer<float> main (1, 256), sidechain (1, 256);
            auto params = coreParams (150.0f, 2.0f, 0.030f);
            core.process (main, sidechain, params, 0);
            for (int i = 0; i < 256; ++i) main.setSample (0, i, 0.5f * std::sin ((float) i * 0.02f));
            core.process (main, sidechain, params, 256);
            const float currentF = core.getSmoothedAllpassFreqForTesting();
            const float targetF = core.getAllpassFreqTargetForTesting();
            const float currentQ = core.getSmoothedAllpassQForTesting();
            const float targetQ = core.getAllpassQTargetForTesting();

            params.allpassSmoothingSeconds = 0.070f;
            core.process (main, sidechain, params, 0);
            expectWithinAbsoluteError (core.getSmoothedAllpassFreqForTesting(), currentF, 1.0e-7f);
            expectWithinAbsoluteError (core.getAllpassFreqTargetForTesting(), targetF, 1.0e-7f);
            expectWithinAbsoluteError (core.getSmoothedAllpassQForTesting(), currentQ, 1.0e-7f);
            expectWithinAbsoluteError (core.getAllpassQTargetForTesting(), targetQ, 1.0e-7f);
            params.allpassSmoothingSeconds = 0.030f;
            core.process (main, sidechain, params, 0);
            expectWithinAbsoluteError (core.getSmoothedAllpassFreqForTesting(), currentF, 1.0e-7f);
            expectWithinAbsoluteError (core.getAllpassFreqTargetForTesting(), targetF, 1.0e-7f);
            expectWithinAbsoluteError (core.getSmoothedAllpassQForTesting(), currentQ, 1.0e-7f);
            expectWithinAbsoluteError (core.getAllpassQTargetForTesting(), targetQ, 1.0e-7f);
        }

        beginTest ("T14: mode, strength, note-target, and smoothing transitions stay below hard-reset margin");
        {
            // A hard allpass-state reset produces a large one-sample jump. This
            // 40 mFS floor (or 3x the steady signal derivative) is deliberately
            // tight relative to the fixture's ~5 mFS steady derivative.
            float worstObservedDerivative = 0.0f;
            float tightestDerivativeLimit = std::numeric_limits<float>::max();
            for (double sampleRate : { 44100.0, 48000.0, 96000.0 })
            for (int blockSize : { 32, 256, 2048 })
            {
                const int first = (int) std::round (sampleRate * 0.40 / (double) blockSize) * blockSize;
                const int second = (int) std::round (sampleRate * 0.68 / (double) blockSize) * blockSize;
                const auto staticParams = coreParams (73.0f, 1.1f, 0.030f);
                const auto dynamicParams = coreParams (130.0f, 2.0f, 0.070f);
                const auto globalParams = coreParams (50.0f, 0.7f, 0.070f);
                const auto noteA = coreParams (70.0f, 0.8f, 0.070f);
                const auto noteB = coreParams (150.0f, 2.2f, 0.070f);
                const auto reference = renderCoreAutomation (sampleRate, blockSize, staticParams, staticParams,
                                                              staticParams, first);
                const int latency = (int) std::ceil (sampleRate * 0.020);
                const auto margin = [&] (const std::vector<float>& changed, int change)
                {
                    const int begin = change + latency - blockSize;
                    const int end = change + latency + (int) std::round (sampleRate * 0.14);
                    const float steady = maxAdjacentDerivative (reference, begin, end);
                    const float actual = maxAdjacentDerivative (changed, begin, end);
                    const float limit = std::max (0.040f, steady * 3.0f);
                    worstObservedDerivative = std::max (worstObservedDerivative, actual);
                    tightestDerivativeLimit = std::min (tightestDerivativeLimit, limit);
                    expectLessThan (actual, limit);
                };

                margin (renderCoreAutomation (sampleRate, blockSize, staticParams, dynamicParams,
                                               dynamicParams, first), first); // Static -> Dynamic
                margin (renderCoreAutomation (sampleRate, blockSize, dynamicParams, staticParams,
                                               staticParams, first), first); // Dynamic -> Static
                const auto strength = renderCoreAutomation (sampleRate, blockSize, globalParams, dynamicParams,
                                                            globalParams, first, second);
                margin (strength, first);  // Strength 0 -> 1
                margin (strength, second); // Strength 1 -> 0
                margin (renderCoreAutomation (sampleRate, blockSize, noteA, noteB, noteB, first), first);

                const auto smoothOnly = renderCoreAutomation (sampleRate, blockSize,
                                                               dynamicParams, coreParams (130.0f, 2.0f, 0.030f),
                                                               dynamicParams, first, second);
                const auto steadyDynamic = renderCoreAutomation (sampleRate, blockSize, dynamicParams,
                                                                  dynamicParams, dynamicParams, first, second);
                expectLessThan (maxAbsDifference (smoothOnly, steadyDynamic), 1.0e-6f,
                                "changing only smoothing duration must not reset audio/filter state");
            }
            logMessage ("T14 worst transition derivative=" + juce::String (worstObservedDerivative, 6)
                        + " tightest hard-reset limit=" + juce::String (tightestDerivativeLimit, 6));
        }
    }
};

static DynamicRuntimeSelectorTests dynamicRuntimeSelectorTests;
static DynamicSmoothingTests dynamicSmoothingTests;
static DynamicProcessorIntegrationTests dynamicProcessorIntegrationTests;
static DynamicTransitionTests dynamicTransitionTests;
