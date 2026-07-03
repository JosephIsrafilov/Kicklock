#pragma once

#include <vector>
#include <cmath>
#include <algorithm>

// Offline cross-correlation aligner used by the Analyze button. Given raw
// (pre-processing) mono bass and kick, it finds the delay (and whether a
// polarity flip helps) that best time-aligns them, plus before/after match
// percentages.
//
// Why band-limiting: kick and bass only interact (reinforce or cancel) down in
// the sub/low range. Their broadband waveforms barely resemble each other, so a
// full-range cross-correlation locks onto whatever loud transient junk happens
// to line up in a given capture -> jumpy, "random"-feeling results. We therefore
// band-pass BOTH signals to the overlap region (default 30-120 Hz) with the same
// filter before correlating. Identical filtering imposes identical group delay
// on both, so the measured lag between them is preserved while the noise is gone.
//
// Pure and header-only (no JUCE deps) so the unit tests can exercise it
// directly. Runs on the message thread, so plain heap allocation is fine.
struct AlignmentResult
{
    bool  valid          = false; // false when the low band is too quiet to judge
    float delayMs        = 0.0f;  // recommended delay in the plugin's sign convention
    bool  invertPolarity = false;
    float beforeMatch    = 50.0f; // low-band phase-match % at the current (zero) offset
    float afterMatch     = 50.0f; // low-band phase-match % after delay + polarity (+ rotator)

    // Phase-rotator recommendation. adjustRotator is false when no rotator
    // setting improves on delay+polarity alone, in which case the caller should
    // leave the rotator parameters untouched.
    bool  adjustRotator  = false;
    float rotatorFreqHz  = 200.0f;
    float rotatorQ       = 0.7f;
    int   rotatorStages  = 2;      // 2, 3 or 4
};

class AlignmentAnalyzer
{
public:
    // Sign convention matches the processor: a positive delayMs delays the
    // main (bass) bus, a negative delayMs delays the sidechain (kick) bus.
    //
    // Derivation: with c[D] = sum_n bass[n] * kick[n+D], the peak at D*>0 means
    // kick[m] ~= bass[m-D*], i.e. the bass event leads the kick, so we delay the
    // bass (positive delayMs). D*<0 means the kick leads, so we delay the kick
    // (negative delayMs). delayMs = D* / sampleRate * 1000.
    static AlignmentResult analyze (const float* bass,
                                    const float* kick,
                                    int numSamples,
                                    double sampleRate,
                                    float maxDelayMs,
                                    float lowHz  = 30.0f,
                                    float highHz = 120.0f,
                                    int   maxWindow = 32768)
    {
        AlignmentResult result;

        if (bass == nullptr || kick == nullptr || numSamples <= 0 || sampleRate <= 0.0)
            return result;

        // Analyse the most recent `window` samples for a bounded, snappy cost.
        const int window = std::min (numSamples, std::max (1, maxWindow));
        const int base   = numSamples - window;

        // Band-pass copies (same filter on both -> relative delay preserved).
        std::vector<float> a (bass + base, bass + base + window);
        std::vector<float> b (kick + base, kick + base + window);
        bandPass (a, sampleRate, lowHz, highHz);
        bandPass (b, sampleRate, lowHz, highHz);

        // Energy of the band-limited signals: normalisation + silence gate.
        double ea = 0.0, eb = 0.0;
        for (int n = 0; n < window; ++n)
        {
            ea += (double) a[(size_t) n] * (double) a[(size_t) n];
            eb += (double) b[(size_t) n] * (double) b[(size_t) n];
        }

        const double norm = std::sqrt (ea * eb);
        if (norm < 1.0e-6)
            return result; // no meaningful low-end on one or both inputs

        int maxLag = (int) std::llround (maxDelayMs / 1000.0 * sampleRate);
        maxLag = std::clamp (maxLag, 1, window - 1);

        const float* ap = a.data();
        const float* bp = b.data();

        // Correlation at zero offset = the "before" low-band match.
        const double r0 = correlationAt (ap, bp, window, 0) / norm;

        int    bestLag = 0;
        double bestAbs = -1.0;
        double bestVal = 0.0;

        for (int d = -maxLag; d <= maxLag; ++d)
        {
            const double c = correlationAt (ap, bp, window, d) / norm;
            const double m = std::abs (c);
            if (m > bestAbs)
            {
                bestAbs = m;
                bestVal = c;
                bestLag = d;
            }
        }

        // Sub-sample refinement: parabolic fit through the peak and its two
        // neighbours so the recommended delay isn't quantised to whole samples
        // (which otherwise makes repeated presses jump by a sample or two).
        double refinedLag = (double) bestLag;
        if (bestLag > -maxLag && bestLag < maxLag)
        {
            const double ym1 = std::abs (correlationAt (ap, bp, window, bestLag - 1) / norm);
            const double y0  = bestAbs;
            const double yp1 = std::abs (correlationAt (ap, bp, window, bestLag + 1) / norm);
            const double denom = ym1 - 2.0 * y0 + yp1;
            if (std::abs (denom) > 1.0e-12)
                refinedLag += 0.5 * (ym1 - yp1) / denom;
        }

        result.valid          = true;
        result.delayMs        = (float) (refinedLag / sampleRate * 1000.0);
        result.delayMs        = std::clamp (result.delayMs, -maxDelayMs, maxDelayMs);
        result.invertPolarity = bestVal < 0.0;
        result.beforeMatch    = toPercent (r0);
        result.afterMatch     = toPercent (std::abs (bestVal)); // aligned + polarity applied

        // --- Phase-rotator search ----------------------------------------
        // Delay+polarity gets the coarse alignment; the rotator squeezes out
        // the residual phase offset the integer/fractional delay can't. Build
        // the post-delay+polarity band-limited bass, then grid-search rotator
        // settings for the best remaining match, keeping it only if it beats
        // delay+polarity alone by a worthwhile margin.
        const int   intLag = (int) std::llround (refinedLag);
        const float sign   = result.invertPolarity ? -1.0f : 1.0f;

        // Align a copy of the band-limited bass to the kick using intLag, in the
        // same overlap region used for correlation, so rotator gains are judged
        // on the aligned signals.
        const int nStart = std::max (0, -intLag);
        const int nEnd   = std::min (window, window - intLag);
        const int alignedLen = nEnd - nStart;

        if (alignedLen > 64)
        {
            std::vector<float> kickAligned ((size_t) alignedLen);
            for (int i = 0; i < alignedLen; ++i)
                kickAligned[(size_t) i] = bp[nStart + i + intLag];

            // Baseline: aligned bass (with polarity) vs kick, no rotator.
            std::vector<float> bassBase ((size_t) alignedLen);
            for (int i = 0; i < alignedLen; ++i)
                bassBase[(size_t) i] = sign * ap[nStart + i];

            const double baseAbs = std::abs (findPeak (bassBase.data(), kickAligned.data(),
                                                       alignedLen, 1).signed_);

            double bestRotAbs = baseAbs;

            // Coarse grid over the sub/low overlap region. Kept small so the
            // press stays snappy.
            const float freqCandidates[]  = { 40.0f, 60.0f, 80.0f, 100.0f, 140.0f, 200.0f };
            const float qCandidates[]     = { 0.5f, 0.7f, 1.0f, 2.0f, 4.0f };
            const int   stageCandidates[] = { 2, 3, 4 };

            for (int stages : stageCandidates)
            {
                for (float freq : freqCandidates)
                {
                    if ((double) freq >= sampleRate * 0.5)
                        continue;

                    for (float qv : qCandidates)
                    {
                        std::vector<float> trial = bassBase;
                        applyAllpassCascade (trial, sampleRate, freq, qv, stages);

                        const double m = std::abs (findPeak (trial.data(), kickAligned.data(),
                                                             alignedLen, 1).signed_);
                        if (m > bestRotAbs + 1.0e-4)
                        {
                            bestRotAbs           = m;
                            result.adjustRotator = true;
                            result.rotatorFreqHz = freq;
                            result.rotatorQ      = qv;
                            result.rotatorStages = stages;
                        }
                    }
                }
            }

            if (result.adjustRotator)
                result.afterMatch = toPercent (bestRotAbs);
        }

        return result;
    }

private:
    // sum_n a[n] * b[n + d] over the valid overlap only.
    static double correlationAt (const float* a, const float* b, int window, int d)
    {
        const int nStart = std::max (0, -d);
        const int nEnd   = std::min (window, window - d);

        double sum = 0.0;
        for (int n = nStart; n < nEnd; ++n)
            sum += (double) a[n] * (double) b[n + d];

        return sum;
    }

    struct Peak
    {
        double signed_ = 0.0; // best normalised correlation, sign preserved
        double abs_    = 0.0; // |signed_|
        int    lag     = 0;   // integer lag at the peak
    };

    // Scan lags in [-maxLag, maxLag] for the strongest |correlation|, normalised
    // by the two signals' energies. Returns zeroed Peak if either is silent.
    static Peak findPeak (const float* a, const float* b, int window, int maxLag)
    {
        double ea = 0.0, eb = 0.0;
        for (int n = 0; n < window; ++n)
        {
            ea += (double) a[n] * (double) a[n];
            eb += (double) b[n] * (double) b[n];
        }

        Peak peak;
        const double norm = std::sqrt (ea * eb);
        if (norm < 1.0e-9)
            return peak;

        for (int d = -maxLag; d <= maxLag; ++d)
        {
            const double c = correlationAt (a, b, window, d) / norm;
            const double m = std::abs (c);
            if (m > peak.abs_)
            {
                peak.abs_    = m;
                peak.signed_ = c;
                peak.lag     = d;
            }
        }
        return peak;
    }

    // Apply `stages` cascaded 2nd-order allpass sections to x IN PLACE, as a
    // single causal forward pass per stage. This mirrors the real-time rotator
    // (juce::dsp::IIR::Filter::processSample), including the phase shift it
    // introduces -- which is the whole point, so it must NOT be zero-phase.
    // Coefficients replicate juce::dsp::IIR::Coefficients::makeAllPass exactly.
    static void applyAllpassCascade (std::vector<float>& x, double fs,
                                     float freqHz, float q, int stages)
    {
        const double n      = 1.0 / std::tan (Biquad::kPi * (double) freqHz / fs);
        const double nSq    = n * n;
        const double invQ   = 1.0 / (double) q;
        const double c1     = 1.0 / (1.0 + invQ * n + nSq);
        const double b0     = c1 * (1.0 - n * invQ + nSq);
        const double b1     = c1 * 2.0 * (1.0 - nSq);
        // Denominator is the reverse of the numerator: a1 = b1, a2 = b0.

        const int len = (int) x.size();
        for (int s = 0; s < stages; ++s)
        {
            double z1 = 0.0, z2 = 0.0; // transposed direct form II
            for (int i = 0; i < len; ++i)
            {
                const double in  = x[(size_t) i];
                const double out = b0 * in + z1;
                z1 = b1 * in - b1 * out + z2;
                z2 = 1.0 * in - b0 * out;
                x[(size_t) i] = (float) out;
            }
        }
    }

    // Map Pearson-style r in [-1, 1] to [0, 100], matching CorrelationMeter.
    static float toPercent (double r)
    {
        r = std::clamp (r, -1.0, 1.0);
        return (float) ((r + 1.0) * 50.0);
    }

    // In-place band-pass: 2nd-order RBJ high-pass then low-pass, run forward
    // then backward for zero net phase (so the peak isn't shifted by the
    // filter's own group delay). Butterworth Q for a maximally-flat band.
    static void bandPass (std::vector<float>& x, double fs, float lowHz, float highHz)
    {
        const double q = 0.70710678; // 1/sqrt(2)
        Biquad hp; hp.makeHighPass (fs, lowHz,  q);
        Biquad lp; lp.makeLowPass  (fs, highHz, q);

        applyForwardBack (x, hp);
        applyForwardBack (x, lp);
    }

    struct Biquad
    {
        static constexpr double kPi = 3.14159265358979323846;

        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

        void makeLowPass (double fs, double f, double q)
        {
            const double w0 = 2.0 * kPi * f / fs;
            const double cw = std::cos (w0), sw = std::sin (w0);
            const double alpha = sw / (2.0 * q);
            const double a0 = 1.0 + alpha;
            b0 = ((1.0 - cw) * 0.5) / a0;
            b1 = (1.0 - cw) / a0;
            b2 = ((1.0 - cw) * 0.5) / a0;
            a1 = (-2.0 * cw) / a0;
            a2 = (1.0 - alpha) / a0;
        }

        void makeHighPass (double fs, double f, double q)
        {
            const double w0 = 2.0 * kPi * f / fs;
            const double cw = std::cos (w0), sw = std::sin (w0);
            const double alpha = sw / (2.0 * q);
            const double a0 = 1.0 + alpha;
            b0 = ((1.0 + cw) * 0.5) / a0;
            b1 = -(1.0 + cw) / a0;
            b2 = ((1.0 + cw) * 0.5) / a0;
            a1 = (-2.0 * cw) / a0;
            a2 = (1.0 - alpha) / a0;
        }
    };

    // One forward pass then one reverse pass (filtfilt-style) => zero phase.
    static void applyForwardBack (std::vector<float>& x, const Biquad& c)
    {
        const int n = (int) x.size();

        double z1 = 0.0, z2 = 0.0; // transposed direct form II state
        for (int i = 0; i < n; ++i)
        {
            const double in = x[(size_t) i];
            const double out = c.b0 * in + z1;
            z1 = c.b1 * in - c.a1 * out + z2;
            z2 = c.b2 * in - c.a2 * out;
            x[(size_t) i] = (float) out;
        }

        z1 = z2 = 0.0;
        for (int i = n - 1; i >= 0; --i)
        {
            const double in = x[(size_t) i];
            const double out = c.b0 * in + z1;
            z1 = c.b1 * in - c.a1 * out + z2;
            z2 = c.b2 * in - c.a2 * out;
            x[(size_t) i] = (float) out;
        }
    }
};
