#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include <juce_core/juce_core.h>

#include "AllpassPhaseRotator.h"
#include "AlignmentAnalyzer.h"
#include "FractionalDelayLine.h"
#include "MultiBandCorrelation.h"

enum class PhaseFixQuality
{
    NotEnoughSignal,
    NoUsefulChange,
    AlreadyGood,
    PartialImprovement,
    StrongImprovement,
    LargeTimingOffset,
    TimelineMoveRequired,
    Unstable
};

struct PhaseFixRenderSettings
{
    bool bassPolarityInvert = false;
    float bassDelayMs = 0.0f;
    bool phaseFilterEnabled = false;
    float phaseFilterFreqHz = 200.0f;
    float phaseFilterQ = 0.7f;
    int phaseFilterStages = 2;
    InterpolationType delayInterpolation = InterpolationType::Linear;
};

struct PhaseFixScore
{
    MultiBandCorrelationResult multi;
    float matchPercent = 50.0f;
    float confidence = 0.0f;
};

struct PhaseFixResult
{
    bool valid = false;
    bool enoughSignal = false;
    bool bassPolarityInvert = false;
    float bassDelayMs = 0.0f;
    bool phaseFilterEnabled = false;
    float phaseFilterFreqHz = 200.0f;
    float phaseFilterQ = 0.7f;
    int phaseFilterStages = 2;

    float beforeMatchPercent = 50.0f;
    float afterMatchPercent = 50.0f;
    float predictedAfterMatchPercent = 50.0f;
    float verifiedAfterMatchPercent = -1.0f;
    float verificationDeltaPercent = 0.0f;
    bool verificationWarning = false;
    float improvementPercent = 0.0f;
    float confidence = 0.0f;

    bool requiresTimelineMove = false;
    float suggestedKickMoveMs = 0.0f;
    bool largeTimingOffset = false;
    float detectedTimingOffsetMs = 0.0f;
    bool unstableRecommendation = false;
    int contributingHits = 0;
    bool singleHitAnalysis = false;

    PhaseFixQuality quality = PhaseFixQuality::NoUsefulChange;
    bool applyAllowed = false;
    bool optionalApplyAllowed = false;

    juce::String message;
};

class PhaseFixEngine
{
public:
    static constexpr float defaultAutoFixMaxDelayMs = 5.0f;
    static constexpr float extendedAutoFixMaxDelayMs = 15.0f;
    static constexpr float absoluteManualMaxDelayMs = 50.0f;

    // Transient-hit window (ReVision-style): crop analysis to just around the
    // kick hit so inter-hit material can't skew the low-end correlation. Matches
    // the HitCaptureBuffer's own capture window closely enough that a snapshot
    // fed straight in is left essentially untouched.
    static constexpr float hitPreRollMs = 20.0f;
    static constexpr float hitPostRollMs = 150.0f;

    static PhaseFixScore scoreSettings (const float* bass,
                                        const float* kick,
                                        int numSamples,
                                        double sampleRate,
                                        const PhaseFixRenderSettings& settings,
                                        float renderMaxDelayMs = absoluteManualMaxDelayMs)
    {
        PhaseFixScore result;

        if (bass == nullptr || kick == nullptr || numSamples <= 0 || sampleRate <= 0.0)
            return result;

        std::vector<float> work ((size_t) numSamples, 0.0f);
        renderBassCandidate (bass, numSamples, sampleRate, settings, renderMaxDelayMs, work);

        const auto scored = score (work.data(), kick, numSamples, sampleRate);
        result.multi = scored.multi;
        result.matchPercent = scored.match;
        result.confidence = scored.confidence;
        return result;
    }

    // Transient-hit windowing entry point (ReVision-style). Kick and bass only
    // reveal their true low-end phase relationship right around the hit;
    // correlating over a loose span lets inter-hit silence and sustained
    // material wash out the reading, and lets the search lock onto whatever
    // arbitrary offset happens to line up. So before running the grid search we
    // locate the most prominent kick transient and crop BOTH signals to the same
    // [-hitPreRollMs, +hitPostRollMs] index range around it. Cropping both with
    // the identical range preserves their relative lag exactly, so the delay the
    // search recovers is unchanged while the out-of-window noise is removed.
    //
    // The crop is a no-op when the caller already passed a tight per-hit slice
    // (the located peak sits near the start, the window clamps to the whole
    // slice), so this is safe to layer under the processor's per-hit aggregation
    // and to feed a HitCaptureBuffer snapshot straight into.
    static PhaseFixResult analyze (const float* bass,
                                   const float* kick,
                                   int numSamples,
                                   double sampleRate,
                                   float maxDelayMs = defaultAutoFixMaxDelayMs,
                                   InterpolationType delayInterpolation = InterpolationType::Linear)
    {
        if (bass != nullptr && kick != nullptr && numSamples > 32 && sampleRate > 0.0)
        {
            const int peak = locateDominantTransient (kick, numSamples, sampleRate);
            if (peak >= 0)
            {
                const int preSamples  = juce::jmax (1, (int) std::lround (sampleRate * (double) hitPreRollMs  / 1000.0));
                const int postSamples = juce::jmax (1, (int) std::lround (sampleRate * (double) hitPostRollMs / 1000.0));

                const int start = juce::jlimit (0, numSamples - 1, peak - preSamples);
                const int end   = juce::jmin (numSamples, peak + postSamples);
                const int windowLength = end - start;

                // Only re-window when it actually tightens the span; a crop that
                // still spans the whole buffer would just repeat the same work.
                if (windowLength > 32 && windowLength < numSamples)
                    return analyzeCore (bass + start, kick + start, windowLength,
                                        sampleRate, maxDelayMs, delayInterpolation);
            }
        }

        return analyzeCore (bass, kick, numSamples, sampleRate, maxDelayMs, delayInterpolation);
    }

        static PhaseFixResult analyzeCore (const float* bass,
                                       const float* kick,
                                       int numSamples,
                                       double sampleRate,
                                       float maxDelayMs = defaultAutoFixMaxDelayMs,
                                       InterpolationType delayInterpolation = InterpolationType::Linear)
    {
        PhaseFixResult result;
        result.contributingHits = 1;
        result.singleHitAnalysis = true;

        if (bass == nullptr || kick == nullptr || numSamples <= 32 || sampleRate <= 0.0)
        {
            updateDerivedResultFields (result);
            return result;
        }

        const auto align = AlignmentAnalyzer::analyze (bass, kick, numSamples,
                                                       sampleRate, maxDelayMs,
                                                       30.0f, 120.0f, 16384);

        if (! align.valid)
        {
            updateDerivedResultFields (result);
            return result;
        }

        result.valid = true;
        result.enoughSignal = true; 
        result.bassPolarityInvert = align.invertPolarity;
        result.bassDelayMs = std::max (0.0f, align.delayMs);
        result.phaseFilterEnabled = align.adjustRotator;
        result.phaseFilterFreqHz = align.rotatorFreqHz;
        result.phaseFilterQ = align.rotatorQ;
        result.phaseFilterStages = align.rotatorStages;
        
        result.beforeMatchPercent = align.beforeMatch;
        result.afterMatchPercent = align.afterMatch;
        result.predictedAfterMatchPercent = align.afterMatch;
        result.improvementPercent = align.afterMatch - align.beforeMatch;
        result.confidence = 1.0f; 

        if (align.delayMs < -0.02f && result.improvementPercent < 5.0f)
        {
            result.requiresTimelineMove = true;
            result.suggestedKickMoveMs = std::abs (align.delayMs);
        }
        else if (std::abs(align.delayMs) > maxDelayMs + 0.25f)
        {
            result.largeTimingOffset = true;
            result.detectedTimingOffsetMs = std::abs(align.delayMs);
        }

        updateDerivedResultFields (result);
        return result;
    }



    static void applyVerification (PhaseFixResult& result, float verifiedAfterMatchPercent) noexcept
    {
        result.verifiedAfterMatchPercent = verifiedAfterMatchPercent;
        result.verificationDeltaPercent =
            std::abs (result.predictedAfterMatchPercent - result.verifiedAfterMatchPercent);
        result.verificationWarning = result.verificationDeltaPercent > verificationWarningThreshold;
    }

    static void updateDerivedResultFields (PhaseFixResult& result)
    {
        result.quality = classifyQuality (result);
        
        // BEST EFFORT MODE: We allow Apply Fix even if large timing offset or timeline move is required,
        // because we clamped the values to safe boundaries in analyzeAggregatedHits.
        result.applyAllowed = result.valid
            && result.enoughSignal
            && ! result.unstableRecommendation
            && (result.quality == PhaseFixQuality::StrongImprovement
                || result.quality == PhaseFixQuality::PartialImprovement
                || result.quality == PhaseFixQuality::LargeTimingOffset
                || result.quality == PhaseFixQuality::TimelineMoveRequired);

        result.optionalApplyAllowed =
            result.valid
            && result.enoughSignal
            && ! result.unstableRecommendation
            && ((result.quality == PhaseFixQuality::PartialImprovement)
                || (result.quality == PhaseFixQuality::AlreadyGood && result.phaseFilterEnabled)
                || (result.quality == PhaseFixQuality::LargeTimingOffset)
                || (result.quality == PhaseFixQuality::TimelineMoveRequired));

        result.message = makeMessage (result);
    }

    static bool canApply (const PhaseFixResult& result) noexcept
    {
        return result.applyAllowed;
    }

private:
    struct Score
    {
        MultiBandCorrelationResult multi;
        float match = 50.0f;
        float confidence = 0.0f;
    };

    struct BandComparison
    {
        float focusBandImprovement = 0.0f;
        float maxHighEnergyBandRegression = 0.0f;
    };

    struct Candidate
    {
        float rawMatchPercent = 50.0f;
        float scoreForSelection = -std::numeric_limits<float>::infinity();
        float confidence = 0.0f;
        MultiBandCorrelationResult multi;
        PhaseFixRenderSettings settings;
    };

    static constexpr float minimumSignalConfidence = 0.05f;
    static constexpr float usefulImprovementThreshold = 5.0f;
    static constexpr float partialImprovementThreshold = 8.0f;
    static constexpr float alreadyGoodThreshold = 85.0f;
    static constexpr float strongAfterThreshold = 75.0f;
    static constexpr float usefulBandConfidence = 0.08f;
    static constexpr float highEnergyBandConfidence = 0.16f;
    static constexpr float conflictBandGapThreshold = 8.0f;
    static constexpr float phaseGlobalKeepThreshold = 1.0f;
    static constexpr float phaseBandKeepThreshold = 5.0f;
    static constexpr float phaseOverallDropTolerance = 1.5f;
    static constexpr float phaseRegressionTolerance = 4.0f;
    static constexpr float largeTimingToleranceMs = 0.25f;
    static constexpr float verificationWarningThreshold = 10.0f;

    static Score score (const float* bass, const float* kick, int numSamples, double sampleRate)
    {
        Score result;
        const auto unconstrained = AlignmentAnalyzer::analyze (bass, kick, numSamples,
                                                               sampleRate, 0.0f,
                                                               30.0f, 120.0f, 16384);
        result.match = unconstrained.beforeMatch;
        result.confidence = 1.0f;
        return result;
    }

    static Candidate makeCandidateFromScore (const Score& value)
    {
        Candidate candidate;
        candidate.rawMatchPercent = value.match;
        candidate.scoreForSelection = value.match;
        candidate.confidence = value.confidence;
        candidate.multi = value.multi;
        return candidate;
    }

    // Finds the sample index of the most prominent kick transient in the buffer
    // using the same envelope-follower shape as the realtime TransientDetector
    // (attack/release energy follower, small threshold + energy gate), then
    // refines to the local energy peak just after each detected onset. Returns
    // -1 when nothing crosses the gate (silence / no clear hit), which tells the
    // caller to fall back to analysing the whole buffer. Read-only, no alloc.
    static int locateDominantTransient (const float* kick, int numSamples, double sampleRate) noexcept
    {
        if (kick == nullptr || numSamples <= 1 || sampleRate <= 0.0)
            return -1;

        // Mirror TransientDetector's defaults so offline detection matches what
        // the audio thread would have latched onto for the same signal.
        constexpr float threshold = 0.004f;
        constexpr float minimumEnergyGate = 0.0004f;
        const float attackCoeff  = timeMsToCoeff (2.0f, sampleRate);
        const float releaseCoeff = timeMsToCoeff (60.0f, sampleRate);
        const int holdoffSamples = juce::jmax (1, (int) std::round (sampleRate * 90.0 / 1000.0));
        const int peakSearch     = juce::jmax (1, (int) std::round (sampleRate * 8.0 / 1000.0));

        float envelope = 0.0f;
        bool wasAbove = false;
        int holdoffRemaining = 0;

        int bestPeak = -1;
        float bestPeakEnergy = -1.0f;

        for (int i = 0; i < numSamples; ++i)
        {
            const float energy = kick[i] * kick[i];
            const float coeff = energy > envelope ? attackCoeff : releaseCoeff;
            envelope = coeff * envelope + (1.0f - coeff) * energy;

            if (holdoffRemaining > 0)
                --holdoffRemaining;

            const bool above = envelope >= threshold && envelope >= minimumEnergyGate;
            const bool detected = above && ! wasAbove && holdoffRemaining <= 0;
            wasAbove = above;

            if (! detected)
                continue;

            holdoffRemaining = holdoffSamples;

            // Refine the onset to the loudest raw sample in the short window just
            // after it — that is the true transient peak we want centred.
            const int searchEnd = juce::jmin (numSamples, i + peakSearch);
            int localPeak = i;
            float localEnergy = kick[i] * kick[i];
            for (int j = i; j < searchEnd; ++j)
            {
                const float e = kick[j] * kick[j];
                if (e > localEnergy)
                {
                    localEnergy = e;
                    localPeak = j;
                }
            }

            if (localEnergy > bestPeakEnergy)
            {
                bestPeakEnergy = localEnergy;
                bestPeak = localPeak;
            }
        }

        return bestPeak;
    }

    static float timeMsToCoeff (float ms, double sampleRate) noexcept
    {
        const double samples = std::max (1.0, sampleRate * (double) ms / 1000.0);
        return (float) std::exp (-1.0 / samples);
    }

    static int findWorstUsefulBandIndex (const MultiBandCorrelationResult& multi) noexcept
    {
        int index = -1;
        float lowestMatch = std::numeric_limits<float>::infinity();

        for (int i = 0; i < MultiBandCorrelationResult::numBands; ++i)
        {
            const auto& band = multi.bands[(size_t) i];
            if (band.confidence < usefulBandConfidence)
                continue;

            if (band.matchPercent < lowestMatch)
            {
                lowestMatch = band.matchPercent;
                index = i;
            }
        }

        return index;
    }

    static BandComparison compareBands (const MultiBandCorrelationResult& baseline,
                                        const MultiBandCorrelationResult& candidate,
                                        int focusBandIndex) noexcept
    {
        BandComparison result;

        if (focusBandIndex >= 0
            && baseline.bands[(size_t) focusBandIndex].confidence >= usefulBandConfidence)
        {
            result.focusBandImprovement =
                candidate.bands[(size_t) focusBandIndex].matchPercent
                - baseline.bands[(size_t) focusBandIndex].matchPercent;
        }

        for (int i = 0; i < MultiBandCorrelationResult::numBands; ++i)
        {
            const auto& baseBand = baseline.bands[(size_t) i];
            const auto& candidateBand = candidate.bands[(size_t) i];

            if (baseBand.confidence >= highEnergyBandConfidence)
            {
                result.maxHighEnergyBandRegression = std::max (
                    result.maxHighEnergyBandRegression,
                    baseBand.matchPercent - candidateBand.matchPercent);
            }
        }

        result.maxHighEnergyBandRegression = std::max (0.0f, result.maxHighEnergyBandRegression);
        return result;
    }

    static void testPhaseCandidate (const float* bass,
                                    const float* kick,
                                    int numSamples,
                                    double sampleRate,
                                    bool invert,
                                    float phaseDelayStart,
                                    float phaseDelayEnd,
                                    float freq,
                                    float q,
                                    int stageCount,
                                    InterpolationType delayInterpolation,
                                    std::vector<float>& work,
                                    const Score& baseline,
                                    int focusBandIndex,
                                    Candidate& best)
    {
        if ((double) freq >= sampleRate * 0.5)
            return;

        testCandidate (bass, kick, numSamples, sampleRate, invert,
                       0.0f, true, freq, q, stageCount, delayInterpolation,
                       work, baseline, focusBandIndex, best);

        for (float delayMs = phaseDelayStart; delayMs <= phaseDelayEnd + 0.0001f; delayMs += 0.05f)
            testCandidate (bass, kick, numSamples, sampleRate, invert,
                           delayMs, true, freq, q, stageCount, delayInterpolation,
                           work, baseline, focusBandIndex, best);
    }

    static void testCandidate (const float* bass,
                               const float* kick,
                               int numSamples,
                               double sampleRate,
                               bool invert,
                               float delayMs,
                               bool phaseEnabled,
                               float phaseFreqHz,
                               float phaseQ,
                               int phaseStages,
                               InterpolationType delayInterpolation,
                               std::vector<float>& work,
                               const Score& baseline,
                               int focusBandIndex,
                               Candidate& best)
    {
        PhaseFixRenderSettings settings;
        settings.bassPolarityInvert = invert;
        settings.bassDelayMs = delayMs;
        settings.phaseFilterEnabled = phaseEnabled;
        settings.phaseFilterFreqHz = phaseFreqHz;
        settings.phaseFilterQ = phaseQ;
        settings.phaseFilterStages = phaseStages;
        settings.delayInterpolation = delayInterpolation;

        renderBassCandidate (bass, numSamples, sampleRate, settings, absoluteManualMaxDelayMs, work);

        const auto candidateScore = score (work.data(), kick, numSamples, sampleRate);
        const auto bandComparison = compareBands (baseline.multi, candidateScore.multi, focusBandIndex);
        const float scoreValue = candidateScore.match
            - delayPenalty (delayMs)
            + 0.15f * bandComparison.focusBandImprovement
            - 0.20f * bandComparison.maxHighEnergyBandRegression;

        if (scoreValue > best.scoreForSelection + 1.0e-4f
            || (std::abs (scoreValue - best.scoreForSelection) <= 1.0e-4f
                && delayMs < best.settings.bassDelayMs - 1.0e-4f)
            || (std::abs (scoreValue - best.scoreForSelection) <= 1.0e-4f
                && std::abs (delayMs - best.settings.bassDelayMs) <= 1.0e-4f
                && candidateScore.match > best.rawMatchPercent + 1.0e-4f))
        {
            best.rawMatchPercent = candidateScore.match;
            best.scoreForSelection = scoreValue;
            best.confidence = candidateScore.confidence;
            best.multi = candidateScore.multi;
            best.settings = settings;
        }
    }

    static bool shouldKeepPhaseCandidate (const Score& before,
                                          const Candidate& bestSafe,
                                          const Candidate& bestPhase,
                                          int focusBandIndex) noexcept
    {
        if (! bestPhase.settings.phaseFilterEnabled)
            return false;

        const auto phaseDelta = compareBands (bestSafe.multi, bestPhase.multi, focusBandIndex);
        const float globalGain = bestPhase.rawMatchPercent - bestSafe.rawMatchPercent;
        const bool improvedGlobally = globalGain >= phaseGlobalKeepThreshold;
        const bool usefulConflictBand = hasUsefulConflictBand (before, focusBandIndex);

        const bool usefulRefinement =
            usefulConflictBand
            && phaseDelta.focusBandImprovement >= phaseBandKeepThreshold
            && globalGain >= -phaseOverallDropTolerance
            && phaseDelta.maxHighEnergyBandRegression <= phaseRegressionTolerance;

        return improvedGlobally || usefulRefinement;
    }

    static bool hasUsefulConflictBand (const Score& before, int focusBandIndex) noexcept
    {
        if (focusBandIndex < 0)
            return false;

        const auto& band = before.multi.bands[(size_t) focusBandIndex];
        return band.confidence >= usefulBandConfidence
            && (before.match - band.matchPercent) >= conflictBandGapThreshold;
    }

    static float delayPenalty (float delayMs) noexcept
    {
        float penalty = 0.0f;

        if (delayMs > 2.0f)
            penalty += (delayMs - 2.0f) * 1.0f;

        if (delayMs > 5.0f)
            penalty += (delayMs - 5.0f) * 3.0f;

        return penalty;
    }

    static void renderBassCandidate (const float* bass,
                                     int numSamples,
                                     double sampleRate,
                                     const PhaseFixRenderSettings& settings,
                                     float renderMaxDelayMs,
                                     std::vector<float>& out)
    {
        const float sign = settings.bassPolarityInvert ? -1.0f : 1.0f;
        const float clampedDelayMs = std::max (0.0f, settings.bassDelayMs);
        const bool delayActive = clampedDelayMs > 1.0e-6f;

        if (delayActive)
        {
            FractionalDelayLine delay;
            delay.prepare (sampleRate, std::max (renderMaxDelayMs, clampedDelayMs));
            delay.setInterpolationType (settings.delayInterpolation);
            delay.setDelaySamples ((float) (sampleRate * (double) clampedDelayMs / 1000.0));

            for (int i = 0; i < numSamples; ++i)
                out[(size_t) i] = delay.processSample (sign * bass[i]);
        }
        else
        {
            for (int i = 0; i < numSamples; ++i)
                out[(size_t) i] = sign * bass[i];
        }

        if (settings.phaseFilterEnabled)
        {
            AllpassPhaseRotator rotator;
            rotator.prepare (sampleRate, settings.phaseFilterStages);
            rotator.setParameters (settings.phaseFilterFreqHz, settings.phaseFilterQ);

            for (int i = 0; i < numSamples; ++i)
                out[(size_t) i] = rotator.processSample (out[(size_t) i]);
        }
    }

    static float estimateConfidence (float signalConfidence,
                                     float afterMatchPercent,
                                     float improvementPercent,
                                     bool singleHitAnalysis,
                                     float consistency) noexcept
    {
        const float absoluteQuality =
            std::clamp ((afterMatchPercent - 45.0f) / 55.0f, 0.0f, 1.0f);
        const float improvementQuality =
            std::clamp (improvementPercent / 20.0f, 0.0f, 1.0f);
        const float hitWeight = singleHitAnalysis ? 0.85f : 1.0f;

        return std::clamp ((0.55f * signalConfidence
                            + 0.30f * absoluteQuality
                            + 0.15f * improvementQuality)
                           * std::clamp (consistency, 0.0f, 1.0f)
                           * hitWeight,
                           0.0f, 1.0f);
    }

    static PhaseFixQuality classifyQuality (const PhaseFixResult& result) noexcept
    {
        if (! result.enoughSignal)
            return PhaseFixQuality::NotEnoughSignal;

        if (result.unstableRecommendation)
            return PhaseFixQuality::Unstable;

        if (result.largeTimingOffset)
            return PhaseFixQuality::LargeTimingOffset;

        if (result.requiresTimelineMove)
            return PhaseFixQuality::TimelineMoveRequired;

        const bool meaningfulTimingCorrection = std::abs(result.bassDelayMs) >= 0.10f;
        const bool meaningfulPolarityCorrection = result.bassPolarityInvert;
        const bool meaningfulPhaseCorrection = result.phaseFilterEnabled;

        if (result.improvementPercent >= 5.0f || meaningfulTimingCorrection || meaningfulPolarityCorrection || meaningfulPhaseCorrection)
        {
            return PhaseFixQuality::StrongImprovement;
        }
        else if (result.improvementPercent > 0.0f)
        {
            return PhaseFixQuality::PartialImprovement;
        }
        
        if (result.beforeMatchPercent >= 80.0f)
        {
            return PhaseFixQuality::AlreadyGood;
        }

        return PhaseFixQuality::NoUsefulChange;
    }

    static juce::String formatActionSummary (const PhaseFixResult& result)
    {
        juce::String summary;
        bool wroteAction = false;

        if (result.bassPolarityInvert)
        {
            summary << "Flip bass polarity";
            wroteAction = true;
        }

        if (result.bassDelayMs > 0.02f)
        {
            if (wroteAction)
                summary << " + ";

            summary << "delay bass " << juce::String (result.bassDelayMs, 2) << " ms";
            wroteAction = true;
        }

        if (result.phaseFilterEnabled)
        {
            if (wroteAction)
                summary << " + ";

            summary << "phase filter around "
                    << juce::String ((int) std::round (result.phaseFilterFreqHz))
                    << " Hz";
            wroteAction = true;
        }

        if (! wroteAction)
            summary << "fine adjustment";

        return summary;
    }

    static juce::String makeMessage (const PhaseFixResult& result)
    {
        switch (result.quality)
        {
            case PhaseFixQuality::NotEnoughSignal:
                return "Waiting for stronger kick and bass signal.";

            case PhaseFixQuality::Unstable:
                return "Unstable result: different hits need different corrections. Try a shorter loop or check the arrangement.";

            case PhaseFixQuality::LargeTimingOffset:
                return "Large timing offset detected: "
                    + juce::String (result.detectedTimingOffsetMs, 1)
                    + " ms. Do not auto-delay bass this far; move the sample/clip in your DAW timeline.";

            case PhaseFixQuality::TimelineMoveRequired:
                return "Move kick later by " + juce::String (result.suggestedKickMoveMs, 2)
                    + " ms in your DAW. This plugin can only process bass.";

            case PhaseFixQuality::AlreadyGood:
                if (result.phaseFilterEnabled)
                {
                    return "Already close: "
                        + juce::String ((int) std::round (result.beforeMatchPercent))
                        + "% aligned. Optional phase filter around "
                        + juce::String ((int) std::round (result.phaseFilterFreqHz))
                        + " Hz may clean up the conflict band.";
                }

                return "Already close. No major correction needed.";

            case PhaseFixQuality::StrongImprovement:
                return "Fix found: Match "
                    + juce::String ((int) std::round (result.beforeMatchPercent))
                    + "% -> "
                    + juce::String ((int) std::round (result.afterMatchPercent))
                    + "%.";

            case PhaseFixQuality::PartialImprovement:
                return "Partial fix found: Match "
                    + juce::String ((int) std::round (result.beforeMatchPercent))
                    + "% -> "
                    + juce::String ((int) std::round (result.afterMatchPercent))
                    + "%. Apply if it sounds better.";

            case PhaseFixQuality::NoUsefulChange:
                return "No useful bass-path fix found. Try a different kick/bass sample or adjust arrangement.";
        }

        return "No useful bass-path fix found.";
    }
};
