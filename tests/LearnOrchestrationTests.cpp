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

    juce::MemoryBlock makeOldStateWithoutMap()
    {
        KickLockAudioProcessor source;
        juce::MemoryBlock state;
        source.getStateInformation (state);
        auto xml = StateCodec::getXmlFromBinary (state.getData(), (int) state.getSize());
        if (xml == nullptr)
            return {};
        auto tree = juce::ValueTree::fromXml (*xml);
        for (int i = tree.getNumChildren() - 1; i >= 0; --i)
            if (tree.getChild (i).hasType (NoteMapKeys::tree))
                tree.removeChild (i, nullptr);
        StateCodec::copyXmlToBinary (*tree.createXml(), state);
        return state;
    }

    void pumpMessages (int milliseconds)
    {
        juce::Thread::sleep (milliseconds);
        juce::Timer::callPendingTimersSynchronously();
    }

    std::vector<float> renderBlock (KickLockAudioProcessor& p)
    {
        constexpr int samples = 512;
        juce::AudioBuffer<float> buffer (juce::jmax (p.getTotalNumInputChannels(),
                                                     p.getTotalNumOutputChannels()), samples);
        buffer.clear();
        for (int i = 0; i < samples; ++i)
        {
            const float value = 0.5f * std::sin ((float) i * 0.031f);
            buffer.setSample (0, i, value);
            if (buffer.getNumChannels() > 1) buffer.setSample (1, i, value);
        }
        juce::MidiBuffer midi;
        p.processBlock (buffer, midi);
        std::vector<float> output ((size_t) samples);
        for (int i = 0; i < samples; ++i) output[(size_t) i] = buffer.getSample (0, i);
        return output;
    }

    bool waitForFlag (const std::atomic<bool>& flag, int timeoutMs)
    {
        for (int elapsed = 0; elapsed < timeoutMs; elapsed += 5)
        {
            if (flag.load (std::memory_order_acquire))
                return true;
            juce::Thread::sleep (5);
        }
        return flag.load (std::memory_order_acquire);
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

        beginTest ("Editor close and reopen leaves processor-owned Learn state intact");
        {
            KickLockAudioProcessor p;
            p.enableAllBuses();
            p.setRateAndBufferSizeDetails (kSampleRate, 128);
            p.prepareToPlay (kSampleRate, 128);
            expect (p.beginLearn());
            expectEquals ((int) p.getLearnState(), (int) LearnState::Preparing);
            {
                std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
                expect (editor != nullptr);
            }
            expectEquals ((int) p.getLearnState(), (int) LearnState::Preparing);
            processEmptyBlock (p);
            expectEquals ((int) p.getLearnState(), (int) LearnState::Capturing);
            {
                std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
                expect (editor != nullptr);
            }
            expectEquals ((int) p.getLearnState(), (int) LearnState::Capturing);
            p.cancelLearn();

            p.setPendingLearnResultForTesting (makeResult(), matchingContext (p));
            {
                std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
                expect (editor != nullptr);
            }
            expectEquals ((int) p.getLearnState(), (int) LearnState::ResultReady);
            expect (p.hasPendingLearnResult());
        }

        beginTest ("Pending Learn result changes no parameters, maps, rendered output, or serialized state");
        {
            KickLockAudioProcessor baseline;
            baseline.setRateAndBufferSizeDetails (kSampleRate, 512);
            baseline.prepareToPlay (kSampleRate, 512);
            KickLockAudioProcessor pending;
            pending.setRateAndBufferSizeDetails (kSampleRate, 512);
            pending.prepareToPlay (kSampleRate, 512);
            const float delayBefore = pending.apvts.getRawParameterValue ("delay_ms")->load();
            pending.setPendingLearnResultForTesting (makeResult(), matchingContext (pending));
            expectWithinAbsoluteError (pending.apvts.getRawParameterValue ("delay_ms")->load(), delayBefore, 1.0e-7f);
            expect (! pending.getMessageOwnedNoteMapForTesting().valid);
            expect (! pending.getActiveNoteMapForTesting().valid);
            const auto a = renderBlock (baseline);
            const auto b = renderBlock (pending);
            float maxDifference = 0.0f;
            for (size_t i = 0; i < a.size(); ++i)
                maxDifference = std::max (maxDifference, std::abs (a[i] - b[i]));
            expectLessThan (maxDifference, 1.0e-7f);
            juce::MemoryBlock state;
            pending.getStateInformation (state);
            expect (! mapFromState (state).valid);
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
            expect (! p.canApplyLatestLearnResult());
            expect (p.getLearnApplyBlockedReason().isNotEmpty());
            expect (! p.applyLatestLearnResult());
            expect (p.hasPendingLearnResult());
            expect (p.getLearnApplyBlockedReason().isNotEmpty());

            KickLockAudioProcessor rate;
            rate.setRateAndBufferSizeDetails (kSampleRate, 128);
            rate.prepareToPlay (kSampleRate, 128);
            rate.setPendingLearnResultForTesting (makeResult(), matchingContext (rate));
            rate.setRateAndBufferSizeDetails (96000.0, 128);
            expect (! rate.applyLatestLearnResult());
            expect (rate.hasPendingLearnResult());

            KickLockAudioProcessor crossover;
            crossover.setRateAndBufferSizeDetails (kSampleRate, 128);
            crossover.prepareToPlay (kSampleRate, 128);
            crossover.setPendingLearnResultForTesting (makeResult(), matchingContext (crossover));
            const auto mapBefore = crossover.getMessageOwnedNoteMapForTesting();
            const float delayBefore = crossover.apvts.getRawParameterValue ("delay_ms")->load();
            setValue (crossover, "crossover_enable", 1.0f);
            expect (! crossover.applyLatestLearnResult());
            expect (crossover.hasPendingLearnResult());
            expectWithinAbsoluteError (crossover.apvts.getRawParameterValue ("delay_ms")->load(), delayBefore, 1.0e-7f);
            expect (crossover.getMessageOwnedNoteMapForTesting().valid == mapBefore.valid);
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

            KickLockAudioProcessor beyond;
            beyond.setRateAndBufferSizeDetails (kSampleRate, 128);
            beyond.prepareToPlay (kSampleRate, 128);
            beyond.setPendingLearnResultForTesting (makeResult(), matchingContext (beyond));
            setValue (beyond, "crossover_freq", 151.01f);
            expect (! beyond.applyLatestLearnResult());
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

        beginTest ("Apply allpass eligibility matches Dynamic runtime eligibility");
        {
            struct Variant { int kind; bool eligible; };
            for (const auto variant : { Variant { 0, true }, Variant { 1, false }, Variant { 2, false },
                                        Variant { 3, false }, Variant { 4, false }, Variant { 5, false } })
            {
                KickLockAudioProcessor p;
                p.setRateAndBufferSizeDetails (kSampleRate, 128);
                p.prepareToPlay (kSampleRate, 128);
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
                if (variant.kind == 1) note.hitCount = NoteMap::kMinHitsPerNote - 1;
                if (variant.kind == 2) note.allpassFreqHz = 0.0f;
                if (variant.kind == 3) note.allpassQ = 0.0f;
                if (variant.kind == 4) note.confidence = NoteMap::kMinRuntimeConfidence - 0.01f;
                if (variant.kind == 5) note.fundamentalSpreadRatio = NoteMap::kNoteSpreadMaxRatio + 0.01f;

                RuntimeBaseSettings base;
                base.delayMs = result.map.base.delayMs;
                base.polarityInvert = result.map.base.polarityInvert;
                base.crossoverEnabled = result.map.base.crossoverEnabled;
                base.crossoverHz = result.map.base.crossoverHz;
                base.allpassStages = result.map.base.allpassStages;
                base.delayInterpolationIndex = result.map.base.delayInterpolationIndex;
                DynamicNoteState noteState;
                const auto runtime = selectDynamicRuntime (result.map, base, 50.0f, 0.7f, 55.0f,
                                                           1.0f, 128, 12000, noteState);
                expect (runtime.usingLearnedNote == variant.eligible);

                p.setPendingLearnResultForTesting (result, matchingContext (p));
                expect (p.applyLatestLearnResult());
                expect ((p.apvts.getRawParameterValue ("allpass_enable")->load() > 0.5f)
                        == variant.eligible);
            }

            for (bool initiallyEnabled : { false, true })
            {
                KickLockAudioProcessor p;
                p.setRateAndBufferSizeDetails (kSampleRate, 128);
                p.prepareToPlay (kSampleRate, 128);
                setValue (p, "allpass_enable", initiallyEnabled ? 1.0f : 0.0f);
                setValue (p, "phaseFilterEnabled", initiallyEnabled ? 1.0f : 0.0f);
                p.setPendingLearnResultForTesting (makeResult(), matchingContext (p));
                expect (p.applyLatestLearnResult());
                expect ((p.apvts.getRawParameterValue ("allpass_enable")->load() > 0.5f)
                        == initiallyEnabled);
            }

            KickLockAudioProcessor global;
            global.setRateAndBufferSizeDetails (kSampleRate, 128);
            global.prepareToPlay (kSampleRate, 128);
            auto globalResult = makeResult();
            globalResult.map.global.rotatorHelps = true;
            global.setPendingLearnResultForTesting (globalResult, matchingContext (global));
            expect (global.applyLatestLearnResult());
            expect (global.apvts.getRawParameterValue ("allpass_enable")->load() > 0.5f);
        }

        beginTest ("Resolved Learn states reset completely after restore, Clear, Revert, and Discard");
        {
            KickLockAudioProcessor validSource;
            juce::MemoryBlock validState;
            validSource.getStateInformation (validState);
            const auto oldState = makeOldStateWithoutMap();
            const std::array<unsigned char, 4> corruptBytes { 0xde, 0xad, 0xbe, 0xef };

            auto expectIdle = [this] (KickLockAudioProcessor& p)
            {
                expectEquals ((int) p.getLearnState(), (int) LearnState::Idle);
                expect (! p.hasPendingLearnResult());
                expect (p.getLearnApplyBlockedReason().isEmpty());
                const auto progress = p.getLearnProgress();
                expectEquals ((int) progress.state, (int) LearnState::Idle);
                expect (! progress.stopRequested);
                expectEquals (progress.capturedHits, 0);
            };

            for (const auto resolved : { LearnState::ResultReady, LearnState::NotEnoughMaterial,
                                         LearnState::Failed })
            {
                auto prime = [&] (KickLockAudioProcessor& p)
                {
                    p.setRateAndBufferSizeDetails (kSampleRate, 128);
                    p.prepareToPlay (kSampleRate, 128);
                    p.setPendingLearnResultForTesting (makeResult(), matchingContext (p));
                    p.setResolvedLearnStateForTesting (resolved);
                };

                for (int restoreKind = 0; restoreKind < 3; ++restoreKind)
                {
                    KickLockAudioProcessor p;
                    prime (p);
                    if (restoreKind == 0)
                        p.setStateInformation (validState.getData(), (int) validState.getSize());
                    else if (restoreKind == 1)
                        p.setStateInformation (oldState.getData(), (int) oldState.getSize());
                    else
                        p.setStateInformation (corruptBytes.data(), (int) corruptBytes.size());
                    expectIdle (p);
                    expect (! p.applyLatestLearnResult(), "restored stale candidate cannot be applied");
                }

                KickLockAudioProcessor clear;
                prime (clear);
                expect (clear.clearNoteMap());
                expectIdle (clear);

                KickLockAudioProcessor revert;
                revert.setRateAndBufferSizeDetails (kSampleRate, 128);
                revert.prepareToPlay (kSampleRate, 128);
                revert.ensureRevertBundleCapturedForTesting();
                revert.setPendingLearnResultForTesting (makeResult(), matchingContext (revert));
                revert.setResolvedLearnStateForTesting (resolved);
                expect (revert.revertLatestFix());
                expectIdle (revert);

                KickLockAudioProcessor discard;
                prime (discard);
                expect (discard.discardLatestLearnResult());
                expectIdle (discard);
            }
        }

        beginTest ("Queue pressure retains only the latest pending map and publishes it intact");
        {
            KickLockAudioProcessor p;
            p.setRateAndBufferSizeDetails (kSampleRate, 128);
            p.prepareToPlay (kSampleRate, 128);
            for (int i = 0; i < 4; ++i)
                p.requestMapPublicationForTesting (makeValidMap ((float) i));
            p.requestMapPublicationForTesting (makeValidMap (50.0f));
            p.requestMapPublicationForTesting (makeValidMap (60.0f));
            expect (p.isMapPublicationRetryScheduledForTesting());
            processEmptyBlock (p);
            expect (p.serviceMapPublicationRetryForTesting());
            processEmptyBlock (p);
            expectWithinAbsoluteError (p.getActiveNoteMapForTesting().base.delayMs, 60.0f, 1.0e-6f);
            expectEquals (p.getPendingMapUpdates(), 0);
        }

        beginTest ("Scheduled map retry is synchronously fenced by destruction");
        {
            for (int repeat = 0; repeat < 20; ++repeat)
            {
                auto callbacks = std::make_shared<std::atomic<int>> (0);
                {
                    auto p = std::make_unique<KickLockAudioProcessor>();
                    p->setRateAndBufferSizeDetails (kSampleRate, 128);
                    p->prepareToPlay (kSampleRate, 128);
                    p->setMapPublicationRetryObserverForTesting (callbacks);
                    for (int i = 0; i < 5; ++i)
                        p->requestMapPublicationForTesting (makeValidMap ((float) i));
                    expect (p->isMapPublicationRetryScheduledForTesting());
                }
                pumpMessages (30);
                expectEquals (callbacks->load(), 0,
                              "no timer callback may touch the destroyed processor");
            }
        }

        beginTest ("In-flight map timer callback fences destruction");
        {
            auto callbacks = std::make_shared<std::atomic<int>> (0);
            auto p = std::make_unique<KickLockAudioProcessor>();
            p->setRateAndBufferSizeDetails (kSampleRate, 128);
            p->prepareToPlay (kSampleRate, 128);
            p->setMapPublicationRetryObserverForTesting (callbacks);
            const auto pause = p->getMapTimerCallbackPauseControlForTesting();
            pause->pause();
            for (int i = 0; i < 5; ++i)
                p->requestMapPublicationForTesting (makeValidMap ((float) i));
            expect (p->isMapPublicationRetryScheduledForTesting());

            juce::Thread::sleep (30);
            std::thread timerThread ([] { juce::Timer::callPendingTimersSynchronously(); });
            expect (pause->waitUntilEntered (500));

            std::atomic<bool> destroyed { false };
            std::thread destroyThread ([owned = std::move (p), &destroyed] () mutable
            {
                owned.reset();
                destroyed.store (true, std::memory_order_release);
            });
            juce::Thread::sleep (50);
            expect (! destroyed.load (std::memory_order_acquire),
                    "destruction must wait for the in-flight timer callback");

            pause->release();
            timerThread.join();
            destroyThread.join();
            expect (destroyed.load (std::memory_order_acquire));
            expectEquals (callbacks->load(), 1);
            pumpMessages (30);
            expectEquals (callbacks->load(), 1,
                          "no map timer callback may run after destruction");
        }

        beginTest ("Lifecycle cancellation is measured and bounded in every worker stage");
        {
            auto elapsedMs = [] (auto&& action)
            {
                const auto start = std::chrono::steady_clock::now();
                action();
                return std::chrono::duration_cast<std::chrono::milliseconds> (
                    std::chrono::steady_clock::now() - start).count();
            };

            const auto preparing = elapsedMs ([]
            {
                auto p = std::make_unique<KickLockAudioProcessor>();
                p->setRateAndBufferSizeDetails (kSampleRate, 128);
                p->prepareToPlay (kSampleRate, 128);
                p->beginLearn();
            });
            expectLessThan ((int) preparing, 500);

            KickLockAudioProcessor capturing;
            capturing.setRateAndBufferSizeDetails (kSampleRate, 128);
            capturing.prepareToPlay (kSampleRate, 128);
            expect (capturing.beginLearn());
            processEmptyBlock (capturing);
            const auto captureCancel = elapsedMs ([&] { capturing.cancelLearn(); });
            expectLessThan ((int) captureCancel, 500);

            KickLockAudioProcessor stopping;
            stopping.setRateAndBufferSizeDetails (kSampleRate, 128);
            stopping.prepareToPlay (kSampleRate, 128);
            stopping.setLearnWorkerPauseStateForTesting (LearnState::Draining);
            expect (stopping.beginLearn());
            processEmptyBlock (stopping);
            expect (stopping.stopLearn());
            expectEquals ((int) stopping.getLearnState(), (int) LearnState::Stopping);
            const auto stoppingCancel = elapsedMs ([&] { stopping.cancelLearn(); });
            stopping.setLearnWorkerPauseStateForTesting (LearnState::Idle);
            expectLessThan ((int) stoppingCancel, 500);

            for (const auto stage : { LearnState::Draining, LearnState::Finalizing })
            {
                KickLockAudioProcessor p;
                p.setRateAndBufferSizeDetails (kSampleRate, 128);
                p.prepareToPlay (kSampleRate, 128);
                p.setLearnWorkerPauseStateForTesting (stage);
                expect (p.beginLearn());
                processEmptyBlock (p);
                expect (p.stopLearn());
                expect (waitForState (p, stage));
                const auto duration = elapsedMs ([&] { p.cancelLearn(); });
                p.setLearnWorkerPauseStateForTesting (LearnState::Idle);
                expectLessThan ((int) duration, stage == LearnState::Finalizing ? 1500 : 500);
                expectEquals ((int) p.getLearnState(), (int) LearnState::Idle);
                expect (! p.hasPendingLearnResult());
                expect (! p.hasValidNoteMap());
            }

            KickLockAudioProcessor reprepare;
            reprepare.setRateAndBufferSizeDetails (kSampleRate, 128);
            reprepare.prepareToPlay (kSampleRate, 128);
            expect (reprepare.beginLearn());
            processEmptyBlock (reprepare);
            const auto prepareDuration = elapsedMs ([&]
            {
                reprepare.setRateAndBufferSizeDetails (96000.0, 128);
                reprepare.prepareToPlay (96000.0, 128);
            });
            expectLessThan ((int) prepareDuration, 500);
            logMessage ("Phase5 lifecycle ms: preparing=" + juce::String ((int) preparing)
                        + " capturing=" + juce::String ((int) captureCancel)
                        + " stopping=" + juce::String ((int) stoppingCancel)
                        + " reprepare=" + juce::String ((int) prepareDuration));
        }

        beginTest ("Timed Learn-worker waits fall back safely on non-audio lifecycle paths");
        {
            auto startPausedFinalization = [this] (KickLockAudioProcessor& p)
            {
                p.setRateAndBufferSizeDetails (kSampleRate, 128);
                p.prepareToPlay (kSampleRate, 128);
                const auto pause = p.getLearnWorkerPauseControlForTesting();
                p.setLearnWorkerPauseStateForTesting (LearnState::Finalizing, true);
                expect (p.beginLearn());
                processEmptyBlock (p);
                expect (p.stopLearn());
                expect (pause->waitUntilEntered (500));
                return pause;
            };

            {
                auto p = std::make_unique<KickLockAudioProcessor>();
                const auto pause = startPausedFinalization (*p);
                std::atomic<bool> destroyed { false };
                std::thread destroyThread ([owned = std::move (p), &destroyed] () mutable
                {
                    owned.reset();
                    destroyed.store (true, std::memory_order_release);
                });
                juce::Thread::sleep (1600);
                expect (! destroyed.load (std::memory_order_acquire));
                pause->release();
                destroyThread.join();
                expect (destroyed.load (std::memory_order_acquire));
            }

            {
                KickLockAudioProcessor p;
                const auto pause = startPausedFinalization (p);
                std::atomic<bool> prepared { false };
                std::thread prepareThread ([&]
                {
                    p.setRateAndBufferSizeDetails (96000.0, 128);
                    p.prepareToPlay (96000.0, 128);
                    prepared.store (true, std::memory_order_release);
                });
                juce::Thread::sleep (1600);
                expect (! prepared.load (std::memory_order_acquire));
                pause->release();
                prepareThread.join();
                expect (prepared.load (std::memory_order_acquire));
                expectEquals (p.getLatencySamples(), (int) std::ceil (96000.0 * 0.020));
                processEmptyBlock (p);
            }

            {
                KickLockAudioProcessor p;
                const auto pause = startPausedFinalization (p);
                std::atomic<bool> cancelled { false };
                std::thread cancelThread ([&]
                {
                    p.cancelLearn();
                    cancelled.store (true, std::memory_order_release);
                });
                juce::Thread::sleep (1600);
                expect (! cancelled.load (std::memory_order_acquire));
                pause->release();
                cancelThread.join();
                expect (cancelled.load (std::memory_order_acquire));
                expectEquals ((int) p.getLearnState(), (int) LearnState::Idle);
                expect (! p.hasPendingLearnResult());
                expect (! p.hasValidNoteMap());
            }

            {
                KickLockAudioProcessor stateSource;
                juce::MemoryBlock state;
                stateSource.getStateInformation (state);

                KickLockAudioProcessor p;
                const auto pause = startPausedFinalization (p);
                std::atomic<bool> restored { false };
                std::thread restoreThread ([&]
                {
                    p.setStateInformation (state.getData(), (int) state.getSize());
                    restored.store (true, std::memory_order_release);
                });
                juce::Thread::sleep (1600);
                expect (! restored.load (std::memory_order_acquire));
                pause->release();
                restoreThread.join();
                expect (restored.load (std::memory_order_acquire));
                expectEquals ((int) p.getLearnState(), (int) LearnState::Idle);
                expect (! p.hasPendingLearnResult());
            }
        }

        beginTest ("Stop during a hit finishes that window, blocks new triggers, and preserves sequence order");
        {
            LearnHitQueue queue;
            queue.prepare (1000.0, 4, 2.0f, 8.0f);
            for (int i = 0; i < 2; ++i) queue.pushSample ((float) i, (float) i, false, 55.0f);
            queue.pushSample (2.0f, 2.0f, true, 55.0f);
            queue.stopAcceptingNewHits();
            for (int i = 3; i < 12; ++i) queue.pushSample ((float) i, (float) i, i == 5, 55.0f);
            for (int i = 12; i < 24; ++i) queue.pushSample ((float) i, (float) i, i == 14, 55.0f);
            expectEquals (queue.getAcceptedHitCount(), 1);
            expect (! queue.hasInProgressCapture());
            LearnHitWindow window;
            expect (queue.pop (window));
            expectEquals (window.sequence, 0);
            expect (! queue.pop (window));
        }

        beginTest ("Stale session cannot apply over a newer session");
        {
            KickLockAudioProcessor p;
            p.setRateAndBufferSizeDetails (kSampleRate, 128);
            p.prepareToPlay (kSampleRate, 128);
            p.setPendingLearnResultForTesting (makeResult(), matchingContext (p));
            const auto current = p.getLearnSessionIdForTesting();
            auto staleContext = matchingContext (p);
            staleContext.sessionId = current + 100;
            p.setPendingLearnResultForTesting (makeResult(), staleContext);
            expect (! p.applyLatestLearnResult());
            expect (p.hasPendingLearnResult());
            expect (! p.hasValidNoteMap());
        }

        beginTest ("Apply, Clear, Revert, and restore each reach the audio-owned map");
        {
            KickLockAudioProcessor p;
            p.setRateAndBufferSizeDetails (kSampleRate, 128);
            p.prepareToPlay (kSampleRate, 128);
            p.setPendingLearnResultForTesting (makeResult(), matchingContext (p));
            expect (p.applyLatestLearnResult());
            processEmptyBlock (p);
            expect (p.getActiveNoteMapForTesting().valid);
            juce::MemoryBlock appliedState;
            p.getStateInformation (appliedState);

            KickLockAudioProcessor restored;
            restored.setRateAndBufferSizeDetails (kSampleRate, 128);
            restored.prepareToPlay (kSampleRate, 128);
            restored.setStateInformation (appliedState.getData(), (int) appliedState.getSize());
            processEmptyBlock (restored);
            expect (restored.getActiveNoteMapForTesting().valid);
            expect (restored.clearNoteMap());
            processEmptyBlock (restored);
            expect (! restored.getActiveNoteMapForTesting().valid);
            expect (restored.revertLatestFix());
            processEmptyBlock (restored);
            expect (restored.getActiveNoteMapForTesting().valid);
            expect (! restored.revertLatestFix());

            const std::array<unsigned char, 4> corrupt { 1, 2, 3, 4 };
            restored.setStateInformation (corrupt.data(), (int) corrupt.size());
            processEmptyBlock (restored);
            expect (! restored.getActiveNoteMapForTesting().valid);
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
