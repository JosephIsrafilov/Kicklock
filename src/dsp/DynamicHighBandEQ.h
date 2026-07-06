#pragma once

#include <JuceHeader.h>
#include <array>
#include <cmath>

class DynamicHighBandEQ
{
public:
    void prepare (const juce::dsp::ProcessSpec& newSpec)
    {
        spec = newSpec;
        for (auto& filter : filters)
        {
            filter.setType (juce::dsp::StateVariableTPTFilterType::bandpass);
            filter.prepare (spec);
        }
        setFrequency (frequencyHz);
        setQ (q);
        setMaxBoostDb (maxBoostDb);
        reset();
    }

    void reset()
    {
        for (auto& filter : filters)
            filter.reset();
    }

    void setFrequency (float hz)
    {
        const float maxHz = spec.sampleRate > 0.0 ? (float) (spec.sampleRate * 0.45) : 8000.0f;
        frequencyHz = juce::jlimit (500.0f, maxHz, hz);
        for (auto& filter : filters)
            filter.setCutoffFrequency (frequencyHz);
    }

    void setQ (float newQ)
    {
        q = juce::jlimit (0.5f, 12.0f, newQ);
        const float resonance = juce::jlimit (0.05f, 1.0f, 1.0f / q);
        for (auto& filter : filters)
            filter.setResonance (resonance);
    }

    void setMaxBoostDb (float db) noexcept
    {
        maxBoostDb = juce::jlimit (0.0f, 24.0f, db);
        maxLinBoost = std::pow (10.0f, maxBoostDb / 20.0f) - 1.0f;
    }

    float processSample (int ch, float x, float env, float amount) noexcept
    {
        const int channel = juce::jlimit (0, (int) filters.size() - 1, ch);
        const float band = filters[(size_t) channel].processSample (channel, x);
        const float gain = juce::jlimit (0.0f, 1.0f, env) * juce::jlimit (0.0f, 1.0f, amount) * maxLinBoost;
        return x + band * gain;
    }

private:
    juce::dsp::ProcessSpec spec { 44100.0, 1, 2 };
    float frequencyHz = 3200.0f;
    float q = 4.0f;
    float maxBoostDb = 9.0f;
    float maxLinBoost = std::pow (10.0f, 9.0f / 20.0f) - 1.0f;
    std::array<juce::dsp::StateVariableTPTFilter<float>, 2> filters;
};
