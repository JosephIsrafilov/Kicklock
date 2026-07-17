#pragma once

#include <cmath>

#include "DynamicStateMap.h"

// Pure Dynamic allpass coordinate and coefficient math. This intentionally
// does not depend on JUCE DSP coefficient storage or processor state.
struct DynamicAllpassCoefficients
{
    double b0 = 0.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

struct DynamicAllpassPoleCoordinates
{
    double log2FrequencyHz = 0.0;
    double logPoleDamping = 0.0;
};

struct DynamicAllpassFrequencyQ
{
    double frequencyHz = 0.0;
    double q = 0.0;
};

namespace DynamicAllpassPoleDomain
{
    inline constexpr double kTwoPi = 6.283185307179586476925286766559;

    inline bool isFinite (double value) noexcept
    {
        return std::isfinite (value);
    }

    inline bool isValidSampleRate (double sampleRate) noexcept
    {
        return isFinite (sampleRate)
            && sampleRate > 2.0 * (double) DynamicStateMapContract::kAllpassFrequencyMaxHz;
    }

    inline bool isValidFrequency (double frequencyHz) noexcept
    {
        return isFinite (frequencyHz)
            && frequencyHz >= (double) DynamicStateMapContract::kAllpassFrequencyMinHz
            && frequencyHz <= (double) DynamicStateMapContract::kAllpassFrequencyMaxHz;
    }

    inline bool isValidQ (double q) noexcept
    {
        return isFinite (q)
            && q >= (double) DynamicStateMapContract::kAllpassQMin
            && q <= (double) DynamicStateMapContract::kAllpassQMax;
    }

    inline bool isValidCoefficients (const DynamicAllpassCoefficients& coefficients) noexcept
    {
        const double radiusSquared = coefficients.a2;
        return isFinite (coefficients.b0) && isFinite (coefficients.b1)
            && isFinite (coefficients.b2) && isFinite (coefficients.a1)
            && isFinite (coefficients.a2) && radiusSquared > 0.0 && radiusSquared < 1.0;
    }

    // Inverse log/exp chains can land a few ULPs outside the frozen production
    // envelope even when intermediates are valid. Snap only within that epsilon.
    inline bool snapToProductionFrequencyQ (double& frequencyHz, double& q) noexcept
    {
        constexpr double kSnapEpsilon = 1.0e-9;
        const double frequencyMin = (double) DynamicStateMapContract::kAllpassFrequencyMinHz;
        const double frequencyMax = (double) DynamicStateMapContract::kAllpassFrequencyMaxHz;
        const double qMin = (double) DynamicStateMapContract::kAllpassQMin;
        const double qMax = (double) DynamicStateMapContract::kAllpassQMax;

        if (! isFinite (frequencyHz) || ! isFinite (q))
            return false;
        if (frequencyHz < frequencyMin - kSnapEpsilon || frequencyHz > frequencyMax + kSnapEpsilon
            || q < qMin - kSnapEpsilon || q > qMax + kSnapEpsilon)
            return false;

        if (frequencyHz < frequencyMin)
            frequencyHz = frequencyMin;
        else if (frequencyHz > frequencyMax)
            frequencyHz = frequencyMax;

        if (q < qMin)
            q = qMin;
        else if (q > qMax)
            q = qMax;

        return isValidFrequency (frequencyHz) && isValidQ (q);
    }

    inline bool frequencyQToPoleCoordinates (double frequencyHz,
                                             double q,
                                             double sampleRate,
                                             DynamicAllpassPoleCoordinates& coordinates) noexcept
    {
        if (! isValidFrequency (frequencyHz) || ! isValidQ (q) || ! isValidSampleRate (sampleRate))
            return false;

        const double w0 = kTwoPi * frequencyHz / sampleRate;
        const double sine = std::sin (w0);
        const double alpha = sine / (2.0 * q);
        if (! isFinite (w0) || ! isFinite (sine) || ! isFinite (alpha)
            || alpha <= 0.0 || alpha >= 1.0)
            return false;

        const double radiusSquared = (1.0 - alpha) / (1.0 + alpha);
        if (! isFinite (radiusSquared) || radiusSquared <= 0.0 || radiusSquared >= 1.0)
            return false;

        const double radius = std::sqrt (radiusSquared);
        const double poleDamping = -std::log (radius);
        const double logPoleDamping = std::log (poleDamping);
        const double log2FrequencyHz = std::log2 (frequencyHz);
        if (! isFinite (radius) || radius <= 0.0 || radius >= 1.0
            || ! isFinite (poleDamping) || poleDamping <= 0.0
            || ! isFinite (logPoleDamping) || ! isFinite (log2FrequencyHz))
            return false;

        coordinates = { log2FrequencyHz, logPoleDamping };
        return true;
    }

    inline bool poleCoordinatesToFrequencyQ (double log2FrequencyHz,
                                             double logPoleDamping,
                                             double sampleRate,
                                             DynamicAllpassFrequencyQ& frequencyQ) noexcept
    {
        if (! isFinite (log2FrequencyHz) || ! isFinite (logPoleDamping)
            || ! isValidSampleRate (sampleRate))
            return false;

        double frequencyHz = std::exp2 (log2FrequencyHz);
        const double poleDamping = std::exp (logPoleDamping);
        const double radius = std::exp (-poleDamping);
        const double radiusSquared = radius * radius;
        if (! isFinite (frequencyHz) || ! isFinite (poleDamping) || poleDamping <= 0.0
            || ! isFinite (radius) || radius <= 0.0 || radius >= 1.0
            || ! isFinite (radiusSquared) || radiusSquared <= 0.0 || radiusSquared >= 1.0)
            return false;

        const double alpha = (1.0 - radiusSquared) / (1.0 + radiusSquared);
        const double w0 = kTwoPi * frequencyHz / sampleRate;
        const double sine = std::sin (w0);
        double q = sine / (2.0 * alpha);
        if (! isFinite (alpha) || alpha <= 0.0 || alpha >= 1.0
            || ! isFinite (w0) || ! isFinite (sine) || sine <= 0.0 || ! isFinite (q)
            || ! snapToProductionFrequencyQ (frequencyHz, q))
            return false;

        frequencyQ = { frequencyHz, q };
        return true;
    }

    inline bool makeCoefficients (double frequencyHz,
                                  double q,
                                  double sampleRate,
                                  DynamicAllpassCoefficients& coefficients) noexcept
    {
        if (! isValidFrequency (frequencyHz) || ! isValidQ (q) || ! isValidSampleRate (sampleRate))
            return false;

        const double w0 = kTwoPi * frequencyHz / sampleRate;
        const double alpha = std::sin (w0) / (2.0 * q);
        const double denominator = 1.0 + alpha;
        if (! isFinite (w0) || ! isFinite (alpha) || alpha <= 0.0 || alpha >= 1.0
            || ! isFinite (denominator) || denominator <= 0.0)
            return false;

        const double a1 = -2.0 * std::cos (w0) / denominator;
        const double a2 = (1.0 - alpha) / denominator;
        const DynamicAllpassCoefficients result { a2, a1, 1.0, a1, a2 };
        if (! isValidCoefficients (result))
            return false;

        coefficients = result;
        return true;
    }

    inline bool getLogPoleDampingEnvelope (double frequencyHz,
                                            double sampleRate,
                                            double& minimumLogPoleDamping,
                                            double& maximumLogPoleDamping) noexcept
    {
        DynamicAllpassPoleCoordinates highestQ;
        DynamicAllpassPoleCoordinates lowestQ;
        if (! frequencyQToPoleCoordinates (frequencyHz,
                                            (double) DynamicStateMapContract::kAllpassQMax,
                                            sampleRate,
                                            highestQ)
            || ! frequencyQToPoleCoordinates (frequencyHz,
                                               (double) DynamicStateMapContract::kAllpassQMin,
                                               sampleRate,
                                               lowestQ)
            || highestQ.logPoleDamping > lowestQ.logPoleDamping)
            return false;

        minimumLogPoleDamping = highestQ.logPoleDamping;
        maximumLogPoleDamping = lowestQ.logPoleDamping;
        return true;
    }
}
