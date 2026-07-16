#pragma once

#include <array>
#include <algorithm>
#include <juce_dsp/juce_dsp.h>

// Cascade of up to 4 first-order/second-order allpass IIR stages
// (juce::dsp::IIR::Filter configured via makeAllPass) used to rotate
// phase without affecting magnitude response.
class AllpassPhaseRotator
{
public:
    void prepare (double newSampleRate, int numStages)
    {
        sampleRate = newSampleRate;
        activeStages = std::clamp (numStages, 2, 4);
        setRampDurationMs (3.0f);

        for (auto& stage : stages)
        {
            // Preallocate second-order storage before audio starts. Runtime
            // coefficient updates below only copy into this fixed array.
            stage.coefficients = new juce::dsp::IIR::Coefficients<float> (1.0f, 0.0f, 0.0f,
                                                                            1.0f, 0.0f, 0.0f);
            stage.prepare ({ sampleRate, 1, 1 });
            stage.reset();
        }

        parametersInitialised = false;
        rampSamplesRemaining = 0;
        coefficientUpdateCountdown = 0;
    }

    void reset()
    {
        for (auto& stage : stages)
            stage.reset();
    }

    void setRampDurationMs (float milliseconds)
    {
        const float ms = std::clamp (std::isfinite (milliseconds) ? milliseconds : 3.0f,
                                     1.0f, 5.0f);
        rampSamples = std::max (1, (int) std::lround (sampleRate * ms / 1000.0));
    }

    // The first configuration establishes filter order before streaming. Later
    // state changes preserve delay state and ramp the target parameters over
    // 1-5 ms instead of hard-resetting a live cascade.
    void setParameters (float frequencyHz, float q)
    {
        targetFrequencyHz = std::clamp (frequencyHz, 20.0f, 500.0f);
        targetQ = std::clamp (q, 0.1f, 10.0f);
        if (! parametersInitialised)
        {
            currentFrequencyHz = targetFrequencyHz;
            currentQ = targetQ;
            applyCoefficients (true);
            parametersInitialised = true;
            rampSamplesRemaining = 0;
        }
        else if (std::abs (targetFrequencyHz - currentFrequencyHz) > 1.0e-5f
                 || std::abs (targetQ - currentQ) > 1.0e-6f)
            rampSamplesRemaining = rampSamples;
    }

    float processSample (float input)
    {
        if (rampSamplesRemaining > 0)
        {
            currentFrequencyHz += (targetFrequencyHz - currentFrequencyHz)
                                / (float) rampSamplesRemaining;
            currentQ += (targetQ - currentQ) / (float) rampSamplesRemaining;
            --rampSamplesRemaining;
            if (coefficientUpdateCountdown <= 0 || rampSamplesRemaining == 0)
            {
                applyCoefficients (false);
                coefficientUpdateCountdown = kCoefficientUpdatePeriod;
            }
            else
                --coefficientUpdateCountdown;
        }

        float output = input;

        for (int i = 0; i < activeStages; ++i)
            output = stages[(size_t) i].processSample (output);

        return output;
    }

private:
    void applyCoefficients (bool resetStages)
    {
        const auto coefficients = juce::dsp::IIR::ArrayCoefficients<float>::makeAllPass (
            sampleRate, currentFrequencyHz, currentQ);
        for (auto& stage : stages)
        {
            *stage.coefficients = coefficients;
            if (resetStages)
                stage.reset();
        }
    }

    static constexpr int kCoefficientUpdatePeriod = 8;
    double sampleRate = 0.0;
    int activeStages = 2;
    float currentFrequencyHz = 50.0f;
    float currentQ = 0.7f;
    float targetFrequencyHz = 50.0f;
    float targetQ = 0.7f;
    int rampSamples = 1;
    int rampSamplesRemaining = 0;
    int coefficientUpdateCountdown = 0;
    bool parametersInitialised = false;

    std::array<juce::dsp::IIR::Filter<float>, 4> stages;
};
