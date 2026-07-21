#include "TestCommon.h"

#include "dsp/DynamicLearnFormation.h"

// =============================================================================
// Phase 12: persistence through the REAL processor mutation API. Existing
// persistence tests (DynamicStateMapTests.cpp) only ever construct
// DynamicStateMap values directly and round-trip them through
// dynamicStateMapToValueTree()/dynamicStateMapFromValueTree(). These tests
// close the gap: they mutate through applyDynamicStateEdit() (the same path
// the UI now calls), save via getStateInformation(), reload into a fresh
// processor via setStateInformation(), and confirm the exact values survive.
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

    // Two independent Auto States so per-state isolation across the
    // persistence round trip can be checked, not just a single value.
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

    // Reloads state into a fresh processor prepared at the same rate/block -
    // matches how a real host reload actually behaves (prepareToPlay then
    // setStateInformation, or vice versa; here setStateInformation after
    // prepare mirrors the existing DynamicReleaseGateTests.cpp convention).
    std::unique_ptr<KickLockAudioProcessor> reloadInFreshProcessor (
        const juce::MemoryBlock& savedState, double sampleRate = 48000.0, int blockSize = 512)
    {
        auto restored = std::make_unique<KickLockAudioProcessor>();
        restored->enableAllBuses();
        restored->setRateAndBufferSizeDetails (sampleRate, blockSize);
        restored->prepareToPlay (sampleRate, blockSize);
        restored->setStateInformation (savedState.getData(), (int) savedState.getSize());

        // activeDynamicMapSource (which getDynamicStateForUi() gates on) is
        // an audio-thread-set atomic - it only updates once a block has
        // actually been processed, matching how a real host reload behaves.
        const int channels = juce::jmax (restored->getTotalNumInputChannels(), restored->getTotalNumOutputChannels());
        juce::AudioBuffer<float> silence (channels, blockSize);
        silence.clear();
        juce::MidiBuffer midi;
        restored->processBlock (silence, midi);
        return restored;
    }
}

class DynamicStateEditPersistenceTests : public juce::UnitTest
{
public:
    DynamicStateEditPersistenceTests()
        : juce::UnitTest ("DynamicStateEditPersistence", "Dynamic State Map") {}

    void runTest() override
    {
        beginTest ("A manual trim written via applyDynamicStateEdit survives save/reload exactly");
        {
            KickLockAudioProcessor processor;
            const auto map = twoStateMap();
            prepareApplied (processor, map);
            const auto before = processor.getMessageOwnedDynamicStateMapForTesting();
            const uint64_t targetId = before.states[0].stableStateId;
            const uint64_t otherId = before.states[1].stableStateId;

            KickLockAudioProcessor::DynamicStateEditRequest request;
            request.kind = KickLockAudioProcessor::DynamicStateEditKind::SetManualTrim;
            request.stableStateId = targetId;
            request.trim = { 1.25f, -3.5f, 0.3f };
            expect (processor.applyDynamicStateEdit (request).success);

            juce::MemoryBlock savedState;
            processor.getStateInformation (savedState);

            const auto restored = reloadInFreshProcessor (savedState);
            const auto restoredState = restored->getDynamicStateForUi (targetId);
            expect (restoredState.occupied);
            expectWithinAbsoluteError (restoredState.manualTrim.delayTrimMs, 1.25f, 1.0e-5f);
            expectWithinAbsoluteError (restoredState.manualTrim.frequencyTrimSemitones, -3.5f, 1.0e-5f);
            expectWithinAbsoluteError (restoredState.manualTrim.logPoleDampingTrim, 0.3f, 1.0e-5f);

            const auto restoredOther = restored->getDynamicStateForUi (otherId);
            expect (restoredOther.occupied);
            expect (isZeroDynamicManualTrim (restoredOther.manualTrim),
                    "editing one State must not touch the other State's persisted trim");
        }

        beginTest ("A promoted Manual State's package survives save/reload exactly");
        {
            KickLockAudioProcessor processor;
            const auto map = twoStateMap();
            prepareApplied (processor, map);
            const auto before = processor.getMessageOwnedDynamicStateMapForTesting();
            const uint64_t targetId = before.states[0].stableStateId;

            KickLockAudioProcessor::DynamicStateEditRequest promote;
            promote.kind = KickLockAudioProcessor::DynamicStateEditKind::PromoteToManual;
            promote.stableStateId = targetId;
            expect (processor.applyDynamicStateEdit (promote).success);

            KickLockAudioProcessor::DynamicStateEditRequest trim;
            trim.kind = KickLockAudioProcessor::DynamicStateEditKind::SetManualTrim;
            trim.stableStateId = targetId;
            trim.trim = { -0.75f, 2.0f, -0.2f };
            expect (processor.applyDynamicStateEdit (trim).success);

            juce::MemoryBlock savedState;
            processor.getStateInformation (savedState);

            const auto restored = reloadInFreshProcessor (savedState);
            const auto restoredState = restored->getDynamicStateForUi (targetId);
            expect (restoredState.occupied);
            expect (restoredState.origin == DynamicStateOrigin::Manual);
            expect (restoredState.hasManualBasePackage);
            expect (! restoredState.hasLearnedPackage);
            expectWithinAbsoluteError (restoredState.manualTrim.delayTrimMs, -0.75f, 1.0e-5f);
        }

        beginTest ("A Manual State created from scratch via the transaction API survives save/reload");
        {
            KickLockAudioProcessor processor;
            const auto map = twoStateMap();
            prepareApplied (processor, map);

            KickLockAudioProcessor::DynamicStateEditRequest create;
            create.kind = KickLockAudioProcessor::DynamicStateEditKind::CreateManualState;
            create.creation.fingerprint = editPrototype (0.0f);
            create.creation.hitCount = 4;
            create.creation.evidence = DynamicStateEvidence::Stable;
            const auto createOutcome = processor.applyDynamicStateEdit (create);
            expect (createOutcome.success);
            const uint64_t createdId = createOutcome.stableStateId;

            juce::MemoryBlock savedState;
            processor.getStateInformation (savedState);

            const auto restored = reloadInFreshProcessor (savedState);
            const auto restoredState = restored->getDynamicStateForUi (createdId);
            expect (restoredState.occupied);
            expect (restoredState.origin == DynamicStateOrigin::Manual);
            expect (restoredState.hasManualBasePackage);
            expectEquals ((int) restoredState.hitCount, 4);
        }

        beginTest ("Runtime-only fields never persist: reload always starts Verified unavailable and selection cleared");
        {
            KickLockAudioProcessor processor;
            const auto map = twoStateMap();
            prepareApplied (processor, map);
            const auto before = processor.getMessageOwnedDynamicStateMapForTesting();
            const uint64_t targetId = before.states[0].stableStateId;

            processor.setFocusedStableStateId (targetId);

            juce::MemoryBlock savedState;
            processor.getStateInformation (savedState);

            const auto restored = reloadInFreshProcessor (savedState);
            expectEquals ((int64_t) restored->getFocusedStableStateId(), (int64_t) 0,
                         "Focus selection is UI-only and must never be restored from saved state");
            expect (restored->getDynamicVerifiedMeasurementForTesting (targetId).availability
                    == DynamicMeasurementAvailability::Unavailable);
        }

        beginTest ("Disabled/bypassed flags on a Manual State survive save/reload");
        {
            KickLockAudioProcessor processor;
            const auto map = twoStateMap();
            prepareApplied (processor, map);
            const auto before = processor.getMessageOwnedDynamicStateMapForTesting();
            const uint64_t targetId = before.states[0].stableStateId;

            KickLockAudioProcessor::DynamicStateEditRequest bypass;
            bypass.kind = KickLockAudioProcessor::DynamicStateEditKind::SetBypassed;
            bypass.stableStateId = targetId;
            bypass.boolValue = true;
            expect (processor.applyDynamicStateEdit (bypass).success);

            juce::MemoryBlock savedState;
            processor.getStateInformation (savedState);

            const auto restored = reloadInFreshProcessor (savedState);
            const auto restoredState = restored->getDynamicStateForUi (targetId);
            expect (restoredState.occupied);
            expect (restoredState.bypassed);
            expect (restoredState.enabled);
        }
    }
};

static DynamicStateEditPersistenceTests dynamicStateEditPersistenceTests;
