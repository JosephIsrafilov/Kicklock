#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>

#include "LinkwitzRileyCrossover.h"

class MultibandPhaseCore
{
public:
    struct Params
    {
        bool crossoverEnabled = true;
        float crossoverHz = 150.0f;
        float userDelayMs = 0.0f;
        bool polarityInvert = false;
        bool allpassEnabled = false;
        float allpassFreqHz = 50.0f;
        float allpassQ = 0.7f;
        int allpassStages = 2;
        float allpassSmoothingSeconds = 0.030f;
    };

    void prepare (double sr, int maxBlock, int channels, float latencyBudgetMs = 20.0f)
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        numChannels = juce::jlimit (1, 2, channels);
        reportedLatency = juce::jmax (0, (int) std::ceil (sampleRate * (double) latencyBudgetMs / 1000.0));
        scratchSamples = juce::jmax (1, maxBlock, 8192);

        lowBuffer.setSize (numChannels, scratchSamples, false, false, true);
        highBuffer.setSize (numChannels, scratchSamples, false, false, true);
        inputBuffer.setSize (numChannels, scratchSamples, false, false, true);

        const juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) scratchSamples, (juce::uint32) numChannels };
        crossover.prepare (spec);

        lowDelay.setMaximumDelayInSamples (reportedLatency * 2 + 8);
        lowDelay.prepare (spec);
        highDelay.setMaximumDelayInSamples (reportedLatency + 8);
        highDelay.prepare (spec);

        initialiseAllpassFilters();

        smoothedCrossover.reset (sampleRate, 0.030);
        smoothedDelay.reset (sampleRate, 0.020);
        smoothedPolarity.reset (sampleRate, 0.010);
        smoothedAllpassFreq.reset (sampleRate, 0.030);
        smoothedAllpassQ.reset (sampleRate, 0.030);
        smoothedAllpassWet.reset (sampleRate, 0.010);
        allpassSmoothingSeconds = 0.030f;
        reset();
    }

    void reset()
    {
        crossover.reset();
        lowDelay.reset();
        highDelay.reset();

        for (auto& channel : allpassFilters)
            for (auto& stage : channel)
                stage.reset();

        smoothedCrossover.setCurrentAndTargetValue (150.0f);
        smoothedDelay.setCurrentAndTargetValue (0.0f);
        smoothedPolarity.setCurrentAndTargetValue (1.0f);
        smoothedAllpassFreq.setCurrentAndTargetValue (50.0f);
        smoothedAllpassQ.setCurrentAndTargetValue (0.7f);
        smoothedAllpassWet.setCurrentAndTargetValue (0.0f);
        activeStages = 2;
        lastAllpassFreq = -1.0f;
        lastAllpassQ = -1.0f;
        delayStateInitialised = false;
        delayCrossfadeActive = false;
        delayCrossfadePosition = 0;
        delayCrossfadeSamples = juce::jmax (1, (int) std::round (sampleRate * delayCrossfadeMs / 1000.0));
        pendingStages = activeStages;
        stageSwitchPending = false;
        desiredAllpassWet = 0.0f;
        allpassCoeffUpdateCountdown = 0;
    }

    int reportLatencySamples() const noexcept { return reportedLatency; }
    float getAllpassSmoothingSecondsForTesting() const noexcept { return allpassSmoothingSeconds; }
    float getSmoothedAllpassFreqForTesting() const noexcept { return smoothedAllpassFreq.getCurrentValue(); }
    float getAllpassFreqTargetForTesting() const noexcept { return smoothedAllpassFreq.getTargetValue(); }
    float getSmoothedAllpassQForTesting() const noexcept { return smoothedAllpassQ.getCurrentValue(); }
    float getAllpassQTargetForTesting() const noexcept { return smoothedAllpassQ.getTargetValue(); }

    void process (juce::AudioBuffer<float>& main,
                  const juce::AudioBuffer<float>& sidechain,
                  const Params& params,
                  int n)
    {
        // The high band is a latency-compensated passthrough and the low-band
        // correction is fully self-contained, so the sidechain is no longer
        // read here. The parameter is kept for a stable call site.
        juce::ignoreUnused (sidechain);

        currentCrossoverEnabled = params.crossoverEnabled;
        smoothedCrossover.setTargetValue (juce::jlimit (40.0f, 500.0f, params.crossoverHz));

        const float targetDelayOffset = juce::jlimit (-(float) reportedLatency, (float) reportedLatency,
                                                      (float) (params.userDelayMs * sampleRate / 1000.0));
        if (! delayStateInitialised)
        {
            smoothedDelay.setCurrentAndTargetValue (targetDelayOffset);
            delayStateInitialised = true;
        }
        else if (! delayCrossfadeActive
                 && std::abs (targetDelayOffset - smoothedDelay.getCurrentValue()) >= (float) (sampleRate * 0.001))
        {
            delayCrossfadeActive = true;
            delayCrossfadeFrom = smoothedDelay.getCurrentValue();
            delayCrossfadeTo = targetDelayOffset;
            delayCrossfadePosition = 0;
            delayCrossfadeSamples = juce::jmax (1, (int) std::round (sampleRate * delayCrossfadeMs / 1000.0));
            smoothedDelay.setCurrentAndTargetValue (targetDelayOffset);
        }
        else if (! delayCrossfadeActive)
        {
            smoothedDelay.setTargetValue (targetDelayOffset);
        }

        smoothedPolarity.setTargetValue (params.polarityInvert ? -1.0f : 1.0f);
        setAllpassSmoothingSeconds (params.allpassSmoothingSeconds);
        smoothedAllpassFreq.setTargetValue (juce::jlimit (20.0f, 500.0f, params.allpassFreqHz));
        smoothedAllpassQ.setTargetValue (juce::jlimit (0.1f, 10.0f, params.allpassQ));
        desiredAllpassWet = params.allpassEnabled ? 1.0f : 0.0f;

        const int requestedStages = juce::jlimit (2, 4, params.allpassStages);
        if (requestedStages != activeStages && (! stageSwitchPending || requestedStages != pendingStages))
        {
            pendingStages = requestedStages;
            if (smoothedAllpassWet.getCurrentValue() <= 1.0e-4f && desiredAllpassWet <= 1.0e-4f)
                applyPendingStageCount();
            else
                stageSwitchPending = true;
        }

        if (stageSwitchPending)
        {
            if (smoothedAllpassWet.getCurrentValue() <= 1.0e-4f && ! smoothedAllpassWet.isSmoothing())
            {
                applyPendingStageCount();
                smoothedAllpassWet.setTargetValue (desiredAllpassWet);
            }
            else
            {
                smoothedAllpassWet.setTargetValue (0.0f);
            }
        }
        else
        {
            smoothedAllpassWet.setTargetValue (desiredAllpassWet);
        }

        int offset = 0;
        while (offset < n)
        {
            const int chunk = juce::jmin (scratchSamples, n - offset);
            processChunk (main, offset, chunk);
            offset += chunk;
        }
    }

private:
    void setAllpassSmoothingSeconds (float requestedSeconds)
    {
        const float seconds = std::isfinite (requestedSeconds)
                                ? juce::jlimit (0.001f, 1.0f, requestedSeconds)
                                : 0.030f;
        if (std::abs (seconds - allpassSmoothingSeconds) <= 1.0e-6f)
            return;

        const float currentFreq = smoothedAllpassFreq.getCurrentValue();
        const float targetFreq = smoothedAllpassFreq.getTargetValue();
        const float currentQ = smoothedAllpassQ.getCurrentValue();
        const float targetQ = smoothedAllpassQ.getTargetValue();
        smoothedAllpassFreq.reset (sampleRate, seconds);
        smoothedAllpassQ.reset (sampleRate, seconds);
        smoothedAllpassFreq.setCurrentAndTargetValue (currentFreq);
        smoothedAllpassQ.setCurrentAndTargetValue (currentQ);
        smoothedAllpassFreq.setTargetValue (targetFreq);
        smoothedAllpassQ.setTargetValue (targetQ);
        allpassSmoothingSeconds = seconds;
    }

    bool currentCrossoverEnabled = true;

    void initialiseAllpassFilters()
    {
        for (auto& channel : allpassFilters)
        {
            for (auto& stage : channel)
            {
                // SECOND-order identity (b = [1,0,0], a = [1,0,0]), not first-order:
                // updateAllpassCoefficients() later assigns a biquad makeAllPass set
                // on the audio thread. If the initial coefficients were first-order,
                // that assignment would grow the coefficient array AND change the
                // filter order (resizing the state buffer) — both heap allocations
                // on the audio thread the first time the phase filter engages.
                // Matching the order here means every later update is a same-size
                // element copy and the state buffer never reallocates.
                stage.coefficients = new juce::dsp::IIR::Coefficients<float> (1.0f, 0.0f, 0.0f,
                                                                              1.0f, 0.0f, 0.0f);
                stage.prepare ({ sampleRate, 1, 1 });
            }
        }
    }

    void updateAllpassCoefficients (float frequencyHz, float q)
    {
        const float freq = juce::jlimit (20.0f, 500.0f, frequencyHz);
        const float limitedQ = juce::jlimit (0.1f, 10.0f, q);
        if (std::abs (freq - lastAllpassFreq) < 0.01f && std::abs (limitedQ - lastAllpassQ) < 1.0e-4f)
            return;

        const auto coeffs = juce::dsp::IIR::ArrayCoefficients<float>::makeAllPass (sampleRate, freq, limitedQ);
        for (auto& channel : allpassFilters)
            for (auto& stage : channel)
                *stage.coefficients = coeffs;

        lastAllpassFreq = freq;
        lastAllpassQ = limitedQ;
    }

    void applyPendingStageCount()
    {
        activeStages = juce::jlimit (2, 4, pendingStages);
        for (auto& channel : allpassFilters)
            for (auto& stage : channel)
                stage.reset();
        lastAllpassFreq = -1.0f;
        lastAllpassQ = -1.0f;
        allpassCoeffUpdateCountdown = 0;
        stageSwitchPending = false;
    }

    void processChunk (juce::AudioBuffer<float>& main,
                       int offset,
                       int n)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            inputBuffer.copyFrom (ch, 0, main, ch, offset, n);

        const float crossoverHz = smoothedCrossover.getNextValue();
        if (smoothedCrossover.isSmoothing())
            smoothedCrossover.skip (n - 1);
        
        if (currentCrossoverEnabled)
        {
            // setCrossoverFrequency recomputes both LR filters' coefficients on
            // every call, so only push it when the (smoothed) value has actually
            // moved — otherwise this burns trig math on every chunk forever.
            if (std::abs (crossoverHz - crossover.getCrossoverFrequency()) > 0.01f)
                crossover.setCrossoverFrequency (crossoverHz);

            crossover.split (inputBuffer, lowBuffer, highBuffer, n);
        }
        else
        {
            for (int ch = 0; ch < numChannels; ++ch)
            {
                lowBuffer.copyFrom (ch, 0, inputBuffer, ch, 0, n);
                highBuffer.clear (ch, 0, n);
            }
        }

        // The high band carries no sidechain-reactive processing: it is a plain
        // latency-compensated passthrough (see the delay below). Only the low
        // band receives delay/polarity/phase-filter correction.

        // Channel write pointers for the outer sample loop.
        float* mainWrite[2] = {
            numChannels > 0 ? main.getWritePointer (0, offset) : nullptr,
            numChannels > 1 ? main.getWritePointer (1, offset) : nullptr
        };

        const float* lowPtr[2] = {
            numChannels > 0 ? lowBuffer.getReadPointer (0) : nullptr,
            numChannels > 1 ? lowBuffer.getReadPointer (1) : nullptr
        };
        const float* highPtr[2] = {
            numChannels > 0 ? highBuffer.getReadPointer (0) : nullptr,
            numChannels > 1 ? highBuffer.getReadPointer (1) : nullptr
        };
        const float maxLowDelay = (float) lowDelay.getMaximumDelayInSamples();
        const float reportedLat = (float) reportedLatency;

        for (int i = 0; i < n; ++i)
        {
            float fineOffset = smoothedDelay.getCurrentValue();
            float fineOffsetA = fineOffset;
            float fineOffsetB = fineOffset;
            float delayGainA = 0.0f;
            float delayGainB = 1.0f;

            if (delayCrossfadeActive)
            {
                const float denom = (float) juce::jmax (1, delayCrossfadeSamples - 1);
                const float x = (float) delayCrossfadePosition / denom;
                const float theta = x * juce::MathConstants<float>::halfPi;
                delayGainA = std::cos (theta);
                delayGainB = std::sin (theta);
                fineOffsetA = delayCrossfadeFrom;
                fineOffsetB = delayCrossfadeTo;

                if (++delayCrossfadePosition >= delayCrossfadeSamples)
                {
                    delayCrossfadeActive = false;
                    smoothedDelay.setCurrentAndTargetValue (delayCrossfadeTo);
                    fineOffset = delayCrossfadeTo;
                    fineOffsetA = fineOffsetB = fineOffset;
                    delayGainA = 0.0f;
                    delayGainB = 1.0f;
                }
            }
            else
            {
                fineOffset = smoothedDelay.getNextValue();
                fineOffsetA = fineOffsetB = fineOffset;
            }

            const float polarity = smoothedPolarity.getNextValue();
            const float allpassWet = smoothedAllpassWet.getNextValue();
            const float allpassFreq = smoothedAllpassFreq.getNextValue();
            const float allpassQ = smoothedAllpassQ.getNextValue();

            if (allpassWet > 1.0e-5f || smoothedAllpassWet.isSmoothing())
            {
                if (allpassCoeffUpdateCountdown <= 0)
                {
                    updateAllpassCoefficients (allpassFreq, allpassQ);
                    allpassCoeffUpdateCountdown = allpassCoeffUpdatePeriod;
                }
                else
                {
                    --allpassCoeffUpdateCountdown;
                }
            }
            else
            {
                allpassCoeffUpdateCountdown = 0;
            }

            const float lowDelaySamplesB = juce::jlimit (0.0f, maxLowDelay, reportedLat + fineOffsetB);
            const float lowDelaySamplesA = delayCrossfadeActive ? 
                                           juce::jlimit (0.0f, maxLowDelay, reportedLat + fineOffsetA) : 
                                           lowDelaySamplesB;

            for (int ch = 0; ch < numChannels; ++ch)
            {
                lowDelay.pushSample (ch, lowPtr[ch][i]);
                highDelay.pushSample (ch, highPtr[ch][i]);

                float low = 0.0f;
                if (delayCrossfadeActive)
                {
                    const float oldTap = lowDelay.popSample (ch, lowDelaySamplesA, false);
                    const float newTap = lowDelay.popSample (ch, lowDelaySamplesB, true);
                    low = delayGainA * oldTap + delayGainB * newTap;
                }
                else
                {
                    low = lowDelay.popSample (ch, lowDelaySamplesB);
                }

                low *= polarity;

                if (allpassWet > 1.0e-5f || smoothedAllpassWet.isSmoothing())
                {
                    const float dry = low;
                    float wet = low;
                    for (int s = 0; s < activeStages; ++s)
                        wet = allpassFilters[(size_t) ch][(size_t) s].processSample (wet);
                    low = dry + allpassWet * (wet - dry);
                }

                // The high band is a plain latency-compensated passthrough; only
                // the low band carries the delay/polarity/phase-filter correction.
                const float high = highDelay.popSample (ch, reportedLat);
                if (mainWrite[ch] != nullptr)
                    mainWrite[ch][i] = low + high;
            }
        }
    }

    static constexpr int maxAllpassStages = 4;
    static constexpr float delayCrossfadeMs = 45.0f;
    static constexpr int allpassCoeffUpdatePeriod = 16;

    double sampleRate = 44100.0;
    int numChannels = 2;
    int reportedLatency = 0;
    int scratchSamples = 1;
    int activeStages = 2;
    float lastAllpassFreq = -1.0f;
    float lastAllpassQ = -1.0f;
    bool delayStateInitialised = false;
    bool delayCrossfadeActive = false;
    float delayCrossfadeFrom = 0.0f;
    float delayCrossfadeTo = 0.0f;
    int delayCrossfadePosition = 0;
    int delayCrossfadeSamples = 1;
    int pendingStages = 2;
    bool stageSwitchPending = false;
    float desiredAllpassWet = 0.0f;
    int allpassCoeffUpdateCountdown = 0;
    float allpassSmoothingSeconds = 0.030f;

    juce::AudioBuffer<float> inputBuffer;
    juce::AudioBuffer<float> lowBuffer;
    juce::AudioBuffer<float> highBuffer;
    LinkwitzRileyCrossover crossover;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> lowDelay;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> highDelay;
    std::array<std::array<juce::dsp::IIR::Filter<float>, (size_t) maxAllpassStages>, 2> allpassFilters;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedCrossover;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedDelay;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedPolarity;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedAllpassFreq;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedAllpassQ;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedAllpassWet;
};
