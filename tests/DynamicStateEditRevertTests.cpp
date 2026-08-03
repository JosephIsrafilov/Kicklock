#include "TestCommon.h"

#include "dsp/DynamicLearnFormation.h"
#include "dsp/DynamicStateSerialization.h"

// =============================================================================
// Phase 12: Revert after a per-state edit. Existing Revert coverage
// (DynamicWorkspaceTests.cpp, DynamicReleaseGateTests.cpp) only exercises
// Revert after a whole-map Apply/Clear. These tests specifically prove Revert
// restores the EXACT prior map (every field the serializer round-trips, not
// just a couple of spot fields) after a real per-state edit through
// applyDynamicStateEdit().
// =============================================================================

namespace
{
    DynamicFingerprintPrototype editPrototype (float seed)
    {
        DynamicFingerprintPrototype result;
        result.valid = true;
        result.featureCount = DynamicFingerprintContract::kFeatureCount;
        for (int i = 0; i < DynamicFingerprintContract::kFeatureCount; ++i)
            result.features[(size_t) i] = std::clamp (seed + 0.003f * (float) i, -0.95f, 0.95f);
        return result;
    }

    DynamicLearnHit editHit (int sequence, int64_t trigger, float seed)
    {
        DynamicLearnHit result;
        result.sequence = sequence;
        result.triggerSample = trigger;
        result.fingerprint = editPrototype (seed);
        result.fingerprintValidity = DynamicFingerprintValidity::Valid;
        result.timingEligible = true;
        result.absoluteDelayMs = 0.5f;
        result.timingConfidence = 0.9f;
        result.lowBandEnergy = 1.0f;
        result.correctionBeneficial = true;
        result.allpassFreqHz = 100.0f;
        result.allpassQ = 0.8f;
        return result;
    }

    DynamicStateMap twoStateMap()
    {
        std::vector<DynamicLearnHit> hits;
        for (int repeat = 0; repeat < 5; ++repeat)
            hits.push_back (editHit (repeat, repeat * 10000, -0.7f + 0.001f * (float) repeat));
        for (int repeat = 0; repeat < 5; ++repeat)
            hits.push_back (editHit (10 + repeat, 100000 + repeat * 10000, 0.7f + 0.001f * (float) repeat));

        DynamicLearnFormationContext context;
        context.sampleRate = 48000.0;
        context.crossoverEnabled = true;
        context.crossoverHz = 150.0f;
        context.allpassEnabled = true;
        context.fallbackAllpassFreqHz = 100.0f;
        context.fallbackAllpassQ = 0.7f;
        return formDynamicStateMap (hits, context).map;
    }

    void setParameter (KickLockAudioProcessor& processor, const char* id, float value)
    {
        if (auto* parameter = processor.apvts.getParameter (id))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    }

    void prepareApplied (KickLockAudioProcessor& processor, const DynamicStateMap& map,
                        double sampleRate = 48000.0, int blockSize = 512)
    {
        processor.enableAllBuses();
        processor.setRateAndBufferSizeDetails (sampleRate, blockSize);
        processor.prepareToPlay (sampleRate, blockSize);
        setParameter (processor, "correction_mode", 1.0f);
        processor.publishDynamicStateMapForTesting (map);

        const int channels = juce::jmax (processor.getTotalNumInputChannels(), processor.getTotalNumOutputChannels());
        juce::AudioBuffer<float> silence (channels, blockSize);
        silence.clear();
        juce::MidiBuffer midi;
        processor.processBlock (silence, midi);
    }

    // Field-by-field comparison mirroring DynamicStateMapTests.cpp's own
    // mapsEqual() - every persisted field, not a handful of spot checks.
    bool statesEqual (const DynamicState& a, const DynamicState& b)
    {
        if (a.occupied != b.occupied)
            return false;
        if (! a.occupied)
            return true;
        return a.stableStateId == b.stableStateId
            && a.fingerprint.valid == b.fingerprint.valid
            && a.fingerprint.featureCount == b.fingerprint.featureCount
            && a.fingerprint.features == b.fingerprint.features
            && a.hasLearnedPackage == b.hasLearnedPackage
            && a.learnedPackage.delayDeltaMs == b.learnedPackage.delayDeltaMs
            && a.learnedPackage.allpassFreqHz == b.learnedPackage.allpassFreqHz
            && a.learnedPackage.allpassQ == b.learnedPackage.allpassQ
            && a.hasManualBasePackage == b.hasManualBasePackage
            && a.manualBasePackage.delayDeltaMs == b.manualBasePackage.delayDeltaMs
            && a.manualBasePackage.allpassFreqHz == b.manualBasePackage.allpassFreqHz
            && a.manualBasePackage.allpassQ == b.manualBasePackage.allpassQ
            && a.manualTrim.delayTrimMs == b.manualTrim.delayTrimMs
            && a.manualTrim.frequencyTrimSemitones == b.manualTrim.frequencyTrimSemitones
            && a.manualTrim.logPoleDampingTrim == b.manualTrim.logPoleDampingTrim
            && a.origin == b.origin && a.evidence == b.evidence
            && a.enabled == b.enabled && a.bypassed == b.bypassed
            && a.hitCount == b.hitCount && a.repeatability == b.repeatability && a.ambiguity == b.ambiguity
            && a.hasLikelyMidi == b.hasLikelyMidi && a.likelyMidi == b.likelyMidi
            && a.hasLikelyPitchHz == b.hasLikelyPitchHz && a.likelyPitchHz == b.likelyPitchHz;
    }

    bool mapsExactlyEqual (const DynamicStateMap& a, const DynamicStateMap& b)
    {
        if (a.valid != b.valid || a.nextStateId != b.nextStateId
            || a.globalBase.globalBaseDelayMs != b.globalBase.globalBaseDelayMs
            || a.globalBase.polarityInvert != b.globalBase.polarityInvert
            || a.globalBase.globalAllpassFreqHz != b.globalBase.globalAllpassFreqHz
            || a.globalBase.globalAllpassQ != b.globalBase.globalAllpassQ
            || a.calibration.absoluteDistanceThreshold != b.calibration.absoluteDistanceThreshold
            || a.calibration.ambiguityMargin != b.calibration.ambiguityMargin)
            return false;
        for (int i = 0; i < DynamicStateMapContract::kMaxPersistentStates; ++i)
            if (! statesEqual (a.states[(size_t) i], b.states[(size_t) i]))
                return false;
        return true;
    }
}

class DynamicStateEditRevertTests : public juce::UnitTest
{
public:
    DynamicStateEditRevertTests()
        : juce::UnitTest ("DynamicStateEditRevert", "Dynamic State Map") {}

    void runTest() override
    {
        beginTest ("Revert after a per-state edit restores the exact prior map");
        {
            KickLockAudioProcessor processor;
            const auto map = twoStateMap();
            prepareApplied (processor, map);
            const auto beforeEdit = processor.getMessageOwnedDynamicStateMapForTesting();
            expect (mapsExactlyEqual (beforeEdit, map), "setup: applied map matches what was published");

            const uint64_t targetId = beforeEdit.states[0].stableStateId;
            KickLockAudioProcessor::DynamicStateEditRequest request;
            request.kind = KickLockAudioProcessor::DynamicStateEditKind::SetManualTrim;
            request.stableStateId = targetId;
            request.trim = { 1.0f, -2.0f, 0.2f };
            expect (processor.applyDynamicStateEdit (request).success);

            const auto afterEdit = processor.getMessageOwnedDynamicStateMapForTesting();
            expect (! mapsExactlyEqual (afterEdit, beforeEdit), "setup: the edit actually changed the map");

            expect (processor.revertLatestFix());
            const auto afterRevert = processor.getMessageOwnedDynamicStateMapForTesting();
            expect (mapsExactlyEqual (afterRevert, beforeEdit),
                    "Revert restores every persisted field to exactly what it was before the edit");
        }

        beginTest ("Revert after promote-then-edit restores the exact prior (pre-promotion) map");
        {
            KickLockAudioProcessor processor;
            const auto map = twoStateMap();
            prepareApplied (processor, map);
            const auto beforeEdit = processor.getMessageOwnedDynamicStateMapForTesting();
            const uint64_t targetId = beforeEdit.states[0].stableStateId;
            expect (beforeEdit.states[0].origin == DynamicStateOrigin::Auto, "setup: starts Auto");

            KickLockAudioProcessor::DynamicStateEditRequest promote;
            promote.kind = KickLockAudioProcessor::DynamicStateEditKind::PromoteToManual;
            promote.stableStateId = targetId;
            expect (processor.applyDynamicStateEdit (promote).success);

            KickLockAudioProcessor::DynamicStateEditRequest trim;
            trim.kind = KickLockAudioProcessor::DynamicStateEditKind::SetManualTrim;
            trim.stableStateId = targetId;
            trim.trim = { 0.5f, 1.0f, -0.1f };
            expect (processor.applyDynamicStateEdit (trim).success);

            expect (processor.revertLatestFix());
            const auto afterRevert = processor.getMessageOwnedDynamicStateMapForTesting();
            expect (mapsExactlyEqual (afterRevert, beforeEdit),
                    "Revert undoes both the promotion and the subsequent trim, restoring the exact original Auto State");
            expect (afterRevert.states[0].origin == DynamicStateOrigin::Auto);
        }

        beginTest ("Revert does not restore runtime selection or verification history (by contract)");
        {
            KickLockAudioProcessor processor;
            const auto map = twoStateMap();
            prepareApplied (processor, map);
            const auto beforeEdit = processor.getMessageOwnedDynamicStateMapForTesting();
            const uint64_t targetId = beforeEdit.states[0].stableStateId;

            KickLockAudioProcessor::DynamicStateEditRequest request;
            request.kind = KickLockAudioProcessor::DynamicStateEditKind::SetManualTrim;
            request.stableStateId = targetId;
            request.trim = { 0.2f, 0.0f, 0.0f };
            expect (processor.applyDynamicStateEdit (request).success);
            expect (processor.revertLatestFix());

            // Verified is fresh runtime evidence, never restored by Revert -
            // it simply stays whatever the (also non-persistent) runtime
            // state currently is, which after this sequence is unavailable.
            expect (processor.getDynamicVerifiedMeasurementForTesting (targetId).availability
                    == DynamicMeasurementAvailability::Unavailable);
        }
    }
};

static DynamicStateEditRevertTests dynamicStateEditRevertTests;
