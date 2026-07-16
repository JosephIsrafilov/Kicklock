#include "TestCommon.h"
#include "DynamicStateMap.h"
#include "DynamicStateSerialization.h"
#include "NotePhaseMap.h"

#include <limits>
#include <memory>
#include <utility>

namespace
{
    DynamicZonePackage validPackage()
    {
        return { 0.0f, 100.0f, 0.7f };
    }

    DynamicFingerprintPrototype validFingerprint()
    {
        DynamicFingerprintPrototype fingerprint;
        fingerprint.valid = true;
        fingerprint.featureCount = 4;
        fingerprint.features[0] = -0.8f;
        fingerprint.features[1] = 0.25f;
        fingerprint.features[2] = -0.15f;
        fingerprint.features[3] = 0.5f;
        return fingerprint;
    }

    DynamicState validAutoState (uint64_t stateId, uint32_t hitCount = 3)
    {
        DynamicState state;
        state.occupied = true;
        state.stableStateId = stateId;
        state.fingerprint = validFingerprint();
        state.hasLearnedPackage = true;
        state.learnedPackage = validPackage();
        state.hitCount = hitCount;
        state.repeatability = 0.85f;
        state.ambiguity = 0.1f;
        state.evidence = hitCount >= DynamicStateMapContract::kStableAutoMinimumRepeatableHits
            ? DynamicStateEvidence::Stable : DynamicStateEvidence::Candidate;
        return state;
    }

    DynamicStateMap validMap (int stateCount = 1)
    {
        DynamicStateMap map = makeEmptyDynamicStateMap();
        map.valid = true;
        map.calibration = { true, 0.20f, 0.05f };
        map.globalBase.globalBaseDelayMs = -4.0f;
        for (int i = 0; i < stateCount; ++i)
            map.states[(size_t) i] = validAutoState ((uint64_t) i + 1u);
        map.nextStateId = (uint64_t) stateCount + 1u;
        return map;
    }

    struct DynamicStateMapStateCodec : juce::AudioProcessor
    {
        using juce::AudioProcessor::copyXmlToBinary;
        using juce::AudioProcessor::getXmlFromBinary;
    };

    DynamicStateMap completePersistentMap()
    {
        auto map = validMap (3);
        map.globalBase.globalBaseDelayMs = 7.0f;
        map.globalBase.polarityInvert = true;
        map.globalBase.crossoverEnabled = false;
        map.globalBase.crossoverHz = 220.0f;
        map.globalBase.allpassEnabled = false;
        map.globalBase.globalAllpassFreqHz = 160.0f;
        map.globalBase.globalAllpassQ = 1.4f;
        map.globalBase.allpassStages = 4;
        map.globalBase.delayInterpolationIndex = 1;
        map.globalBase.learnedSampleRate = 96000.0;
        map.diagnostics = { DynamicMapDiagnostic::NoConfidentAutoFix, 12, 3, 2, 3 };
        map.states[0].stableStateId = std::numeric_limits<uint64_t>::max() - 3u;
        map.states[1].stableStateId = std::numeric_limits<uint64_t>::max() - 2u;
        map.states[2].stableStateId = std::numeric_limits<uint64_t>::max() - 1u;
        map.nextStateId = std::numeric_limits<uint64_t>::max();

        map.states[1].hasLearnedPackage = false;
        map.states[1].bypassed = true;

        map.states[2].enabled = false;
        map.states[2].hasLikelyMidi = true;
        map.states[2].likelyMidi = 36;
        map.states[2].hasLikelyPitchHz = true;
        map.states[2].likelyPitchHz = 65.4064f;
        map.states[2].origin = DynamicStateOrigin::Manual;
        map.states[2].hasLearnedPackage = false;
        map.states[2].hasManualBasePackage = true;
        map.states[2].manualBasePackage = { 1.5f, 175.0f, 1.2f };
        map.states[2].manualTrim = { -0.5f, 3.0f, -0.25f };
        return map;
    }

    bool mapsEqual (const DynamicStateMap& left, const DynamicStateMap& right)
    {
        if (left.valid != right.valid || left.schemaVersion != right.schemaVersion
            || left.fingerprintExtractorVersion != right.fingerprintExtractorVersion
            || left.nextStateId != right.nextStateId
            || left.globalBase.globalBaseDelayMs != right.globalBase.globalBaseDelayMs
            || left.globalBase.polarityInvert != right.globalBase.polarityInvert
            || left.globalBase.crossoverEnabled != right.globalBase.crossoverEnabled
            || left.globalBase.crossoverHz != right.globalBase.crossoverHz
            || left.globalBase.allpassEnabled != right.globalBase.allpassEnabled
            || left.globalBase.globalAllpassFreqHz != right.globalBase.globalAllpassFreqHz
            || left.globalBase.globalAllpassQ != right.globalBase.globalAllpassQ
            || left.globalBase.allpassStages != right.globalBase.allpassStages
            || left.globalBase.delayInterpolationIndex != right.globalBase.delayInterpolationIndex
            || left.globalBase.learnedSampleRate != right.globalBase.learnedSampleRate
            || left.calibration.valid != right.calibration.valid
            || left.calibration.absoluteDistanceThreshold != right.calibration.absoluteDistanceThreshold
            || left.calibration.ambiguityMargin != right.calibration.ambiguityMargin
            || left.diagnostics.diagnostic != right.diagnostics.diagnostic
            || left.diagnostics.analyzedHitCount != right.diagnostics.analyzedHitCount
            || left.diagnostics.rejectedHitCount != right.diagnostics.rejectedHitCount
            || left.diagnostics.unstableHitCount != right.diagnostics.unstableHitCount
            || left.diagnostics.repeatableClusterCount != right.diagnostics.repeatableClusterCount)
            return false;

        for (size_t i = 0; i < left.states.size(); ++i)
        {
            const auto& a = left.states[i];
            const auto& b = right.states[i];
            if (a.occupied != b.occupied)
                return false;
            if (! a.occupied)
                continue;
            if (a.stableStateId != b.stableStateId || a.fingerprint.valid != b.fingerprint.valid
                || a.fingerprint.featureCount != b.fingerprint.featureCount
                || a.fingerprint.features != b.fingerprint.features
                || a.hasLearnedPackage != b.hasLearnedPackage
                || a.learnedPackage.delayDeltaMs != b.learnedPackage.delayDeltaMs
                || a.learnedPackage.allpassFreqHz != b.learnedPackage.allpassFreqHz
                || a.learnedPackage.allpassQ != b.learnedPackage.allpassQ
                || a.hasManualBasePackage != b.hasManualBasePackage
                || a.manualBasePackage.delayDeltaMs != b.manualBasePackage.delayDeltaMs
                || a.manualBasePackage.allpassFreqHz != b.manualBasePackage.allpassFreqHz
                || a.manualBasePackage.allpassQ != b.manualBasePackage.allpassQ
                || a.manualTrim.delayTrimMs != b.manualTrim.delayTrimMs
                || a.manualTrim.frequencyTrimSemitones != b.manualTrim.frequencyTrimSemitones
                || a.manualTrim.logPoleDampingTrim != b.manualTrim.logPoleDampingTrim
                || a.origin != b.origin || a.evidence != b.evidence || a.enabled != b.enabled
                || a.bypassed != b.bypassed || a.hitCount != b.hitCount
                || a.repeatability != b.repeatability || a.ambiguity != b.ambiguity
                || a.hasLikelyMidi != b.hasLikelyMidi || a.likelyMidi != b.likelyMidi
                || a.hasLikelyPitchHz != b.hasLikelyPitchHz || a.likelyPitchHz != b.likelyPitchHz)
                return false;
        }
        return true;
    }

    juce::ValueTree onlyStateChild (const juce::ValueTree& tree)
    {
        for (int i = 0; i < tree.getNumChildren(); ++i)
            if (tree.getChild (i).hasType (juce::Identifier (DynamicStateMapKeys::state)))
                return tree.getChild (i);
        return {};
    }
}

class DynamicStateMapTests : public juce::UnitTest
{
public:
    DynamicStateMapTests() : juce::UnitTest ("DynamicStateMap", "Dynamic State Map") {}

    void runTest() override
    {
        using namespace DynamicStateMapContract;

        beginTest ("Package accepts frozen boundaries and rejects malformed values");
        {
            expect (isValidDynamicZonePackage ({ kStateDelayDeltaMinMs, kAllpassFrequencyMinHz, kAllpassQMin }));
            expect (isValidDynamicZonePackage ({ kStateDelayDeltaMaxMs, kAllpassFrequencyMaxHz, kAllpassQMax }));
            expect (! isValidDynamicZonePackage ({ kStateDelayDeltaMinMs - 0.01f, 100.0f, 0.7f }));
            expect (! isValidDynamicZonePackage ({ kStateDelayDeltaMaxMs + 0.01f, 100.0f, 0.7f }));
            expect (! isValidDynamicZonePackage ({ 0.0f, kAllpassFrequencyMinHz - 0.01f, 0.7f }));
            expect (! isValidDynamicZonePackage ({ 0.0f, kAllpassFrequencyMaxHz + 0.01f, 0.7f }));
            expect (! isValidDynamicZonePackage ({ 0.0f, 100.0f, kAllpassQMin - 0.01f }));
            expect (! isValidDynamicZonePackage ({ 0.0f, 100.0f, kAllpassQMax + 0.01f }));
            expect (! isValidDynamicZonePackage ({ std::numeric_limits<float>::quiet_NaN(), 100.0f, 0.7f }));
            expect (! isValidDynamicZonePackage ({ 0.0f, std::numeric_limits<float>::infinity(), 0.7f }));
        }

        beginTest ("Look-ahead invariant accepts exact boundary only");
        {
            expect (hasDynamicLookAheadBudget (20.0f, -4.0f, -3.0f, 4.0f, 8.0f, 1.0f));
            expect (! hasDynamicLookAheadBudget (20.0f, -4.0f, -3.001f, 4.0f, 8.0f, 1.0f));
            expect (! hasDynamicLookAheadBudget (20.0f, -4.001f, -3.0f, 4.0f, 8.0f, 1.0f));
            expect (! hasDynamicLookAheadBudget (std::numeric_limits<float>::quiet_NaN(), -4.0f, -3.0f,
                                                  4.0f, 8.0f, 1.0f));
            expect (! hasDynamicLookAheadBudget (20.0f, -4.0f, -3.0f,
                                                  std::numeric_limits<float>::infinity(), 8.0f, 1.0f));
        }

        beginTest ("Auto and Manual state evidence semantics are strict");
        {
            auto candidate = validAutoState (1, 3);
            expect (isValidDynamicState (candidate));
            candidate.evidence = DynamicStateEvidence::Stable;
            expect (! isValidDynamicState (candidate));
            candidate.hitCount = 5;
            expect (isValidDynamicState (candidate));

            DynamicState manual = validAutoState (2, 3);
            manual.origin = DynamicStateOrigin::Manual;
            manual.hasLearnedPackage = false;
            manual.hasManualBasePackage = true;
            manual.manualBasePackage = validPackage();
            expect (isValidDynamicState (manual));
            manual.evidence = DynamicStateEvidence::Stable;
            expect (isValidDynamicState (manual));
            manual.hitCount = 2;
            expect (! isValidDynamicState (manual));

            DynamicState recognized = validAutoState (3, 3);
            recognized.hasLearnedPackage = false;
            expect (isValidDynamicState (recognized));
            recognized.manualTrim.delayTrimMs = 0.1f;
            expect (! isValidDynamicState (recognized));

            auto invalid = validAutoState (4, 3);
            invalid.hasManualBasePackage = true;
            invalid.manualBasePackage = validPackage();
            expect (! isValidDynamicState (invalid));
            invalid = manual;
            invalid.hasManualBasePackage = false;
            expect (! isValidDynamicState (invalid));
            invalid = manual;
            invalid.hasLearnedPackage = true;
            invalid.learnedPackage = validPackage();
            expect (! isValidDynamicState (invalid));
            invalid = validAutoState (5, 3);
            invalid.enabled = false;
            invalid.bypassed = true;
            expect (! isValidDynamicState (invalid));
            invalid = validAutoState (0, 3);
            expect (! isValidDynamicState (invalid));
            invalid = validAutoState (6, 3);
            invalid.fingerprint.valid = false;
            expect (! isValidDynamicState (invalid));
            invalid = validAutoState (7, 3);
            invalid.fingerprint.features[0] = 1.01f;
            expect (! isValidDynamicState (invalid));
            invalid = validAutoState (8, 3);
            invalid.fingerprint.features[4] = 0.01f;
            expect (! isValidDynamicState (invalid));
            invalid = validAutoState (9, 3);
            invalid.manualTrim = { 3.0f, 24.0f, 2.0f };
            expect (isValidDynamicState (invalid));
            invalid.manualTrim.delayTrimMs = 3.01f;
            expect (! isValidDynamicState (invalid));
        }

        beginTest ("Effective stored delay delta is bounded after manual trim");
        {
            auto state = validAutoState (10, 3);
            float effectiveDelayDeltaMs = 0.0f;

            state.learnedPackage.delayDeltaMs = -3.0f;
            expect (getEffectiveStoredDynamicStateDelayDeltaMs (state, effectiveDelayDeltaMs));
            expectWithinAbsoluteError (effectiveDelayDeltaMs, -3.0f, 1.0e-6f);
            expect (isValidDynamicState (state));

            state.learnedPackage.delayDeltaMs = 3.0f;
            expect (getEffectiveStoredDynamicStateDelayDeltaMs (state, effectiveDelayDeltaMs));
            expectWithinAbsoluteError (effectiveDelayDeltaMs, 3.0f, 1.0e-6f);

            state.learnedPackage.delayDeltaMs = -2.0f;
            state.manualTrim.delayTrimMs = -1.0f;
            expect (getEffectiveStoredDynamicStateDelayDeltaMs (state, effectiveDelayDeltaMs));
            expectWithinAbsoluteError (effectiveDelayDeltaMs, -3.0f, 1.0e-6f);

            state.learnedPackage.delayDeltaMs = 2.0f;
            state.manualTrim.delayTrimMs = 1.0f;
            expect (getEffectiveStoredDynamicStateDelayDeltaMs (state, effectiveDelayDeltaMs));
            expectWithinAbsoluteError (effectiveDelayDeltaMs, 3.0f, 1.0e-6f);

            state.learnedPackage.delayDeltaMs = -3.0f;
            state.manualTrim.delayTrimMs = -0.001f;
            expect (! getEffectiveStoredDynamicStateDelayDeltaMs (state, effectiveDelayDeltaMs));
            expect (! isValidDynamicState (state));

            state.learnedPackage.delayDeltaMs = 3.0f;
            state.manualTrim.delayTrimMs = 0.001f;
            expect (! getEffectiveStoredDynamicStateDelayDeltaMs (state, effectiveDelayDeltaMs));
            expect (! isValidDynamicState (state));

            state.learnedPackage.delayDeltaMs = 0.0f;
            state.manualTrim.delayTrimMs = -3.0f;
            expect (getEffectiveStoredDynamicStateDelayDeltaMs (state, effectiveDelayDeltaMs));
            expectWithinAbsoluteError (effectiveDelayDeltaMs, -3.0f, 1.0e-6f);
            state.manualTrim.delayTrimMs = 3.0f;
            expect (getEffectiveStoredDynamicStateDelayDeltaMs (state, effectiveDelayDeltaMs));
            expectWithinAbsoluteError (effectiveDelayDeltaMs, 3.0f, 1.0e-6f);

            state.hasLearnedPackage = false;
            state.manualTrim = makeZeroDynamicManualTrim();
            expect (getEffectiveStoredDynamicStateDelayDeltaMs (state, effectiveDelayDeltaMs));
            expectWithinAbsoluteError (effectiveDelayDeltaMs, 0.0f, 1.0e-6f);
            expect (isValidDynamicState (state));
            state.manualTrim.delayTrimMs = 0.1f;
            expect (! getEffectiveStoredDynamicStateDelayDeltaMs (state, effectiveDelayDeltaMs));
            expect (! isValidDynamicState (state));

            auto manual = validAutoState (11, 3);
            manual.origin = DynamicStateOrigin::Manual;
            manual.hasLearnedPackage = false;
            manual.hasManualBasePackage = true;
            manual.manualBasePackage = { -2.0f, 100.0f, 0.7f };
            manual.manualTrim.delayTrimMs = -1.0f;
            expect (getEffectiveStoredDynamicStateDelayDeltaMs (manual, effectiveDelayDeltaMs));
            expectWithinAbsoluteError (effectiveDelayDeltaMs, -3.0f, 1.0e-6f);

            auto map = validMap (1);
            map.states[0].learnedPackage.delayDeltaMs = -2.0f;
            map.states[0].manualTrim.delayTrimMs = -1.0f;
            expect (isStructurallyValidDynamicStateMap (map));
            expect (isRuntimeEligibleDynamicStateMap (map));
        }

        beginTest ("Map validation enforces identity, calibration, capacity and eligibility");
        {
            const auto empty = makeEmptyDynamicStateMap();
            expect (! empty.valid);
            expect (! isStructurallyValidDynamicStateMap (empty));
            const auto emptyTree = dynamicStateMapToValueTree (empty);
            expect (emptyTree.isEquivalentTo (dynamicStateMapToValueTree (empty)));
            expectEquals (emptyTree.getNumChildren(), 0);
            expect (! dynamicStateMapFromValueTree (emptyTree).valid);

            auto one = validMap (1);
            expect (isStructurallyValidDynamicStateMap (one));
            expect (isRuntimeEligibleDynamicStateMap (one));
            auto eight = validMap (8);
            expect (isStructurallyValidDynamicStateMap (eight));
            expect (isRuntimeEligibleDynamicStateMap (eight));

            auto duplicate = validMap (2);
            duplicate.states[1].stableStateId = duplicate.states[0].stableStateId;
            expect (! isStructurallyValidDynamicStateMap (duplicate));
            auto nextId = validMap (1);
            nextId.nextStateId = 1;
            expect (! isStructurallyValidDynamicStateMap (nextId));
            auto invalidCalibration = validMap (1);
            invalidCalibration.calibration.ambiguityMargin = 0.21f;
            expect (! isStructurallyValidDynamicStateMap (invalidCalibration));
            auto invalidBase = validMap (1);
            invalidBase.globalBase.globalBaseDelayMs = kGlobalBaseDelayMaxMs + 0.01f;
            expect (! isStructurallyValidDynamicStateMap (invalidBase));
            invalidBase = validMap (1);
            invalidBase.globalBase.globalAllpassFreqHz = kAllpassFrequencyMinHz - 0.01f;
            expect (! isStructurallyValidDynamicStateMap (invalidBase));
        }

        beginTest ("Persistent map round-trips candidates, no-fix, bypass and disabled states");
        {
            const auto map = completePersistentMap();

            const auto tree = dynamicStateMapToValueTree (map);
            const auto parsed = dynamicStateMapFromValueTree (tree);
            expect (mapsEqual (map, parsed));
            expect (! tree.hasProperty ("colour"));
            expect (! tree.hasProperty ("effectivePackage"));
            expect (! tree.hasProperty ("activeState"));
            const auto child = onlyStateChild (tree);
            expect (! child.hasProperty ("colour"));
            expect (! child.hasProperty ("effectivePackage"));
            expect (! child.hasProperty ("fadePosition"));
        }

        beginTest ("ValueTree XML round-trip restores canonical string properties");
        {
            const auto map = completePersistentMap();
            const auto tree = dynamicStateMapToValueTree (map);
            const std::unique_ptr<juce::XmlElement> xml (tree.createXml());
            expect (xml != nullptr);
            if (xml != nullptr)
            {
                const auto restored = juce::ValueTree::fromXml (*xml);
                const auto state = onlyStateChild (restored);
                expect (restored.getProperty (DynamicStateMapKeys::schemaVersion).isString());
                expect (restored.getProperty (DynamicStateMapKeys::extractorVersion).isString());
                expect (restored.getProperty (DynamicStateMapKeys::valid).isString());
                expect (restored.getProperty (DynamicStateMapKeys::globalBaseDelayMs).isString());
                expect (restored.getProperty (DynamicStateMapKeys::learnedSampleRate).isString());
                expect (restored.getProperty (DynamicStateMapKeys::analyzedHitCount).isString());
                expect (state.getProperty (DynamicStateMapKeys::stateId).isString());
                expect (state.getProperty (DynamicStateMapKeys::origin).isString());
                expect (state.getProperty (DynamicStateMapKeys::hitCount).isString());
                expect (mapsEqual (map, dynamicStateMapFromValueTree (restored)));
            }
        }

        beginTest ("AudioProcessor XML binary round-trip restores DynamicStateMap");
        {
            const auto map = completePersistentMap();
            const auto tree = dynamicStateMapToValueTree (map);
            const std::unique_ptr<juce::XmlElement> sourceXml (tree.createXml());
            juce::MemoryBlock binary;
            expect (sourceXml != nullptr);
            if (sourceXml != nullptr)
            {
                DynamicStateMapStateCodec::copyXmlToBinary (*sourceXml, binary);
                const std::unique_ptr<juce::XmlElement> restoredXml (
                    DynamicStateMapStateCodec::getXmlFromBinary (binary.getData(), (int) binary.getSize()));
                expect (restoredXml != nullptr);
                if (restoredXml != nullptr)
                    expect (mapsEqual (map, dynamicStateMapFromValueTree (
                        juce::ValueTree::fromXml (*restoredXml))));
            }
        }

        beginTest ("Parser accepts strict XML strings and rejects malformed required strings");
        {
            auto map = validMap (1);
            map.globalBase.learnedSampleRate = 48000.0;
            const auto makeTree = [&map] { return dynamicStateMapToValueTree (map); };

            auto strings = makeTree();
            strings.setProperty (DynamicStateMapKeys::schemaVersion, "1", nullptr);
            strings.setProperty (DynamicStateMapKeys::extractorVersion, "1", nullptr);
            strings.setProperty (DynamicStateMapKeys::valid, "1", nullptr);
            strings.setProperty (DynamicStateMapKeys::polarityInvert, "0", nullptr);
            strings.setProperty (DynamicStateMapKeys::globalBaseDelayMs, "-4.0", nullptr);
            strings.setProperty (DynamicStateMapKeys::learnedSampleRate, "4.8e4", nullptr);
            strings.setProperty (DynamicStateMapKeys::analyzedHitCount, "0", nullptr);
            auto state = onlyStateChild (strings);
            state.setProperty (DynamicStateMapKeys::fingerprintValid, "1", nullptr);
            state.setProperty (DynamicStateMapKeys::origin, "0", nullptr);
            state.setProperty (DynamicStateMapKeys::hitCount, "3", nullptr);
            expect (dynamicStateMapFromValueTree (strings).valid);

            strings = makeTree();
            strings.setProperty (DynamicStateMapKeys::polarityInvert, "false", nullptr);
            expect (dynamicStateMapFromValueTree (strings).valid);

            for (const char* malformed : { "yes", " 1", "2" })
            {
                strings = makeTree();
                strings.setProperty (DynamicStateMapKeys::valid, malformed, nullptr);
                expect (! dynamicStateMapFromValueTree (strings).valid);
            }

            strings = makeTree();
            strings.setProperty (DynamicStateMapKeys::schemaVersion, "2", nullptr);
            expect (! dynamicStateMapFromValueTree (strings).valid);
            strings = makeTree();
            strings.setProperty (DynamicStateMapKeys::schemaVersion, " 1", nullptr);
            expect (! dynamicStateMapFromValueTree (strings).valid);
            strings = makeTree();
            onlyStateChild (strings).setProperty (DynamicStateMapKeys::origin, "9", nullptr);
            expect (! dynamicStateMapFromValueTree (strings).valid);

            for (const char* malformed : { "-1", "3.0", "3x", "4294967296", " 3" })
            {
                strings = makeTree();
                onlyStateChild (strings).setProperty (DynamicStateMapKeys::hitCount, malformed, nullptr);
                expect (! dynamicStateMapFromValueTree (strings).valid);
            }

            strings = makeTree();
            strings.setProperty (DynamicStateMapKeys::globalBaseDelayMs, "1e0", nullptr);
            expect (dynamicStateMapFromValueTree (strings).valid);
            for (const char* malformed : { "NaN", "Inf", "1.25x", " 1.25", "", "1e999" })
            {
                strings = makeTree();
                strings.setProperty (DynamicStateMapKeys::globalBaseDelayMs, malformed, nullptr);
                expect (! dynamicStateMapFromValueTree (strings).valid);
            }
            strings = makeTree();
            strings.setProperty (DynamicStateMapKeys::learnedSampleRate, "1e999", nullptr);
            expect (! dynamicStateMapFromValueTree (strings).valid);
        }

        beginTest ("State IDs serialize losslessly and malformed IDs reject map");
        {
            auto map = validMap (1);
            map.states[0].stableStateId = std::numeric_limits<uint64_t>::max() - 1u;
            map.nextStateId = std::numeric_limits<uint64_t>::max();
            auto tree = dynamicStateMapToValueTree (map);
            expectEquals (dynamicStateMapFromValueTree (tree).states[0].stableStateId,
                          std::numeric_limits<uint64_t>::max() - 1u);
            expect (tree.getProperty (DynamicStateMapKeys::nextStateId).toString()
                    == "18446744073709551615");

            auto child = onlyStateChild (tree);
            expect (child.getProperty (DynamicStateMapKeys::stateId).toString()
                    == "18446744073709551614");
            child.setProperty (DynamicStateMapKeys::stateId, "", nullptr);
            expect (! dynamicStateMapFromValueTree (tree).valid);
            child.setProperty (DynamicStateMapKeys::stateId, "18446744073709551616", nullptr);
            expect (! dynamicStateMapFromValueTree (tree).valid);
            child.setProperty (DynamicStateMapKeys::stateId, "-1", nullptr);
            expect (! dynamicStateMapFromValueTree (tree).valid);
            child.setProperty (DynamicStateMapKeys::stateId, "12x", nullptr);
            expect (! dynamicStateMapFromValueTree (tree).valid);

            auto malformedNextStateId = dynamicStateMapToValueTree (validMap (1));
            malformedNextStateId.setProperty (DynamicStateMapKeys::nextStateId, "", nullptr);
            expect (! dynamicStateMapFromValueTree (malformedNextStateId).valid);
            malformedNextStateId.setProperty (DynamicStateMapKeys::nextStateId, "-1", nullptr);
            expect (! dynamicStateMapFromValueTree (malformedNextStateId).valid);
            malformedNextStateId.setProperty (DynamicStateMapKeys::nextStateId, "12x", nullptr);
            expect (! dynamicStateMapFromValueTree (malformedNextStateId).valid);
            malformedNextStateId.setProperty (DynamicStateMapKeys::nextStateId,
                                              "18446744073709551616", nullptr);
            expect (! dynamicStateMapFromValueTree (malformedNextStateId).valid);
        }

        beginTest ("Parser rejects malformed maps and preserves deterministic state order");
        {
            auto map = validMap (2);
            const auto first = dynamicStateMapToValueTree (map);
            const auto second = dynamicStateMapToValueTree (map);
            expect (first.isEquivalentTo (second));
            expectEquals (first.getNumChildren(), 2);

            auto reordered = validMap (2);
            std::swap (reordered.states[0], reordered.states[1]);
            const auto sorted = dynamicStateMapToValueTree (reordered);
            expect (first.isEquivalentTo (sorted));
            expect (sorted.getChild (0).getProperty (DynamicStateMapKeys::stateId).toString() == "1");
            expect (sorted.getChild (1).getProperty (DynamicStateMapKeys::stateId).toString() == "2");

            auto malformed = first.createCopy();
            malformed.removeProperty (DynamicStateMapKeys::schemaVersion, nullptr);
            expect (! dynamicStateMapFromValueTree (malformed).valid);
            malformed = first.createCopy();
            malformed.setProperty (DynamicStateMapKeys::schemaVersion, 2, nullptr);
            expect (! dynamicStateMapFromValueTree (malformed).valid);
            malformed = first.createCopy();
            malformed.setProperty (DynamicStateMapKeys::globalAllpassQ,
                                    std::numeric_limits<double>::quiet_NaN(), nullptr);
            expect (! dynamicStateMapFromValueTree (malformed).valid);
            malformed = first.createCopy();
            malformed.setProperty (DynamicStateMapKeys::globalAllpassQ,
                                    std::numeric_limits<double>::infinity(), nullptr);
            expect (! dynamicStateMapFromValueTree (malformed).valid);
            malformed = first.createCopy();
            onlyStateChild (malformed).setProperty (DynamicStateMapKeys::origin, 99, nullptr);
            expect (! dynamicStateMapFromValueTree (malformed).valid);
            malformed = first.createCopy();
            malformed.setProperty (DynamicStateMapKeys::diagnostic, 99, nullptr);
            expect (! dynamicStateMapFromValueTree (malformed).valid);
            malformed = first.createCopy();
            malformed.setProperty (DynamicStateMapKeys::calibrationValid, false, nullptr);
            expect (! dynamicStateMapFromValueTree (malformed).valid);
            malformed = first.createCopy();
            malformed.setProperty ("futureOptionalProperty", "ignored", nullptr);
            expect (dynamicStateMapFromValueTree (malformed).valid);

            for (int i = 0; i < 7; ++i)
                malformed.appendChild (onlyStateChild (first).createCopy(), nullptr);
            expect (! dynamicStateMapFromValueTree (malformed).valid);
        }

        beginTest ("New Dynamic parser rejects KLNoteMap and never writes it");
        {
            auto legacy = NoteMap::makeEmptyNoteMap();
            legacy.valid = true;
            legacy.global.allpassFreqHz = 100.0f;
            legacy.global.allpassQ = 1.0f;
            legacy.base.crossoverHz = 150.0f;
            legacy.base.allpassStages = 2;
            const auto legacyTree = noteMapToValueTree (legacy);
            const auto legacyTreeBeforeParse = legacyTree.createCopy();
            expect (! dynamicStateMapFromValueTree (legacyTree).valid);
            expect (legacyTree.isEquivalentTo (legacyTreeBeforeParse));
            const juce::ValueTree foreignTree { "ForeignTree" };
            expect (! dynamicStateMapFromValueTree (foreignTree).valid);
            expect (dynamicStateMapToValueTree (validMap()).hasType (
                juce::Identifier (DynamicStateMapKeys::tree)));
        }
    }
};

static DynamicStateMapTests dynamicStateMapTests;
