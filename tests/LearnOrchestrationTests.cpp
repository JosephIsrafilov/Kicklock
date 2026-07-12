#include "TestCommon.h"

namespace
{
    void setValue (KickLockAudioProcessor& p, const char* id, float value)
    {
        if (auto* parameter = p.apvts.getParameter (id))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    }

    NotePhaseMapSnapshot makeValidMap (float delay = 2.0f)
    {
        auto map = NoteMap::makeEmptyNoteMap();
        map.valid = true;
        map.base.delayMs = delay;
        map.base.crossoverEnabled = false;
        map.base.crossoverHz = 150.0f;
        map.base.allpassStages = 2;
        map.base.delayInterpolationIndex = 0;
        map.base.learnedSampleRate = kSampleRate;
        map.global.allpassFreqHz = 95.0f;
        map.global.allpassQ = 1.2f;
        map.global.confidence = 0.8f;
        map.global.hitCount = 8;
        map.global.rotatorHelps = false;
        return map;
    }

    LearnFinalizeResult makeResult()
    {
        LearnFinalizeResult result;
        result.valid = true;
        result.map = makeValidMap();
        result.globalFix.valid = true;
        result.globalFix.enoughSignal = true;
        result.globalFix.bassDelayMs = 2.0f;
        result.globalFix.bassPolarityInvert = true;
        result.globalFix.phaseFilterEnabled = false;
        return result;
    }

    LearnSessionContext matchingContext (KickLockAudioProcessor& p)
    {
        LearnSessionContext context;
        context.sampleRate = p.getSampleRate();
        context.crossoverEnabled = false;
        context.crossoverHz = 150.0f;
        context.delayInterpolation = InterpolationType::Linear;
        return context;
    }

    bool waitForState (KickLockAudioProcessor& p, LearnState expected)
    {
        for (int i = 0; i < 200; ++i)
        {
            if (p.getLearnState() == expected)
                return true;
            juce::Thread::sleep (2);
        }
        return p.getLearnState() == expected;
    }

    void processEmptyBlock (KickLockAudioProcessor& p)
    {
        juce::AudioBuffer<float> buffer (juce::jmax (p.getTotalNumInputChannels(),
                                                     p.getTotalNumOutputChannels()), 128);
        buffer.clear();
        juce::MidiBuffer midi;
        p.processBlock (buffer, midi);
    }
}

class LearnOrchestrationTests : public juce::UnitTest
{
public:
    LearnOrchestrationTests() : juce::UnitTest ("LearnOrchestration", "Phase5") {}

    void runTest() override
    {
        beginTest ("Start, stop, and zero-hit Learn resolve without changing audio state");
        {
            KickLockAudioProcessor p;
            p.enableAllBuses();
            p.setRateAndBufferSizeDetails (kSampleRate, 128);
            p.prepareToPlay (kSampleRate, 128);
            const auto before = p.getMessageOwnedNoteMapForTesting();
            expect (p.beginLearn());
            expectEquals ((int) p.getLearnState(), (int) LearnState::Preparing);
            processEmptyBlock (p);
            expectEquals ((int) p.getLearnState(), (int) LearnState::Capturing);
            expect (! p.beginLearn());
            expect (p.stopLearn());
            expect (waitForState (p, LearnState::NotEnoughMaterial));
            expect (! p.hasPendingLearnResult());
            expect (p.getMessageOwnedNoteMapForTesting().valid == before.valid);
        }

        beginTest ("Pending Learn result is isolated until Apply");
        {
            KickLockAudioProcessor p;
            p.setRateAndBufferSizeDetails (kSampleRate, 128);
            p.prepareToPlay (kSampleRate, 128);
            setValue (p, "delay_ms", -4.0f);
            setValue (p, "delayMs", -4.0f);
            setValue (p, "correction_mode", 1.0f);
            setValue (p, "dynamic_strength", 0.25f);
            const float beforeDelay = p.apvts.getRawParameterValue ("delay_ms")->load();
            const auto beforeMap = p.getMessageOwnedNoteMapForTesting();
            auto context = matchingContext (p);
            p.setPendingLearnResultForTesting (makeResult(), context);
            expect (p.hasPendingLearnResult());
            expectEquals ((int) p.getLearnState(), (int) LearnState::ResultReady);
            expectWithinAbsoluteError (p.apvts.getRawParameterValue ("delay_ms")->load(), beforeDelay, 1.0e-6f);
            expect (p.getMessageOwnedNoteMapForTesting().valid == beforeMap.valid);
            expect (p.applyLatestLearnResult());
            expectWithinAbsoluteError (p.apvts.getRawParameterValue ("delay_ms")->load(), 2.0f, 0.01f);
            expectWithinAbsoluteError (p.apvts.getRawParameterValue ("polarity_invert")->load(), 1.0f, 1.0e-6f);
            expectWithinAbsoluteError (p.apvts.getRawParameterValue ("correction_mode")->load(), 1.0f, 1.0e-6f);
            expectWithinAbsoluteError (p.apvts.getRawParameterValue ("dynamic_strength")->load(), 0.25f, 0.01f);
            expect (! p.hasPendingLearnResult());
            expect (p.hasValidNoteMap());
        }

        beginTest ("Apply rejects changed context and preserves the candidate");
        {
            KickLockAudioProcessor p;
            p.setRateAndBufferSizeDetails (kSampleRate, 128);
            p.prepareToPlay (kSampleRate, 128);
            auto context = matchingContext (p);
            p.setPendingLearnResultForTesting (makeResult(), context);
            setValue (p, "crossover_freq", 180.0f);
            expect (! p.applyLatestLearnResult());
            expect (p.hasPendingLearnResult());
            expect (p.getLearnApplyBlockedReason().isNotEmpty());
        }

        beginTest ("Cancel invalidates the session and does not publish a result");
        {
            KickLockAudioProcessor p;
            p.setRateAndBufferSizeDetails (kSampleRate, 128);
            p.prepareToPlay (kSampleRate, 128);
            expect (p.beginLearn());
            const auto session = p.getLearnSessionIdForTesting();
            p.cancelLearn();
            expectEquals ((int) p.getLearnState(), (int) LearnState::Idle);
            expect (! p.hasPendingLearnResult());
            expect (p.getLearnSessionIdForTesting() != session);
        }

        beginTest ("Serialization, Clear Map, and map-aware Revert round-trip");
        {
            KickLockAudioProcessor source;
            source.setRateAndBufferSizeDetails (kSampleRate, 128);
            source.prepareToPlay (kSampleRate, 128);
            source.setPendingLearnResultForTesting (makeResult(), matchingContext (source));
            expect (source.applyLatestLearnResult());
            juce::MemoryBlock state;
            source.getStateInformation (state);

            auto xml = juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize());
            expect (xml != nullptr);
            if (xml != nullptr)
            {
                int maps = 0;
                for (const auto& child : juce::ValueTree::fromXml (*xml))
                    maps += child.hasType (NoteMapKeys::tree) ? 1 : 0;
                expectEquals (maps, 1);
            }

            KickLockAudioProcessor restored;
            restored.setRateAndBufferSizeDetails (kSampleRate, 128);
            restored.prepareToPlay (kSampleRate, 128);
            restored.setStateInformation (state.getData(), (int) state.getSize());
            expect (restored.hasValidNoteMap());
            processEmptyBlock (restored);
            expect (restored.getActiveNoteMapForTesting().valid);

            expect (restored.revertLatestFix() == false);
            // Exercise the map-only rollback path independently of an Apply
            // bundle: this is the normal backend Clear Map operation after a
            // previously applied map has no outstanding rollback point.
            const auto applied = restored.getMessageOwnedNoteMapForTesting();
            expect (restored.publishNoteMapForTesting (applied));
            processEmptyBlock (restored);
            expect (restored.clearNoteMap());
            expect (! restored.hasValidNoteMap());
            expect (restored.revertLatestFix());
            expect (restored.hasValidNoteMap());
        }

        beginTest ("A/B switching does not replace the applied map");
        {
            KickLockAudioProcessor p;
            p.setRateAndBufferSizeDetails (kSampleRate, 128);
            p.prepareToPlay (kSampleRate, 128);
            p.setPendingLearnResultForTesting (makeResult(), matchingContext (p));
            expect (p.applyLatestLearnResult());
            p.selectCompareSlot (1);
            p.selectCompareSlot (0);
            expect (p.hasValidNoteMap());
        }
    }
};

static LearnOrchestrationTests learnOrchestrationTests;
