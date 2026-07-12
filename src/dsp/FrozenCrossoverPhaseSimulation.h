#pragma once

#include <cmath>
#include <vector>

#include <juce_dsp/juce_dsp.h>

// The Learn queue stores raw low-band bass.  Static analysis feeds that same
// path through JUCE's Linkwitz-Riley allpass when crossover is enabled.  Keep
// this worker-only simulation deliberately small and reset it for every hit so
// one captured window can never leak filter state into another.
class FrozenCrossoverPhaseSimulation
{
public:
    static bool apply (std::vector<float>& bass, double sampleRate, float crossoverHz)
    {
        if (bass.empty() || ! (sampleRate > 0.0) || ! std::isfinite (sampleRate)
            || ! std::isfinite (crossoverHz))
            return false;

        juce::dsp::LinkwitzRileyFilter<float> filter;
        filter.setType (juce::dsp::LinkwitzRileyFilterType::allpass);
        filter.prepare ({ sampleRate, (juce::uint32) bass.size(), 1 });
        filter.setCutoffFrequency (juce::jlimit (20.0f,
                                                  (float) (sampleRate * 0.45),
                                                  crossoverHz));
        filter.reset();
        for (float& sample : bass)
            sample = filter.processSample (0, sample);
        return true;
    }
};
