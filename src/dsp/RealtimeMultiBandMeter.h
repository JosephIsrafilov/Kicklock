#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include "PhaseBands.h"

// Realtime multi-band phase-match meter (P5, live path).
//
// Runs on the audio thread like CorrelationMeter: prepare() allocates, then
// pushSample()/reset() never allocate, lock, or block. For each of the shared
// PhaseBands, both bass and kick are passed through the SAME causal band-pass
// (HP then LP biquad) so in-band relative phase is preserved, then a rolling
// Pearson correlation over the last window is maintained with running sums
// (O(numBands) per sample). Getters blend the per-band correlations with the
// shared decision weights, exactly like the offline MultiBandCorrelation, so
// the live meter and the analyzer agree on band definitions and weighting.
//
// The live numbers still differ from the offline analyzer because this filter
// is causal (has group delay) while the offline one is zero-phase filtfilt;
// that is expected and the UI labels the two distinctly (see P8).
class RealtimeMultiBandMeter
{
public:
    void prepare (double newSampleRate, int windowSizeSamples)
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        windowSize = windowSizeSamples > 0 ? windowSizeSamples : 1;

        for (int b = 0; b < numBands; ++b)
        {
            auto& band = bands[(size_t) b];
            band.hp.makeHighPass (sampleRate, PhaseBands::table[(size_t) b].lowHz, butterworthQ);
            band.lp.makeLowPass (sampleRate, PhaseBands::table[(size_t) b].highHz, butterworthQ);
            band.weight = PhaseBands::table[(size_t) b].weight;
            band.active = (double) PhaseBands::table[(size_t) b].highHz < sampleRate * 0.5;

            band.bufA.assign ((size_t) windowSize, 0.0f);
            band.bufB.assign ((size_t) windowSize, 0.0f);
        }

        reset();
    }

    void reset()
    {
        for (auto& band : bands)
        {
            band.hpStateBassZ1 = band.hpStateBassZ2 = 0.0f;
            band.lpStateBassZ1 = band.lpStateBassZ2 = 0.0f;
            band.hpStateKickZ1 = band.hpStateKickZ2 = 0.0f;
            band.lpStateKickZ1 = band.lpStateKickZ2 = 0.0f;

            band.sumAB = band.sumA2 = band.sumB2 = 0.0;
            band.writeIndex = 0;
            band.numValid = 0;

            std::fill (band.bufA.begin(), band.bufA.end(), 0.0f);
            std::fill (band.bufB.begin(), band.bufB.end(), 0.0f);
        }
    }

    // Audio thread. O(numBands), no allocation.
    void pushSample (float bass, float kick) noexcept
    {
        for (auto& band : bands)
        {
            if (! band.active || band.bufA.empty())
                continue;

            const float a = band.lp.process (band.hp.process (bass,
                                                    band.hpStateBassZ1, band.hpStateBassZ2),
                                                  band.lpStateBassZ1, band.lpStateBassZ2);
            const float b = band.lp.process (band.hp.process (kick,
                                                    band.hpStateKickZ1, band.hpStateKickZ2),
                                                  band.lpStateKickZ1, band.lpStateKickZ2);

            if (band.numValid >= windowSize)
            {
                const float oldA = band.bufA[(size_t) band.writeIndex];
                const float oldB = band.bufB[(size_t) band.writeIndex];
                band.sumAB -= (double) oldA * (double) oldB;
                band.sumA2 -= (double) oldA * (double) oldA;
                band.sumB2 -= (double) oldB * (double) oldB;
            }
            else
            {
                ++band.numValid;
            }

            band.bufA[(size_t) band.writeIndex] = a;
            band.bufB[(size_t) band.writeIndex] = b;
            band.sumAB += (double) a * (double) b;
            band.sumA2 += (double) a * (double) a;
            band.sumB2 += (double) b * (double) b;
            band.writeIndex = (band.writeIndex + 1) % windowSize;
        }
    }

    static constexpr int getNumBands() noexcept { return numBands; }

    float getBandMatchPercent (int bandIndex) const noexcept
    {
        if (bandIndex < 0 || bandIndex >= numBands)
            return 50.0f;

        double corr = 0.0;
        if (! bandCorrelation (bands[(size_t) bandIndex], corr))
            return 50.0f;

        return toPercent (corr);
    }

    // Low-end-weighted overall match (SUB + LOW dominate). Main live meter.
    float getWeightedMatchPercent() const noexcept
    {
        double sum = 0.0, weight = 0.0;
        for (int b = 0; b < numBands; ++b)
        {
            double corr = 0.0, conf = 0.0;
            if (! bandCorrelation (bands[(size_t) b], corr, &conf))
                continue;

            const double w = (double) bands[(size_t) b].weight * conf;
            sum += corr * w;
            weight += w;
        }

        return weight > 1.0e-9 ? toPercent (sum / weight) : 50.0f;
    }

    // SUB + LOW only.
    float getLowEndMatchPercent() const noexcept
    {
        double sum = 0.0, weight = 0.0;
        for (int b = 0; b < PhaseBands::lowEndBandCount && b < numBands; ++b)
        {
            double corr = 0.0, conf = 0.0;
            if (! bandCorrelation (bands[(size_t) b], corr, &conf))
                continue;

            sum += corr * conf;
            weight += conf;
        }

        return weight > 1.0e-9 ? toPercent (sum / weight) : 50.0f;
    }

    // Energy-only blend across the full span (no low-end bias).
    float getBroadbandMatchPercent() const noexcept
    {
        double sum = 0.0, weight = 0.0;
        for (int b = 0; b < numBands; ++b)
        {
            double corr = 0.0, conf = 0.0;
            if (! bandCorrelation (bands[(size_t) b], corr, &conf))
                continue;

            sum += corr * conf;
            weight += conf;
        }

        return weight > 1.0e-9 ? toPercent (sum / weight) : 50.0f;
    }

private:
    static constexpr int numBands = PhaseBands::numBands;
    static constexpr double butterworthQ = 0.70710678;

    struct BiquadCoeffs
    {
        static constexpr double pi = 3.14159265358979323846;
        float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;

        void makeLowPass (double fs, double f, double q)
        {
            const double w0 = 2.0 * pi * f / fs;
            const double cw = std::cos (w0), sw = std::sin (w0);
            const double alpha = sw / (2.0 * q);
            const double a0 = 1.0 + alpha;
            b0 = (float) (((1.0 - cw) * 0.5) / a0);
            b1 = (float) ((1.0 - cw) / a0);
            b2 = (float) (((1.0 - cw) * 0.5) / a0);
            a1 = (float) ((-2.0 * cw) / a0);
            a2 = (float) ((1.0 - alpha) / a0);
        }

        void makeHighPass (double fs, double f, double q)
        {
            const double w0 = 2.0 * pi * f / fs;
            const double cw = std::cos (w0), sw = std::sin (w0);
            const double alpha = sw / (2.0 * q);
            const double a0 = 1.0 + alpha;
            b0 = (float) (((1.0 + cw) * 0.5) / a0);
            b1 = (float) (-(1.0 + cw) / a0);
            b2 = (float) (((1.0 + cw) * 0.5) / a0);
            a1 = (float) ((-2.0 * cw) / a0);
            a2 = (float) ((1.0 - alpha) / a0);
        }

        // Transposed direct form II, one call per (filter, signal) using that
        // pair's own z-state.
        float process (float in, float& z1, float& z2) const noexcept
        {
            const float out = b0 * in + z1;
            z1 = b1 * in - a1 * out + z2;
            z2 = b2 * in - a2 * out;
            return out;
        }
    };

    struct BandState
    {
        BiquadCoeffs hp, lp;
        float weight = 0.0f;
        bool active = true;

        float hpStateBassZ1 = 0.0f, hpStateBassZ2 = 0.0f;
        float lpStateBassZ1 = 0.0f, lpStateBassZ2 = 0.0f;
        float hpStateKickZ1 = 0.0f, hpStateKickZ2 = 0.0f;
        float lpStateKickZ1 = 0.0f, lpStateKickZ2 = 0.0f;

        std::vector<float> bufA, bufB;
        double sumAB = 0.0, sumA2 = 0.0, sumB2 = 0.0;
        int writeIndex = 0;
        int numValid = 0;
    };

    // Pearson r over the band's rolling window (mean-agnostic: the band-passed
    // signals are already ~zero-mean). Returns false when the band is silent.
    static bool bandCorrelation (const BandState& band, double& corrOut, double* confOut = nullptr) noexcept
    {
        if (! band.active || band.numValid <= 0)
            return false;

        const double norm = std::sqrt (band.sumA2 * band.sumB2);
        if (norm <= 1.0e-12)
            return false;

        corrOut = std::clamp (band.sumAB / norm, -1.0, 1.0);

        if (confOut != nullptr)
            *confOut = std::clamp (norm / (double) band.numValid * 200.0, 0.0, 1.0);

        return true;
    }

    static float toPercent (double r) noexcept
    {
        return (float) ((std::clamp (r, -1.0, 1.0) + 1.0) * 50.0);
    }

    double sampleRate = 44100.0;
    int windowSize = 1;
    std::array<BandState, numBands> bands;
};
