#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>

#include "DynamicHighBandEQ.h"
#include "LinkwitzRileyCrossover.h"
#include "TransientEnvelopeFollower.h"
#include "TransientHealthMeter.h"

class MultibandPhaseCore
{
public:
    struct Params
    {
        float crossoverHz = 150.0f;
        float userDelayMs = 0.0f;
        bool polarityInvert = false;
        bool allpassEnabled = false;
        float allpassFreqHz = 50.0f;
        float allpassQ = 0.7f;
        int allpassStages = 2;
        float dynEqFreqHz = 3200.0f;
        float dynEqQ = 4.0f;
        float dynEqMaxBoostDb = 9.0f;
        float dynEqAmount = 0.0f;
        float dynEqAttackMs = 2.0f;
        float dynEqHoldMs = 18.0f;
        float dynEqReleaseMs = 80.0f;
        float dynEqTriggerRatio = 1.6f;
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
        dynEq.prepare (spec);

        lowDelay.setMaximumDelayInSamples (reportedLatency * 2 + 8);
        lowDelay.prepare (spec);
        highDelay.setMaximumDelayInSamples (reportedLatency + 8);
        highDelay.prepare (spec);

        envelope.prepare (sampleRate);
        health.prepare (sampleRate, 180.0f);

        initialiseAllpassFilters();

        smoothedCrossover.reset (sampleRate, 0.030);
        smoothedDelay.reset (sampleRate, 0.020);
        smoothedPolarity.reset (sampleRate, 0.010);
        smoothedAllpassFreq.reset (sampleRate, 0.030);
        smoothedAllpassQ.reset (sampleRate, 0.030);
        smoothedAllpassWet.reset (sampleRate, 0.010);
        smoothedDynEqFreq.reset (sampleRate, 0.030);
        smoothedDynEqQ.reset (sampleRate, 0.030);
        smoothedDynEqBoost.reset (sampleRate, 0.030);
        smoothedDynEqAmount.reset (sampleRate, 0.010);
        reset();
    }

    void reset()
    {
        crossover.reset();
        dynEq.reset();
        lowDelay.reset();
        highDelay.reset();
        envelope.reset();
        health.reset();

        for (auto& channel : allpassFilters)
            for (auto& stage : channel)
                stage.reset();

        smoothedCrossover.setCurrentAndTargetValue (150.0f);
        smoothedDelay.setCurrentAndTargetValue (0.0f);
        smoothedPolarity.setCurrentAndTargetValue (1.0f);
        smoothedAllpassFreq.setCurrentAndTargetValue (50.0f);
        smoothedAllpassQ.setCurrentAndTargetValue (0.7f);
        smoothedAllpassWet.setCurrentAndTargetValue (0.0f);
        smoothedDynEqFreq.setCurrentAndTargetValue (3200.0f);
        smoothedDynEqQ.setCurrentAndTargetValue (4.0f);
        smoothedDynEqBoost.setCurrentAndTargetValue (9.0f);
        smoothedDynEqAmount.setCurrentAndTargetValue (0.0f);
        activeStages = 2;
        lastAllpassFreq = -1.0f;
        lastAllpassQ = -1.0f;
        lastEqFreq = -1.0f;
        lastEqQ = -1.0f;
        lastEqBoost = -1.0f;
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
    const TransientHealthMeter& getHealthMeter() const noexcept { return health; }

    void process (juce::AudioBuffer<float>& main,
                  const juce::AudioBuffer<float>& sidechain,
                  const Params& params,
                  int n)
    {
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
        smoothedAllpassFreq.setTargetValue (juce::jlimit (20.0f, 500.0f, params.allpassFreqHz));
        smoothedAllpassQ.setTargetValue (juce::jlimit (0.1f, 10.0f, params.allpassQ));
        desiredAllpassWet = params.allpassEnabled ? 1.0f : 0.0f;
        smoothedDynEqFreq.setTargetValue (juce::jlimit (1000.0f, 8000.0f, params.dynEqFreqHz));
        smoothedDynEqQ.setTargetValue (juce::jlimit (0.5f, 12.0f, params.dynEqQ));
        smoothedDynEqBoost.setTargetValue (juce::jlimit (0.0f, 18.0f, params.dynEqMaxBoostDb));
        smoothedDynEqAmount.setTargetValue (juce::jlimit (0.0f, 1.0f, params.dynEqAmount));
        envelope.setWindow (params.dynEqAttackMs, params.dynEqHoldMs, params.dynEqReleaseMs);
        envelope.setTriggerRatio (params.dynEqTriggerRatio);

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
            processChunk (main, sidechain, offset, chunk);
            offset += chunk;
        }
    }

private:
    void initialiseAllpassFilters()
    {
        for (auto& channel : allpassFilters)
        {
            for (auto& stage : channel)
            {
                stage.coefficients = new juce::dsp::IIR::Coefficients<float> (1.0f, 0.0f, 1.0f, 0.0f);
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

    void updateDynamicEq (float frequencyHz, float q, float boostDb)
    {
        if (std::abs (frequencyHz - lastEqFreq) >= 0.5f)
        {
            dynEq.setFrequency (frequencyHz);
            lastEqFreq = frequencyHz;
        }
        if (std::abs (q - lastEqQ) >= 0.005f)
        {
            dynEq.setQ (q);
            lastEqQ = q;
        }
        if (std::abs (boostDb - lastEqBoost) >= 0.02f)
        {
            dynEq.setMaxBoostDb (boostDb);
            lastEqBoost = boostDb;
        }
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
                       const juce::AudioBuffer<float>& sidechain,
                       int offset,
                       int n)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            inputBuffer.copyFrom (ch, 0, main, ch, offset, n);

        const float crossoverHz = smoothedCrossover.getNextValue();
        if (smoothedCrossover.isSmoothing())
            smoothedCrossover.skip (n - 1);
        crossover.setCrossoverFrequency (crossoverHz);
        crossover.split (inputBuffer, lowBuffer, highBuffer, n);

        float prePeak = 0.0f;
        float postPeak = 0.0f;
        const int sideChannels = sidechain.getNumChannels();

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
            const float eqFreq = smoothedDynEqFreq.getNextValue();
            const float eqQ = smoothedDynEqQ.getNextValue();
            const float eqBoost = smoothedDynEqBoost.getNextValue();
            const float eqAmount = smoothedDynEqAmount.getNextValue();

            updateDynamicEq (eqFreq, eqQ, eqBoost);
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

            float kickMono = 0.0f;
            for (int ch = 0; ch < sideChannels; ++ch)
                kickMono += sidechain.getSample (ch, offset + i);
            kickMono = sideChannels > 0 ? kickMono / (float) sideChannels : 0.0f;
            const float env = envelope.processSample (kickMono);

            for (int ch = 0; ch < numChannels; ++ch)
            {
                lowDelay.pushSample (ch, lowBuffer.getSample (ch, i));
                highDelay.pushSample (ch, highBuffer.getSample (ch, i));

                const float lowDelaySamplesA = juce::jlimit (0.0f,
                                                             (float) lowDelay.getMaximumDelayInSamples(),
                                                             (float) reportedLatency + fineOffsetA);
                const float lowDelaySamplesB = juce::jlimit (0.0f,
                                                             (float) lowDelay.getMaximumDelayInSamples(),
                                                             (float) reportedLatency + fineOffsetB);
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

                const float highPre = highDelay.popSample (ch, (float) reportedLatency);
                const float highPost = dynEq.processSample (ch, highPre, env, eqAmount);

                prePeak = juce::jmax (prePeak, std::abs (highPre));
                postPeak = juce::jmax (postPeak, std::abs (highPost));
                main.setSample (ch, offset + i, low + highPost);
            }
        }

        health.pushBlock (prePeak, postPeak);
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
    float lastEqFreq = -1.0f;
    float lastEqQ = -1.0f;
    float lastEqBoost = -1.0f;
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

    juce::AudioBuffer<float> inputBuffer;
    juce::AudioBuffer<float> lowBuffer;
    juce::AudioBuffer<float> highBuffer;
    LinkwitzRileyCrossover crossover;
    TransientEnvelopeFollower envelope;
    DynamicHighBandEQ dynEq;
    TransientHealthMeter health;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> lowDelay;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> highDelay;
    std::array<std::array<juce::dsp::IIR::Filter<float>, (size_t) maxAllpassStages>, 2> allpassFilters;

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedCrossover;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedDelay;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedPolarity;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedAllpassFreq;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedAllpassQ;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedAllpassWet;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedDynEqFreq;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedDynEqQ;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedDynEqBoost;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedDynEqAmount;
};
