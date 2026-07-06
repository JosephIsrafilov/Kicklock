#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <vector>

#include <juce_dsp/juce_dsp.h>

#include "FftPlanCache.h"

class FrequencyDomainPhaseRefiner
{
public:
    enum class WeightingMode
    {
        Coherence,
        PlainPhat
    };

    struct Hit
    {
        const float* bass = nullptr;
        const float* kick = nullptr;
        int numSamples = 0;
        float coarseDelaySamples = 0.0f;
        bool polarityInvert = false;
    };

    struct Result
    {
        bool valid = false;
        float medianDelaySamples = 0.0f;
        std::vector<float> delaySamples;
        std::vector<float> phaseDelaySamples;
        std::vector<float> peakDelaySamples;
        std::vector<float> phaseResidualRadians;
        std::vector<float> phaseConfidence;
        std::vector<bool> usedFallback;
    };

    static Result refine (const std::vector<Hit>& hits,
                          double sampleRate,
                          float lowHz = 30.0f,
                          float highHz = 120.0f,
                          WeightingMode mode = WeightingMode::Coherence)
    {
        Result result;

        if (hits.empty() || sampleRate <= 0.0)
            return result;

        int maxLength = 0;
        for (const auto& hit : hits)
            if (hit.bass != nullptr && hit.kick != nullptr && hit.numSamples > 32)
                maxLength = std::max (maxLength, hit.numSamples);

        if (maxLength <= 32)
            return result;

        const int fftSize = nextPowerOfTwo (2 * maxLength - 1);
        const int fftOrder = orderForSize (fftSize);
        const int numBins = fftSize / 2 + 1;
        const double binHz = sampleRate / (double) fftSize;

        auto& fft = FftPlanCache::get (fftOrder);
        std::vector<std::complex<double>> sxyByHit;
        std::vector<double> meanPxx ((size_t) numBins, 0.0);
        std::vector<double> meanPyy ((size_t) numBins, 0.0);
        std::vector<std::complex<double>> meanSxy ((size_t) numBins);
        std::vector<char> validHit;
        std::vector<float> fftBass (2 * (size_t) fftSize, 0.0f);
        std::vector<float> fftKick (2 * (size_t) fftSize, 0.0f);

        sxyByHit.reserve ((size_t) hits.size() * (size_t) numBins);
        validHit.reserve (hits.size());

        int numValidHits = 0;
        double maxCombinedPower = 0.0;

        for (const auto& hit : hits)
        {
            if (hit.bass == nullptr || hit.kick == nullptr || hit.numSamples <= 32)
            {
                validHit.push_back (0);
                sxyByHit.insert (sxyByHit.end(), (size_t) numBins, {});
                continue;
            }

            std::fill (fftBass.begin(), fftBass.end(), 0.0f);
            std::fill (fftKick.begin(), fftKick.end(), 0.0f);

            copyWindow (hit.bass, hit.numSamples, sampleRate, fftBass);
            copyWindow (hit.kick, hit.numSamples, sampleRate, fftKick);

            fft.performRealOnlyForwardTransform (fftBass.data(), true);
            fft.performRealOnlyForwardTransform (fftKick.data(), true);

            const double sign = hit.polarityInvert ? -1.0 : 1.0;
            validHit.push_back (1);
            ++numValidHits;

            for (int bin = 0; bin < numBins; ++bin)
            {
                const std::complex<double> x { fftBass[(size_t) (2 * bin)],
                                               fftBass[(size_t) (2 * bin + 1)] };
                const std::complex<double> y { fftKick[(size_t) (2 * bin)],
                                               fftKick[(size_t) (2 * bin + 1)] };
                const auto sxy = sign * x * std::conj (y);
                const double pxx = std::norm (x);
                const double pyy = std::norm (y);

                sxyByHit.push_back (sxy);
                meanSxy[(size_t) bin] += sxy;
                meanPxx[(size_t) bin] += pxx;
                meanPyy[(size_t) bin] += pyy;
                maxCombinedPower = std::max (maxCombinedPower, pxx + pyy);
            }
        }

        if (numValidHits == 0 || maxCombinedPower <= 0.0)
            return result;

        for (int bin = 0; bin < numBins; ++bin)
        {
            meanSxy[(size_t) bin] /= (double) numValidHits;
            meanPxx[(size_t) bin] /= (double) numValidHits;
            meanPyy[(size_t) bin] /= (double) numValidHits;
        }

        std::vector<double> weights ((size_t) numBins, 0.0);
        double maxBandPower = 0.0;
        for (int bin = 1; bin < numBins; ++bin)
        {
            const double hz = (double) bin * binHz;
            if (hz >= (double) lowHz && hz <= (double) highHz)
                maxBandPower = std::max (maxBandPower, meanPxx[(size_t) bin] + meanPyy[(size_t) bin]);
        }

        const double powerFloor = maxBandPower * 1.0e-3;

        for (int bin = 1; bin < numBins; ++bin)
        {
            const double hz = (double) bin * binHz;
            if (hz < (double) lowHz || hz > (double) highHz)
                continue;

            const double combinedPower = meanPxx[(size_t) bin] + meanPyy[(size_t) bin];
            if (combinedPower < powerFloor)
                continue;

            if (mode == WeightingMode::PlainPhat || numValidHits < 4)
            {
                weights[(size_t) bin] = 1.0;
                continue;
            }

            const double denom = meanPxx[(size_t) bin] * meanPyy[(size_t) bin] + 1.0e-24;
            const double coherence = std::clamp (std::norm (meanSxy[(size_t) bin]) / denom, 0.0, 1.0);
            const double shrink = (double) (numValidHits - 3) / (double) (numValidHits + 1);
            const double corrected = std::clamp (coherence * shrink, 0.0, 1.0);
            weights[(size_t) bin] = corrected / (corrected + 0.05);
        }

        result.delaySamples.reserve (hits.size());
        result.phaseDelaySamples.reserve (hits.size());
        result.peakDelaySamples.reserve (hits.size());
        result.phaseResidualRadians.reserve (hits.size());
        result.phaseConfidence.reserve (hits.size());
        result.usedFallback.reserve (hits.size());

        size_t spectrumOffset = 0;
        for (size_t hitIndex = 0; hitIndex < hits.size(); ++hitIndex)
        {
            const auto& hit = hits[hitIndex];
            const auto* spectrum = sxyByHit.data() + spectrumOffset;
            spectrumOffset += (size_t) numBins;

            if (validHit[hitIndex] == 0)
            {
                result.delaySamples.push_back (hit.coarseDelaySamples);
                result.phaseDelaySamples.push_back (std::numeric_limits<float>::quiet_NaN());
                result.peakDelaySamples.push_back (std::numeric_limits<float>::quiet_NaN());
                result.phaseResidualRadians.push_back (std::numeric_limits<float>::quiet_NaN());
                result.phaseConfidence.push_back (0.0f);
                result.usedFallback.push_back (true);
                continue;
            }

            float phaseResidual = std::numeric_limits<float>::quiet_NaN();
            float phaseConfidence = 0.0f;
            const auto phaseDelay = estimateFromPhaseSlope (spectrum,
                                                            weights,
                                                            fftSize,
                                                            hit.coarseDelaySamples,
                                                            phaseResidual,
                                                            phaseConfidence);
            if (numValidHits < 4)
                phaseConfidence *= (float) numValidHits / 4.0f;

            float peakDelay = std::numeric_limits<float>::quiet_NaN();
            result.phaseDelaySamples.push_back (phaseDelay);
            result.phaseResidualRadians.push_back (phaseResidual);
            result.phaseConfidence.push_back (phaseConfidence);

            float candidate = std::isfinite (phaseDelay) ? phaseDelay : peakDelay;
            bool fallback = ! std::isfinite (candidate)
                            || std::abs (candidate - hit.coarseDelaySamples) > 1.0f;

            if (! std::isfinite (phaseDelay))
            {
                peakDelay = estimateFromAnchoredIfft (spectrum,
                                                      weights,
                                                      fft,
                                                      fftSize,
                                                      hit.coarseDelaySamples);

                if (std::isfinite (peakDelay)
                    && std::abs (peakDelay - hit.coarseDelaySamples) <= 1.0f)
                {
                    candidate = peakDelay;
                    fallback = false;
                }
            }

            result.peakDelaySamples.push_back (peakDelay);

            if (fallback)
                candidate = hit.coarseDelaySamples;

            result.delaySamples.push_back (candidate);
            result.usedFallback.push_back (fallback);
        }

        std::vector<float> sorted = result.delaySamples;
        sorted.erase (std::remove_if (sorted.begin(), sorted.end(),
                                      [] (float v) { return ! std::isfinite (v); }),
                      sorted.end());

        if (sorted.empty())
            return result;

        const auto mid = sorted.begin() + (ptrdiff_t) (sorted.size() / 2);
        std::nth_element (sorted.begin(), mid, sorted.end());
        result.medianDelaySamples = *mid;

        if ((sorted.size() & 1u) == 0u)
        {
            const auto lower = std::max_element (sorted.begin(), mid);
            result.medianDelaySamples = 0.5f * (*lower + *mid);
        }

        result.valid = true;
        return result;
    }

private:
    static int nextPowerOfTwo (int value) noexcept
    {
        int n = 1;
        while (n < value)
            n <<= 1;
        return n;
    }

    static int orderForSize (int size) noexcept
    {
        int order = 0;
        while ((1 << order) < size)
            ++order;
        return order;
    }

    static void copyWindow (const float* input,
                            int numSamples,
                            double sampleRate,
                            std::vector<float>& fftBuffer)
    {
        const int taperSamples = std::clamp ((int) std::lround (sampleRate * 0.007),
                                             1,
                                             std::max (1, numSamples / 4));

        for (int i = 0; i < numSamples; ++i)
        {
            float window = 1.0f;

            if (i < taperSamples)
            {
                const double x = (double) i / (double) taperSamples;
                window = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * x));
            }
            else if (i >= numSamples - taperSamples)
            {
                const double x = (double) (numSamples - 1 - i) / (double) taperSamples;
                window = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * x));
            }

            fftBuffer[(size_t) i] = input[i] * window;
        }
    }

    static float estimateFromAnchoredIfft (const std::complex<double>* spectrum,
                                           const std::vector<double>& weights,
                                           juce::dsp::FFT& fft,
                                           int fftSize,
                                           float coarseDelaySamples)
    {
        std::vector<float> work (2 * (size_t) fftSize, 0.0f);
        const int numBins = fftSize / 2 + 1;

        for (int bin = 1; bin < numBins; ++bin)
        {
            const double weight = weights[(size_t) bin];
            if (weight <= 0.0)
                continue;

            const auto sxy = spectrum[bin];
            const double mag = std::abs (sxy);
            if (mag <= 1.0e-18)
                continue;

            const auto value = (weight / (mag + 1.0e-18)) * sxy;
            work[(size_t) (2 * bin)] = (float) value.real();
            work[(size_t) (2 * bin + 1)] = (float) value.imag();
        }

        fft.performRealOnlyInverseTransform (work.data());

        const float coarseCorrelationLag = -coarseDelaySamples;
        const int centre = (int) std::lround (coarseCorrelationLag);
        const int radius = 3;

        int bestLag = centre;
        double bestMagnitude = -1.0;

        for (int lag = centre - radius; lag <= centre + radius; ++lag)
        {
            const int index = lag >= 0 ? lag : fftSize + lag;
            if (index < 0 || index >= fftSize)
                continue;

            const double magnitude = std::abs ((double) work[(size_t) index]);
            if (magnitude > bestMagnitude)
            {
                bestMagnitude = magnitude;
                bestLag = lag;
            }
        }

        double refinedLag = (double) bestLag;
        const int prev = bestLag - 1;
        const int next = bestLag + 1;
        const int i0 = prev >= 0 ? prev : fftSize + prev;
        const int i1 = bestLag >= 0 ? bestLag : fftSize + bestLag;
        const int i2 = next >= 0 ? next : fftSize + next;

        if (i0 >= 0 && i0 < fftSize && i1 >= 0 && i1 < fftSize && i2 >= 0 && i2 < fftSize)
        {
            const double y0 = std::abs ((double) work[(size_t) i0]);
            const double y1 = std::abs ((double) work[(size_t) i1]);
            const double y2 = std::abs ((double) work[(size_t) i2]);
            const double denom = y0 - 2.0 * y1 + y2;
            if (std::abs (denom) > 1.0e-12)
                refinedLag += 0.5 * (y0 - y2) / denom;
        }

        return (float) -refinedLag;
    }

    static float estimateFromPhaseSlope (const std::complex<double>* spectrum,
                                         const std::vector<double>& weights,
                                         int fftSize,
                                         float coarseDelaySamples,
                                         float& residualRadiansOut,
                                         float& confidenceOut)
    {
        const int numBins = fftSize / 2 + 1;
        double sumXX = 0.0;
        double sumXY = 0.0;
        double weightSum = 0.0;

        struct BinPoint
        {
            double omega = 0.0;
            double phase = 0.0;
            double weight = 0.0;
        };

        std::vector<BinPoint> points;
        points.reserve ((size_t) numBins);

        for (int bin = 1; bin < numBins; ++bin)
        {
            const double weight = weights[(size_t) bin];
            if (weight <= 0.0)
                continue;

            const auto sxy = spectrum[bin];
            if (std::abs (sxy) <= 1.0e-18)
                continue;

            const double omega = juce::MathConstants<double>::twoPi * (double) bin / (double) fftSize;
            const double angle = std::atan2 (sxy.imag(), sxy.real());
            const double binWeight = weight * std::abs (sxy);
            const double predicted = omega * (double) coarseDelaySamples;
            const double branch = std::round ((predicted - angle) / juce::MathConstants<double>::twoPi);
            const double unwrapped = angle + juce::MathConstants<double>::twoPi * branch;

            points.push_back ({ omega, unwrapped, binWeight });
            sumXX += binWeight * omega * omega;
            sumXY += binWeight * omega * unwrapped;
            weightSum += binWeight;
        }

        if (weightSum <= 1.0e-18 || points.size() < 2)
            return std::numeric_limits<float>::quiet_NaN();

        if (sumXX <= 1.0e-18)
            return std::numeric_limits<float>::quiet_NaN();

        // With spectrum = X * conj(Y), and Y being a delayed copy of X by tau,
        // phase(X * conj(Y)) = +omega * tau. Positive return means "delay bass"
        // in the existing PhaseFixEngine convention.
        const double slope = sumXY / sumXX;

        double residual = 0.0;
        for (const auto& point : points)
        {
            const double e = point.phase - slope * point.omega;
            residual += point.weight * e * e;
        }

        residual = std::sqrt (residual / weightSum);
        residualRadiansOut = (float) residual;
        confidenceOut = (float) std::clamp (1.0 - residual / 0.35, 0.0, 1.0);

        if (std::abs (slope - (double) coarseDelaySamples) > 1.5)
            return std::numeric_limits<float>::quiet_NaN();

        return (float) slope;
    }

};
