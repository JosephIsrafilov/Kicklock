#pragma once

#include <algorithm>
#include <memory>
#include <vector>

#include <juce_dsp/juce_dsp.h>

class FftPlanCache
{
public:
    static juce::dsp::FFT& get (int order)
    {
        order = std::max (0, order);

        thread_local std::vector<std::unique_ptr<juce::dsp::FFT>> plans;
        if ((int) plans.size() <= order)
            plans.resize ((size_t) order + 1);

        auto& plan = plans[(size_t) order];
        if (plan == nullptr)
            plan = std::make_unique<juce::dsp::FFT> (order);

        return *plan;
    }
};
