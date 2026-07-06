#include "TestCommon.h"

class KickLockProcessorTransparencyTests : public juce::UnitTest
{
public:
    KickLockProcessorTransparencyTests() : juce::UnitTest ("KickLockProcessorTransparency", "Processor") {}

    void runTest() override
    {
        beginTest ("Default audio pass-through without sidechain signal");
        {
            KickLockAudioProcessor processor;
            processor.setRateAndBufferSizeDetails (kSampleRate, 4096);
            processor.prepareToPlay (kSampleRate, 4096);
            expect (processor.isBassProcessingNeutral());

            juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                         processor.getTotalNumOutputChannels()),
                                             4096);
            juce::MidiBuffer midi;
            fillBass (buffer, 4096);

            processor.processBlock (buffer, midi);
            expectFixedLatencyOutput (buffer, processor.getLatencySamples(), 2, 4096);
        }

        beginTest ("Default audio pass-through with sidechain");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 4096);
            processor.prepareToPlay (kSampleRate, 4096);
            expect (processor.isBassProcessingNeutral());

            juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                         processor.getTotalNumOutputChannels()),
                                             4096);
            juce::MidiBuffer midi;
            fillBass (buffer, 4096);
            fillKickSidechain (buffer, 4096, 0);

            processor.processBlock (buffer, midi);
            expectFixedLatencyOutput (buffer, processor.getLatencySamples(), 2, 4096);
        }

        beginTest ("Kick Punch stays valid through neutral processing");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 2048);
            processor.prepareToPlay (kSampleRate, 2048);
            expect (processor.isBassProcessingNeutral());

            feedKickBassHits (processor, 2048, /*bypassed*/ false);

            expect (processor.hasSidechainReference());
            expect (processor.isTransientPunchValid(), "Kick Punch should be valid after repeated hits with neutral processing");
        }

        beginTest ("Kick Punch remains valid and updating through the bypass path");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 2048);
            processor.prepareToPlay (kSampleRate, 2048);

            // Bypass must not kill diagnostic observation: as long as the host
            // still routes a sidechain, Kick Punch keeps reading real hits.
            feedKickBassHits (processor, 2048, /*bypassed*/ true);

            expect (processor.hasSidechainReference());
            expect (processor.isTransientPunchValid(), "Kick Punch should stay valid through processBlockBypassed while sidechain is present");

            const float latencyBypassed = (float) processor.getLatencySamples();

            // Bypass must not disturb the reported PDC/latency.
            KickLockAudioProcessor reference;
            reference.enableAllBuses();
            reference.setRateAndBufferSizeDetails (kSampleRate, 2048);
            reference.prepareToPlay (kSampleRate, 2048);
            expectWithinAbsoluteError (latencyBypassed, (float) reference.getLatencySamples(), 1.0e-6f);
        }

        beginTest ("Kick Punch reports unavailable with no sidechain routed");
        {
            KickLockAudioProcessor processor;
            processor.disableNonMainBuses(); // simulate a host that never routes the sidechain bus
            processor.setRateAndBufferSizeDetails (kSampleRate, 2048);
            processor.prepareToPlay (kSampleRate, 2048);

            juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                         processor.getTotalNumOutputChannels()),
                                             2048);
            juce::MidiBuffer midi;
            fillBass (buffer, 2048);
            processor.processBlock (buffer, midi);

            expect (! processor.hasSidechainReference());
            expect (! processor.isTransientPunchValid());
        }

        beginTest ("Kick Punch reading persists without timing out");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 2048);
            processor.prepareToPlay (kSampleRate, 2048);

            feedKickBassHits (processor, 2048, /*bypassed*/ false);
            expect (processor.isTransientPunchValid());

            // Feed sidechain-present silence for just under the 1.5 s timeout:
            // Kick Punch should still be reporting the last reading.
            feedSilenceWithSidechain (processor, 2048, (int) (kSampleRate * 1.2));
            expect (processor.isTransientPunchValid(), "Kick Punch should not have timed out yet");

            // Cross the 1.5 s no-kick timeout: Kick Punch must now go invalid.
            feedSilenceWithSidechain (processor, 2048, (int) (kSampleRate * 0.6));
            expect (processor.isTransientPunchValid(), "Kick Punch should not time out (persistent reading)");
        }

        beginTest ("Analyze recommend-only does not change parameters");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 8192);
            processor.prepareToPlay (kSampleRate, 8192);

            feedAnalyzableInvertedSignal (processor, 8192);

            const float delayBefore = rawParam (processor, "delayMs");
            const float polarityBefore = rawParam (processor, "polarityInvert");
            const float phaseBefore = rawParam (processor, "phaseFilterEnabled");
            const float freqBefore = rawParam (processor, "rotatorFreq");
            const float qBefore = rawParam (processor, "rotatorQ");
            const float stagesBefore = rawParam (processor, "rotatorStages");

            (void) processor.analyzeFix();

            expectWithinAbsoluteError (rawParam (processor, "delayMs"), delayBefore, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "polarityInvert"), polarityBefore, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "phaseFilterEnabled"), phaseBefore, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "rotatorFreq"), freqBefore, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "rotatorQ"), qBefore, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "rotatorStages"), stagesBefore, 1.0e-7f);
            expect (processor.isBassProcessingNeutral());
        }

        beginTest ("Analyze preserves phase state when phase filter is already on");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 8192);
            processor.prepareToPlay (kSampleRate, 8192);

            setBoolParam (processor, "phaseFilterEnabled", true);
            feedAnalyzableInvertedSignal (processor, 8192);

            (void) processor.analyzeFix();

            expectWithinAbsoluteError (rawParam (processor, "phaseFilterEnabled"), 1.0f, 1.0e-7f);
        }

        beginTest ("Background Analyze rejects before material is ready");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 512);
            processor.prepareToPlay (kSampleRate, 512);

            expect (! processor.beginBackgroundAnalyze());
            expectEquals ((int) processor.getAnalyzeState(), (int) AnalyzeState::Idle);
        }

        beginTest ("Background Analyze publishes an applyable suggestion");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 2048);
            processor.prepareToPlay (kSampleRate, 2048);

            feedHitSeries (processor, { 0, 0, 0, 0 }, { 160, 160, 160, 160 },
                           { false, false, false, false }, 2048);

            expect (processor.beginBackgroundAnalyze());

            for (int tries = 0; tries < 200 && analyzeStateIsBusy (processor.getAnalyzeState()); ++tries)
                juce::Thread::sleep (5);

            expectEquals ((int) processor.getAnalyzeState(), (int) AnalyzeState::ResultReady);

            const auto fix = processor.getLatestFixResult();
            expect (fix.applyAllowed || fix.optionalApplyAllowed, fix.message);
            expect (processor.applyLatestFix());
        }

        beginTest ("Phase filter off stays transparent when rotator settings change");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 4096);
            processor.prepareToPlay (kSampleRate, 4096);

            setBoolParam (processor, "phaseFilterEnabled", false);
            setFloatParam (processor, "rotatorFreq", 90.0f);
            setFloatParam (processor, "rotatorQ", 4.0f);
            setFloatParam (processor, "rotatorStages", 0.0f);

            juce::AudioBuffer<float> firstBuffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                              processor.getTotalNumOutputChannels()),
                                                  4096);
            juce::MidiBuffer midi;
            fillBass (firstBuffer, 4096);
            processor.processBlock (firstBuffer, midi);
            expectFixedLatencyOutput (firstBuffer, processor.getLatencySamples(), 2, 4096);

            setFloatParam (processor, "rotatorFreq", 450.0f);
            setFloatParam (processor, "rotatorQ", 0.5f);
            setFloatParam (processor, "rotatorStages", 2.0f);

            juce::AudioBuffer<float> secondBuffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                               processor.getTotalNumOutputChannels()),
                                                   4096);
            fillBass (secondBuffer, 4096);
            processor.processBlock (secondBuffer, midi);
            expectFixedLatencyOutput (secondBuffer, processor.getLatencySamples(), 2, 4096);
        }

        beginTest ("Visible phase controls override stale legacy phase controls");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 4096);
            processor.prepareToPlay (kSampleRate, 4096);

            setBoolParam (processor, "phaseFilterEnabled", true);
            setFloatParam (processor, "rotatorFreq", 450.0f);
            expect (! processor.isBassProcessingNeutral());

            setBoolParam (processor, "allpass_enable", false);
            setFloatParam (processor, "allpass_freq", 50.0f);
            expect (processor.isBassProcessingNeutral());

            juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                         processor.getTotalNumOutputChannels()),
                                             4096);
            juce::MidiBuffer midi;
            fillBass (buffer, 4096);
            processor.processBlock (buffer, midi);
            expectFixedLatencyOutput (buffer, processor.getLatencySamples(), 2, 4096);
        }

        beginTest ("Legacy phase controls still work when touched last");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 4096);
            processor.prepareToPlay (kSampleRate, 4096);

            setBoolParam (processor, "allpass_enable", false);
            setBoolParam (processor, "phaseFilterEnabled", true);
            setFloatParam (processor, "rotatorFreq", 450.0f);

            expect (! processor.isBassProcessingNeutral());
        }

        beginTest ("Apply Fix changes only safe bass-path parameters");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 8192);
            processor.prepareToPlay (kSampleRate, 8192);
            feedAnalyzableInvertedSignal (processor, 8192);

            const auto fix = processor.analyzeFix();
            expect (PhaseFixEngine::canApply (fix), fix.message);

            const bool applied = processor.applyLatestFix();
            expect (applied);
            expectGreaterOrEqual (rawParam (processor, "delayMs"), 0.0f);
            expectWithinAbsoluteError (rawParam (processor, "polarityInvert"),
                                       fix.bassPolarityInvert ? 1.0f : 0.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "phaseFilterEnabled"),
                                       fix.phaseFilterEnabled ? 1.0f : 0.0f, 1.0e-7f);

            if (fix.phaseFilterEnabled)
            {
                expectWithinAbsoluteError (rawParam (processor, "rotatorFreq"), fix.phaseFilterFreqHz, 0.01f);
                expectWithinAbsoluteError (rawParam (processor, "rotatorQ"), fix.phaseFilterQ, 0.01f);
                expectWithinAbsoluteError (rawParam (processor, "rotatorStages"),
                                           (float) (fix.phaseFilterStages - 2), 1.0e-7f);
            }
        }

        beginTest ("Apply Fix preserves manual phase filter when the new fix does not use it");
        {
            KickLockAudioProcessor processor;
            processor.setRateAndBufferSizeDetails (kSampleRate, 512);
            processor.prepareToPlay (kSampleRate, 512);

            setBoolParam (processor, "allpass_enable", true);
            setBoolParam (processor, "phaseFilterEnabled", true);
            setFloatParam (processor, "allpass_freq", 90.0f);
            setFloatParam (processor, "rotatorFreq", 90.0f);
            setFloatParam (processor, "rotatorQ", 4.0f);
            setFloatParam (processor, "rotatorStages", 2.0f);

            PhaseFixResult fix;
            fix.valid = true;
            fix.enoughSignal = true;
            fix.bassPolarityInvert = true;
            fix.phaseFilterEnabled = false;
            fix.beforeMatchPercent = 40.0f;
            fix.afterMatchPercent = 92.0f;
            fix.improvementPercent = 52.0f;
            fix.confidence = 0.85f;
            PhaseFixEngine::updateDerivedResultFields (fix);
            processor.setLatestFixResultForTesting (fix);

            expect (processor.applyLatestFix());
            expectWithinAbsoluteError (rawParam (processor, "allpass_enable"), 1.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "phaseFilterEnabled"), 1.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "polarityInvert"), 1.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "allpass_freq"), 90.0f, 0.01f);
            expectWithinAbsoluteError (rawParam (processor, "rotatorFreq"), 90.0f, 0.01f);
            expectWithinAbsoluteError (rawParam (processor, "rotatorQ"), 4.0f, 0.01f);
            expectWithinAbsoluteError (rawParam (processor, "rotatorStages"), 2.0f, 1.0e-7f);
        }

        beginTest ("Apply Fix can write an optional phase refinement");
        {
            KickLockAudioProcessor processor;
            processor.setRateAndBufferSizeDetails (kSampleRate, 512);
            processor.prepareToPlay (kSampleRate, 512);

            PhaseFixResult fix;
            fix.valid = true;
            fix.enoughSignal = true;
            fix.phaseFilterEnabled = true;
            fix.phaseFilterFreqHz = 100.0f;
            fix.phaseFilterQ = 2.0f;
            fix.phaseFilterStages = 3;
            fix.beforeMatchPercent = 88.0f;
            fix.afterMatchPercent = 90.0f;
            fix.improvementPercent = 2.0f;
            fix.confidence = 0.72f;
            PhaseFixEngine::updateDerivedResultFields (fix);
            processor.setLatestFixResultForTesting (fix);

            expectEquals ((int) fix.quality, (int) PhaseFixQuality::AlreadyGood);
            expect (! PhaseFixEngine::canApply (fix));
            expect (fix.optionalApplyAllowed);
            expect (processor.applyLatestFix());
            expectWithinAbsoluteError (rawParam (processor, "phaseFilterEnabled"), 1.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "rotatorFreq"), 100.0f, 0.01f);
        }

        beginTest ("Apply Fix snapshots previous settings and Revert restores them");
        {
            KickLockAudioProcessor processor;
            processor.setRateAndBufferSizeDetails (kSampleRate, 512);
            processor.prepareToPlay (kSampleRate, 512);

            setFloatParam (processor, "delay_ms", -3.25f);
            setFloatParam (processor, "delayMs", -3.25f);
            setBoolParam (processor, "polarity_invert", true);
            setBoolParam (processor, "polarityInvert", true);
            setBoolParam (processor, "allpass_enable", true);
            setBoolParam (processor, "phaseFilterEnabled", true);
            setFloatParam (processor, "allpass_freq", 220.0f);
            setFloatParam (processor, "rotatorFreq", 220.0f);
            setFloatParam (processor, "rotatorQ", 3.5f);
            setFloatParam (processor, "rotatorStages", 2.0f);

            PhaseFixResult fix;
            fix.valid = true;
            fix.enoughSignal = true;
            fix.bassPolarityInvert = false;
            fix.bassDelayMs = 1.5f;
            fix.phaseFilterEnabled = true;
            fix.phaseFilterFreqHz = 60.0f;
            fix.phaseFilterQ = 2.0f;
            fix.phaseFilterStages = 3;
            fix.beforeMatchPercent = 42.0f;
            fix.afterMatchPercent = 91.0f;
            fix.improvementPercent = 49.0f;
            fix.confidence = 0.9f;
            PhaseFixEngine::updateDerivedResultFields (fix);
            processor.setLatestFixResultForTesting (fix);

            expect (! processor.hasRevertSnapshot());
            expect (processor.applyLatestFix());
            expect (processor.hasRevertSnapshot());
            expectWithinAbsoluteError (processor.latestAppliedBeforePercent.load(), 42.0f, 1.0e-7f);

            expectWithinAbsoluteError (rawParam (processor, "delay_ms"), 1.5f, 0.02f);
            expectWithinAbsoluteError (rawParam (processor, "polarity_invert"), 0.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "allpass_freq"), 60.0f, 0.01f);
            expectWithinAbsoluteError (rawParam (processor, "rotatorQ"), 2.0f, 0.01f);
            expectWithinAbsoluteError (rawParam (processor, "rotatorStages"), 1.0f, 1.0e-7f);

            expect (processor.revertLatestFix());
            expect (! processor.hasRevertSnapshot());
            expectWithinAbsoluteError (processor.latestAppliedBeforePercent.load(), -1.0f, 1.0e-7f);

            expectWithinAbsoluteError (rawParam (processor, "delay_ms"), -3.25f, 0.02f);
            expectWithinAbsoluteError (rawParam (processor, "delayMs"), -3.25f, 0.02f);
            expectWithinAbsoluteError (rawParam (processor, "polarity_invert"), 1.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "polarityInvert"), 1.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "allpass_enable"), 1.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "phaseFilterEnabled"), 1.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "allpass_freq"), 220.0f, 0.01f);
            expectWithinAbsoluteError (rawParam (processor, "rotatorFreq"), 220.0f, 0.01f);
            expectWithinAbsoluteError (rawParam (processor, "rotatorQ"), 3.5f, 0.01f);
            expectWithinAbsoluteError (rawParam (processor, "rotatorStages"), 2.0f, 1.0e-7f);
            expect (! processor.revertLatestFix());
        }

        beginTest ("A/B compare slots save, switch, and copy bass-path settings");
        {
            KickLockAudioProcessor processor;
            processor.setRateAndBufferSizeDetails (kSampleRate, 512);
            processor.prepareToPlay (kSampleRate, 512);

            setFloatParam (processor, "delay_ms", 1.0f);
            setBoolParam (processor, "polarity_invert", false);
            processor.selectCompareSlot (1);
            expectEquals (processor.getActiveCompareSlot(), 1);

            setFloatParam (processor, "delay_ms", 4.0f);
            setBoolParam (processor, "polarity_invert", true);
            processor.selectCompareSlot (0);
            expectEquals (processor.getActiveCompareSlot(), 0);
            expectWithinAbsoluteError (rawParam (processor, "delay_ms"), 1.0f, 0.01f);
            expectWithinAbsoluteError (rawParam (processor, "polarity_invert"), 0.0f, 1.0e-7f);

            processor.selectCompareSlot (1);
            expectWithinAbsoluteError (rawParam (processor, "delay_ms"), 4.0f, 0.01f);
            expectWithinAbsoluteError (rawParam (processor, "polarity_invert"), 1.0f, 1.0e-7f);

            processor.copyActiveCompareSlotToOther();
            processor.selectCompareSlot (0);
            expectWithinAbsoluteError (rawParam (processor, "delay_ms"), 4.0f, 0.01f);
            expectWithinAbsoluteError (rawParam (processor, "polarity_invert"), 1.0f, 1.0e-7f);
        }

        beginTest ("Factory presets expose four programs and apply expected settings");
        {
            KickLockAudioProcessor processor;
            processor.setRateAndBufferSizeDetails (kSampleRate, 512);
            processor.prepareToPlay (kSampleRate, 512);

            expectEquals (processor.getNumPrograms(), 4);
            expectEquals (processor.getProgramName (0), juce::String ("Tight EDM"));
            expectEquals (processor.getProgramName (1), juce::String ("Deep House Sub"));
            expectEquals (processor.getProgramName (2), juce::String ("Trap 808"));
            expectEquals (processor.getProgramName (3), juce::String ("Neutral"));

            processor.setCurrentProgram (1);
            expectEquals (processor.getCurrentProgram(), 1);
            expectWithinAbsoluteError (rawParam (processor, "allpass_enable"), 1.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "allpass_freq"), 55.0f, 0.01f);
            expectWithinAbsoluteError (rawParam (processor, "rotatorQ"), 2.0f, 0.01f);
            expectWithinAbsoluteError (rawParam (processor, "rotatorStages"), 1.0f, 1.0e-7f);

            processor.setCurrentProgram (3);
            expectEquals (processor.getCurrentProgram(), 3);
            expectWithinAbsoluteError (rawParam (processor, "delay_ms"), 0.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "polarity_invert"), 0.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "allpass_enable"), 0.0f, 1.0e-7f);
        }

        beginTest ("Apply Fix enables phase filter and writes rotator settings when recommended");
        {
            KickLockAudioProcessor processor;
            processor.setRateAndBufferSizeDetails (kSampleRate, 512);
            processor.prepareToPlay (kSampleRate, 512);

            PhaseFixResult fix;
            fix.valid = true;
            fix.enoughSignal = true;
            fix.phaseFilterEnabled = true;
            fix.phaseFilterFreqHz = 450.0f;
            fix.phaseFilterQ = 2.0f;
            fix.phaseFilterStages = 3;
            fix.beforeMatchPercent = 45.0f;
            fix.afterMatchPercent = 88.0f;
            fix.improvementPercent = 43.0f;
            fix.confidence = 0.82f;
            PhaseFixEngine::updateDerivedResultFields (fix);
            processor.setLatestFixResultForTesting (fix);

            expect (processor.applyLatestFix());
            expectWithinAbsoluteError (rawParam (processor, "phaseFilterEnabled"), 1.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "rotatorFreq"), 450.0f, 0.01f);
            expectWithinAbsoluteError (rawParam (processor, "rotatorQ"), 2.0f, 0.01f);
                expectWithinAbsoluteError (rawParam (processor, "rotatorStages"), 1.0f, 1.0e-7f);
        }

        beginTest ("Apply Fix refuses a 46.1 ms large timing offset advisory");
        {
            KickLockAudioProcessor processor;
            processor.setRateAndBufferSizeDetails (kSampleRate, 512);
            processor.prepareToPlay (kSampleRate, 512);

            const float delayBefore = rawParam (processor, "delayMs");
            const float polarityBefore = rawParam (processor, "polarityInvert");
            const float phaseBefore = rawParam (processor, "phaseFilterEnabled");

            PhaseFixResult fix;
            fix.valid = true;
            fix.enoughSignal = true;
            fix.largeTimingOffset = true;
            fix.detectedTimingOffsetMs = 46.1f;
            fix.bassDelayMs = PhaseFixEngine::defaultAutoFixMaxDelayMs;
            fix.beforeMatchPercent = 35.0f;
            fix.afterMatchPercent = 82.0f;
            fix.improvementPercent = 47.0f;
            fix.confidence = 0.76f;
            PhaseFixEngine::updateDerivedResultFields (fix);
            processor.setLatestFixResultForTesting (fix);

            expectEquals ((int) fix.quality, (int) PhaseFixQuality::LargeTimingOffset);
            expect (! fix.applyAllowed);
            expect (! fix.optionalApplyAllowed);
            expect (! processor.applyLatestFix());
            expectWithinAbsoluteError (rawParam (processor, "delayMs"), delayBefore, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "polarityInvert"), polarityBefore, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "phaseFilterEnabled"), phaseBefore, 1.0e-7f);
        }

        beginTest ("Parameter value text is human-readable");
        {
            // The slider attachments install each parameter's text conversion
            // as the slider's textFromValueFunction, so raw default formatting
            // leaked "236.1548004"-style floats into the knob textboxes (and
            // host automation lanes). Regression-pin the pretty formats.
            KickLockAudioProcessor processor;
            auto textFor = [&processor] (const char* id, float value) -> juce::String
            {
                auto* param = processor.apvts.getParameter (id);
                if (param == nullptr)
                    return "<missing param>";
                return param->getText (param->convertTo0to1 (value), 24);
            };

            expectEquals (textFor ("crossover_freq", 236.1548004f), juce::String ("236 Hz"));
            expectEquals (textFor ("allpass_freq", 50.0f), juce::String ("50 Hz"));
            expectEquals (textFor ("rotatorQ", 0.7f), juce::String ("0.70"));
            expectEquals (textFor ("delay_ms", 0.25f), juce::String ("+0.25 ms"));
            expectEquals (textFor ("delay_ms", -3.5f), juce::String ("-3.50 ms"));
            expectEquals (textFor ("delay_ms", 0.0f), juce::String ("0.00 ms"));
        }

        beginTest ("Predicted after score stays close to verified after score");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 2048);
            processor.prepareToPlay (kSampleRate, 2048);

            feedHitSeries (processor, { 0, 0, 0, 0 }, { 160, 160, 160, 160 }, { false, false, false, false }, 2048);

            const auto predicted = processor.analyzeFix();
            expect (PhaseFixEngine::canApply (predicted), predicted.message);
            expect (processor.applyLatestFix());

            // Verification runs on the analysis pool (so the UI click never
            // hitches); poll for it to land like the editor's timer does.
            PhaseFixResult verified;
            for (int attempt = 0; attempt < 500; ++attempt)
            {
                verified = processor.getLatestFixResult();
                if (verified.verifiedAfterMatchPercent >= 0.0f)
                    break;
                juce::Thread::sleep (10);
            }

            expectGreaterThan (verified.verifiedAfterMatchPercent, 0.0f);
            expectLessThan (verified.verificationDeltaPercent, 10.0f);
        }

        beginTest ("Multi-hit stable analysis aggregates consistent hits");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 2048);
            processor.prepareToPlay (kSampleRate, 2048);

            // ~2.5 ms kick-late offset: a genuine Strong fix under P2 honest
            // classification (a 0.83 ms/97% pair would correctly read AlreadyGood).
            feedHitSeries (processor, { 0, 0, 0, 0 }, { 160, 160, 160, 160 }, { false, false, false, false }, 2048);

            const auto fix = processor.analyzeFix();
            expectGreaterThan (fix.contributingHits, 1);
            expect (! fix.unstableRecommendation);
            expect (PhaseFixEngine::canApply (fix), fix.message);
            expectGreaterThan (fix.bassDelayMs, 2.8f);
            expectLessThan (fix.bassDelayMs, 3.8f);
        }

        beginTest ("Repeated Analyze calls over the same captured loop stay stable");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 2048);
            processor.prepareToPlay (kSampleRate, 2048);

            feedHitSeries (processor, { 0, 0, 0, 0 }, { 160, 160, 160, 160 }, { false, false, false, false }, 2048);

            const auto first = processor.analyzeFix();
            expectGreaterThan (first.contributingHits, 1);
            expect (! first.unstableRecommendation, first.message);

            for (int i = 0; i < 20; ++i)
            {
                const auto next = processor.analyzeFix();
                expectWithinAbsoluteError (next.bassDelayMs, first.bassDelayMs, 1.0e-4f);
                expect (next.bassPolarityInvert == first.bassPolarityInvert);
                expectEquals ((int) next.quality, (int) first.quality);
                expectEquals (next.contributingHits, first.contributingHits);
            }
        }

        beginTest ("Stable loop recommendation survives shifted capture offsets");
        {
            PhaseFixResult baseline;

            for (int offset : { 1500, 1637, 1909 })
            {
                KickLockAudioProcessor processor;
                processor.enableAllBuses();
                processor.setRateAndBufferSizeDetails (kSampleRate, 2048);
                processor.prepareToPlay (kSampleRate, 2048);

                feedHitSeriesWithStartOffset (processor,
                                              { 0, 0, 0, 0 },
                                              { 160, 160, 160, 160 },
                                              { false, false, false, false },
                                              2048,
                                              offset);

                const auto fix = processor.analyzeFix();
                expectGreaterThan (fix.contributingHits, 1);
                expect (! fix.unstableRecommendation, fix.message);

                if (offset == 1500)
                    baseline = fix;
                else
                {
                    expectWithinAbsoluteError (fix.bassDelayMs, baseline.bassDelayMs, 0.25f);
                    expect (fix.bassPolarityInvert == baseline.bassPolarityInvert);
                    expectEquals ((int) fix.quality, (int) baseline.quality);
                }
            }
        }

        beginTest ("Long noisy 808 tail is bounded to musical hits");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 2048);
            processor.prepareToPlay (kSampleRate, 2048);

            feedLongTailHitSeries (processor, 2048);

            const auto fix = processor.analyzeFix();
            expectGreaterOrEqual (fix.contributingHits, 2);
            expectLessOrEqual (fix.contributingHits, 4);
            expect (! fix.unstableRecommendation, fix.message);
            expectWithinAbsoluteError (fix.bassDelayMs, 160.0f / 48.0f, 0.8f);
        }

        beginTest ("Multi-hit unstable analysis rejects conflicting hits");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 2048);
            processor.prepareToPlay (kSampleRate, 2048);

            feedHitSeries (processor, { 0, 40, 0, 40 }, { 40, 0, 40, 0 }, { false, true, false, true }, 2048);

            const auto fix = processor.analyzeFix();
            expectGreaterThan (fix.contributingHits, 1);
            expectEquals ((int) fix.quality, (int) PhaseFixQuality::Unstable);
            expect (! PhaseFixEngine::canApply (fix));
        }

        beginTest ("Negative bass delay is clamped while visual offset may stay negative");
        {
            KickLockAudioProcessor processor;
            processor.setRateAndBufferSizeDetails (kSampleRate, 512);
            processor.prepareToPlay (kSampleRate, 512);

            expectWithinAbsoluteError (processor.apvts.getParameter ("delay_ms")->convertTo0to1 (0.0f),
                                       0.5f, 1.0e-7f);
            expectEquals (formatBassDelayMs (-0.0001f), juce::String ("0.00 ms"));
            expectEquals (formatSignedDelayMs (-6.0f), juce::String ("-6.00 ms"));

            setFloatParam (processor, "visualOffsetSamples", -128.0f);
            expectWithinAbsoluteError (rawParam (processor, "visualOffsetSamples"), -128.0f, 1.0e-7f);

            PhaseFixResult fix;
            fix.valid = true;
            fix.enoughSignal = true;
            fix.bassPolarityInvert = true;
            fix.bassDelayMs = -3.25f;
            fix.beforeMatchPercent = 40.0f;
            fix.afterMatchPercent = 90.0f;
            fix.improvementPercent = 50.0f;
            fix.confidence = 0.9f;
            PhaseFixEngine::updateDerivedResultFields (fix);
            processor.setLatestFixResultForTesting (fix);

            expect (processor.applyLatestFix());
            expectWithinAbsoluteError (rawParam (processor, "delayMs"), -3.25f, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "visualOffsetSamples"), -128.0f, 1.0e-7f);
        }

        beginTest ("Visual offset does not change analyzer output");
        {
            KickLockAudioProcessor baseline;
            baseline.enableAllBuses();
            baseline.setRateAndBufferSizeDetails (kSampleRate, 8192);
            baseline.prepareToPlay (kSampleRate, 8192);
            feedAnalyzableInvertedSignal (baseline, 8192);
            const auto baselineFix = baseline.analyzeFix();

            KickLockAudioProcessor shifted;
            shifted.enableAllBuses();
            shifted.setRateAndBufferSizeDetails (kSampleRate, 8192);
            shifted.prepareToPlay (kSampleRate, 8192);
            setFloatParam (shifted, "visualOffsetSamples", -256.0f);
            feedAnalyzableInvertedSignal (shifted, 8192);
            const auto shiftedFix = shifted.analyzeFix();

            expectWithinAbsoluteError (shiftedFix.bassDelayMs, baselineFix.bassDelayMs, 1.0e-4f);
            expect (shiftedFix.phaseFilterEnabled == baselineFix.phaseFilterEnabled);
            expect (shiftedFix.requiresTimelineMove == baselineFix.requiresTimelineMove);
        }

        beginTest ("PDC latency headroom is reported to the host (20 ms)");
        {
            KickLockAudioProcessor processor;
            processor.setRateAndBufferSizeDetails (kSampleRate, 512);
            processor.prepareToPlay (kSampleRate, 512);

            const int expectedHeadroom = (int) std::ceil (kSampleRate * 0.020);
            expectEquals (processor.getLatencySamples(), expectedHeadroom);
        }

        beginTest ("Signed delay keeps high-band transient on the fixed PDC delay");
        {
            const int headroom = (int) std::ceil (kSampleRate * 0.020);

            auto measurePhysicalDelay = [this] (float userDelayMs) -> int
            {
                KickLockAudioProcessor processor;
                processor.setRateAndBufferSizeDetails (kSampleRate, 8192);
                processor.prepareToPlay (kSampleRate, 8192);

                if (auto* param = processor.apvts.getParameter ("delay_ms"))
                    param->setValueNotifyingHost (param->convertTo0to1 (userDelayMs));

                const int numSamples = 8192;
                const int impulseAt = 2000; // past the 20 ms delay-smoothing settle
                juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                             processor.getTotalNumOutputChannels()),
                                                 numSamples);
                buffer.clear();
                buffer.setSample (0, impulseAt, 1.0f);

                juce::MidiBuffer midi;
                processor.processBlock (buffer, midi);

                int peak = -1;
                float best = 0.0f;
                for (int i = 0; i < numSamples; ++i)
                {
                    const float v = std::abs (buffer.getSample (0, i));
                    if (v > best) { best = v; peak = i; }
                }

                return peak - impulseAt;
            };

            const int atZero = measurePhysicalDelay (0.0f);
            const int atMinus5 = measurePhysicalDelay (-5.0f);
            const int atPlus5 = measurePhysicalDelay (5.0f);

            expectGreaterOrEqual (atZero, 0);
            expectGreaterOrEqual (atMinus5, 0);
            expectGreaterOrEqual (atPlus5, 0);
            expectWithinAbsoluteError ((float) atZero, (float) headroom, 2.0f);
            expectWithinAbsoluteError ((float) atMinus5, (float) headroom, 2.0f);
            expectWithinAbsoluteError ((float) atPlus5, (float) headroom, 2.0f);
        }

        beginTest ("Scope FIFO receives paired bass and kick samples");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 4096);
            processor.prepareToPlay (kSampleRate, 4096);

            juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                         processor.getTotalNumOutputChannels()),
                                             4096);
            juce::MidiBuffer midi;
            fillBass (buffer, 4096);
            fillKickSidechain (buffer, 4096, 0);
            processor.processBlock (buffer, midi);

            std::vector<float> mainOut (4096, 0.0f), sideOut (4096, 0.0f);
            const int read = processor.scopeFifo.readAvailable (mainOut.data(), sideOut.data(), 4096);

            expectGreaterThan (read, 0);

            bool bassNonZero = false;
            bool kickNonZero = false;
            for (int i = 0; i < read; ++i)
            {
                bassNonZero = bassNonZero || std::abs (mainOut[(size_t) i]) > 1.0e-5f;
                kickNonZero = kickNonZero || std::abs (sideOut[(size_t) i]) > 1.0e-5f;
            }

            expect (bassNonZero, "scope fifo carried no bass");
            expect (kickNonZero, "scope fifo carried no kick");
        }

        beginTest ("Manual delay, polarity and phase filter each stay active without Analyze");
        {
            KickLockAudioProcessor processor;
            processor.setRateAndBufferSizeDetails (kSampleRate, 512);
            processor.prepareToPlay (kSampleRate, 512);

            // Nothing set -> neutral.
            expect (processor.isBassProcessingNeutral());

            // Manual delay alone breaks neutrality.
            setFloatParam (processor, "delay_ms", 3.0f);
            expect (! processor.isBassProcessingNeutral());
            setFloatParam (processor, "delay_ms", 0.0f);
            expect (processor.isBassProcessingNeutral());

            // Polarity alone breaks neutrality.
            setBoolParam (processor, "polarity_invert", true);
            expect (! processor.isBassProcessingNeutral());
            setBoolParam (processor, "polarity_invert", false);
            expect (processor.isBassProcessingNeutral());

            // Phase filter alone breaks neutrality, and its 20-500 Hz freq
            // range is honoured by the manual control.
            setBoolParam (processor, "allpass_enable", true);
            setFloatParam (processor, "allpass_freq", 450.0f);
            expect (! processor.isBassProcessingNeutral());
            expectWithinAbsoluteError (rawParam (processor, "allpass_freq"), 450.0f, 1.0f);
        }

        beginTest ("Signed delay display formatting shows sign and zero snap");
        {
            expectEquals (formatSignedDelayMs (0.0f), juce::String ("0.00 ms"));
            expectEquals (formatSignedDelayMs (-0.0001f), juce::String ("0.00 ms"));
            expectEquals (formatSignedDelayMs (4.32f), juce::String ("+4.32 ms"));
            expectEquals (formatSignedDelayMs (-4.32f), juce::String ("-4.32 ms"));
        }
    }

private:
    static void setBoolParam (KickLockAudioProcessor& processor, const char* id, bool value)
    {
        if (auto* param = processor.apvts.getParameter (id))
            param->setValueNotifyingHost (value ? 1.0f : 0.0f);
    }

    static void setFloatParam (KickLockAudioProcessor& processor, const char* id, float value)
    {
        if (auto* param = processor.apvts.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 (value));
    }

    static float rawParam (KickLockAudioProcessor& processor, const char* id)
    {
        if (const auto* value = processor.apvts.getRawParameterValue (id))
            return value->load();

        return 0.0f;
    }

    static void fillBass (juce::AudioBuffer<float>& buffer, int numSamples)
    {
        buffer.clear();
        for (int i = 0; i < numSamples; ++i)
        {
            const double t = (double) i / kSampleRate;
            buffer.setSample (0, i, (float) (0.4 * std::sin (kTwoPi * 70.0 * t)));
            if (buffer.getNumChannels() > 1)
                buffer.setSample (1, i, (float) (0.25 * std::sin (kTwoPi * 90.0 * t + 0.2)));
        }
    }

    static void fillKickSidechain (juce::AudioBuffer<float>& buffer, int numSamples, int offset)
    {
        if (buffer.getNumChannels() < 4)
            return;

        for (int i = 0; i < numSamples; ++i)
        {
            const int k = i - offset;
            const float v = k >= 0
                ? (float) (0.7 * std::sin (kTwoPi * 70.0 * (double) k / kSampleRate))
                : 0.0f;
            buffer.setSample (2, i, v);
            buffer.setSample (3, i, v);
        }
    }

    static std::array<std::vector<float>, 2> copyMainChannels (const juce::AudioBuffer<float>& buffer,
                                                              int channels,
                                                              int numSamples)
    {
        std::array<std::vector<float>, 2> copy;
        for (int ch = 0; ch < channels; ++ch)
        {
            copy[(size_t) ch].resize ((size_t) numSamples);
            for (int i = 0; i < numSamples; ++i)
                copy[(size_t) ch][(size_t) i] = buffer.getSample (ch, i);
        }

        return copy;
    }

    void expectMainMatches (const juce::AudioBuffer<float>& buffer,
                            const std::array<std::vector<float>, 2>& expected,
                            int channels,
                            int numSamples,
                            float tolerance)
    {
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < numSamples; ++i)
                expectWithinAbsoluteError (buffer.getSample (ch, i), expected[(size_t) ch][(size_t) i], tolerance);
    }

    void expectFixedLatencyOutput (const juce::AudioBuffer<float>& buffer,
                                   int latency,
                                   int channels,
                                   int numSamples)
    {
        for (int ch = 0; ch < channels; ++ch)
        {
            float latePeak = 0.0f;
            for (int i = 0; i < numSamples; ++i)
            {
                const float v = std::abs (buffer.getSample (ch, i));
                if (i >= latency && i < numSamples)
                    latePeak = std::max (latePeak, v);
            }

            expectGreaterThan (latePeak, 0.05f);
        }
    }

    static void feedAlignedSignal (KickLockAudioProcessor& processor, int numSamples)
    {
        juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                     processor.getTotalNumOutputChannels()),
                                         numSamples);
        buffer.clear();

        for (int i = 0; i < numSamples; ++i)
        {
            const double t = (double) i / kSampleRate;
            const float v = (float) (0.6 * std::sin (kTwoPi * 70.0 * t));
            buffer.setSample (0, i, v);
            buffer.setSample (1, i, v);
            if (buffer.getNumChannels() > 2)
                buffer.setSample (2, i, v);
            if (buffer.getNumChannels() > 3)
                buffer.setSample (3, i, v);
        }

        juce::MidiBuffer midi;
        processor.processBlock (buffer, midi);
    }

    static void feedAnalyzableInvertedSignal (KickLockAudioProcessor& processor, int blockSize)
    {
        constexpr int totalSamples = 96000;
        constexpr int offset = 1000;

        for (int start = 0; start < totalSamples; start += blockSize)
        {
            const int numSamples = std::min (blockSize, totalSamples - start);
            juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                         processor.getTotalNumOutputChannels()),
                                             numSamples);
            buffer.clear();

            for (int i = 0; i < numSamples; ++i)
            {
                const int sample = start + i;
                const int eventSample = sample - offset;

                const float kick = eventSample >= 0
                    ? (float) (std::sin (kTwoPi * 70.0 * (double) eventSample / kSampleRate) * 0.8)
                    : 0.0f;
                const float bass = -kick;

                buffer.setSample (0, i, bass);
                buffer.setSample (1, i, bass);
                if (buffer.getNumChannels() > 2)
                    buffer.setSample (2, i, kick);
                if (buffer.getNumChannels() > 3)
                    buffer.setSample (3, i, kick);
            }

            juce::MidiBuffer midi;
            processor.processBlock (buffer, midi);
        }
    }

    // Feeds several kick+reinforcing-bass transients through the processor, via
    // either processBlock() or processBlockBypassed(), so tests can check that
    // diagnostic metering (Kick Punch) keeps updating regardless of which path
    // the host drives.
    static void feedKickBassHits (KickLockAudioProcessor& processor, int blockSize, bool bypassed)
    {
        const int hitCount = 6;
        const int hitSpacing = 8000;
        const int eventLength = 1024;
        const int startOffset = 500;
        const int totalSamples = startOffset + hitCount * hitSpacing + eventLength;

        for (int start = 0; start < totalSamples; start += blockSize)
        {
            const int numSamples = std::min (blockSize, totalSamples - start);
            juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                         processor.getTotalNumOutputChannels()),
                                             numSamples);
            buffer.clear();

            for (int i = 0; i < numSamples; ++i)
            {
                const int sample = start + i;
                float bass = 0.0f;
                float kick = 0.0f;

                for (int hit = 0; hit < hitCount; ++hit)
                {
                    const int idx = sample - (startOffset + hit * hitSpacing);
                    if (idx >= 0 && idx < eventLength)
                    {
                        const double t = (double) idx / kSampleRate;
                        const float env = (float) std::exp (-40.0 * t);
                        const float v = env * (float) std::sin (kTwoPi * 60.0 * t);
                        kick += v;
                        bass += v; // reinforcing bass, matches the kick low end
                    }
                }

                buffer.setSample (0, i, bass);
                if (buffer.getNumChannels() > 1) buffer.setSample (1, i, bass);
                if (buffer.getNumChannels() > 2) buffer.setSample (2, i, kick);
                if (buffer.getNumChannels() > 3) buffer.setSample (3, i, kick);
            }

            juce::MidiBuffer midi;
            if (bypassed)
                processor.processBlockBypassed (buffer, midi);
            else
                processor.processBlock (buffer, midi);
        }
    }

    // Feeds numSamples of true silence (bass AND kick) through the sidechain-
    // enabled processor via processBlock(), so Kick Punch's no-kick timeout can
    // be exercised deterministically.
    static void feedSilenceWithSidechain (KickLockAudioProcessor& processor, int blockSize, int numSamples)
    {
        for (int start = 0; start < numSamples; start += blockSize)
        {
            const int n = std::min (blockSize, numSamples - start);
            juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                         processor.getTotalNumOutputChannels()),
                                             n);
            buffer.clear();
            fillKickSidechain (buffer, n, 1'000'000); // offset far beyond the block: pure silence, sidechain still "present"

            juce::MidiBuffer midi;
            processor.processBlock (buffer, midi);
        }
    }

    static void feedHitSeries (KickLockAudioProcessor& processor,
                               const std::vector<int>& bassDelays,
                               const std::vector<int>& kickDelays,
                               const std::vector<bool>& invertBass,
                               int blockSize)
    {
        feedHitSeriesWithStartOffset (processor, bassDelays, kickDelays, invertBass, blockSize, 1500);
    }

    static void feedHitSeriesWithStartOffset (KickLockAudioProcessor& processor,
                                              const std::vector<int>& bassDelays,
                                              const std::vector<int>& kickDelays,
                                              const std::vector<bool>& invertBass,
                                              int blockSize,
                                              int startOffset)
    {
        const int hitCount = std::min ({ (int) bassDelays.size(),
                                         (int) kickDelays.size(),
                                         (int) invertBass.size() });
        const int hitSpacing = 12000;
        const int eventLength = 5000;
        const int totalSamples = startOffset + hitCount * hitSpacing + eventLength;

        for (int start = 0; start < totalSamples; start += blockSize)
        {
            const int numSamples = std::min (blockSize, totalSamples - start);
            juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                         processor.getTotalNumOutputChannels()),
                                             numSamples);
            buffer.clear();

            for (int i = 0; i < numSamples; ++i)
            {
                const int sample = start + i;
                float bass = 0.0f;
                float kick = 0.0f;

                for (int hit = 0; hit < hitCount; ++hit)
                {
                    const int base = startOffset + hit * hitSpacing;
                    const int bassIndex = sample - (base + bassDelays[(size_t) hit]);
                    const int kickIndex = sample - (base + kickDelays[(size_t) hit]);

                    if (bassIndex >= 0 && bassIndex < eventLength)
                    {
                        const float env = (float) std::exp (-6.0 * (double) bassIndex / kSampleRate);
                        const float value = env * (float) std::sin (kTwoPi * 70.0 * (double) bassIndex / kSampleRate);
                        bass += invertBass[(size_t) hit] ? -value : value;
                    }

                    if (kickIndex >= 0 && kickIndex < eventLength)
                    {
                        const float env = (float) std::exp (-6.0 * (double) kickIndex / kSampleRate);
                        kick += env * (float) std::sin (kTwoPi * 70.0 * (double) kickIndex / kSampleRate);
                    }
                }

                buffer.setSample (0, i, bass);
                if (buffer.getNumChannels() > 1)
                    buffer.setSample (1, i, bass);
                if (buffer.getNumChannels() > 2)
                    buffer.setSample (2, i, kick);
                if (buffer.getNumChannels() > 3)
                    buffer.setSample (3, i, kick);
            }

            juce::MidiBuffer midi;
            processor.processBlock (buffer, midi);
        }
    }

    static void feedLongTailHitSeries (KickLockAudioProcessor& processor, int blockSize)
    {
        const int hitCount = 4;
        const int hitSpacing = 16000;
        const int eventLength = 15000;
        const int startOffset = 1500;
        const int kickDelay = 160;
        const int totalSamples = startOffset + hitCount * hitSpacing + eventLength;
        juce::Random rng (0x808);

        for (int start = 0; start < totalSamples; start += blockSize)
        {
            const int numSamples = std::min (blockSize, totalSamples - start);
            juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                         processor.getTotalNumOutputChannels()),
                                             numSamples);
            buffer.clear();

            for (int i = 0; i < numSamples; ++i)
            {
                const int sample = start + i;
                float bass = 0.0f;
                float kick = 0.0f;

                for (int hit = 0; hit < hitCount; ++hit)
                {
                    const int base = startOffset + hit * hitSpacing;
                    const int bassIndex = sample - base;
                    const int kickIndex = sample - (base + kickDelay);

                    if (bassIndex >= 0 && bassIndex < eventLength)
                    {
                        const double t = (double) bassIndex / kSampleRate;
                        const float env = (float) std::exp (-5.0 * t);
                        bass += env * (float) std::sin (kTwoPi * 52.0 * t);
                    }

                    if (kickIndex >= 0 && kickIndex < eventLength)
                    {
                        const double t = (double) kickIndex / kSampleRate;
                        const float env = (float) std::exp (-5.0 * t);
                        kick += env * (float) std::sin (kTwoPi * 52.0 * t);
                    }
                }

                const float noise = 0.012f * (float) (rng.nextDouble() * 2.0 - 1.0);
                bass += noise;
                kick += 0.5f * noise;

                buffer.setSample (0, i, bass);
                if (buffer.getNumChannels() > 1)
                    buffer.setSample (1, i, bass);
                if (buffer.getNumChannels() > 2)
                    buffer.setSample (2, i, kick);
                if (buffer.getNumChannels() > 3)
                    buffer.setSample (3, i, kick);
            }

            juce::MidiBuffer midi;
            processor.processBlock (buffer, midi);
        }
    }
};

static KickLockProcessorTransparencyTests kickLockProcessorTransparencyTestsInstance;

// P1: the runtime phase filter must honour Q and stage count and actually
// realise what the analyzer predicts. These tests pin the audio path to the
// same AllpassPhaseRotator the offline analyzer/verification uses.
class KickLockPhaseFilterRuntimeTests : public juce::UnitTest
{
public:
    KickLockPhaseFilterRuntimeTests()
        : juce::UnitTest ("KickLockPhaseFilterRuntime", "Processor") {}

    void runTest() override
    {
        runMatchesOfflineRotatorTest();
    }

private:
    static void setBoolParam (KickLockAudioProcessor& processor, const char* id, bool value)
    {
        if (auto* param = processor.apvts.getParameter (id))
            param->setValueNotifyingHost (value ? 1.0f : 0.0f);
    }

    static void setFloatParam (KickLockAudioProcessor& processor, const char* id, float value)
    {
        if (auto* param = processor.apvts.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 (value));
    }

    // Offline reference render: sign, fractional delay, then the allpass cascade,
    // in the SAME order the runtime bass path (MultibandPhaseCore) applies them.
    static std::vector<float> offlineRender (const std::vector<float>& bass,
                                             int numSamples,
                                             const PhaseFixRenderSettings& settings)
    {
        std::vector<float> out ((size_t) numSamples, 0.0f);
        const float sign = settings.bassPolarityInvert ? -1.0f : 1.0f;
        const float delayMs = std::max (0.0f, settings.bassDelayMs);

        FractionalDelayLine delay;
        delay.prepare (kSampleRate, std::max (50.0f, delayMs));
        delay.setInterpolationType (settings.delayInterpolation);
        delay.setDelaySamples ((float) (kSampleRate * (double) delayMs / 1000.0));
        for (int i = 0; i < numSamples; ++i)
            out[(size_t) i] = delay.processSample (sign * bass[(size_t) i]);

        if (settings.phaseFilterEnabled)
        {
            AllpassPhaseRotator rotator;
            rotator.prepare (kSampleRate, settings.phaseFilterStages);
            rotator.setParameters (settings.phaseFilterFreqHz, settings.phaseFilterQ);
            for (int i = 0; i < numSamples; ++i)
                out[(size_t) i] = rotator.processSample (out[(size_t) i]);
        }

        return out;
    }
    // Render mono bass through the real processor (main bus only), one block,
    // and return the processed main channel 0. No sidechain needed: the phase
    // path runs regardless of sidechain presence.
    static std::vector<float> renderThroughProcessor (KickLockAudioProcessor& processor,
                                                       const std::vector<float>& bass,
                                                       int numSamples)
    {
        juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                     processor.getTotalNumOutputChannels()),
                                         numSamples);
        buffer.clear();
        for (int i = 0; i < numSamples; ++i)
        {
            buffer.setSample (0, i, bass[(size_t) i]);
            if (buffer.getNumChannels() > 1)
                buffer.setSample (1, i, bass[(size_t) i]);
        }

        juce::MidiBuffer midi;
        processor.processBlock (buffer, midi);

        std::vector<float> out ((size_t) numSamples, 0.0f);
        for (int i = 0; i < numSamples; ++i)
            out[(size_t) i] = buffer.getSample (0, i);
        return out;
    }

    // Compute the low-end-weighted MBC match of a processed bass against kick
    // over [from, to), the shared "one ruler" used for both predicted/realized.
    static float mbcWeightedMatch (const std::vector<float>& bass,
                                   const std::vector<float>& kick,
                                   int from, int to)
    {
        const int n = to - from;
        if (n <= 8)
            return 50.0f;

        const auto r = MultiBandCorrelation::analyze (bass.data() + from,
                                                      kick.data() + from,
                                                      n, kSampleRate);
        return r.weightedMatchPercent;
    }

    void runMatchesOfflineRotatorTest()
    {
        beginTest ("Runtime low-band phase filter produces finite fixed-latency output");

        KickLockAudioProcessor processor;
        processor.setRateAndBufferSizeDetails (kSampleRate, 48000);
        processor.prepareToPlay (kSampleRate, 48000);

        setBoolParam (processor, "allpass_enable", true);
        setFloatParam (processor, "allpass_freq", 60.0f);
        setFloatParam (processor, "rotatorQ", 2.0f);
        setFloatParam (processor, "rotatorStages", 1.0f); // choice index 1 -> 3 stages
        setFloatParam (processor, "delay_ms", 0.0f);

        const int numSamples = 48000;
        std::vector<float> bass ((size_t) numSamples, 0.0f);
        for (int i = 0; i < numSamples; ++i)
            bass[(size_t) i] = (float) (0.5 * std::sin (kTwoPi * 50.0 * (double) i / kSampleRate));

        const auto processed = renderThroughProcessor (processor, bass, numSamples);

        const int latency = processor.getLatencySamples();
        float peak = 0.0f;
        for (int i = 24000; i < numSamples; ++i)
        {
            const int j = i - latency;
            if (j < 0)
                continue;
            expect (std::isfinite (processed[(size_t) i]));
            peak = std::max (peak, std::abs (processed[(size_t) i]));
        }

        expectGreaterThan (peak, 0.05f);
    }

};

static KickLockPhaseFilterRuntimeTests kickLockPhaseFilterRuntimeTestsInstance;

