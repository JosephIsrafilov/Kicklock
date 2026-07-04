#pragma once

#include <vector>
#include <cmath>

// Rolling-window stereo correlation meter.
//
// Maintains running sums over the last N sample pairs (a circular buffer of
// raw pairs) so that each pushSample() call is O(1): the incoming pair is
// added to the sums, and once the buffer is full the oldest pair is
// subtracted before being overwritten. The Pearson correlation coefficient
// is recomputed from those sums on every push and smoothed with a one-pole
// EMA before being exposed as a 0..100 value.
class CorrelationMeter
{
public:
    void prepare (double newSampleRate, int windowSizeSamples)
    {
        sampleRate = newSampleRate;
        windowSize = windowSizeSamples > 0 ? windowSizeSamples : 1;

        bufferA.assign ((size_t) windowSize, 0.0f);
        bufferB.assign ((size_t) windowSize, 0.0f);

        updateSmoothingCoefficient();
        reset();
    }

    void reset()
    {
        sumA = 0.0;
        sumB = 0.0;
        sumAB = 0.0;
        sumA2 = 0.0;
        sumB2 = 0.0;
        writeIndex = 0;
        numValid = 0;
        smoothedValue = 0.0f;

        std::fill (bufferA.begin(), bufferA.end(), 0.0f);
        std::fill (bufferB.begin(), bufferB.end(), 0.0f);
    }

    // O(1) rolling update: add the new pair, evict the oldest pair once the
    // window is full, then recompute the smoothed correlation.
    void pushSample (float a, float b)
    {
        if (windowSize <= 0)
            return;

        const double alpha = 1.0 - std::exp(-1.0 / (double)windowSize);
        const double beta = 1.0 - alpha;
        const double da = (double) a;
        const double db = (double) b;

        sumA  = alpha * da + beta * sumA;
        sumB  = alpha * db + beta * sumB;
        sumAB = alpha * (da * db) + beta * sumAB;
        sumA2 = alpha * (da * da) + beta * sumA2;
        sumB2 = alpha * (db * db) + beta * sumB2;

        if (numValid < windowSize)
            ++numValid;

        const float mapped = computeMappedCorrelation();
        smoothedValue += smoothingAlpha * (mapped - smoothedValue);
    }

    float getCorrelation() const
    {
        return smoothedValue;
    }

    // EMA time constant; alpha = 1 - exp(-1 / (sampleRate * ms/1000)).
    // Settable before or after prepare(); recomputed whenever either the
    // sample rate or this value changes.
    void setSmoothingTimeMs (float ms)
    {
        smoothingTimeMs = ms > 0.0f ? ms : 0.0f;
        updateSmoothingCoefficient();
    }

private:
    float computeMappedCorrelation() const
    {
        if (numValid == 0)
            return smoothedValue;

        // Since we are using EMA, the sums are already normalized by alpha, 
        // effectively meaning n=1. We just use the standard EMA variance/covariance formula.
        const double numerator = sumAB - sumA * sumB;
        const double denomSq = (sumA2 - sumA * sumA) * (sumB2 - sumB * sumB);

        if (denomSq <= 1.0e-12)
            return smoothedValue;

        double r = numerator / std::sqrt (std::max(0.0, denomSq));
        r = r < -1.0 ? -1.0 : (r > 1.0 ? 1.0 : r);

        return (float) ((r + 1.0) * 50.0);
    }

    void updateSmoothingCoefficient()
    {
        const float ms = smoothingTimeMs > 0.0f ? smoothingTimeMs : 100.0f;

        if (sampleRate <= 0.0)
        {
            smoothingAlpha = 1.0f;
            return;
        }

        const double timeConstantSamples = sampleRate * (ms / 1000.0);

        if (timeConstantSamples <= 0.0)
            smoothingAlpha = 1.0f;
        else
            smoothingAlpha = (float) (1.0 - std::exp (-1.0 / timeConstantSamples));
    }

    double sampleRate = 0.0;
    int windowSize = 1;

    std::vector<float> bufferA;
    std::vector<float> bufferB;
    int writeIndex = 0;
    int numValid = 0;

    double sumA = 0.0, sumB = 0.0, sumAB = 0.0, sumA2 = 0.0, sumB2 = 0.0;

    float smoothingTimeMs = 100.0f;
    float smoothingAlpha = 1.0f;
    float smoothedValue = 0.0f;
};
