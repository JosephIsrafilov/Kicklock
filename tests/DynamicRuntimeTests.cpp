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

            state.reset();
            const auto unknown = select (map, NoteQuantizer::midiToHz (34), 1.0f, state);
            expect (unknown.mapUsable && unknown.fallbackActive && ! unknown.usingLearnedNote);
            expectEquals (unknown.selectedMidi, 34);
            expectWithinAbsoluteError (unknown.targetFreqHz, 50.0f, 1.0e-5f);

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
                expectGreaterOrEqual (result.targetFreqHz, previous);
                previous = result.targetFreqHz;
            }
            DynamicNoteState state;
            const auto clamped = select (map, NoteQuantizer::midiToHz (33), 5.0f, state);
            expectWithinAbsoluteError (clamped.targetFreqHz, 100.0f, 1.0e-5f);
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

static DynamicRuntimeSelectorTests dynamicRuntimeSelectorTests;
static DynamicSmoothingTests dynamicSmoothingTests;
