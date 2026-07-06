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

        for (auto& stage : stages)
            stage.reset();
    }

    void reset()
    {
        for (auto& stage : stages)
            stage.reset();
    }

    // Builds one allpass coefficients object and shares it across all 4
    // stages (only the first `activeStages` of which are actually used in
    // processSample). Resets stage state afterwards: the default-constructed
    // filters carry first-order coefficients, and the biquad assigned here
    // changes the filter order, so the state buffer must be resized to match
    // before processSample runs. All callers set parameters before streaming,
    // so clearing state here never interrupts audio.
    void setParameters (float frequencyHz, float q)
    {
        auto coefficients = juce::dsp::IIR::Coefficients<float>::makeAllPass (sampleRate, frequencyHz, q);

        for (auto& stage : stages)
        {
            stage.coefficients = coefficients;
            stage.reset();
        }
    }

    float processSample (float input)
    {
        float output = input;

        for (int i = 0; i < activeStages; ++i)
            output = stages[(size_t) i].processSample (output);

        return output;
    }

private:
    double sampleRate = 0.0;
    int activeStages = 2;

    std::array<juce::dsp::IIR::Filter<float>, 4> stages;
};
