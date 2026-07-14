#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>

// Learn-only timing refinement. The DTW path is constrained around the FFT
// coarse delay and is reduced to one scalar fractional delay; no path is ever
// published to or used by the realtime DSP.
class ConstrainedDtwRefiner
{
public:
    static constexpr int maxBandRadiusSamples = 16;
    static constexpr int maxBandWidth = maxBandRadiusSamples * 2 + 1;

    struct Scratch
    {
        std::array<double, maxBandWidth> previousCost {};
        std::array<double, maxBandWidth> currentCost {};
        std::array<double, maxBandWidth> previousResidualSum {};
        std::array<double, maxBandWidth> currentResidualSum {};
        std::array<int, maxBandWidth> previousCount {};
        std::array<int, maxBandWidth> currentCount {};

        void reset() noexcept
        {
            previousCost.fill (infinity());
            currentCost.fill (infinity());
            previousResidualSum.fill (0.0);
            currentResidualSum.fill (0.0);
            previousCount.fill (0);
            currentCount.fill (0);
        }

    private:
        static constexpr double infinity() noexcept
        {
            return std::numeric_limits<double>::infinity();
        }
    };

    struct Result
    {
        bool valid = false;
        bool ambiguous = false;
        bool cancelled = false;
        float delaySamples = 0.0f;
        float confidence = 0.0f;
    };

    static Result refine (const float* bass,
                          const float* kick,
                          int numSamples,
                          double sampleRate,
                          float coarseDelaySamples,
                          bool invertPolarity,
                          Scratch& scratch,
                          const std::function<bool()>& shouldCancel = {})
    {
        Result result;
        if (bass == nullptr || kick == nullptr || numSamples < 64 || ! std::isfinite (sampleRate)
            || sampleRate <= 0.0 || ! std::isfinite (coarseDelaySamples))
            return result;

        const float correctionBudgetSamples = (float) (sampleRate * 0.020);
        if (std::abs (coarseDelaySamples) > correctionBudgetSamples + 0.5f)
            return result;

        double peak = 0.0;
        for (int i = 0; i < numSamples; ++i)
        {
            if ((i & 255) == 0 && cancelled (shouldCancel))
                return cancelledResult();
            peak = std::max (peak, std::abs ((double) bass[i]));
            peak = std::max (peak, std::abs ((double) kick[i]));
        }
        if (peak <= 1.0e-8)
            return result;

        // Work on the overlap produced by the coarse scalar delay. The DTW band
        // then models only a small residual, so its width is fixed at 33 cells.
        const int first = std::max (0, (int) std::ceil (-(double) coarseDelaySamples));
        const int last = std::min (numSamples - 1,
                                   (int) std::floor ((double) (numSamples - 1) - coarseDelaySamples));
        const int length = last - first + 1;
        if (length < 64)
            return result;

        scratch.reset();
        const int radius = maxBandRadiusSamples;
        const int centre = radius;
        const double sign = invertPolarity ? -1.0 : 1.0;
        const double scale = peak * peak;

        for (int i = 0; i < length; ++i)
        {
            if ((i & 63) == 0 && cancelled (shouldCancel))
                return cancelledResult();

            scratch.currentCost.fill (infinity());
            scratch.currentResidualSum.fill (0.0);
            scratch.currentCount.fill (0);

            for (int slot = 0; slot < maxBandWidth; ++slot)
            {
                const int residual = slot - centre;
                const int j = i + residual;
                if (j < 0 || j >= length)
                    continue;

                const double kickIndex = (double) first + (double) j + (double) coarseDelaySamples;
                const float alignedKick = sampleLinear (kick, numSamples, kickIndex);
                const double difference = sign * (double) bass[first + i] - (double) alignedKick;
                const double localCost = difference * difference / scale;

                if (i == 0 && j == 0)
                {
                    scratch.currentCost[(size_t) slot] = localCost;
                    scratch.currentResidualSum[(size_t) slot] = (double) residual;
                    scratch.currentCount[(size_t) slot] = 1;
                    continue;
                }

                int bestSlot = -1;
                double bestCost = infinity();
                if (slot < maxBandWidth && scratch.previousCost[(size_t) slot] < bestCost)
                {
                    bestCost = scratch.previousCost[(size_t) slot];
                    bestSlot = slot; // diagonal wins deterministic ties
                }
                if (slot + 1 < maxBandWidth && scratch.previousCost[(size_t) (slot + 1)] < bestCost)
                {
                    bestCost = scratch.previousCost[(size_t) (slot + 1)];
                    bestSlot = slot + 1;
                }
                if (slot > 0 && scratch.currentCost[(size_t) (slot - 1)] < bestCost)
                {
                    bestCost = scratch.currentCost[(size_t) (slot - 1)];
                    bestSlot = slot - 1;
                }
                if (bestSlot < 0 || ! std::isfinite (bestCost))
                    continue;

                const bool fromCurrentRow = bestSlot == slot - 1;
                const auto& priorCost = fromCurrentRow ? scratch.currentCost : scratch.previousCost;
                const auto& priorResidualSum = fromCurrentRow ? scratch.currentResidualSum
                                                               : scratch.previousResidualSum;
                const auto& priorCount = fromCurrentRow ? scratch.currentCount : scratch.previousCount;
                scratch.currentCost[(size_t) slot] = priorCost[(size_t) bestSlot] + localCost;
                scratch.currentResidualSum[(size_t) slot] = priorResidualSum[(size_t) bestSlot]
                                                         + (double) residual;
                scratch.currentCount[(size_t) slot] = priorCount[(size_t) bestSlot] + 1;
            }

            scratch.previousCost.swap (scratch.currentCost);
            scratch.previousResidualSum.swap (scratch.currentResidualSum);
            scratch.previousCount.swap (scratch.currentCount);
        }

        if (! std::isfinite (scratch.previousCost[(size_t) centre])
            || scratch.previousCount[(size_t) centre] == 0)
            return result;

        const float dtwDelay = coarseDelaySamples
            + (float) (scratch.previousResidualSum[(size_t) centre]
                       / (double) scratch.previousCount[(size_t) centre]);
        const float boundedDtwDelay = std::clamp (dtwDelay,
                                                  coarseDelaySamples - (float) radius,
                                                  coarseDelaySamples + (float) radius);

        double bestScalarError = infinity();
        int bestScalarOffset = 0;
        for (int offset = -radius; offset <= radius; ++offset)
        {
            const double error = scalarError (bass, kick, numSamples, sign,
                                              coarseDelaySamples + (float) offset, shouldCancel);
            if (cancelled (shouldCancel))
                return cancelledResult();
            const float candidateDelay = coarseDelaySamples + (float) offset;
            const float currentDelay = coarseDelaySamples + (float) bestScalarOffset;
            if (error < bestScalarError - 1.0e-12
                || (std::abs (error - bestScalarError) <= 1.0e-12
                    && std::abs (candidateDelay - boundedDtwDelay)
                        < std::abs (currentDelay - boundedDtwDelay)))
            {
                bestScalarError = error;
                bestScalarOffset = offset;
            }
        }

        double secondScalarError = infinity();
        for (int offset = -radius; offset <= radius; ++offset)
        {
            if (std::abs (offset - bestScalarOffset) < radius / 2)
                continue;
            const double error = scalarError (bass, kick, numSamples, sign,
                                              coarseDelaySamples + (float) offset, shouldCancel);
            if (cancelled (shouldCancel))
                return cancelledResult();
            secondScalarError = std::min (secondScalarError, error);
        }

        result.ambiguous = std::isfinite (secondScalarError)
            && secondScalarError <= bestScalarError * 1.015 + 1.0e-9;
        // The DTW path chooses the local bounded search band. Fit the published
        // scalar around the best unwarped sample alignment inside that band.
        const float scalarCentre = coarseDelaySamples + (float) bestScalarOffset;
        const double errorMinus = scalarError (bass, kick, numSamples, sign,
                                               scalarCentre - 1.0f, shouldCancel);
        const double errorCentre = scalarError (bass, kick, numSamples, sign,
                                                scalarCentre, shouldCancel);
        const double errorPlus = scalarError (bass, kick, numSamples, sign,
                                              scalarCentre + 1.0f, shouldCancel);
        if (cancelled (shouldCancel) || ! std::isfinite (errorCentre))
            return cancelled (shouldCancel) ? cancelledResult() : result;

        float fractional = scalarCentre;
        const double denominator = errorMinus - 2.0 * errorCentre + errorPlus;
        if (std::isfinite (denominator) && denominator > 1.0e-12)
        {
            const double offset = std::clamp (0.5 * (errorMinus - errorPlus) / denominator,
                                              -0.5, 0.5);
            fractional += (float) offset;
        }
        fractional = std::clamp (fractional,
                                 coarseDelaySamples - (float) radius,
                                 coarseDelaySamples + (float) radius);
        result.delaySamples = fractional;
        result.confidence = (float) std::clamp (1.0 / (1.0 + errorCentre), 0.0, 1.0);
        result.valid = ! result.ambiguous
            && std::abs (result.delaySamples) <= correctionBudgetSamples + 1.0e-4f;
        return result;
    }

private:
    static constexpr double infinity() noexcept
    {
        return std::numeric_limits<double>::infinity();
    }

    static bool cancelled (const std::function<bool()>& shouldCancel)
    {
        return shouldCancel && shouldCancel();
    }

    static Result cancelledResult() noexcept
    {
        Result result;
        result.cancelled = true;
        return result;
    }

    static float sampleLinear (const float* values, int count, double index) noexcept
    {
        if (index < 0.0 || index >= (double) (count - 1))
            return 0.0f;
        const int base = (int) std::floor (index);
        const float fraction = (float) (index - (double) base);
        return values[base] + fraction * (values[base + 1] - values[base]);
    }

    static double scalarError (const float* bass,
                               const float* kick,
                               int count,
                               double sign,
                               float delaySamples,
                               const std::function<bool()>& shouldCancel)
    {
        double error = 0.0;
        int compared = 0;
        for (int i = 0; i < count; ++i)
        {
            if ((i & 255) == 0 && cancelled (shouldCancel))
                return infinity();
            const double kickIndex = (double) i + (double) delaySamples;
            if (kickIndex < 0.0 || kickIndex >= (double) (count - 1))
                continue;
            const double difference = sign * (double) bass[i]
                - (double) sampleLinear (kick, count, kickIndex);
            error += difference * difference;
            ++compared;
        }
        return compared > 0 ? error / (double) compared : infinity();
    }
};
