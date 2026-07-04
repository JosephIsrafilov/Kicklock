#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "PhaseBands.h"

struct BandCorrelationResult
{
    float lowHz = 0.0f;
    float highHz = 0.0f;
    float weight = 0.0f;       // decision weight from the shared band plan
    float bassEnergy = 0.0f;
    float kickEnergy = 0.0f;
    float correlation = 0.0f;
    float matchPercent = 50.0f;
    float confidence = 0.0f;   // energy-based reliability of this band
};

struct MultiBandCorrelationResult
{
    static constexpr int numBands = PhaseBands::numBands;

    std::array<BandCorrelationResult, numBands> bands {};

    // Low-end-weighted overall score (SUB + LOW dominate).
    // This is the number that drives decisions and the main Phase Match meter.
    float weightedCorrelation = 0.0f;
    float weightedMatchPercent = 50.0f;

    // SUB + LOW only, for the "sub/low band match" read-out.
    float lowEndMatchPercent = 50.0f;

    // Even (energy-only) blend across the full 20 Hz-500 Hz span, for the broad
    // low/body read-out. Not weighted toward the low end, so it shows the wideband
    // relationship without steering decisions.
    float broadbandMatchPercent = 50.0f;

    // Most out-of-phase band among the confident ones (worst conflict), or -1.
    int worstConflictBandIndex = -1;

    float confidence = 0.0f;
};

// Offline multi-band phase/correlation (message thread; allocates freely).
// Band-passes both signals to each band with the SAME zero-phase filtfilt so
// relative phase is preserved, correlates per band, then blends the bands with
// the shared decision weights so upper-body content can never dominate the
// sub/low alignment.
class MultiBandCorrelation
{
public:
    static MultiBandCorrelationResult analyze (const float* bass,
                                               const float* kick,
                                               int numSamples,
                                               double sampleRate)
    {
        MultiBandCorrelationResult result;

        for (int i = 0; i < MultiBandCorrelationResult::numBands; ++i)
        {
            result.bands[(size_t) i].lowHz = PhaseBands::table[(size_t) i].lowHz;
            result.bands[(size_t) i].highHz = PhaseBands::table[(size_t) i].highHz;
            result.bands[(size_t) i].weight = PhaseBands::table[(size_t) i].weight;
        }

        if (bass == nullptr || kick == nullptr || numSamples <= 8 || sampleRate <= 0.0)
            return result;

        double weightedSum = 0.0;      // decision-weighted corr accumulator
        double weightSum = 0.0;
        double lowEndSum = 0.0;        // SUB + LOW only
        double lowEndWeight = 0.0;
        double broadSum = 0.0;         // energy-only across all bands
        double broadWeight = 0.0;

        float worstMatch = std::numeric_limits<float>::infinity();

        for (int band = 0; band < MultiBandCorrelationResult::numBands; ++band)
        {
            auto& out = result.bands[(size_t) band];

            // Skip bands whose high edge is above Nyquist.
            if ((double) out.highHz >= sampleRate * 0.5)
                continue;

            std::vector<float> a (bass, bass + numSamples);
            std::vector<float> b (kick, kick + numSamples);
            bandPass (a, sampleRate, out.lowHz, out.highHz);
            bandPass (b, sampleRate, out.lowHz, out.highHz);

            double sumAB = 0.0, sumA2 = 0.0, sumB2 = 0.0;
            for (int i = 0; i < numSamples; ++i)
            {
                const double av = a[(size_t) i];
                const double bv = b[(size_t) i];
                sumAB += av * bv;
                sumA2 += av * av;
                sumB2 += bv * bv;
            }

            out.bassEnergy = (float) (sumA2 / (double) numSamples);
            out.kickEnergy = (float) (sumB2 / (double) numSamples);

            const double norm = std::sqrt (sumA2 * sumB2);
            if (norm <= 1.0e-10)
                continue;

            const double corr = std::clamp (sumAB / norm, -1.0, 1.0);
            const double energyConfidence = std::clamp (norm / (double) numSamples * 200.0, 0.0, 1.0);

            out.correlation = (float) corr;
            out.matchPercent = toPercent (corr);
            out.confidence = (float) energyConfidence;

            // Overall: band decision weight * energy reliability.
            const double decisionWeight = (double) out.weight * energyConfidence;
            weightedSum += corr * decisionWeight;
            weightSum += decisionWeight;

            if (band < PhaseBands::lowEndBandCount)
            {
                lowEndSum += corr * energyConfidence;
                lowEndWeight += energyConfidence;
            }

            broadSum += corr * energyConfidence;
            broadWeight += energyConfidence;

            // Worst conflict = lowest match among sufficiently-confident bands.
            if (energyConfidence >= 0.08 && out.matchPercent < worstMatch)
            {
                worstMatch = out.matchPercent;
                result.worstConflictBandIndex = band;
            }
        }

        if (weightSum > 1.0e-9)
        {
            result.weightedCorrelation = (float) std::clamp (weightedSum / weightSum, -1.0, 1.0);
            result.weightedMatchPercent = toPercent (result.weightedCorrelation);

            // Confidence normalised by the total possible decision weight so a
            // fully-confident low end reads as high confidence.
            double maxWeight = 0.0;
            for (const auto& def : PhaseBands::table)
                maxWeight += (double) def.weight;
            result.confidence = (float) std::clamp (weightSum / maxWeight, 0.0, 1.0);
        }

        if (lowEndWeight > 1.0e-9)
            result.lowEndMatchPercent = toPercent (std::clamp (lowEndSum / lowEndWeight, -1.0, 1.0));

        if (broadWeight > 1.0e-9)
            result.broadbandMatchPercent = toPercent (std::clamp (broadSum / broadWeight, -1.0, 1.0));

        return result;
    }

private:
    static float toPercent (double r)
    {
        return (float) ((std::clamp (r, -1.0, 1.0) + 1.0) * 50.0);
    }

    static void bandPass (std::vector<float>& x, double fs, float lowHz, float highHz)
    {
        const double q = 0.70710678;
        Biquad hp; hp.makeHighPass (fs, lowHz, q);
        Biquad lp; lp.makeLowPass (fs, highHz, q);

        applyForwardBack (x, hp);
        applyForwardBack (x, lp);
    }

    struct Biquad
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
