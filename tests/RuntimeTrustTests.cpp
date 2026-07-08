#include "TestCommon.h"

namespace
{
    float maxAdjacentDelta (const std::vector<float>& x, int start, int end)
    {
        start = std::max (1, start);
        end = std::min ((int) x.size(), end);

        float maxDelta = 0.0f;
        for (int i = start; i < end; ++i)
            maxDelta = std::max (maxDelta, std::abs (x[(size_t) i] - x[(size_t) i - 1]));

        return maxDelta;
    }

    std::vector<float> renderCoreTransition (double sampleRate,
                                             float firstDelayMs,
                                             float secondDelayMs,
                                             bool firstPolarity,
                                             bool secondPolarity,
                                             int firstStages,
                                             int secondStages)
    {
        constexpr int blockSize = 256;
        const int totalSamples = (int) std::round (sampleRate * 1.4);
        const int changeSample = (int) std::round (sampleRate * 0.65);

        MultibandPhaseCore core;
        core.prepare (sampleRate, blockSize, 1, 20.0f);

        juce::AudioBuffer<float> main (1, blockSize);
        juce::AudioBuffer<float> sidechain (1, blockSize);
        std::vector<float> output ((size_t) totalSamples, 0.0f);

        MultibandPhaseCore::Params params;
        params.crossoverHz = 150.0f;
        params.allpassEnabled = true;
        params.allpassFreqHz = 70.0f;
        params.allpassQ = 1.0f;

        for (int offset = 0; offset < totalSamples; offset += blockSize)
        {
            const int n = std::min (blockSize, totalSamples - offset);
            main.setSize (1, n, false, false, true);
            sidechain.setSize (1, n, false, false, true);
            main.clear();
            sidechain.clear();

            const bool afterChange = offset >= changeSample;
            params.userDelayMs = afterChange ? secondDelayMs : firstDelayMs;
            params.polarityInvert = afterChange ? secondPolarity : firstPolarity;
            params.allpassStages = afterChange ? secondStages : firstStages;

            for (int i = 0; i < n; ++i)
            {
                const double t = (double) (offset + i) / sampleRate;
                const float sine = (float) std::sin (kTwoPi * 50.0 * t);
                const float tail = (float) (std::sin (kTwoPi * 48.0 * t) * std::exp (-std::max (0.0, t - 0.2) * 1.5));
                main.setSample (0, i, 0.7f * sine + 0.25f * tail);
            }

            core.process (main, sidechain, params, n);

            for (int i = 0; i < n; ++i)
                output[(size_t) (offset + i)] = main.getSample (0, i);
        }

        return output;
    }

    void feedMeterFixture (KickLockAudioProcessor& processor,
                           double sampleRate,
                           bool invertLow,
                           bool applyPolarity,
                           int blockSize = 512)
    {
        processor.enableAllBuses();
        processor.setRateAndBufferSizeDetails (sampleRate, blockSize);
        processor.prepareToPlay (sampleRate, blockSize);
        if (auto* p = processor.apvts.getParameter ("crossover_enable")) p->setValueNotifyingHost (0.0f);

        if (auto* param = processor.apvts.getParameter ("polarity_invert"))
            param->setValueNotifyingHost (applyPolarity ? 1.0f : 0.0f);

        const int totalSamples = (int) std::round (sampleRate * 1.2);
        for (int offset = 0; offset < totalSamples; offset += blockSize)
        {
            const int n = std::min (blockSize, totalSamples - offset);
            juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                         processor.getTotalNumOutputChannels()),
                                             n);
            buffer.clear();

            for (int i = 0; i < n; ++i)
            {
                const double t = (double) (offset + i) / sampleRate;
                const float low = (float) std::sin (kTwoPi * 50.0 * t);
                const float highNoise = 0.65f * (float) std::sin (kTwoPi * 5000.0 * t + 0.4);
                const float bass = (invertLow ? -low : low) + highNoise;
                const float kick = low;

                buffer.setSample (0, i, bass);
                if (buffer.getNumChannels() > 1) buffer.setSample (1, i, bass);
                if (buffer.getNumChannels() > 2) buffer.setSample (2, i, kick);
                if (buffer.getNumChannels() > 3) buffer.setSample (3, i, kick);
            }

            juce::MidiBuffer midi;
            processor.processBlock (buffer, midi);
        }
    }

    void makeParityFixture (std::vector<float>& bass,
                            std::vector<float>& kick,
                            double sampleRate,
                            int numSamples)
    {
        bass.assign ((size_t) numSamples, 0.0f);
        kick.assign ((size_t) numSamples, 0.0f);

        const float delaySamples = (float) (sampleRate * 0.0025);
        for (int i = 0; i < numSamples; ++i)
        {
            const double t = (double) i / sampleRate;
            kick[(size_t) i] = (float) (0.55 * std::sin (kTwoPi * 55.0 * t)
                                      + 0.22 * std::sin (kTwoPi * 82.0 * t + 0.3));

            const double delayed = (double) i - (double) delaySamples;
            if (delayed >= 0.0)
            {
                const double td = delayed / sampleRate;
                bass[(size_t) i] = (float) -(0.55 * std::sin (kTwoPi * 55.0 * td)
                                           + 0.22 * std::sin (kTwoPi * 82.0 * td + 0.3));
            }
        }
    }

    void setProcessorFix (KickLockAudioProcessor& processor, const PhaseFixRenderSettings& settings)
    {
        if (auto* p = processor.apvts.getParameter ("delay_ms"))
            p->setValueNotifyingHost (p->convertTo0to1 (settings.bassDelayMs));
        if (auto* p = processor.apvts.getParameter ("polarity_invert"))
            p->setValueNotifyingHost (settings.bassPolarityInvert ? 1.0f : 0.0f);
        if (auto* p = processor.apvts.getParameter ("allpass_enable"))
            p->setValueNotifyingHost (settings.phaseFilterEnabled ? 1.0f : 0.0f);
        if (auto* p = processor.apvts.getParameter ("allpass_freq"))
            p->setValueNotifyingHost (p->convertTo0to1 (settings.phaseFilterFreqHz));
        if (auto* p = processor.apvts.getParameter ("rotatorQ"))
            p->setValueNotifyingHost (p->convertTo0to1 (settings.phaseFilterQ));
        if (auto* p = processor.apvts.getParameter ("rotatorStages"))
            p->setValueNotifyingHost (p->convertTo0to1 ((float) juce::jlimit (0, 2, settings.phaseFilterStages - 2)));
        if (auto* p = processor.apvts.getParameter ("crossover_enable"))
            p->setValueNotifyingHost (1.0f);
    }

    std::vector<float> renderRuntime (const std::vector<float>& bass,
                                      const std::vector<float>& kick,
                                      double sampleRate,
                                      int blockSize,
                                      const PhaseFixRenderSettings& settings,
                                      int& latencyOut)
    {
        KickLockAudioProcessor processor;
        processor.enableAllBuses();
        processor.setRateAndBufferSizeDetails (sampleRate, blockSize);
        processor.prepareToPlay (sampleRate, blockSize);
        setProcessorFix (processor, settings);
        latencyOut = processor.getLatencySamples();

        std::vector<float> out (bass.size(), 0.0f);
        for (int offset = 0; offset < (int) bass.size(); offset += blockSize)
        {
            const int n = std::min (blockSize, (int) bass.size() - offset);
            juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                         processor.getTotalNumOutputChannels()),
                                             n);
            buffer.clear();

            for (int i = 0; i < n; ++i)
            {
                const int index = offset + i;
                buffer.setSample (0, i, bass[(size_t) index]);
                if (buffer.getNumChannels() > 1) buffer.setSample (1, i, bass[(size_t) index]);
                if (buffer.getNumChannels() > 2) buffer.setSample (2, i, kick[(size_t) index]);
                if (buffer.getNumChannels() > 3) buffer.setSample (3, i, kick[(size_t) index]);
            }

            juce::MidiBuffer midi;
            processor.processBlock (buffer, midi);

            for (int i = 0; i < n; ++i)
                out[(size_t) (offset + i)] = buffer.getSample (0, i);
        }

        return out;
    }

    std::vector<float> extractLowBand (const std::vector<float>& in,
                                       double sampleRate,
                                       float crossoverHz = 150.0f)
    {
        juce::AudioBuffer<float> input (1, (int) in.size());
        juce::AudioBuffer<float> low (1, (int) in.size());
        juce::AudioBuffer<float> high (1, (int) in.size());

        for (int i = 0; i < (int) in.size(); ++i)
            input.setSample (0, i, in[(size_t) i]);

        LinkwitzRileyCrossover crossover;
        crossover.prepare ({ sampleRate, (juce::uint32) juce::jmax (1, (int) in.size()), 1 });
        crossover.setCrossoverFrequency (crossoverHz);
        crossover.split (input, low, high, (int) in.size());

        std::vector<float> out (in.size(), 0.0f);
        for (int i = 0; i < (int) out.size(); ++i)
            out[(size_t) i] = low.getSample (0, i);
        return out;
    }

    float rmsDbDifference (const std::vector<float>& a,
                           int aStart,
                           const std::vector<float>& b,
                           int bStart,
                           int n)
    {
        double err = 0.0;
        double ref = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const float av = a[(size_t) (aStart + i)];
            const float bv = b[(size_t) (bStart + i)];
            const double d = (double) av - (double) bv;
            err += d * d;
            ref += (double) av * (double) av;
        }

        return 10.0f * std::log10 ((float) ((err + 1.0e-20) / (ref + 1.0e-20)));
    }
}

class RuntimeTrustTests : public juce::UnitTest
{
public:
    RuntimeTrustTests() : juce::UnitTest ("RuntimeTrust", "Processor") {}

    void runTest() override
    {
        beginTest ("Large delay and polarity Apply-style jump has no click-sized discontinuity");
        {
            const auto out = renderCoreTransition (kSampleRate, 0.0f, 6.0f, false, true, 2, 2);
            const int change = (int) std::round (kSampleRate * 0.65);

            expectLessThan (maxAdjacentDelta (out, change - 64, change + 4096), 0.12f);
        }

        beginTest ("Allpass stage change is sequenced without click-sized discontinuity");
        {
            const auto out = renderCoreTransition (kSampleRate, 0.0f, 0.0f, false, false, 2, 3);
            const int change = (int) std::round (kSampleRate * 0.65);

            expectLessThan (maxAdjacentDelta (out, change - 64, change + 4096), 0.12f);
        }

        beginTest ("Headline alignment follows low end despite high-frequency bass noise");
        {
            std::array<float, 4> readings {};
            int index = 0;
            for (double sr : { 44100.0, 48000.0, 88200.0, 96000.0 })
            {
                KickLockAudioProcessor processor;
                feedMeterFixture (processor, sr, false, false);

                const float headline = processor.correlationPercent.load();
                readings[(size_t) index++] = headline;
                expectGreaterThan (headline, 85.0f, "sr=" + juce::String (sr, 0));
            }

            const auto [minIt, maxIt] = std::minmax_element (readings.begin(), readings.end());
            expectLessThan (*maxIt - *minIt, 2.0f);
        }

        beginTest ("Headline follows processed low-end improvement, not broadband noise");
        {
            KickLockAudioProcessor dry;
            feedMeterFixture (dry, kSampleRate, true, false);

            KickLockAudioProcessor fixed;
            feedMeterFixture (fixed, kSampleRate, true, true);

            expectLessThan (dry.correlationPercent.load(), 25.0f);
            expectGreaterThan (fixed.correlationPercent.load(), 75.0f);
        }

        beginTest ("Changing crossover after Analyze invalidates Apply");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 2048);
            processor.prepareToPlay (kSampleRate, 2048);
            if (auto* p = processor.apvts.getParameter ("crossover_enable")) p->setValueNotifyingHost (0.0f);

            const std::vector<int> bassDelays (4, 0);
            const std::vector<int> kickDelays (4, 120);
            const std::vector<bool> invertBass (4, false);
            feedHitSeries (processor, bassDelays, kickDelays, invertBass, 2048);

            const auto fix = processor.analyzeFix();
            expect (fix.applyAllowed || fix.optionalApplyAllowed, fix.message);

            if (auto* param = processor.apvts.getParameter ("crossover_freq"))
                param->setValueNotifyingHost (param->convertTo0to1 (260.0f));

            expect (! processor.applyLatestFix());
            expect (processor.getLatestFixResult().verificationWarning);
        }

        beginTest ("Offline render matches runtime low-band Apply path after latency alignment");
        {
            constexpr int numSamples = 96000;
            std::vector<float> bass, kick;
            makeParityFixture (bass, kick, kSampleRate, numSamples);

            PhaseFixRenderSettings settings;
            settings.bassPolarityInvert = true;
            settings.bassDelayMs = -2.5f;
            settings.phaseFilterEnabled = true;
            settings.phaseFilterFreqHz = 80.0f;
            settings.phaseFilterQ = 1.0f;
            settings.phaseFilterStages = 2;
            settings.delayInterpolation = InterpolationType::Linear;

            const auto rawBassLow = extractLowBand (bass, kSampleRate);
            std::vector<float> predictedCoreLow;
            PhaseFixEngine::renderCandidate (rawBassLow.data(), (int) rawBassLow.size(), kSampleRate,
                                             settings, predictedCoreLow);

            int latency = 0;
            const auto runtime = renderRuntime (bass, kick, kSampleRate, 512, settings, latency);
            const auto predictedLow = extractLowBand (predictedCoreLow, kSampleRate);
            const auto runtimeLow = extractLowBand (runtime, kSampleRate);

            const int analysisStart = 16000;
            const int analysisLength = 52000;
            const int runtimeStart = analysisStart + latency;
            const int n = std::min (analysisLength, (int) runtimeLow.size() - runtimeStart);
            const float nullDb = rmsDbDifference (predictedLow, analysisStart,
                                                  runtimeLow, runtimeStart,
                                                  n);

            const auto predictedScore = MultiBandCorrelation::analyze (predictedLow.data() + analysisStart,
                                                                       kick.data() + analysisStart,
                                                                       n,
                                                                       kSampleRate);
            const auto runtimeScore = MultiBandCorrelation::analyze (runtimeLow.data() + runtimeStart,
                                                                     kick.data() + analysisStart,
                                                                     n,
                                                                     kSampleRate);
            const float scoreDelta = std::abs (predictedScore.weightedMatchPercent
                                               - runtimeScore.weightedMatchPercent);

            logMessage ("low-band parity nullDb=" + juce::String (nullDb, 2)
                        + " predictedScore=" + juce::String (predictedScore.weightedMatchPercent, 2)
                        + " runtimeScore=" + juce::String (runtimeScore.weightedMatchPercent, 2)
                        + " delta=" + juce::String (scoreDelta, 2));

            expectLessThan (nullDb, -28.0f);
            expectLessThan (scoreDelta, 3.0f);
        }
    }

private:
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
                if (buffer.getNumChannels() > 1) buffer.setSample (1, i, bass);
                if (buffer.getNumChannels() > 2) buffer.setSample (2, i, kick);
                if (buffer.getNumChannels() > 3) buffer.setSample (3, i, kick);
            }

            juce::MidiBuffer midi;
            processor.processBlock (buffer, midi);
        }
    }
};

static RuntimeTrustTests runtimeTrustTestsInstance;
