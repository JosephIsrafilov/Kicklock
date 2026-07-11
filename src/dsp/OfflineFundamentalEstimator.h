#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

// Independent pitch estimate used only to cross-check the live PitchTracker
// during Learn. It is a worker-thread-only, deterministic offline estimator; it
// does NOT use the live PitchTracker and must never be called from
// processBlock().
struct FundamentalEstimate
{
    bool  valid = false;
    float frequencyHz = 0.0f;
    float confidence = 0.0f;
};

// OfflineFundamentalEstimator
// ---------------------------
// Algorithm: YIN-lite (difference function -> cumulative mean normalized
// difference function, "CMNDF"), with an absolute threshold, first-dip
// selection, and parabolic interpolation.
//
// Octave / harmonic tie-break (explicit policy):
//   The CMNDF is scanned from the shortest lag upward and the FIRST lag whose
//   CMNDF drops below the absolute threshold (refined to its local minimum) is
//   chosen. Because a signal that still contains a fundamental of period P is
//   NOT well-correlated at P/2 (the half-period shift inverts the fundamental),
//   the CMNDF at P/2 stays above threshold whenever the fundamental carries
//   "sufficient evidence", so P (the true fundamental) is chosen even when the
//   second harmonic is stronger. Only when the fundamental is essentially
//   absent does the scan fall through to a higher harmonic. The estimator never
//   silently reinterprets a detected octave as the live tracked fundamental;
//   it simply reports the period it found and its confidence, and leaves any
//   octave policy to the Learn acceptance logic.
//
// Confidence semantics:
//   confidence = clamp(1 - CMNDF_at_selected_lag, 0, 1). 1.0 means a perfectly
//   periodic window; values near 0 mean aperiodic/noisy. `valid` additionally
//   requires the signal energy to clear a small floor and the confidence to
//   clear kMinConfidence (0.50). The Learn pipeline applies the stricter
//   kMinOfflinePitchConfidence (0.55) on top of this.
//
// Minimum window requirement:
//   numSamples must be >= 2 * ceil(sampleRate / minHz), i.e. at least ~2 periods
//   of the lowest searchable frequency, otherwise the estimate is invalid.
class OfflineFundamentalEstimator
{
public:
    // Confidence floor for the returned `valid` flag. Deliberately below the
    // Learn acceptance threshold so acceptance stays the caller's decision.
    static constexpr float kMinConfidence = 0.50f;

    // RMS floor (post DC-removal) below which the window is treated as
    // silence / too quiet to estimate.
    static constexpr float kMinRms = 1.0e-4f;

    // CMNDF absolute threshold: the first lag below this (refined to a local
    // minimum) is taken as the period.
    static constexpr float kCmndfThreshold = 0.15f;

    static FundamentalEstimate estimate (const float* bass,
                                         int numSamples,
                                         double sampleRate,
                                         float minHz = 25.0f,
                                         float maxHz = 300.0f)
    {
        FundamentalEstimate result;

        if (bass == nullptr || numSamples <= 0)
            return result;
        if (! (sampleRate > 0.0) || ! std::isfinite (sampleRate))
            return result;
        if (! (minHz > 0.0f) || ! (maxHz > minHz)
            || ! std::isfinite (minHz) || ! std::isfinite (maxHz))
            return result;

        const int minLag = (int) std::floor (sampleRate / (double) maxHz);
        const int maxLag = (int) std::ceil  (sampleRate / (double) minHz);
        if (minLag < 1 || maxLag <= minLag)
            return result;

        // Minimum window: at least two periods of the lowest frequency.
        if (numSamples < 2 * maxLag)
            return result;

        // DC-removed working copy. Reject non-finite input outright.
        double mean = 0.0;
        for (int i = 0; i < numSamples; ++i)
        {
            const float s = bass[i];
            if (! std::isfinite (s))
                return result;
            mean += (double) s;
        }
        mean /= (double) numSamples;

        std::vector<float> x ((size_t) numSamples);
        for (int i = 0; i < numSamples; ++i)
            x[(size_t) i] = bass[i] - (float) mean;

        // Integration window: use the available span, capped so the cost stays
        // bounded on a worker thread (<= ~2 * maxLag^2 multiply-adds).
        const int available = numSamples - maxLag;      // >= maxLag by the check above
        const int window = available < 2 * maxLag ? available : 2 * maxLag;

        // RMS gate (silence / very low amplitude).
        double energy = 0.0;
        for (int i = 0; i < window; ++i)
            energy += (double) x[(size_t) i] * (double) x[(size_t) i];
        const double rms = std::sqrt (energy / (double) window);
        if (! std::isfinite (rms) || rms < (double) kMinRms)
            return result;

        // Difference function d(tau) for tau in [1, maxLag].
        std::vector<float> cmndf ((size_t) (maxLag + 1), 1.0f);
        double runningSum = 0.0;
        for (int tau = 1; tau <= maxLag; ++tau)
        {
            double d = 0.0;
            for (int i = 0; i < window; ++i)
            {
                const double diff = (double) x[(size_t) i] - (double) x[(size_t) (i + tau)];
                d += diff * diff;
            }

            runningSum += d;
            cmndf[(size_t) tau] = runningSum > 0.0
                                      ? (float) (d * (double) tau / runningSum)
                                      : 1.0f;
        }

        // First lag below the absolute threshold, refined to its local minimum;
        // otherwise the global minimum over the search range.
        int tauBest = -1;
        for (int tau = minLag; tau <= maxLag; ++tau)
        {
            if (cmndf[(size_t) tau] < kCmndfThreshold)
            {
                while (tau + 1 <= maxLag && cmndf[(size_t) (tau + 1)] < cmndf[(size_t) tau])
                    ++tau;
                tauBest = tau;
                break;
            }
        }

        if (tauBest < 0)
        {
            float best = cmndf[(size_t) minLag];
            tauBest = minLag;
            for (int tau = minLag + 1; tau <= maxLag; ++tau)
            {
                if (cmndf[(size_t) tau] < best)
                {
                    best = cmndf[(size_t) tau];
                    tauBest = tau;
                }
            }
        }

        // Parabolic interpolation around the chosen lag.
        float refinedTau = (float) tauBest;
        if (tauBest > minLag && tauBest < maxLag)
        {
            const float s0 = cmndf[(size_t) (tauBest - 1)];
            const float s1 = cmndf[(size_t) tauBest];
            const float s2 = cmndf[(size_t) (tauBest + 1)];
            const float denom = s0 - 2.0f * s1 + s2;
            if (std::abs (denom) > 1.0e-12f)
                refinedTau = (float) tauBest + 0.5f * (s0 - s2) / denom;
        }

        if (! (refinedTau > 0.0f) || ! std::isfinite (refinedTau))
            return result;

        const float frequencyHz = (float) (sampleRate / (double) refinedTau);
        const float confidence = std::clamp (1.0f - cmndf[(size_t) tauBest], 0.0f, 1.0f);

        if (! std::isfinite (frequencyHz)
            || frequencyHz < minHz || frequencyHz > maxHz
            || confidence < kMinConfidence)
            return result;

        result.valid = true;
        result.frequencyHz = frequencyHz;
        result.confidence = confidence;
        return result;
    }
};
