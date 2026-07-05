#pragma once

#include <atomic>
#include <cmath>
#include <JuceHeader.h>

class TransientHealthMeter
{
public:
    void prepare (double sampleRate, float relMs)
    {
        const double samples = juce::jmax (1.0, sampleRate * (double) juce::jmax (1.0f, relMs) / 1000.0);
        releaseCoeff = (float) std::exp (-1.0 / samples);
        prePeak = 0.0f;
        postPeak = 0.0f;
        prePeakAtomic.store (0.0f);
        postPeakAtomic.store (0.0f);
        healthDbAtomic.store (0.0f);
    }

    void reset() noexcept
    {
        prePeak = 0.0f;
        postPeak = 0.0f;
        prePeakAtomic.store (0.0f, std::memory_order_relaxed);
        postPeakAtomic.store (0.0f, std::memory_order_relaxed);
        healthDbAtomic.store (0.0f, std::memory_order_relaxed);
    }

    void pushBlock (float pre, float post) noexcept
    {
        prePeak = juce::jmax (std::abs (pre), prePeak * releaseCoeff);
        postPeak = juce::jmax (std::abs (post), postPeak * releaseCoeff);

        constexpr float floor = 1.0e-6f;
        const float health = 20.0f * std::log10 ((postPeak + floor) / (prePeak + floor));
        prePeakAtomic.store (prePeak, std::memory_order_release);
        postPeakAtomic.store (postPeak, std::memory_order_release);
        healthDbAtomic.store (std::isfinite (health) ? health : 0.0f, std::memory_order_release);
    }

    float getPrePeak() const noexcept { return prePeakAtomic.load (std::memory_order_acquire); }
    float getPostPeak() const noexcept { return postPeakAtomic.load (std::memory_order_acquire); }
    float getHealthDb() const noexcept { return healthDbAtomic.load (std::memory_order_acquire); }

private:
    float releaseCoeff = 0.999f;
    float prePeak = 0.0f;
    float postPeak = 0.0f;
    std::atomic<float> prePeakAtomic { 0.0f };
    std::atomic<float> postPeakAtomic { 0.0f };
    std::atomic<float> healthDbAtomic { 0.0f };
};
