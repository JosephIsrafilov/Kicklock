#include "TestCommon.h"

#include "dsp/DynamicLearnFormation.h"

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

    DynamicLearnHit editHit (int sequence, int64_t trigger, float seed, bool correctionCapable)
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
        result.correctionBeneficial = correctionCapable;
        result.allpassFreqHz = 100.0f;
        result.allpassQ = 0.8f;
        return result;
    }

    // Two well-separated, repeatable families so the formed map contains two
    // independent, correction-eligible Auto States for isolation tests.
    DynamicStateMap twoStateEditMap()
    {
        std::vector<DynamicLearnHit> hits;
        for (int repeat = 0; repeat < 5; ++repeat)
            hits.push_back (editHit (repeat, repeat * 10000, -0.7f + 0.001f * (float) repeat, true));
        for (int repeat = 0; repeat < 5; ++repeat)
            hits.push_back (editHit (10 + repeat, 100000 + repeat * 10000, 0.7f + 0.001f * (float) repeat, true));

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

    void prepareDynamic (KickLockAudioProcessor& processor, const DynamicStateMap& map,
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
}

class DynamicStateEditProcessorTests : public juce::UnitTest
{
public:
    DynamicStateEditProcessorTests()
        : juce::UnitTest ("DynamicStateEditProcessor", "Dynamic State Map") {}

    void runTest() override
    {
        beginTest ("Rejects edits when no New Dynamic map is applied");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (48000.0, 512);
            processor.prepareToPlay (48000.0, 512);

            KickLockAudioProcessor::DynamicStateEditRequest request;
            request.kind = KickLockAudioProcessor::DynamicStateEditKind::SetManualTrim;
            request.stableStateId = 1;
            request.trim = { 0.5f, 0.0f, 0.0f };
            const auto outcome = processor.applyDynamicStateEdit (request);
            expect (! outcome.success);
            expect (outcome.reason == DynamicStateEditRejectionReason::NoMapApplied);
        }

        beginTest ("setManualTrim publishes atomically and only affects the target State");
        {
            KickLockAudioProcessor processor;
            const auto map = twoStateEditMap();
            prepareDynamic (processor, map);
            expect (processor.getActiveDynamicMapSourceForTesting() == DynamicMapSource::NewDynamicStateMap);

            const auto before = processor.getMessageOwnedDynamicStateMapForTesting();
            expect (getOccupiedDynamicStateCount (before) == 2);
            const uint64_t targetId = before.states[0].stableStateId;
            const uint64_t otherId = before.states[1].stableStateId;

            KickLockAudioProcessor::DynamicStateEditRequest request;
            request.kind = KickLockAudioProcessor::DynamicStateEditKind::SetManualTrim;
            request.stableStateId = targetId;
            request.trim = { 1.2f, 3.0f, -0.4f };
            const auto outcome = processor.applyDynamicStateEdit (request);
            expect (outcome.success);
            expect (outcome.reason == DynamicStateEditRejectionReason::None);

            const auto after = processor.getMessageOwnedDynamicStateMapForTesting();
            const auto edited = processor.getDynamicStateForUi (targetId);
            expect (edited.manualTrim.delayTrimMs == 1.2f);
            expect (edited.manualTrim.frequencyTrimSemitones == 3.0f);
            expect (edited.manualTrim.logPoleDampingTrim == -0.4f);

            const auto untouched = processor.getDynamicStateForUi (otherId);
            expect (isZeroDynamicManualTrim (untouched.manualTrim));
            expect (untouched.hitCount == before.states[1].hitCount);
        }

        beginTest ("Invalid edit leaves the applied map completely untouched");
        {
            KickLockAudioProcessor processor;
            const auto map = twoStateEditMap();
            prepareDynamic (processor, map);
            const auto before = processor.getMessageOwnedDynamicStateMapForTesting();
            const uint64_t targetId = before.states[0].stableStateId;

            KickLockAudioProcessor::DynamicStateEditRequest request;
            request.kind = KickLockAudioProcessor::DynamicStateEditKind::SetManualTrim;
            request.stableStateId = targetId;
            request.trim = { 99.0f, 0.0f, 0.0f }; // out of range
            const auto outcome = processor.applyDynamicStateEdit (request);
            expect (! outcome.success);
            expect (outcome.reason == DynamicStateEditRejectionReason::InvalidTrim);

            const auto after = processor.getMessageOwnedDynamicStateMapForTesting();
            expect (after.states[0].manualTrim.delayTrimMs == before.states[0].manualTrim.delayTrimMs);
            expect (after.nextStateId == before.nextStateId);
        }

        beginTest ("promoteToManual and createManualState round-trip through the processor");
        {
            KickLockAudioProcessor processor;
            const auto map = twoStateEditMap();
            prepareDynamic (processor, map);
            const auto before = processor.getMessageOwnedDynamicStateMapForTesting();
            const uint64_t targetId = before.states[0].stableStateId;

            KickLockAudioProcessor::DynamicStateEditRequest promote;
            promote.kind = KickLockAudioProcessor::DynamicStateEditKind::PromoteToManual;
            promote.stableStateId = targetId;
            const auto promoteOutcome = processor.applyDynamicStateEdit (promote);
            expect (promoteOutcome.success);
            expect (processor.getDynamicStateForUi (targetId).origin == DynamicStateOrigin::Manual);

            KickLockAudioProcessor::DynamicStateEditRequest create;
            create.kind = KickLockAudioProcessor::DynamicStateEditKind::CreateManualState;
            create.creation.fingerprint = editPrototype (0.0f);
            create.creation.hitCount = 4;
            create.creation.evidence = DynamicStateEvidence::Stable;
            const auto createOutcome = processor.applyDynamicStateEdit (create);
            expect (createOutcome.success);
            expect (createOutcome.stableStateId == before.nextStateId);
            expect (processor.getDynamicStateForUi (createOutcome.stableStateId).occupied);

            KickLockAudioProcessor::DynamicStateEditRequest remove;
            remove.kind = KickLockAudioProcessor::DynamicStateEditKind::RemoveManualState;
            remove.stableStateId = targetId;
            const auto removeOutcome = processor.applyDynamicStateEdit (remove);
            expect (removeOutcome.success);
            expect (! processor.getDynamicStateForUi (targetId).occupied);
        }
    }
};

static DynamicStateEditProcessorTests dynamicStateEditProcessorTests;
