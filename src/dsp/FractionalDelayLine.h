#pragma once

#include <vector>
#include <cmath>
#include <algorithm>

enum class InterpolationType { Linear, Allpass };

// Simple circular-buffer fractional delay line with a choice of
// linear or first-order allpass interpolation for the fractional part.
class FractionalDelayLine
{
public:
    void prepare (double newSampleRate, float maxDelayMs)
    {
        sampleRate = newSampleRate;

        const double maxDelaySamplesD = (maxDelayMs / 1000.0) * sampleRate;
        maxDelaySamples = (float) maxDelaySamplesD;

        const int bufferLength = (int) std::ceil (maxDelaySamplesD) + 1;
        buffer.assign ((size_t) std::max (bufferLength, 1), 0.0f);

        setRampDurationMs (3.0f);

        reset();
    }

    void reset()
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writeIndex = 0;
        allpassXPrev = 0.0f;
        allpassYPrev = 0.0f;
        currentDelayInSamples = delayInSamples;
        delayInitialised = false;
        rampSamplesRemaining = 0;
    }

    // State changes are sample-ramped rather than changing the fractional tap
    // abruptly.  The 1-5 ms range is short enough for Dynamic onset matching
    // while avoiding a zipper discontinuity when dense kicks change state.
    void setRampDurationMs (float milliseconds)
    {
        const float ms = std::clamp (std::isfinite (milliseconds) ? milliseconds : 3.0f,
                                     1.0f, 5.0f);
        delayRampSamples = std::max (1, (int) std::lround (sampleRate * ms / 1000.0));
    }

    void setDelaySamples (float delaySamples)
    {
        delayInSamples = std::clamp (delaySamples, 0.0f, maxDelaySamples);
        if (! delayInitialised)
        {
            currentDelayInSamples = delayInSamples;
            delayInitialised = true;
            rampSamplesRemaining = 0;
        }
        else if (std::abs (delayInSamples - currentDelayInSamples) > 1.0e-6f)
        {
            rampSamplesRemaining = delayRampSamples;
        }
    }

    // Switching interpolation type resets only the allpass interpolator
    // state, leaving the delay buffer contents intact.
    void setInterpolationType (InterpolationType newType)
    {
        interpolationType = newType;
        allpassXPrev = 0.0f;
        allpassYPrev = 0.0f;
    }

    float processSample (float input)
    {
        if (buffer.empty())
            return 0.0f;

        const int bufferSize = (int) buffer.size();

        buffer[(size_t) writeIndex] = input;
        writeIndex = (writeIndex + 1) % bufferSize;

        if (rampSamplesRemaining > 0)
        {
            currentDelayInSamples += (delayInSamples - currentDelayInSamples)
                                   / (float) rampSamplesRemaining;
            --rampSamplesRemaining;
        }

        const int integerDelay = (int) currentDelayInSamples;
        const float frac = currentDelayInSamples - (float) integerDelay;

        // read position of the two neighboring integer-delay samples
        int readIndex0 = writeIndex - 1 - integerDelay;
        readIndex0 = ((readIndex0 % bufferSize) + bufferSize) % bufferSize;
        int readIndex1 = readIndex0 - 1;
        readIndex1 = ((readIndex1 % bufferSize) + bufferSize) % bufferSize;

        const float x0 = buffer[(size_t) readIndex0];
        const float x1 = buffer[(size_t) readIndex1];

        if (interpolationType == InterpolationType::Linear)
            return x0 + frac * (x1 - x0);

        if (frac <= 1.0e-4f)
            return x0;

        // First-order allpass fractional interpolator:
        // eta = (1 - frac) / (1 + frac) derived from matching the allpass
        // filter's group delay at DC to the desired fractional delay.
        // y[n] = eta * x[n] + x[n-1] - eta * y[n-1]
        const float eta = (1.0f - frac) / (1.0f + frac);
        const float y = eta * x0 + allpassXPrev - eta * allpassYPrev;

        allpassXPrev = x0;
        allpassYPrev = y;

        return y;
    }

private:
    double sampleRate = 0.0;
    float maxDelaySamples = 0.0f;
    float delayInSamples = 0.0f;
    float currentDelayInSamples = 0.0f;
    int delayRampSamples = 1;
    int rampSamplesRemaining = 0;
    bool delayInitialised = false;

    std::vector<float> buffer;
    int writeIndex = 0;

    InterpolationType interpolationType = InterpolationType::Linear;

    float allpassXPrev = 0.0f;
    float allpassYPrev = 0.0f;
};
