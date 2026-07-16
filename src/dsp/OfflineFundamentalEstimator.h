#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
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

    // Worker-only evidence retained for Learn diagnostics. These values are
    // never serialized or read by the audio thread.
    int selectedLag = 0;
    int halfLag = 0;
    int doubleLag = 0;
    float selectedCmndf = 1.0f;
    float halfLagCmndf = 1.0f;
    float doubleLagCmndf = 1.0f;
    float selectedHarmonicScore = 0.0f;
    float higherOctaveHarmonicScore = 0.0f;
    bool octaveCorrected = false;
    bool octaveAmbiguous = false;
};

enum class OctaveEvidenceResolution
{
    keepSelected,
    promoteHigher,
    ambiguous
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
//   absent does the scan fall through to a higher harmonic. When a selected
//   subharmonic has a credible half-lag periodic minimum, the estimator compares
//   normalized harmonic evidence for both hypotheses: it promotes only a
//   stronger higher octave and otherwise reports an explicit ambiguity to Learn.
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

    static OctaveEvidenceResolution resolveOctaveEvidence (float selectedCmndf,
                                                            bool higherIsPeriodic,
                                                            float selectedHarmonicScore,
                                                            float higherHarmonicScore) noexcept
    {
        // A clear first YIN dip is positive evidence for the lower hypothesis.
        // This protects genuine low fundamentals with loud second harmonics.
        if (selectedCmndf <= kCmndfThreshold || ! higherIsPeriodic)
            return OctaveEvidenceResolution::keepSelected;
        return higherHarmonicScore > selectedHarmonicScore + 1.0e-6f
            ? OctaveEvidenceResolution::promoteHigher
            : OctaveEvidenceResolution::ambiguous;
    }

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

        result.selectedLag = tauBest;
        result.selectedCmndf = cmndf[(size_t) tauBest];

        // A YIN first-dip can lock to a subharmonic when a bass waveform
        // alternates subtly from cycle to cycle. Evaluate the corresponding
        // higher-octave lag with a second, normalized harmonic-energy view.
        // We only promote it when it is itself a credible periodic minimum and
        // the harmonic view prefers it. If the periodicity evidence exists but
        // the harmonic view cannot decide, keep the hit explicitly ambiguous
        // rather than silently teaching Dynamic an octave-low state.
        if (tauBest >= 2 * minLag)
        {
            const int nominalHalfLag = tauBest / 2;
            const int halfSearchRadius = std::max (2, nominalHalfLag / 32);
            int halfLag = nominalHalfLag;
            float halfBest = cmndf[(size_t) nominalHalfLag];
            for (int lag = std::max (minLag, nominalHalfLag - halfSearchRadius);
                 lag <= std::min (maxLag - 1, nominalHalfLag + halfSearchRadius); ++lag)
            {
                if (cmndf[(size_t) lag] < halfBest)
                {
                    halfBest = cmndf[(size_t) lag];
                    halfLag = lag;
                }
            }
            if (halfLag >= minLag && halfLag < maxLag)
            {
                result.halfLag = halfLag;
                result.halfLagCmndf = cmndf[(size_t) halfLag];
                const bool halfIsLocalMinimum = cmndf[(size_t) halfLag] <= cmndf[(size_t) (halfLag - 1)]
                                             && cmndf[(size_t) halfLag] <= cmndf[(size_t) (halfLag + 1)];
                const bool halfIsPeriodic = halfIsLocalMinimum
                                         && 1.0f - result.halfLagCmndf >= kMinConfidence;
                if (halfIsPeriodic)
                {
                    const float lowHz = (float) (sampleRate / (double) tauBest);
                    const float highHz = (float) (sampleRate / (double) halfLag);
                    result.selectedHarmonicScore = harmonicScore (x, window, sampleRate, lowHz);
                    result.higherOctaveHarmonicScore = harmonicScore (x, window, sampleRate, highHz);
                    const auto resolution = resolveOctaveEvidence (
                        result.selectedCmndf, halfIsPeriodic,
                        result.selectedHarmonicScore, result.higherOctaveHarmonicScore);
                    if (resolution == OctaveEvidenceResolution::promoteHigher)
                    {
                        tauBest = halfLag;
                        result.octaveCorrected = true;
                    }
                    else if (resolution == OctaveEvidenceResolution::ambiguous)
                    {
                        result.octaveAmbiguous = true;
                    }
                }
            }
        }
        if (tauBest * 2 <= maxLag)
        {
            result.doubleLag = tauBest * 2;
            result.doubleLagCmndf = cmndf[(size_t) result.doubleLag];
        }

        // Parabolic interpolation around the final, possibly octave-corrected lag.
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
        result.selectedLag = tauBest;
        result.selectedCmndf = cmndf[(size_t) tauBest];
        return result;
    }

private:
    static float harmonicScore (const std::vector<float>& x, int n, double sampleRate, float fundamentalHz)
    {
        if (n <= 1 || ! (fundamentalHz > 0.0f) || ! std::isfinite (fundamentalHz))
            return 0.0f;

        double totalEnergy = 0.0;
        for (int i = 0; i < n; ++i)
            totalEnergy += (double) x[(size_t) i] * (double) x[(size_t) i];
        if (! (totalEnergy > 1.0e-20) || ! std::isfinite (totalEnergy))
            return 0.0f;

        // The fundamental receives the greatest weight. A low-octave
        // hypothesis can explain a strong 2F component only as its second
        // harmonic, whereas the correct higher hypothesis explains it as F.
        static constexpr float weights[] = { 1.0f, 0.5f, 1.0f / 3.0f, 0.25f };
        double weightedEnergy = 0.0;
        for (int harmonic = 1; harmonic <= 4; ++harmonic)
        {
            const double frequency = (double) fundamentalHz * (double) harmonic;
            if (frequency >= sampleRate * 0.45)
                break;

            const double omega = 2.0 * std::numbers::pi_v<double> * frequency / sampleRate;
            const double coefficient = 2.0 * std::cos (omega);
            double q1 = 0.0, q2 = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const double q0 = (double) x[(size_t) i] + coefficient * q1 - q2;
                q2 = q1;
                q1 = q0;
            }
            const double power = q1 * q1 + q2 * q2 - coefficient * q1 * q2;
            weightedEnergy += (double) weights[harmonic - 1] * std::max (0.0, power);
        }

        return (float) std::clamp (weightedEnergy / ((double) n * totalEnergy), 0.0, 1.0);
    }
};
