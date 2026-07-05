#pragma once

#include <JuceHeader.h>

class LinkwitzRileyCrossover
{
public:
    void prepare (const juce::dsp::ProcessSpec& newSpec)
    {
        spec = newSpec;
        lowpass.setType (juce::dsp::LinkwitzRileyFilterType::lowpass);
        highpass.setType (juce::dsp::LinkwitzRileyFilterType::highpass);
        lowpass.prepare (spec);
        highpass.prepare (spec);
        setCrossoverFrequency (crossoverHz);
        reset();
    }

    void reset()
    {
        lowpass.reset();
        highpass.reset();
    }

    void setCrossoverFrequency (float hz)
    {
        const auto upper = spec.sampleRate > 0.0
            ? juce::jmax (20.0f, (float) (spec.sampleRate * 0.45))
            : 20000.0f;
        crossoverHz = juce::jlimit (20.0f, upper, hz);
        lowpass.setCutoffFrequency (crossoverHz);
        highpass.setCutoffFrequency (crossoverHz);
    }

    void split (const juce::AudioBuffer<float>& in,
                juce::AudioBuffer<float>& low,
                juce::AudioBuffer<float>& high,
                int n)
    {
        const int channels = juce::jmin (in.getNumChannels(), low.getNumChannels(), high.getNumChannels());
        n = juce::jlimit (0, juce::jmin (in.getNumSamples(), low.getNumSamples(), high.getNumSamples()), n);

        for (int ch = 0; ch < channels; ++ch)
        {
            low.copyFrom (ch, 0, in, ch, 0, n);
            high.copyFrom (ch, 0, in, ch, 0, n);
        }

        for (int ch = channels; ch < low.getNumChannels(); ++ch)
            low.clear (ch, 0, n);
        for (int ch = channels; ch < high.getNumChannels(); ++ch)
            high.clear (ch, 0, n);

        auto lowBlock = juce::dsp::AudioBlock<float> (low).getSubBlock (0, (size_t) n);
        auto highBlock = juce::dsp::AudioBlock<float> (high).getSubBlock (0, (size_t) n);
        lowpass.process (juce::dsp::ProcessContextReplacing<float> (lowBlock));
        highpass.process (juce::dsp::ProcessContextReplacing<float> (highBlock));
    }

    float getCrossoverFrequency() const noexcept { return crossoverHz; }

private:
    juce::dsp::ProcessSpec spec { 44100.0, 1, 2 };
    float crossoverHz = 150.0f;
    juce::dsp::LinkwitzRileyFilter<float> lowpass;
    juce::dsp::LinkwitzRileyFilter<float> highpass;
};
