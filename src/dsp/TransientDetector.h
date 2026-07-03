#pragma once

#include <algorithm>
#include <cmath>

class TransientDetector
{
public:
    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        updateCoefficients();
        reset();
    }

    void reset() noexcept
    {
        envelope = 0.0f;
        wasAboveThreshold = false;
        holdoffSamplesRemaining = 0;
    }

    void setThreshold (float newThreshold) noexcept
    {
        threshold = std::clamp (newThreshold, 0.0f, 1.0f);
    }

    void setMinimumEnergyGate (float newMinimumEnergy) noexcept
    {
        minimumEnergyGate = std::max (0.0f, newMinimumEnergy);
    }

    void setAttackReleaseMs (float attackMs, float releaseMs) noexcept
    {
        attackTimeMs = std::max (0.1f, attackMs);
        releaseTimeMs = std::max (attackTimeMs, releaseMs);
        updateCoefficients();
    }

    void setHoldoffMs (float newHoldoffMs) noexcept
    {
        holdoffMs = std::max (1.0f, newHoldoffMs);
        updateHoldoffSamples();
    }

    bool processSample (float kickSample) noexcept
    {
        const float energy = kickSample * kickSample;
        const float coeff = energy > envelope ? attackCoeff : releaseCoeff;
        envelope = coeff * envelope + (1.0f - coeff) * energy;

        if (holdoffSamplesRemaining > 0)
            --holdoffSamplesRemaining;

        const bool above = envelope >= threshold && envelope >= minimumEnergyGate;
        const bool detected = above && ! wasAboveThreshold && holdoffSamplesRemaining <= 0;

        wasAboveThreshold = above;

        if (detected)
            holdoffSamplesRemaining = holdoffSamples;

        return detected;
    }

    float getEnvelope() const noexcept { return envelope; }

private:
    void updateCoefficients() noexcept
    {
        attackCoeff = timeMsToCoeff (attackTimeMs);
        releaseCoeff = timeMsToCoeff (releaseTimeMs);
        updateHoldoffSamples();
    }

    void updateHoldoffSamples() noexcept
    {
        holdoffSamples = std::max (1, (int) std::round (sampleRate * holdoffMs / 1000.0));
    }

    float timeMsToCoeff (float ms) const noexcept
    {
        const double samples = std::max (1.0, sampleRate * (double) ms / 1000.0);
        return (float) std::exp (-1.0 / samples);
    }

    double sampleRate = 44100.0;
    float threshold = 0.004f;
    float minimumEnergyGate = 0.0004f;
    float attackTimeMs = 2.0f;
    float releaseTimeMs = 60.0f;
    float holdoffMs = 90.0f;
    float attackCoeff = 0.0f;
    float releaseCoeff = 0.0f;
    float envelope = 0.0f;
    bool wasAboveThreshold = false;
    int holdoffSamples = 1;
    int holdoffSamplesRemaining = 0;
};
