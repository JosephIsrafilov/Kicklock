#include "TestCommon.h"

namespace
{
    struct StateCodec : juce::AudioProcessor
    {
        using juce::AudioProcessor::copyXmlToBinary;
        using juce::AudioProcessor::getXmlFromBinary;
    };

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

    bool waitForState (KickLockAudioProcessor& p, LearnState expected, int attempts = 200)
    {
        for (int i = 0; i < attempts; ++i)
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

    void feedLearnHits (KickLockAudioProcessor& p, bool bypassed)
    {
        constexpr int block = 128;
        const int total = (int) kSampleRate;
        const int spacing = (int) (kSampleRate * 0.25);
        for (int offset = 0; offset < total; offset += block)
        {
            const int n = std::min (block, total - offset);
            juce::AudioBuffer<float> buffer (juce::jmax (p.getTotalNumInputChannels(),
                                                         p.getTotalNumOutputChannels()), n);
            buffer.clear();
            for (int i = 0; i < n; ++i)
            {
                const int sample = offset + i;
                const int inHit = sample % spacing;
                const double t = (double) inHit / kSampleRate;
                const float env = (float) std::exp (-t * 25.0);
                const float tone = env * (float) std::sin (kTwoPi * 60.0 * t);
                buffer.setSample (0, i, tone * 0.8f);
                if (buffer.getNumChannels() > 1) buffer.setSample (1, i, tone * 0.8f);
                if (buffer.getNumChannels() > 2) buffer.setSample (2, i, tone);
                if (buffer.getNumChannels() > 3) buffer.setSample (3, i, tone);
            }
            juce::MidiBuffer midi;
            if (bypassed) p.processBlockBypassed (buffer, midi);
            else p.processBlock (buffer, midi);
        }
    }

    NotePhaseMapSnapshot mapFromState (const juce::MemoryBlock& state)
    {
        auto xml = StateCodec::getXmlFromBinary (state.getData(), (int) state.getSize());
        if (xml == nullptr)
            return NoteMap::makeEmptyNoteMap();
        const auto tree = juce::ValueTree::fromXml (*xml);
        for (const auto& child : tree)
            if (child.hasType (NoteMapKeys::tree))
                return noteMapFromValueTree (child);
        return NoteMap::makeEmptyNoteMap();
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

        beginTest ("Production worker drains real windows and invokes the real finalizer");
        {
            KickLockAudioProcessor p;
            p.enableAllBuses();
            p.setRateAndBufferSizeDetails (kSampleRate, 128);
            p.prepareToPlay (kSampleRate, 128);
            expect (p.beginLearn());
            feedLearnHits (p, false);
            expectGreaterOrEqual (p.getLearnAcceptedHits(), 3);
            expect (p.stopLearn());
            for (int i = 0; i < 2500 && learnStateIsBusy (p.getLearnState()); ++i)
                juce::Thread::sleep (2);
            expect (! learnStateIsBusy (p.getLearnState()));
            expectGreaterOrEqual (p.getLearnProgress().drainedHits, 3);
            expect (! p.getMessageOwnedNoteMapForTesting().valid,
                    "real finalization must remain pending-only even when valid");
        }

        beginTest ("Host bypass continues Learn observation");
        {
            KickLockAudioProcessor p;
            p.enableAllBuses();
            p.setRateAndBufferSizeDetails (kSampleRate, 128);
            p.prepareToPlay (kSampleRate, 128);
            expect (p.beginLearn());
            feedLearnHits (p, true);
            expectGreaterOrEqual (p.getLearnAcceptedHits(), 3);
            p.cancelLearn();
            expectEquals ((int) p.getLearnState(), (int) LearnState::Idle);
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

        beginTest ("Apply crossover tolerance is inclusive and interpolation is exact");
        {
            KickLockAudioProcessor accepted;
            accepted.setRateAndBufferSizeDetails (kSampleRate, 128);
            accepted.prepareToPlay (kSampleRate, 128);
            accepted.setPendingLearnResultForTesting (makeResult(), matchingContext (accepted));
            setValue (accepted, "crossover_freq", 151.0f);
            expect (accepted.applyLatestLearnResult(), accepted.getLearnApplyBlockedReason());

            KickLockAudioProcessor rejected;
            rejected.setRateAndBufferSizeDetails (kSampleRate, 128);
            rejected.prepareToPlay (kSampleRate, 128);
            rejected.setPendingLearnResultForTesting (makeResult(), matchingContext (rejected));
            setValue (rejected, "delayInterp", 1.0f);
            expect (! rejected.applyLatestLearnResult());
            expect (rejected.hasPendingLearnResult());
        }

        beginTest ("Apply allpass policy preserves no-help state and enables a helping note");
        {
            KickLockAudioProcessor noHelp;
            noHelp.setRateAndBufferSizeDetails (kSampleRate, 128);
            noHelp.prepareToPlay (kSampleRate, 128);
            noHelp.setPendingLearnResultForTesting (makeResult(), matchingContext (noHelp));
            expect (noHelp.applyLatestLearnResult());
            expectWithinAbsoluteError (noHelp.apvts.getRawParameterValue ("allpass_enable")->load(), 0.0f, 1.0e-6f);

            KickLockAudioProcessor helping;
            helping.setRateAndBufferSizeDetails (kSampleRate, 128);
            helping.prepareToPlay (kSampleRate, 128);
            auto result = makeResult();
            auto& note = result.map.notes[(size_t) NotePhaseMapSnapshot::indexForMidi (33)];
            note.learned = true;
            note.fundamentalHz = 55.0f;
            note.allpassFreqHz = 110.0f;
            note.allpassQ = 1.0f;
            note.confidence = 0.8f;
            note.fundamentalSpreadRatio = 0.03f;
            note.hitCount = NoteMap::kMinHitsPerNote;
            note.rotatorHelps = true;
            helping.setPendingLearnResultForTesting (result, matchingContext (helping));
            expect (helping.applyLatestLearnResult());
            expectWithinAbsoluteError (helping.apvts.getRawParameterValue ("allpass_enable")->load(), 1.0f, 1.0e-6f);
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

        beginTest ("Re-prepare cancels active Learn and destruction is bounded");
        {
            KickLockAudioProcessor p;
            p.setRateAndBufferSizeDetails (kSampleRate, 128);
            p.prepareToPlay (kSampleRate, 128);
            expect (p.beginLearn());
            processEmptyBlock (p);
            p.setRateAndBufferSizeDetails (96000.0, 128);
            p.prepareToPlay (96000.0, 128);
            expectEquals ((int) p.getLearnState(), (int) LearnState::Idle);

            for (int i = 0; i < 20; ++i)
            {
                auto instance = std::make_unique<KickLockAudioProcessor>();
                instance->setRateAndBufferSizeDetails (kSampleRate, 128);
                instance->prepareToPlay (kSampleRate, 128);
                expect (instance->beginLearn());
                processEmptyBlock (*instance);
                if ((i % 2) == 0) instance->stopLearn();
            }
        }

        beginTest ("Pending candidates are not serialized and old state clears an applied map");
        {
            KickLockAudioProcessor pending;
            pending.setRateAndBufferSizeDetails (kSampleRate, 128);
            pending.prepareToPlay (kSampleRate, 128);
            pending.setPendingLearnResultForTesting (makeResult(), matchingContext (pending));
            juce::MemoryBlock pendingState;
            pending.getStateInformation (pendingState);
            expect (! mapFromState (pendingState).valid);

            KickLockAudioProcessor oldSource;
            juce::MemoryBlock oldState;
            oldSource.getStateInformation (oldState);
            auto oldXml = StateCodec::getXmlFromBinary (oldState.getData(), (int) oldState.getSize());
            expect (oldXml != nullptr);
            if (oldXml != nullptr)
            {
                auto oldTree = juce::ValueTree::fromXml (*oldXml);
                for (int i = oldTree.getNumChildren() - 1; i >= 0; --i)
                    if (oldTree.getChild(i).hasType (NoteMapKeys::tree))
                        oldTree.removeChild (i, nullptr);
                StateCodec::copyXmlToBinary (*oldTree.createXml(), oldState);
            }

            KickLockAudioProcessor target;
            target.setRateAndBufferSizeDetails (kSampleRate, 128);
            target.prepareToPlay (kSampleRate, 128);
            expect (target.publishNoteMapForTesting (makeValidMap()));
            processEmptyBlock (target);
            expect (target.getActiveNoteMapForTesting().valid);
            target.setStateInformation (oldState.getData(), (int) oldState.getSize());
            processEmptyBlock (target);
            expect (! target.hasValidNoteMap());
            expect (! target.getActiveNoteMapForTesting().valid);
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
