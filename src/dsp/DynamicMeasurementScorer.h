#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <JuceHeader.h>

#include "DynamicStateMeasurements.h"
#include "DynamicPackageMorpher.h"
#include "DynamicHotBranchEngine.h"
#include "LinkwitzRileyCrossover.h"
#include "FractionalDelayLine.h"
#include "MultiBandCorrelation.h"

// =============================================================================
// Phase 9: canonical measurement scorer.
//
// One pure, worker-side before/after scorer shared by predicted (Learn-side)
// and verified (runtime) measurement. It reuses:
//   - MultiBandCorrelation::analyze() as the ONLY score metric (the same
//     low-end-weighted ruler already driving Learn/Analyze/Apply);
//   - the exact resolved DynamicPackageResolution (delay, polarity, allpass
//     coefficients/stages, crossover) that production would configure a hot
//     branch from — never a second, simplified approximation;
//   - DynamicHotBranchEngine::processAllpassReference(), the same TDF-II
//     allpass stage math the hot-branch engine itself renders with.
//
// It performs no fingerprint distance, no MIDI/pitch comparison, no APVTS or
// UI reads. It allocates (std::vector scratch) because it only ever runs off
// the audio thread (Learn worker or measurement worker).
// =============================================================================

struct DynamicMeasurementScore
{
    bool valid = false;
    float beforeScore = 0.0f;
    float afterScore = 0.0f;
    float improvementPoints = 0.0f;
    float confidence = 0.0f;
};

class DynamicMeasurementScorer
{
public:
    struct Scratch
    {
        std::vector<float> lowIn;
        std::vector<float> highIn;
        std::vector<float> lowOut;
        std::vector<float> afterBass;
        juce::AudioBuffer<float> splitInput;
        juce::AudioBuffer<float> splitLow;
        juce::AudioBuffer<float> splitHigh;
        LinkwitzRileyCrossover crossover;
        FractionalDelayLine delay;
        std::array<DynamicHotBranchDetail::AllpassStageState, DynamicHotBranchContract::kMaxAllpassStages> allpassState {};
        double preparedSampleRate = 0.0;
        int preparedCapacity = 0;
    };

    // Renders bassIn through the exact resolved package (crossover split,
    // fractional delay + polarity on the low band only, allpass cascade on
    // the low band, then low+high recombination) and scores the result
    // against kickIn on the canonical MultiBandCorrelation ruler. `before` is
    // scored on the untouched bassIn/kickIn pair from the SAME window, so
    // before/after always describe one physical hit.
    static DynamicMeasurementScore score (const float* bassIn,
                                          const float* kickIn,
                                          int numSamples,
                                          double sampleRate,
                                          const DynamicPackageResolution& package,
                                          Scratch& scratch) noexcept
    {
        DynamicMeasurementScore result;
        if (bassIn == nullptr || kickIn == nullptr || numSamples <= 8
            || ! std::isfinite (sampleRate) || sampleRate <= 0.0 || ! package.valid)
            return result;

        const auto before = MultiBandCorrelation::analyze (bassIn, kickIn, numSamples, sampleRate);
        if (! std::isfinite (before.weightedMatchPercent) || ! std::isfinite (before.confidence))
            return result;

        if (! renderPackage (bassIn, numSamples, sampleRate, package, scratch))
            return result;

        const auto after = MultiBandCorrelation::analyze (scratch.afterBass.data(), kickIn, numSamples, sampleRate);
        if (! std::isfinite (after.weightedMatchPercent) || ! std::isfinite (after.confidence))
            return result;

        result.valid = true;
        result.beforeScore = DynamicMeasurementContract::sanitizeScore (before.weightedMatchPercent);
        result.afterScore = DynamicMeasurementContract::sanitizeScore (after.weightedMatchPercent);
        result.improvementPoints = result.afterScore - result.beforeScore;
        result.confidence = DynamicMeasurementContract::sanitizeUnit (std::min (before.confidence, after.confidence));
        return result;
    }

    // Scores an ALREADY-RENDERED before/after pair (the runtime verification
    // path: `afterBass`/`afterKick` are the real captured audible output, not
    // a re-render) on the exact same canonical MultiBandCorrelation ruler as
    // score() above, so predicted and verified numbers are never computed by
    // two different metrics.
    static DynamicMeasurementScore scoreCapturedPair (const float* beforeBass,
                                                       const float* beforeKick,
                                                       const float* afterBass,
                                                       const float* afterKick,
                                                       int numSamples,
                                                       double sampleRate) noexcept
    {
        DynamicMeasurementScore result;
        if (beforeBass == nullptr || beforeKick == nullptr || afterBass == nullptr || afterKick == nullptr
            || numSamples <= 8 || ! std::isfinite (sampleRate) || sampleRate <= 0.0)
            return result;

        const auto before = MultiBandCorrelation::analyze (beforeBass, beforeKick, numSamples, sampleRate);
        const auto after = MultiBandCorrelation::analyze (afterBass, afterKick, numSamples, sampleRate);
        if (! std::isfinite (before.weightedMatchPercent) || ! std::isfinite (before.confidence)
            || ! std::isfinite (after.weightedMatchPercent) || ! std::isfinite (after.confidence))
            return result;

        result.valid = true;
        result.beforeScore = DynamicMeasurementContract::sanitizeScore (before.weightedMatchPercent);
        result.afterScore = DynamicMeasurementContract::sanitizeScore (after.weightedMatchPercent);
        result.improvementPoints = result.afterScore - result.beforeScore;
        result.confidence = DynamicMeasurementContract::sanitizeUnit (std::min (before.confidence, after.confidence));
        return result;
    }

private:
    // Splits bassIn into low/high (or treats it all as low when the crossover
    // is disabled), applies the package's polarity + fractional delay +
    // allpass cascade to the low band only, and recombines with the
    // untouched high band. This mirrors DynamicHotBranchEngine's per-branch
    // low path plus the once-added common high band, without needing the
    // engine's shared multi-branch history or the 20 ms reported-latency
    // scheduling offset (which applies identically to every branch and so
    // cancels out of the *relative* low/high timing measured here).
    static bool renderPackage (const float* bassIn,
                               int numSamples,
                               double sampleRate,
                               const DynamicPackageResolution& package,
                               Scratch& scratch) noexcept
    {
        if (! DynamicAllpassPoleDomain::isValidCoefficients (package.allpassCoefficients)
            || package.allpassStages < 2 || package.allpassStages > DynamicHotBranchContract::kMaxAllpassStages
            || ! std::isfinite (package.effectiveAbsoluteDelayMs)
            || ! std::isfinite (package.crossoverHz))
            return false;

        scratch.lowIn.assign ((size_t) numSamples, 0.0f);
        scratch.highIn.assign ((size_t) numSamples, 0.0f);

        if (package.crossoverEnabled
            && package.crossoverHz >= (double) DynamicStateMapContract::kCrossoverMinHz
            && package.crossoverHz <= (double) DynamicStateMapContract::kCrossoverMaxHz
            && package.crossoverHz < sampleRate * 0.5)
        {
            if (scratch.preparedSampleRate != sampleRate || scratch.preparedCapacity < numSamples)
            {
                juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) numSamples, (juce::uint32) 1 };
                scratch.crossover.prepare (spec);
                scratch.splitInput.setSize (1, numSamples, false, false, true);
                scratch.splitLow.setSize (1, numSamples, false, false, true);
                scratch.splitHigh.setSize (1, numSamples, false, false, true);
                scratch.preparedSampleRate = sampleRate;
                scratch.preparedCapacity = numSamples;
            }
            scratch.crossover.reset();
            scratch.crossover.setCrossoverFrequency ((float) package.crossoverHz);
            scratch.splitInput.copyFrom (0, 0, bassIn, numSamples);
            scratch.crossover.split (scratch.splitInput, scratch.splitLow, scratch.splitHigh, numSamples);
            for (int i = 0; i < numSamples; ++i)
            {
                scratch.lowIn[(size_t) i] = scratch.splitLow.getSample (0, i);
                scratch.highIn[(size_t) i] = scratch.splitHigh.getSample (0, i);
            }
        }
        else
        {
            for (int i = 0; i < numSamples; ++i)
                scratch.lowIn[(size_t) i] = bassIn[i];
        }

        const float clampedDelayMs = std::clamp ((float) package.effectiveAbsoluteDelayMs,
                                                 DynamicStateMapContract::kGlobalBaseDelayMinMs
                                                     + DynamicStateMapContract::kStateDelayDeltaMinMs,
                                                 DynamicStateMapContract::kGlobalBaseDelayMaxMs
                                                     + DynamicStateMapContract::kStateDelayDeltaMaxMs);
        const float maxDelayMs = std::max (1.0f, std::abs (clampedDelayMs) + 1.0f);
        scratch.delay.prepare (sampleRate, maxDelayMs);
        scratch.delay.setInterpolationType (package.delayInterpolationIndex == 1
            ? InterpolationType::Allpass : InterpolationType::Linear);

        const float sign = package.polarityInvert ? -1.0f : 1.0f;
        scratch.lowOut.assign ((size_t) numSamples, 0.0f);

        if (clampedDelayMs >= 0.0f)
        {
            scratch.delay.setDelaySamples ((float) (sampleRate * (double) clampedDelayMs / 1000.0));
            for (int i = 0; i < numSamples; ++i)
                scratch.lowOut[(size_t) i] = scratch.delay.processSample (sign * scratch.lowIn[(size_t) i]);
        }
        else
        {
            // Negative absolute delay is a net advance; render it exactly as
            // PhaseFixEngine::renderBassCandidate does for the same sign
            // convention (future-sample lookup, not a delay-line read).
            const float advanceSamples = (float) (sampleRate * (double) -clampedDelayMs / 1000.0);
            const int wholeSamples = (int) std::floor (advanceSamples);
            const float frac = advanceSamples - (float) wholeSamples;
            for (int i = 0; i < numSamples; ++i)
            {
                const int i0 = i + wholeSamples;
                const int i1 = i0 + 1;
                const float x0 = i0 < numSamples ? scratch.lowIn[(size_t) i0] : 0.0f;
                const float x1 = i1 < numSamples ? scratch.lowIn[(size_t) i1] : 0.0f;
                scratch.lowOut[(size_t) i] = sign * (x0 + frac * (x1 - x0));
            }
        }

        if (package.allpassEnabled)
        {
            for (auto& stage : scratch.allpassState)
                stage.reset();
            for (int i = 0; i < numSamples; ++i)
                scratch.lowOut[(size_t) i] = DynamicHotBranchEngine::processAllpassReference (
                    scratch.lowOut[(size_t) i], package.allpassCoefficients, package.allpassStages, scratch.allpassState);
        }

        scratch.afterBass.assign ((size_t) numSamples, 0.0f);
        for (int i = 0; i < numSamples; ++i)
        {
            const float sum = scratch.lowOut[(size_t) i] + scratch.highIn[(size_t) i];
            scratch.afterBass[(size_t) i] = std::isfinite (sum) ? sum : 0.0f;
        }
        return true;
    }
};
