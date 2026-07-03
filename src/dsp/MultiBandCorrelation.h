#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <vector>

struct BandCorrelationResult
{
    float lowHz = 0.0f;
    float highHz = 0.0f;
    float bassEnergy = 0.0f;
    float kickEnergy = 0.0f;
    float correlation = 0.0f;
    float matchPercent = 50.0f;
    float confidence = 0.0f;
};

struct MultiBandCorrelationResult
{
    static constexpr int numBands = 4;

    std::array<BandCorrelationResult, numBands> bands {};
    float weightedCorrelation = 0.0f;
    float weightedMatchPercent = 50.0f;
    float confidence = 0.0f;
};

class MultiBandCorrelation
{
public:
    static MultiBandCorrelationResult analyze (const float* bass,
                                               const float* kick,
                                               int numSamples,
                                               double sampleRate)
    {
        MultiBandCorrelationResult result;

        constexpr std::array<std::pair<float, float>, MultiBandCorrelationResult::numBands> bands {{
            { 30.0f, 50.0f },
            { 50.0f, 80.0f },
            { 80.0f, 120.0f },
            { 120.0f, 200.0f }
        }};

        if (bass == nullptr || kick == nullptr || numSamples <= 8 || sampleRate <= 0.0)
        {
            for (int i = 0; i < MultiBandCorrelationResult::numBands; ++i)
            {
                result.bands[(size_t) i].lowHz = bands[(size_t) i].first;
                result.bands[(size_t) i].highHz = bands[(size_t) i].second;
            }
            return result;
        }

        double weightedSum = 0.0;
        double weightSum = 0.0;

        for (int band = 0; band < MultiBandCorrelationResult::numBands; ++band)
        {
            auto& out = result.bands[(size_t) band];
            out.lowHz = bands[(size_t) band].first;
            out.highHz = bands[(size_t) band].second;

            std::vector<float> a (bass, bass + numSamples);
            std::vector<float> b (kick, kick + numSamples);
            bandPass (a, sampleRate, out.lowHz, out.highHz);
            bandPass (b, sampleRate, out.lowHz, out.highHz);

            double sumAB = 0.0;
            double sumA2 = 0.0;
            double sumB2 = 0.0;

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
            const double energyWeight = std::sqrt (sumA2 * sumB2) / (double) numSamples;

            out.correlation = (float) corr;
            out.matchPercent = toPercent (corr);
            out.confidence = (float) std::clamp (energyWeight * 200.0, 0.0, 1.0);

            const double weight = (double) out.confidence;
            weightedSum += corr * weight;
            weightSum += weight;
        }

        if (weightSum > 1.0e-9)
        {
            result.weightedCorrelation = (float) std::clamp (weightedSum / weightSum, -1.0, 1.0);
            result.weightedMatchPercent = toPercent (result.weightedCorrelation);
            result.confidence = (float) std::clamp (weightSum / (double) MultiBandCorrelationResult::numBands, 0.0, 1.0);
        }

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
