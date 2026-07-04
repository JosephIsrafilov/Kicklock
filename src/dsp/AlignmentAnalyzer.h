#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include <complex>
#include <juce_dsp/juce_dsp.h>

struct AlignmentResult
{
    bool  valid          = false;
    float delayMs        = 0.0f;
    bool  invertPolarity = false;
    float beforeMatch    = 50.0f;
    float afterMatch     = 50.0f;

    bool  adjustRotator  = false;
    float rotatorFreqHz  = 200.0f;
    float rotatorQ       = 0.7f;
    int   rotatorStages  = 2;
};

class AlignmentAnalyzer
{
public:
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

        const int window = std::min (numSamples, std::max (1, maxWindow));
        const int base   = numSamples - window;

        std::vector<float> a (bass + base, bass + base + window);
        std::vector<float> b (kick + base, kick + base + window);
        bandPass (a, sampleRate, lowHz, highHz);
        bandPass (b, sampleRate, lowHz, highHz);

        double ea = 0.0, eb = 0.0;
        for (int n = 0; n < window; ++n)
        {
            ea += (double) a[(size_t) n] * (double) a[(size_t) n];
            eb += (double) b[(size_t) n] * (double) b[(size_t) n];
        }

        const double norm = std::sqrt (ea * eb);
        if (norm < 1.0e-6)
            return result;

        int maxLag = (int) std::llround (maxDelayMs / 1000.0 * sampleRate);
        maxLag = std::clamp (maxLag, 1, window - 1);

        // FFT-based cross-correlation: xcorr = IFFT(FFT(a) * conj(FFT(b)))
        const int fftOrder = nextPow2Order (2 * window);
        const int fftSize  = 1 << fftOrder;

        juce::dsp::FFT fft (fftOrder);

        // juce::dsp::FFT::performRealOnlyForwardTransform expects the real
        // input packed contiguously in the first fftSize floats of a
        // 2 * fftSize scratch buffer. Writing the samples into every other slot
        // makes the transform see an effectively upsampled signal and doubles
        // the recovered lag.
        std::vector<float> fftA (2 * (size_t) fftSize, 0.0f);
        std::vector<float> fftB (2 * (size_t) fftSize, 0.0f);

        for (int i = 0; i < window; ++i)
        {
            fftA[(size_t) i] = a[(size_t) i];
            fftB[(size_t) i] = b[(size_t) i];
        }

        fft.performRealOnlyForwardTransform (fftA.data(), true);
        fft.performRealOnlyForwardTransform (fftB.data(), true);

        // X * conj(Y) cross-spectrum per user request
        for (int i = 0; i <= fftSize / 2; ++i)
        {
            const float ar = fftA[2 * i];
            const float ai = fftA[2 * i + 1];
            const float br = fftB[2 * i];
            const float bi = fftB[2 * i + 1];
            // (ar + j*ai) * (br - j*bi)
            fftA[2 * i]     = ar * br + ai * bi;
            fftA[2 * i + 1] = ai * br - ar * bi;
        }

        fft.performRealOnlyInverseTransform (fftA.data());

        // fftA now holds circular cross-correlation. Lag d corresponds to:
        //   d >= 0: fftA[d]
        //   d < 0:  fftA[fftSize + d]
        // Correlation at zero offset = "before" match.
        const double r0 = fftA[0] / norm;

        int    bestLag = 0;
        double bestAbs = -1.0;
        double bestVal = 0.0;

        // First pass: the strict global maximum. Using a strict '>' keeps the
        // local peak intact (its neighbouring bins stay available for the
        // parabolic sub-sample refinement below) and is bit-for-bit
        // deterministic on identical input.
        for (int d = -maxLag; d <= maxLag; ++d)
        {
            const int idx = (d >= 0) ? d : (fftSize + d);
            const double c = (double) fftA[idx] / norm;
            const double m = std::abs (c);
            if (m > bestAbs)
            {
                bestAbs = m;
                bestVal = c;
                bestLag = d;
            }
        }

        // Second pass: deterministic tie-break between GENUINELY DISTINCT peaks
        // (separated from the incumbent by more than the parabolic window). When
        // a competing peak is within 2% relative magnitude, prefer (a) a
        // non-inverted polarity, then (b) the smaller |lag|. This stops
        // invert/delay recommendations flip-flopping between near-equal peaks on
        // the same loop, without perturbing the local peak used for refinement.
        constexpr int distinctPeakSeparation = 2;
        for (int d = -maxLag; d <= maxLag; ++d)
        {
            if (std::abs (d - bestLag) <= distinctPeakSeparation)
                continue;

            const int idx = (d >= 0) ? d : (fftSize + d);
            const double c = (double) fftA[idx] / norm;
            const double m = std::abs (c);
            if (m < bestAbs * 0.98)
                continue;

            const bool candNonInverted = c >= 0.0;
            const bool bestNonInverted = bestVal >= 0.0;
            const bool preferCandidate =
                (candNonInverted && ! bestNonInverted)
                || (candNonInverted == bestNonInverted && std::abs (d) < std::abs (bestLag));

            if (preferCandidate)
            {
                bestAbs = m;
                bestVal = c;
                bestLag = d;
            }
        }

        // Parabolic sub-sample refinement
        double refinedLag = (double) bestLag;
        if (bestLag > -maxLag && bestLag < maxLag)
        {
            const int idxM = (bestLag - 1 >= 0) ? (bestLag - 1) : (fftSize + bestLag - 1);
            const int idxP = (bestLag + 1 >= 0) ? (bestLag + 1) : (fftSize + bestLag + 1);
            const double ym1 = std::abs ((double) fftA[idxM] / norm);
            const double y0  = bestAbs;
            const double yp1 = std::abs ((double) fftA[idxP] / norm);
            const double denom = ym1 - 2.0 * y0 + yp1;
            if (std::abs (denom) > 1.0e-12)
                refinedLag += 0.5 * (ym1 - yp1) / denom;
        }

        result.valid          = true;
        // X * conj(Y) flips the lag axis, so we negate refinedLag to keep a positive delay meaning 'kick is late'.
        result.delayMs        = (float) (-refinedLag / sampleRate * 1000.0);
        result.delayMs        = std::clamp (result.delayMs, -maxDelayMs, maxDelayMs);
        result.invertPolarity = bestVal < 0.0;
        result.beforeMatch    = toPercent (r0);
        result.afterMatch     = toPercent (std::abs (bestVal));

        // --- Phase-rotator search ---
        const int   intLag = (int) std::llround (refinedLag);
        const float sign   = result.invertPolarity ? -1.0f : 1.0f;

        const int nStart     = std::max (0, -intLag);
        const int nEnd       = std::min (window, window - intLag);
        const int alignedLen = nEnd - nStart;

        if (alignedLen > 64)
        {
            std::vector<float> kickAligned ((size_t) alignedLen);
            for (int i = 0; i < alignedLen; ++i)
                kickAligned[(size_t) i] = b[(size_t) (nStart + i + intLag)];

            std::vector<float> bassBase ((size_t) alignedLen);
            for (int i = 0; i < alignedLen; ++i)
                bassBase[(size_t) i] = sign * a[(size_t) (nStart + i)];

            const double baseAbs = std::abs (findPeakFFT (bassBase.data(), kickAligned.data(),
                                                           alignedLen, 1).signed_);

            double bestRotAbs = baseAbs;

            // A rotator is only worth enabling if it beats the plain
            // delay+polarity baseline by a musically-meaningful margin. A tiny
            // 1e-4 correlation gain (the old bar) turned the phase filter on for
            // no audible reason. Require +0.03 absolute correlation (~3 match
            // points) over baseline before committing to it, while still using a
            // small epsilon to break ties among rotator candidates.
            constexpr double rotatorEnableGain = 0.03;
            constexpr double rotatorTieEpsilon = 1.0e-4;

            const float freqCandidates[]  = { 40.0f, 60.0f, 80.0f, 100.0f, 140.0f,
                                               200.0f, 250.0f, 350.0f, 500.0f };
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

                        const double m = std::abs (findPeakFFT (trial.data(), kickAligned.data(),
                                                                 alignedLen, 1).signed_);
                        if (m > bestRotAbs + rotatorTieEpsilon)
                        {
                            bestRotAbs           = m;
                            result.rotatorFreqHz = freq;
                            result.rotatorQ      = qv;
                            result.rotatorStages = stages;
                        }
                    }
                }
            }

            // Commit to the rotator only if the best candidate cleared the gain
            // bar over the non-rotator baseline.
            if (bestRotAbs >= baseAbs + rotatorEnableGain)
            {
                result.adjustRotator = true;
                result.afterMatch = toPercent (bestRotAbs);
            }
        }

        return result;
    }

private:
    static int nextPow2Order (int minSize)
    {
        int order = 0;
        while ((1 << order) < minSize)
            ++order;
        return order;
    }

    struct Peak
    {
        double signed_ = 0.0;
        double abs_    = 0.0;
        int    lag     = 0;
    };

    // FFT-based peak finder for short aligned segments (rotator search).
    // maxLag is typically 1 for the rotator search (already coarse-aligned).
    static Peak findPeakFFT (const float* a, const float* b, int window, int maxLag)
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

        // For very small maxLag (rotator search, maxLag=1), direct computation
        // is cheaper than FFT overhead. Use FFT only when lag search is large.
        if (maxLag <= 4)
        {
            for (int d = -maxLag; d <= maxLag; ++d)
            {
                const int nStart = std::max (0, -d);
                const int nEnd   = std::min (window, window - d);
                double sum = 0.0;
                for (int n = nStart; n < nEnd; ++n)
                    sum += (double) a[n] * (double) b[n + d];

                const double c = sum / norm;
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

        // Full FFT cross-correlation for larger lag searches
        const int fftOrder = nextPow2Order (2 * window);
        const int fftSize  = 1 << fftOrder;

        juce::dsp::FFT fft (fftOrder);

        std::vector<float> fftA (2 * (size_t) fftSize, 0.0f);
        std::vector<float> fftB (2 * (size_t) fftSize, 0.0f);

        for (int i = 0; i < window; ++i)
        {
            fftA[i] = a[i];
            fftB[i] = b[i];
        }

        fft.performRealOnlyForwardTransform (fftA.data(), true);
        fft.performRealOnlyForwardTransform (fftB.data(), true);

        for (int i = 0; i <= fftSize / 2; ++i)
        {
            const float ar = fftA[2 * i];
            const float ai = fftA[2 * i + 1];
            const float br = fftB[2 * i];
            const float bi = fftB[2 * i + 1];
            fftA[2 * i]     = ar * br + ai * bi;
            fftA[2 * i + 1] = ai * br - ar * bi;
        }

        fft.performRealOnlyInverseTransform (fftA.data());

        for (int d = -maxLag; d <= maxLag; ++d)
        {
            const int idx = (d >= 0) ? d : (fftSize + d);
            const double c = (double) fftA[idx] / norm;
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

    static void applyAllpassCascade (std::vector<float>& x, double fs,
                                     float freqHz, float q, int stages)
    {
        const double n      = 1.0 / std::tan (Biquad::kPi * (double) freqHz / fs);
        const double nSq    = n * n;
        const double invQ   = 1.0 / (double) q;
        const double c1     = 1.0 / (1.0 + invQ * n + nSq);
        const double b0     = c1 * (1.0 - n * invQ + nSq);
        const double b1     = c1 * 2.0 * (1.0 - nSq);

        const int len = (int) x.size();
        for (int s = 0; s < stages; ++s)
        {
            double z1 = 0.0, z2 = 0.0;
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

    static float toPercent (double r)
    {
        r = std::clamp (r, -1.0, 1.0);
        return (float) ((r + 1.0) * 50.0);
    }

    static void bandPass (std::vector<float>& x, double fs, float lowHz, float highHz)
    {
        const double q = 0.70710678;
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

    static void applyForwardBack (std::vector<float>& x, const Biquad& c)
    {
        const int n = (int) x.size();

        double z1 = 0.0, z2 = 0.0;
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
