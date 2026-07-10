#pragma once

#include <cmath>
#include <algorithm>

#include <juce_dsp/juce_dsp.h>

#include "../dsp/FftPlanCache.h"

namespace SpectrumAnalysis
{
    inline constexpr int fftSize = 8192;
    inline constexpr int fftOrder = 13;
    inline constexpr int maximumBin = fftSize / 2;
    inline constexpr float minimumDb = -144.0f;
    inline constexpr float calibrationDbOffset = -0.8f;

    inline void calculatePowerSpectrumDb (float* leftScratch,
                                          float* rightScratch,
                                          juce::dsp::WindowingFunction<float>& window,
                                          float* dbOutput,
                                          float smoothingFactor) noexcept
    {
        window.multiplyWithWindowingTable (leftScratch, fftSize);
        window.multiplyWithWindowingTable (rightScratch, fftSize);

        std::fill (leftScratch + fftSize, leftScratch + fftSize * 2, 0.0f);
        std::fill (rightScratch + fftSize, rightScratch + fftSize * 2, 0.0f);

        auto& fft = FftPlanCache::get (fftOrder);
        fft.performFrequencyOnlyForwardTransform (leftScratch);
        fft.performFrequencyOnlyForwardTransform (rightScratch);

        const float safeSmoothing = juce::jlimit (0.0f, 1.0f, smoothingFactor);
        for (int bin = 0; bin <= maximumBin; ++bin)
        {
            const float leftMagnitude = (leftScratch[(size_t) bin] / (float) fftSize) * 4.0f;
            const float rightMagnitude = (rightScratch[(size_t) bin] / (float) fftSize) * 4.0f;
            const float powerDomainMagnitude = std::sqrt (0.5f * (leftMagnitude * leftMagnitude
                                                                  + rightMagnitude * rightMagnitude));
            const float db = juce::Decibels::gainToDecibels (powerDomainMagnitude, minimumDb)
                           + calibrationDbOffset;
            dbOutput[(size_t) bin] += (db - dbOutput[(size_t) bin]) * safeSmoothing;
        }

        for (int bin = maximumBin + 1; bin < fftSize; ++bin)
            dbOutput[(size_t) bin] = minimumDb;
    }
}
