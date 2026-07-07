#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

#include "PhaseBands.h"
#include "SubLossMeter.h"

// Realtime multi-band phase-match meter (P5, live path).
//
// Runs on the audio thread like CorrelationMeter: prepare() allocates, then
// pushSample()/reset() never allocate, lock, or block. For each of the shared
// PhaseBands, both bass and kick are passed through the SAME causal band-pass
// (HP then LP biquad) so in-band relative phase is preserved, then a one-pole
// EMA of the covariance/variance products (sumAB/sumA2/sumB2) is maintained
// with a window-length time constant (O(numBands) per sample, no history
// buffers). Getters blend the confident bands weighting each by its RELATIVE
// energy share of the total (P3), optionally times the shared decision weight,
// so the live meter and the analyzer agree on band definitions and weighting
// and a quiet-but-aligned pair still reads correctly instead of pinning to 50%.
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

        // EMA coefficients hoisted out of pushSample: computing
        // 1 - exp(-1/window) PER SAMPLE was a transcendental on the audio
        // thread 48k+ times a second (x2 meter instances) for a value that
        // only ever changes here.
        emaAlpha = 1.0 - std::exp (-1.0 / (double) windowSize);
        emaBeta = 1.0 - emaAlpha;

        for (int b = 0; b < numBands; ++b)
        {
            auto& band = bands[(size_t) b];
            band.hp.makeHighPass (sampleRate, PhaseBands::table[(size_t) b].lowHz, butterworthQ);
            band.lp.makeLowPass (sampleRate, PhaseBands::table[(size_t) b].highHz, butterworthQ);
            band.weight = PhaseBands::table[(size_t) b].weight;
            band.active = (double) PhaseBands::table[(size_t) b].highHz < sampleRate * 0.5;
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
            band.numValid = 0;
        }
    }

    // Audio thread. O(numBands), no allocation, no transcendentals.
    void pushSample (float bass, float kick) noexcept
    {
        if (windowSize <= 0) return;
        const double alpha = emaAlpha;
        const double beta = emaBeta;

        for (auto& band : bands)
        {
            if (! band.active)
                continue;

            const double a = band.lp.process (band.hp.process ((double)bass,
                                                    band.hpStateBassZ1, band.hpStateBassZ2),
                                                  band.lpStateBassZ1, band.lpStateBassZ2);
            const double b = band.lp.process (band.hp.process ((double)kick,
                                                    band.hpStateKickZ1, band.hpStateKickZ2),
                                                  band.lpStateKickZ1, band.lpStateKickZ2);

            band.sumAB = alpha * (a * b) + beta * band.sumAB;
            band.sumA2 = alpha * (a * a) + beta * band.sumA2;
            band.sumB2 = alpha * (b * b) + beta * band.sumB2;

            if (band.numValid < windowSize)
                ++band.numValid;
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
        return blend (0, numBands, /*useDecisionWeight*/ true);
    }

    // SUB + LOW only.
    float getLowEndMatchPercent() const noexcept
    {
        return blend (0, juce::jmin (PhaseBands::lowEndBandCount, numBands), /*useDecisionWeight*/ false);
    }

    // Energy-only blend across the full span (no low-end bias).
    float getBroadbandMatchPercent() const noexcept
    {
        return blend (0, numBands, /*useDecisionWeight*/ false);
    }

    // Live sub/low-end loss, in dB, versus the best physically achievable
    // alignment (correlation -> +1). Aggregates the SUB+LOW bands' own running
    // energy sums into one pair-of-signals loss estimate (see SubLossMeter.h) —
    // treating the two bands' content as one combined low-end signal, which is
    // the same approximation getLowEndMatchPercent() already makes when it
    // blends them by relative energy share. Returns 0 (no loss) while there is
    // no usable low-end signal, matching the other getters' silent-is-neutral
    // convention.
    float getLowEndSubLossDb() const noexcept
    {
        double energyA = 0.0, energyB = 0.0, crossAB = 0.0;
        bool any = false;

        for (int b = 0; b < std::min (PhaseBands::lowEndBandCount, numBands); ++b)
        {
            const auto& band = bands[(size_t) b];
            if (! band.active || band.numValid <= 0)
                continue;

            energyA += band.sumA2;
            energyB += band.sumB2;
            crossAB += band.sumAB;
            any = true;
        }

        if (! any || energyA * energyB <= energyGateFloor * energyGateFloor)
            return 0.0f;

        const double norm = std::sqrt (energyA * energyB);
        const double correlationNow = std::clamp (crossAB / norm, -1.0, 1.0);

        return subLossDb (energyA, energyB, correlationNow, 1.0);
    }

    // True while any band carries enough joint energy for the correlation to
    // mean anything. When this is false every getter returns a neutral 50% —
    // and a UI printing "50%" over silence reads as "half aligned", which is
    // misleading; it should show "no signal" instead.
    bool hasSignal() const noexcept
    {
        double totalEnergy = 0.0;

        for (const auto& band : bands)
        {
            double corr = 0.0, energy = 0.0;
            if (bandCorrelation (band, corr, &energy))
                totalEnergy += energy;
        }

        return totalEnergy > energyGateFloor;
    }

private:
    static constexpr int numBands = PhaseBands::numBands;
    static constexpr double butterworthQ = 0.70710678;
    // -60 dBFS on the joint (geometric-mean) band energy. Below this the pair is
    // effectively silent and every read-out is a neutral 50%.
    static constexpr double energyGateFloor = 1.0e-6;

    struct BiquadCoeffs
    {
        static constexpr double pi = 3.14159265358979323846;
        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

        void makeLowPass (double fs, double f, double q)
        {
            const double w0 = 2.0 * pi * f / fs;
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
            const double w0 = 2.0 * pi * f / fs;
            const double cw = std::cos (w0), sw = std::sin (w0);
            const double alpha = sw / (2.0 * q);
            const double a0 = 1.0 + alpha;
            b0 = ((1.0 + cw) * 0.5) / a0;
            b1 = -(1.0 + cw) / a0;
            b2 = ((1.0 + cw) * 0.5) / a0;
            a1 = (-2.0 * cw) / a0;
            a2 = (1.0 - alpha) / a0;
        }

        // Transposed direct form II, one call per (filter, signal) using that
        // pair's own z-state.
        double process (double in, double& z1, double& z2) const noexcept
        {
            const double out = b0 * in + z1;
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

        double hpStateBassZ1 = 0.0, hpStateBassZ2 = 0.0;
        double lpStateBassZ1 = 0.0, lpStateBassZ2 = 0.0;
        double hpStateKickZ1 = 0.0, hpStateKickZ2 = 0.0;
        double lpStateKickZ1 = 0.0, lpStateKickZ2 = 0.0;

        double sumAB = 0.0, sumA2 = 0.0, sumB2 = 0.0;
        int numValid = 0;
    };

    // Pearson r over the band's rolling EMA window (mean-agnostic: the
    // band-passed signals are already ~zero-mean). Returns false when the band
    // is silent. energyOut (optional) receives the band's joint energy - the
    // geometric mean of the two channels' band energies - for relative-share
    // confidence weighting.
    static bool bandCorrelation (const BandState& band, double& corrOut,
                                 double* energyOut = nullptr) noexcept
    {
        if (! band.active || band.numValid <= 0)
            return false;

        const double denomSq = band.sumA2 * band.sumB2;
        if (denomSq <= 1.0e-12)
            return false;

        const double norm = std::sqrt (std::max (0.0, denomSq));
        if (norm <= 1.0e-12)
            return false;

        corrOut = std::clamp (band.sumAB / norm, -1.0, 1.0);

        if (energyOut != nullptr)
            *energyOut = norm; // sqrt(E_a * E_b): joint band energy

        return true;
    }

    // P3 confidence: blend the confident bands weighting each by its RELATIVE
    // energy share of the total across bands (optionally times the decision
    // weight), not by an absolute level. This is what makes a quiet-but-aligned
    // pair still read correctly - a -30 dBFS aligned bass no longer pins the
    // display at 50%. An absolute -60 dBFS gate on the summed energy keeps
    // genuine silence neutral.
    float blend (int firstBand, int lastBand, bool useDecisionWeight) const noexcept
    {
        double bandCorr[numBands] = {};
        double bandEnergy[numBands] = {};
        bool   bandOk[numBands] = {};
        double totalEnergy = 0.0;

        for (int b = firstBand; b < lastBand; ++b)
        {
            double corr = 0.0, energy = 0.0;
            bandOk[b] = bandCorrelation (bands[(size_t) b], corr, &energy);
            if (! bandOk[b])
                continue;

            bandCorr[b] = corr;
            bandEnergy[b] = energy;
            totalEnergy += energy;
        }

        if (totalEnergy <= energyGateFloor)
            return 50.0f;

        double sum = 0.0, weight = 0.0;
        for (int b = firstBand; b < lastBand; ++b)
        {
            if (! bandOk[b])
                continue;

            const double share = bandEnergy[b] / totalEnergy; // relative, 0..1
            const double w = useDecisionWeight ? (double) bands[(size_t) b].weight * share
                                               : share;
            sum += bandCorr[b] * w;
            weight += w;
        }

        return weight > 1.0e-9 ? toPercent (sum / weight) : 50.0f;
    }

    static float toPercent (double r) noexcept
    {
        return (float) ((std::clamp (r, -1.0, 1.0) + 1.0) * 50.0);
    }

    double sampleRate = 44100.0;
    int windowSize = 1;
    double emaAlpha = 0.0;
    double emaBeta = 1.0;
    std::array<BandState, numBands> bands;
};
