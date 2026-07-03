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

    static PhaseFixResult analyze (const float* bass,
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

        const auto before = score (bass, kick, numSamples, sampleRate);
        result.beforeMatchPercent = before.match;
        result.afterMatchPercent = before.match;
        result.predictedAfterMatchPercent = before.match;
        result.confidence = before.confidence;
        result.enoughSignal = before.confidence >= minimumSignalConfidence;

        if (! result.enoughSignal)
        {
            updateDerivedResultFields (result);
            return result;
        }

        const auto unconstrained = AlignmentAnalyzer::analyze (bass, kick, numSamples,
                                                               sampleRate, absoluteManualMaxDelayMs,
                                                               30.0f, 120.0f, 16384);

        const int focusBandIndex = findWorstUsefulBandIndex (before.multi);

        std::vector<float> work ((size_t) numSamples, 0.0f);
        Candidate bestSafe = makeCandidateFromScore (before);
        bestSafe.settings.delayInterpolation = delayInterpolation;

        for (bool invert : { false, true })
        {
            for (float delayMs = 0.0f; delayMs <= maxDelayMs + 0.0001f; delayMs += 0.10f)
                testCandidate (bass, kick, numSamples, sampleRate, invert, delayMs,
                               false, 200.0f, 0.7f, 2, delayInterpolation,
                               work, before, focusBandIndex, bestSafe);
        }

        const float fineStart = std::max (0.0f, bestSafe.settings.bassDelayMs - 0.25f);
        const float fineEnd = std::min (maxDelayMs, bestSafe.settings.bassDelayMs + 0.25f);
        for (bool invert : { false, true })
        {
            for (float delayMs = fineStart; delayMs <= fineEnd + 0.0001f; delayMs += 0.01f)
                testCandidate (bass, kick, numSamples, sampleRate, invert, delayMs,
                               false, 200.0f, 0.7f, 2, delayInterpolation,
                               work, before, focusBandIndex, bestSafe);
        }

        Candidate best = bestSafe;
        Candidate bestPhase = bestSafe;

        constexpr float broadFreqs[] = { 30.0f, 40.0f, 50.0f, 60.0f, 65.0f, 80.0f,
                                         100.0f, 120.0f, 150.0f, 160.0f, 200.0f };
        constexpr float qs[] = { 0.5f, 0.7f, 1.0f, 1.5f, 2.0f, 4.0f };
        constexpr int stages[] = { 2, 3, 4 };

        const float phaseDelayStart = std::max (0.0f, bestSafe.settings.bassDelayMs - 0.25f);
        const float phaseDelayEnd = std::min (maxDelayMs, bestSafe.settings.bassDelayMs + 0.25f);

        if (focusBandIndex >= 0)
        {
            const auto& focusBand = before.multi.bands[(size_t) focusBandIndex];
            const float focusCenter = 0.5f * (focusBand.lowHz + focusBand.highHz);
            const float focusFreqs[] = {
                std::max (30.0f, focusBand.lowHz),
                focusCenter,
                std::min (200.0f, focusBand.highHz)
            };

            for (bool invert : { false, true })
                for (float freq : focusFreqs)
                    for (float q : qs)
                        for (int stageCount : stages)
                            testPhaseCandidate (bass, kick, numSamples, sampleRate, invert,
                                                phaseDelayStart, phaseDelayEnd, freq, q, stageCount,
                                                delayInterpolation, work, before, focusBandIndex, bestPhase);
        }

        for (bool invert : { false, true })
            for (float freq : broadFreqs)
                for (float q : qs)
                    for (int stageCount : stages)
                        testPhaseCandidate (bass, kick, numSamples, sampleRate, invert,
                                            phaseDelayStart, phaseDelayEnd, freq, q, stageCount,
                                            delayInterpolation, work, before, focusBandIndex, bestPhase);

        if (shouldKeepPhaseCandidate (before, bestSafe, bestPhase, focusBandIndex))
            best = bestPhase;

        result.valid = true;
        result.bassPolarityInvert = best.settings.bassPolarityInvert;
        result.bassDelayMs = std::max (0.0f, best.settings.bassDelayMs);
        result.phaseFilterEnabled = best.settings.phaseFilterEnabled;
        result.phaseFilterFreqHz = best.settings.phaseFilterFreqHz;
        result.phaseFilterQ = best.settings.phaseFilterQ;
        result.phaseFilterStages = best.settings.phaseFilterStages;
        result.afterMatchPercent = best.rawMatchPercent;
        result.predictedAfterMatchPercent = best.rawMatchPercent;
        result.improvementPercent = result.afterMatchPercent - result.beforeMatchPercent;
        result.confidence = estimateConfidence (best.confidence,
                                                result.afterMatchPercent,
                                                result.improvementPercent,
                                                result.singleHitAnalysis,
                                                1.0f);

        const float unconstrainedDelayAbs = unconstrained.valid ? std::abs (unconstrained.delayMs) : 0.0f;
        if (unconstrained.valid && unconstrainedDelayAbs > maxDelayMs + largeTimingToleranceMs)
        {
            result.largeTimingOffset = true;
            result.detectedTimingOffsetMs = unconstrainedDelayAbs;
        }
        else if (unconstrained.valid && unconstrained.delayMs < -0.02f
                 && result.improvementPercent < usefulImprovementThreshold)
        {
            result.requiresTimelineMove = true;
            result.suggestedKickMoveMs = std::abs (unconstrained.delayMs);
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
        result.applyAllowed = result.valid
            && result.enoughSignal
            && ! result.requiresTimelineMove
            && ! result.largeTimingOffset
            && ! result.unstableRecommendation
            && (result.quality == PhaseFixQuality::StrongImprovement
                || result.quality == PhaseFixQuality::PartialImprovement);

        result.optionalApplyAllowed =
            result.valid
            && result.enoughSignal
            && ! result.requiresTimelineMove
            && ! result.largeTimingOffset
            && ! result.unstableRecommendation
            && ((result.quality == PhaseFixQuality::PartialImprovement)
                || (result.quality == PhaseFixQuality::AlreadyGood && result.phaseFilterEnabled));

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
        result.multi = MultiBandCorrelation::analyze (bass, kick, numSamples, sampleRate);
        result.match = result.multi.weightedMatchPercent;
        result.confidence = result.multi.confidence;
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

        bool usefulConflictBand = false;
        if (focusBandIndex >= 0)
        {
            const auto& band = before.multi.bands[(size_t) focusBandIndex];
            usefulConflictBand = band.confidence >= usefulBandConfidence
                && (before.match - band.matchPercent) >= conflictBandGapThreshold;
        }

        const bool usefulRefinement =
            usefulConflictBand
            && phaseDelta.focusBandImprovement >= phaseBandKeepThreshold
            && globalGain >= -phaseOverallDropTolerance
            && phaseDelta.maxHighEnergyBandRegression <= phaseRegressionTolerance;

        return improvedGlobally || usefulRefinement;
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
        if (! result.enoughSignal || result.confidence < minimumSignalConfidence)
            return PhaseFixQuality::NotEnoughSignal;

        if (result.unstableRecommendation)
            return PhaseFixQuality::Unstable;

        if (result.largeTimingOffset)
            return PhaseFixQuality::LargeTimingOffset;

        if (result.requiresTimelineMove)
            return PhaseFixQuality::TimelineMoveRequired;

        if (result.beforeMatchPercent >= alreadyGoodThreshold
            && result.improvementPercent < usefulImprovementThreshold)
        {
            return PhaseFixQuality::AlreadyGood;
        }

        if (result.afterMatchPercent >= strongAfterThreshold
            && result.improvementPercent >= usefulImprovementThreshold)
        {
            return PhaseFixQuality::StrongImprovement;
        }

        if (result.improvementPercent >= partialImprovementThreshold
            || (result.afterMatchPercent >= 60.0f
                && result.improvementPercent >= usefulImprovementThreshold))
        {
            return PhaseFixQuality::PartialImprovement;
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
