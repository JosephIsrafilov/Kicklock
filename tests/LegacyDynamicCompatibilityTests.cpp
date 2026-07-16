#include "TestCommon.h"
#include "dsp/DynamicMapSourceResolver.h"
#include "dsp/DynamicRuntimeSelector.h"
#include "dsp/DynamicStateSerialization.h"
#include "dsp/LegacyDynamicCompatibilityRuntime.h"

#include <memory>
#include <type_traits>

namespace
{
    static_assert (std::is_same_v<RuntimeBaseSettings, LegacyDynamicCompatibility::RuntimeBaseSettings>);
    static_assert (std::is_same_v<DynamicNoteState, LegacyDynamicCompatibility::DynamicNoteState>);
    static_assert (std::is_same_v<DynamicRuntimeSelection, LegacyDynamicCompatibility::DynamicRuntimeSelection>);
    static_assert (std::is_trivially_copyable_v<DynamicMapSource>);

    struct StateCodec : juce::AudioProcessor
    {
        using juce::AudioProcessor::copyXmlToBinary;
        using juce::AudioProcessor::getXmlFromBinary;
    };

    RuntimeBaseSettings legacyBase()
    {
        RuntimeBaseSettings base;
        base.delayMs = 1.0f;
        base.polarityInvert = true;
        base.crossoverEnabled = true;
        base.crossoverHz = 140.0f;
        base.allpassStages = 2;
        base.delayInterpolationIndex = 1;
        return base;
    }

    ConflictFingerprint matchingFingerprint()
    {
        ConflictFingerprint fingerprint;
        fingerprint.valid = true;
        fingerprint.values = { 0.20f, 0.40f, 0.60f, 0.80f };
        return fingerprint;
    }

    ConflictStateEntry legacyState()
    {
        ConflictStateEntry state;
        state.delayMs = 3.0f;
        state.polarityInvert = false;
        state.allpassFreqHz = 200.0f;
        state.allpassQ = 2.3f;
        state.stages = 4;
        state.regionType = ConflictRegion::body;
        state.fingerprint = matchingFingerprint().pack();
        state.hitCount = NoteMap::kMinHitsPerNote;
        state.confidence = 0.9f;
        state.matchPercent = 95.0f;
        state.improvementPoints = 5.0f;
        state.applied = true;
        state.noteLabel = 42;
        return state;
    }

    NoteEntry legacyNote()
    {
        NoteEntry note;
        note.learned = true;
        note.fundamentalHz = NoteQuantizer::midiToHz (33);
        note.allpassFreqHz = 100.0f;
        note.allpassQ = 1.7f;
        note.delayMs = -2.0f;
        note.polarityInvert = false;
        note.timingConfidence = 0.8f;
        note.timingSpreadMs = 0.02f;
        note.confidence = 0.8f;
        note.fundamentalSpreadRatio = 0.03f;
        note.hitCount = NoteMap::kMinHitsPerNote;
        note.rotatorHelps = true;
        return note;
    }

    float legacyInterpolatedFrequency (float globalHz, float selectedHz, float strength) noexcept
    {
        return std::exp (std::log (globalHz) + (std::log (selectedHz) - std::log (globalHz)) * strength);
    }

    NotePhaseMapSnapshot legacyMap (bool withConflictState)
    {
        auto map = NoteMap::makeEmptyNoteMap();
        map.valid = true;
        map.base.delayMs = 1.0f;
        map.base.polarityInvert = true;
        map.base.crossoverEnabled = true;
        map.base.crossoverHz = 140.0f;
        map.base.allpassStages = 2;
        map.base.delayInterpolationIndex = 1;
        map.base.learnedSampleRate = 48000.0;
        map.global.allpassFreqHz = 50.0f;
        map.global.allpassQ = 0.7f;
        map.global.confidence = 0.8f;
        map.global.hitCount = NoteMap::kMinHitsPerNote;
        map.notes[(size_t) NotePhaseMapSnapshot::indexForMidi (33)] = legacyNote();
        if (withConflictState)
            map.states[0] = legacyState();
        return map;
    }

    DynamicStateMap runtimeEligibleDynamicMap()
    {
        auto map = makeEmptyDynamicStateMap();
        map.valid = true;
        map.calibration = { true, 0.20f, 0.05f };
        map.globalBase.globalBaseDelayMs = 0.0f;
        map.states[0].occupied = true;
        map.states[0].stableStateId = 1;
        map.states[0].fingerprint.valid = true;
        map.states[0].fingerprint.featureCount = 1;
        map.states[0].fingerprint.features[0] = 0.25f;
        map.states[0].hasLearnedPackage = true;
        map.states[0].learnedPackage = { 0.0f, 100.0f, 0.7f };
        map.states[0].hitCount = DynamicStateMapContract::kCandidateMinimumRepeatableHits;
        map.states[0].repeatability = 0.8f;
        map.states[0].ambiguity = 0.1f;
        map.nextStateId = 2;
        return map;
    }

    juce::ValueTree childOfType (const juce::ValueTree& parent, const char* type)
    {
        for (int i = 0; i < parent.getNumChildren(); ++i)
            if (parent.getChild (i).hasType (juce::Identifier (type)))
                return parent.getChild (i);
        return {};
    }
}

class LegacyDynamicCompatibilityTests : public juce::UnitTest
{
public:
    LegacyDynamicCompatibilityTests()
        : juce::UnitTest ("LegacyDynamicCompatibility", "Dynamic Legacy Compatibility") {}

    void runTest() override
    {
        beginTest ("Compatibility facade exposes the single legacy implementation");
        {
            expect (&selectDynamicRuntime == &LegacyDynamicCompatibility::selectDynamicRuntime);
            expect (&isStructurallyValidRuntimeMap
                    == &LegacyDynamicCompatibility::isStructurallyValidRuntimeMap);
            expect (&mapContextMatchesCurrentParameters
                    == &LegacyDynamicCompatibility::mapContextMatchesCurrentParameters);
        }

        beginTest ("Conflict-state match preserves legacy delay polarity stages and identity");
        {
            const auto map = legacyMap (true);
            const auto base = legacyBase();
            const auto fingerprint = matchingFingerprint();

            DynamicNoteState state;
            const auto full = selectDynamicRuntime (map, base, 73.0f, 1.1f, 0.0f, 1.0f,
                                                    100, 300, state, &fingerprint);
            expect (full.mapUsable && full.usingLearnedNote && ! full.fallbackActive);
            expectEquals (full.selectedState, 0);
            expectEquals (full.selectedMidi, 42);
            expectWithinAbsoluteError (full.targetFreqHz, legacyInterpolatedFrequency (50.0f, 200.0f, 1.0f), 1.0e-6f);
            expectWithinAbsoluteError (full.targetQ, 2.3f, 1.0e-6f);
            expectWithinAbsoluteError (full.targetDelayMs, 3.0f, 1.0e-6f);
            expect (! full.targetPolarityInvert);
            expectEquals (full.targetStages, 4);

            DynamicNoteState zeroState;
            const auto zero = selectDynamicRuntime (map, base, 73.0f, 1.1f, 0.0f, 0.0f,
                                                    100, 300, zeroState, &fingerprint);
            expectWithinAbsoluteError (zero.targetFreqHz, legacyInterpolatedFrequency (50.0f, 200.0f, 0.0f), 1.0e-6f);
            expectWithinAbsoluteError (zero.targetQ, 0.7f, 1.0e-6f);
            expectWithinAbsoluteError (zero.targetDelayMs, 1.0f, 1.0e-6f);
            expect (zero.targetPolarityInvert);
            expectEquals (zero.targetStages, 2);

            DynamicNoteState midpointState;
            const auto midpoint = selectDynamicRuntime (map, base, 73.0f, 1.1f, 0.0f, 0.5f,
                                                        100, 300, midpointState, &fingerprint);
            expectWithinAbsoluteError (midpoint.targetFreqHz,
                                       legacyInterpolatedFrequency (50.0f, 200.0f, 0.5f), 1.0e-6f);
            expectWithinAbsoluteError (midpoint.targetQ, 1.5f, 1.0e-6f);
            expectWithinAbsoluteError (midpoint.targetDelayMs, 2.0f, 1.0e-6f);
            expect (! midpoint.targetPolarityInvert);
            expectEquals (midpoint.targetStages, 4);
        }

        beginTest ("Conflict-state threshold hold and fallback preserve legacy branch order");
        {
            const auto map = legacyMap (true);
            const auto base = legacyBase();
            const auto matching = matchingFingerprint();
            ConflictFingerprint unlike;
            unlike.valid = true;
            unlike.values = { 1.0f, 0.0f, 0.0f, 0.0f };

            DynamicNoteState state;
            selectDynamicRuntime (map, base, 73.0f, 1.1f, 0.0f, 1.0f, 100, 300, state, &matching);
            const auto held = selectDynamicRuntime (map, base, 73.0f, 1.1f, 0.0f, 1.0f,
                                                    100, 300, state, &unlike);
            expect (held.fallbackActive && held.usingLearnedNote);
            expectGreaterThan (held.fingerprintDistance, 0.18f);
            expectWithinAbsoluteError (held.targetFreqHz,
                                       legacyInterpolatedFrequency (50.0f, 200.0f, 1.0f), 1.0e-6f);
            expectWithinAbsoluteError (held.targetDelayMs, 3.0f, 1.0e-6f);
            expect (! held.targetPolarityInvert);
            expectEquals (held.targetStages, 4);
            expectEquals (held.selectedMidi, 42);
            expectEquals (held.selectedState, -1);

            const auto fallback = selectDynamicRuntime (map, base, 73.0f, 1.1f, 0.0f, 1.0f,
                                                        100, 300, state, &unlike);
            expect (fallback.fallbackActive && ! fallback.usingLearnedNote);
            expectWithinAbsoluteError (fallback.targetFreqHz, 50.0f, 1.0e-6f);
            expectWithinAbsoluteError (fallback.targetQ, 0.7f, 1.0e-6f);
            expectWithinAbsoluteError (fallback.targetDelayMs, 1.0f, 1.0e-6f);
            expect (fallback.targetPolarityInvert);
            expectEquals (fallback.targetStages, 2);
        }

        beginTest ("Note path retains linear Q, global timing, and short dropout hysteresis");
        {
            const auto map = legacyMap (false);
            const auto base = legacyBase();
            DynamicNoteState state;
            const float noteHz = NoteQuantizer::midiToHz (33);
            const auto full = selectDynamicRuntime (map, base, 73.0f, 1.1f, noteHz, 1.0f,
                                                    100, 300, state);
            expect (full.usingLearnedNote && ! full.fallbackActive);
            expectEquals (full.selectedMidi, 33);
            expectWithinAbsoluteError (full.targetFreqHz, legacyInterpolatedFrequency (50.0f, 100.0f, 1.0f), 1.0e-6f);
            expectWithinAbsoluteError (full.targetQ, 1.7f, 1.0e-6f);
            expectWithinAbsoluteError (full.targetDelayMs, 1.0f, 1.0e-6f);
            expect (full.targetPolarityInvert);
            expectEquals (full.targetStages, 2);

            DynamicNoteState midpointState;
            const auto midpoint = selectDynamicRuntime (map, base, 73.0f, 1.1f, noteHz, 0.5f,
                                                        100, 300, midpointState);
            expectWithinAbsoluteError (midpoint.targetFreqHz,
                                       legacyInterpolatedFrequency (50.0f, 100.0f, 0.5f), 1.0e-6f);
            expectWithinAbsoluteError (midpoint.targetQ, 1.2f, 1.0e-6f);

            const auto dropout = selectDynamicRuntime (map, base, 73.0f, 1.1f, 0.0f, 1.0f,
                                                       100, 300, state);
            expect (dropout.fallbackActive);
            expectEquals (state.lastMidi, 33);
            expectWithinAbsoluteError (dropout.targetFreqHz, 50.0f, 1.0e-6f);
            expectWithinAbsoluteError (dropout.targetQ, 0.7f, 1.0e-6f);
            expectWithinAbsoluteError (dropout.targetDelayMs, 1.0f, 1.0e-6f);
            expect (dropout.targetPolarityInvert);
            expectEquals (dropout.targetStages, 2);
            selectDynamicRuntime (map, base, 73.0f, 1.1f, 0.0f, 1.0f, 100, 300, state);
            selectDynamicRuntime (map, base, 73.0f, 1.1f, 0.0f, 1.0f, 100, 300, state);
            expectEquals (state.lastMidi, -1);
        }

        beginTest ("Invalid empty and stale legacy maps use manual fallback values");
        {
            auto stale = legacyMap (false);
            auto staleBase = legacyBase();
            staleBase.delayMs = 1.051f;
            DynamicNoteState state;
            const auto staleResult = selectDynamicRuntime (stale, staleBase, 73.0f, 1.1f,
                                                           NoteQuantizer::midiToHz (33), 1.0f,
                                                           100, 300, state);
            expect (staleResult.fallbackActive && staleResult.mapStale);
            expectWithinAbsoluteError (staleResult.targetFreqHz, 73.0f, 1.0e-6f);
            expectWithinAbsoluteError (staleResult.targetQ, 1.1f, 1.0e-6f);
            expectWithinAbsoluteError (staleResult.targetDelayMs, 1.051f, 1.0e-6f);

            const auto invalid = selectDynamicRuntime (NoteMap::makeEmptyNoteMap(), legacyBase(), 73.0f,
                                                        1.1f, NoteQuantizer::midiToHz (33), 1.0f,
                                                        100, 300, state);
            expect (invalid.fallbackActive && ! invalid.mapUsable && ! invalid.mapStale);
            expectWithinAbsoluteError (invalid.targetFreqHz, 73.0f, 1.0e-6f);
        }

        beginTest ("Resolver prioritizes runtime-eligible DynamicStateMap without conversion");
        {
            const auto dynamic = runtimeEligibleDynamicMap();
            const auto legacy = legacyMap (true);
            expectEquals ((int) resolveDynamicMapSource (dynamic, legacy),
                          (int) DynamicMapSource::NewDynamicStateMap);

            auto invalidDynamic = dynamic;
            invalidDynamic.states[0].learnedPackage.delayDeltaMs = 3.1f;
            expectEquals ((int) resolveDynamicMapSource (invalidDynamic, legacy),
                          (int) DynamicMapSource::LegacyDynamicCompatibility);
            expectEquals ((int) resolveDynamicMapSource (makeEmptyDynamicStateMap(), legacy),
                          (int) DynamicMapSource::LegacyDynamicCompatibility);
            expectEquals ((int) resolveDynamicMapSource (makeEmptyDynamicStateMap(), NoteMap::makeEmptyNoteMap()),
                          (int) DynamicMapSource::None);
        }

        beginTest ("KLNoteMap schemas 2, 3, and 4 remain separate compatibility data");
        {
            const auto source = legacyMap (false);
            auto schema2 = noteMapToValueTree (source);
            schema2.setProperty (NoteMapKeys::schemaVersion, 2, nullptr);
            auto schema2Note = childOfType (schema2, NoteMapKeys::note);
            schema2Note.removeProperty (NoteMapKeys::noteTimingConfidence, nullptr);
            const auto parsed2 = noteMapFromValueTree (schema2);
            expect (parsed2.valid);
            expectEquals ((int) parsed2.schemaVersion, (int) NoteMap::kSchemaVersion);
            expect (! parsed2.states[0].applied);
            expectWithinAbsoluteError (parsed2.notes[(size_t) NotePhaseMapSnapshot::indexForMidi (33)].timingConfidence,
                                       1.0f, 1.0e-6f);

            auto schema3 = noteMapToValueTree (legacyMap (true));
            schema3.setProperty (NoteMapKeys::schemaVersion, 3, nullptr);
            const auto parsed3 = noteMapFromValueTree (schema3);
            expect (parsed3.valid);
            expect (! parsed3.states[0].applied);
            expectWithinAbsoluteError (parsed3.notes[(size_t) NotePhaseMapSnapshot::indexForMidi (33)].timingConfidence,
                                       0.8f, 1.0e-6f);

            const auto schema4 = noteMapFromValueTree (noteMapToValueTree (legacyMap (true)));
            expect (schema4.valid && schema4.states[0].applied);
            expect (! dynamicStateMapFromValueTree (schema2).valid);
            expect (! dynamicStateMapFromValueTree (schema3).valid);
            expect (! dynamicStateMapFromValueTree (noteMapToValueTree (legacyMap (true))).valid);
        }

        beginTest ("Project trees coexist through direct XML and binary XML round trips");
        {
            const auto dynamic = runtimeEligibleDynamicMap();
            const auto legacy = legacyMap (true);
            const auto dynamicTree = dynamicStateMapToValueTree (dynamic);
            const auto legacyTree = noteMapToValueTree (legacy);
            const auto dynamicBefore = dynamicTree.createCopy();
            const auto legacyBefore = legacyTree.createCopy();
            juce::ValueTree project { "Project" };
            project.appendChild (dynamicTree.createCopy(), nullptr);
            project.appendChild (legacyTree.createCopy(), nullptr);
            const auto projectBefore = project.createCopy();

            const auto projectDynamic = childOfType (project, DynamicStateMapKeys::tree);
            const auto projectLegacy = childOfType (project, NoteMapKeys::tree);
            const auto parsedDynamicA = dynamicStateMapFromValueTree (projectDynamic);
            const auto parsedDynamicB = dynamicStateMapFromValueTree (projectDynamic);
            const auto parsedLegacyA = noteMapFromValueTree (projectLegacy);
            const auto parsedLegacyB = noteMapFromValueTree (projectLegacy);
            expect (parsedDynamicA.valid && parsedDynamicB.valid);
            expect (parsedLegacyA.valid && parsedLegacyB.valid);
            expect (! dynamicStateMapFromValueTree (projectLegacy).valid);
            expect (! noteMapFromValueTree (projectDynamic).valid);
            expect (dynamicTree.isEquivalentTo (dynamicBefore));
            expect (legacyTree.isEquivalentTo (legacyBefore));
            expect (dynamicStateMapToValueTree (parsedDynamicA).isEquivalentTo (
                dynamicStateMapToValueTree (parsedDynamicB)));
            expect (noteMapToValueTree (parsedLegacyA).isEquivalentTo (noteMapToValueTree (parsedLegacyB)));
            expect (project.isEquivalentTo (projectBefore));
            expectEquals ((int) resolveDynamicMapSource (parsedDynamicA, parsedLegacyA),
                          (int) DynamicMapSource::NewDynamicStateMap);

            const std::unique_ptr<juce::XmlElement> xml (project.createXml());
            expect (xml != nullptr);
            if (xml != nullptr)
            {
                juce::MemoryBlock binary;
                StateCodec::copyXmlToBinary (*xml, binary);
                const std::unique_ptr<juce::XmlElement> restoredXml (
                    StateCodec::getXmlFromBinary (binary.getData(), (int) binary.getSize()));
                expect (restoredXml != nullptr);
                if (restoredXml != nullptr)
                {
                    const auto restoredProject = juce::ValueTree::fromXml (*restoredXml);
                    const auto restoredDynamic = childOfType (restoredProject, DynamicStateMapKeys::tree);
                    const auto restoredLegacy = childOfType (restoredProject, NoteMapKeys::tree);
                    expect (restoredDynamic.isValid() && restoredLegacy.isValid());
                    expect (dynamicStateMapFromValueTree (restoredDynamic).valid);
                    expect (noteMapFromValueTree (restoredLegacy).valid);
                }
            }

            juce::ValueTree legacyOnly { "Project" };
            legacyOnly.appendChild (legacyTree.createCopy(), nullptr);
            expectEquals ((int) resolveDynamicMapSource (makeEmptyDynamicStateMap(),
                                                          noteMapFromValueTree (childOfType (legacyOnly, NoteMapKeys::tree))),
                          (int) DynamicMapSource::LegacyDynamicCompatibility);

            juce::ValueTree dynamicOnly { "Project" };
            dynamicOnly.appendChild (dynamicTree.createCopy(), nullptr);
            expectEquals ((int) resolveDynamicMapSource (
                              dynamicStateMapFromValueTree (childOfType (dynamicOnly, DynamicStateMapKeys::tree)),
                              NoteMap::makeEmptyNoteMap()),
                          (int) DynamicMapSource::NewDynamicStateMap);

            juce::ValueTree malformedDynamic { "Project" };
            auto badDynamic = dynamicTree.createCopy();
            badDynamic.setProperty (DynamicStateMapKeys::schemaVersion, 99, nullptr);
            malformedDynamic.appendChild (badDynamic, nullptr);
            malformedDynamic.appendChild (legacyTree.createCopy(), nullptr);
            expectEquals ((int) resolveDynamicMapSource (
                              dynamicStateMapFromValueTree (childOfType (malformedDynamic, DynamicStateMapKeys::tree)),
                              noteMapFromValueTree (childOfType (malformedDynamic, NoteMapKeys::tree))),
                          (int) DynamicMapSource::LegacyDynamicCompatibility);

            juce::ValueTree malformedLegacy { "Project" };
            auto badLegacy = legacyTree.createCopy();
            badLegacy.setProperty (NoteMapKeys::schemaVersion, 99, nullptr);
            malformedLegacy.appendChild (dynamicTree.createCopy(), nullptr);
            malformedLegacy.appendChild (badLegacy, nullptr);
            expectEquals ((int) resolveDynamicMapSource (
                              dynamicStateMapFromValueTree (childOfType (malformedLegacy, DynamicStateMapKeys::tree)),
                              noteMapFromValueTree (childOfType (malformedLegacy, NoteMapKeys::tree))),
                          (int) DynamicMapSource::NewDynamicStateMap);

            juce::ValueTree neither { "Project" };
            neither.appendChild (badDynamic.createCopy(), nullptr);
            neither.appendChild (badLegacy.createCopy(), nullptr);
            expectEquals ((int) resolveDynamicMapSource (
                              dynamicStateMapFromValueTree (childOfType (neither, DynamicStateMapKeys::tree)),
                              noteMapFromValueTree (childOfType (neither, NoteMapKeys::tree))),
                          (int) DynamicMapSource::None);
        }
    }
};

static LegacyDynamicCompatibilityTests legacyDynamicCompatibilityTests;
