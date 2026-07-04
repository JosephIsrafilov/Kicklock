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
            processor.setRateAndBufferSizeDetails (kSampleRate, 512);
            processor.prepareToPlay (kSampleRate, 512);
            expect (processor.isBassProcessingNeutral());

            juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                         processor.getTotalNumOutputChannels()),
                                             512);
            juce::MidiBuffer midi;
            fillBass (buffer, 512);

            const auto expected = copyMainChannels (buffer, 2, 512);
            processor.processBlock (buffer, midi);
            expectMainMatches (buffer, expected, 2, 512, 1.0e-7f);
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

            const auto expected = copyMainChannels (buffer, 2, 4096);
            processor.processBlock (buffer, midi);
            expectMainMatches (buffer, expected, 2, 4096, 1.0e-7f);
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
            const auto firstExpected = copyMainChannels (firstBuffer, 2, 4096);
            processor.processBlock (firstBuffer, midi);
            expectMainMatches (firstBuffer, firstExpected, 2, 4096, 1.0e-7f);

            setFloatParam (processor, "rotatorFreq", 1400.0f);
            setFloatParam (processor, "rotatorQ", 0.5f);
            setFloatParam (processor, "rotatorStages", 2.0f);

            juce::AudioBuffer<float> secondBuffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                               processor.getTotalNumOutputChannels()),
                                                   4096);
            fillBass (secondBuffer, 4096);
            const auto secondExpected = copyMainChannels (secondBuffer, 2, 4096);
            processor.processBlock (secondBuffer, midi);
            expectMainMatches (secondBuffer, secondExpected, 2, 4096, 1.0e-7f);
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

        beginTest ("Apply Fix disables stale phase filter when the new fix does not use it");
        {
            KickLockAudioProcessor processor;
            processor.setRateAndBufferSizeDetails (kSampleRate, 512);
            processor.prepareToPlay (kSampleRate, 512);

            setBoolParam (processor, "phaseFilterEnabled", true);
            setFloatParam (processor, "rotatorFreq", 90.0f);

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
            expectWithinAbsoluteError (rawParam (processor, "phaseFilterEnabled"), 0.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "polarityInvert"), 1.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "rotatorFreq"), 90.0f, 0.01f);
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

        beginTest ("Apply Fix enables phase filter and writes rotator settings when recommended");
        {
            KickLockAudioProcessor processor;
            processor.setRateAndBufferSizeDetails (kSampleRate, 512);
            processor.prepareToPlay (kSampleRate, 512);

            PhaseFixResult fix;
            fix.valid = true;
            fix.enoughSignal = true;
            fix.phaseFilterEnabled = true;
            fix.phaseFilterFreqHz = 90.0f;
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
            expectWithinAbsoluteError (rawParam (processor, "rotatorFreq"), 90.0f, 0.01f);
            expectWithinAbsoluteError (rawParam (processor, "rotatorQ"), 2.0f, 0.01f);
                expectWithinAbsoluteError (rawParam (processor, "rotatorStages"), 1.0f, 1.0e-7f);
        }

        beginTest ("Predicted after score stays close to verified after score");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 2048);
            processor.prepareToPlay (kSampleRate, 2048);

            feedHitSeries (processor, { 0, 0, 0, 0 }, { 40, 40, 40, 40 }, { false, false, false, false }, 2048);

            const auto predicted = processor.analyzeFix();
            expect (PhaseFixEngine::canApply (predicted), predicted.message);
            expect (processor.applyLatestFix());

            const auto verified = processor.getLatestFixResult();
            expectGreaterThan (verified.verifiedAfterMatchPercent, 0.0f);
            expectLessThan (verified.verificationDeltaPercent, 10.0f);
        }

        beginTest ("Multi-hit stable analysis aggregates consistent hits");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 2048);
            processor.prepareToPlay (kSampleRate, 2048);

            feedHitSeries (processor, { 0, 0, 0, 0 }, { 40, 40, 40, 40 }, { false, false, false, false }, 2048);

            const auto fix = processor.analyzeFix();
            expectGreaterThan (fix.contributingHits, 1);
            expect (! fix.unstableRecommendation);
            expect (PhaseFixEngine::canApply (fix), fix.message);
            expectGreaterThan (fix.bassDelayMs, 0.5f);
            expectLessThan (fix.bassDelayMs, 1.2f);
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

            expectWithinAbsoluteError (processor.apvts.getParameter ("delayMs")->convertFrom0to1 (0.0f),
                                       0.0f, 1.0e-7f);
            expectEquals (formatBassDelayMs (-0.0001f), juce::String ("0.00 ms"));
            expectEquals (formatBassDelayMs (-6.0f), juce::String ("0.00 ms"));

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
            expectWithinAbsoluteError (rawParam (processor, "delayMs"), 0.0f, 1.0e-7f);
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

        beginTest ("Dry and processed meters split pre and post correction");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 20000);
            processor.prepareToPlay (kSampleRate, 20000);

            feedAlignedSignal (processor, 20000);
            const float neutralDry = processor.dryInputMatchPercent.load();
            const float neutralProcessed = processor.processedMatchPercent.load();
            expectWithinAbsoluteError (neutralDry, neutralProcessed, 0.5f);
            expectWithinAbsoluteError (processor.correlationPercent.load(), neutralDry, 0.5f);

            if (auto* polarity = processor.apvts.getParameter ("polarityInvert"))
                polarity->setValueNotifyingHost (1.0f);

            feedAlignedSignal (processor, 20000);
            const float correctedDry = processor.dryInputMatchPercent.load();
            const float correctedProcessed = processor.processedMatchPercent.load();

            expectGreaterThan (correctedDry, 80.0f);
            expectLessThan (correctedProcessed, correctedDry - 20.0f);
            expectWithinAbsoluteError (processor.correlationPercent.load(), correctedProcessed, 0.5f);
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

    static void feedHitSeries (KickLockAudioProcessor& processor,
                               const std::vector<int>& bassDelays,
                               const std::vector<int>& kickDelays,
                               const std::vector<bool>& invertBass,
                               int blockSize)
    {
        const int hitCount = std::min ({ (int) bassDelays.size(),
                                         (int) kickDelays.size(),
                                         (int) invertBass.size() });
        const int hitSpacing = 12000;
        const int eventLength = 5000;
        const int startOffset = 1500;
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
};

static KickLockProcessorTransparencyTests kickLockProcessorTransparencyTestsInstance;

