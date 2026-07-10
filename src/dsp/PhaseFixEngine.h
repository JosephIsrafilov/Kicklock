#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
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
    float displayBeforeMatchPercent = 50.0f;
    float displayAfterMatchPercent = 50.0f;
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
    static constexpr float defaultAutoFixMaxDelayMs = 20.0f;
    static constexpr float extendedAutoFixMaxDelayMs = 20.0f;
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
                                        float renderMaxDelayMs,
                                        std::vector<float>& bassScratch,
                                        std::vector<float>& kickScratch,
                                        int alignCropStart = 0,
                                        int alignCropLength = 0)
    {
        PhaseFixScore result;

        if (bass == nullptr || kick == nullptr || numSamples <= 0 || sampleRate <= 0.0)
            return result;

        renderCandidatePair (bass, kick, numSamples, sampleRate, settings, renderMaxDelayMs,
                             bassScratch, kickScratch);

        const auto scoredFull = score (bassScratch.data(), kickScratch.data(), numSamples, sampleRate);
        result.multi = scoredFull.multi;
        result.matchPercent = scoredFull.match;
        
        if (alignCropLength > 0 && alignCropStart >= 0 && alignCropStart + alignCropLength <= numSamples)
        {
            const auto scoredCrop = score (bassScratch.data() + alignCropStart, 
                                           kickScratch.data() + alignCropStart, 
                                           alignCropLength, sampleRate);
            result.confidence = scoredCrop.confidence;
        }
        else
        {
            result.confidence = scoredFull.confidence;
        }
        
        return result;
    }

    static PhaseFixScore scoreSettings (const float* bass,
                                        const float* kick,
                                        int numSamples,
                                        double sampleRate,
                                        const PhaseFixRenderSettings& settings,
                                        float renderMaxDelayMs = absoluteManualMaxDelayMs)
    {
        std::vector<float> bassScratch ((size_t) juce::jmax(0, numSamples), 0.0f);
        std::vector<float> kickScratch ((size_t) juce::jmax(0, numSamples), 0.0f);
        return scoreSettings (bass, kick, numSamples, sampleRate, settings, renderMaxDelayMs, bassScratch, kickScratch);
    }

    // Public render helper (P4). Renders the bass candidate (sign, fractional
    // delay, then allpass cascade) into `out` WITHOUT scoring, so callers can
    // window the rendered result and the raw kick identically before scoring -
    // the correct order. Windowing the bass BEFORE the delay is rendered (the
    // old bug) shifts content relative to the window and lets the delay-line
    // fill-in eat early-window energy, skewing the reported after-match.
    static void renderCandidate (const float* bass,
                                 int numSamples,
                                 double sampleRate,
                                 const PhaseFixRenderSettings& settings,
                                 std::vector<float>& out,
                                 float renderMaxDelayMs = absoluteManualMaxDelayMs)
    {
        out.assign ((size_t) juce::jmax (0, numSamples), 0.0f);
        if (bass == nullptr || numSamples <= 0 || sampleRate <= 0.0)
            return;

        renderBassCandidate (bass, numSamples, sampleRate, settings, renderMaxDelayMs, out);
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
                                   InterpolationType delayInterpolation = InterpolationType::Linear,
                                   bool searchRotator = true)
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
                    return analyzeCore (bass, kick, numSamples, sampleRate,
                                        start, windowLength,
                                        maxDelayMs, delayInterpolation, searchRotator);
            }
        }

        return analyzeCore (bass, kick, numSamples, sampleRate, 0, numSamples, maxDelayMs, delayInterpolation, searchRotator);
    }

        static PhaseFixResult analyzeCore (const float* bass,
                                       const float* kick,
                                       int numSamples,
                                       double sampleRate,
                                       int alignCropStart,
                                       int alignCropLength,
                                       float maxDelayMs = defaultAutoFixMaxDelayMs,
                                       InterpolationType delayInterpolation = InterpolationType::Linear,
                                       bool searchRotator = true)
    {
        PhaseFixResult result;
        result.contributingHits = 1;
        result.singleHitAnalysis = true;

        if (bass == nullptr || kick == nullptr || numSamples <= 32 || sampleRate <= 0.0)
        {
            updateDerivedResultFields (result);
            return result;
        }

        const int bassPeak = locateDominantTransient (bass + alignCropStart, alignCropLength, sampleRate);
        const int kickPeak = locateDominantTransient (kick + alignCropStart, alignCropLength, sampleRate);
        const bool hasTransientOffset = bassPeak >= 0 && kickPeak >= 0;
        const float transientOffsetMs = hasTransientOffset
            ? (float) ((double) (kickPeak - bassPeak) * 1000.0 / sampleRate)
            : 0.0f;

        const auto align = AlignmentAnalyzer::analyze (bass + alignCropStart, kick + alignCropStart, alignCropLength,
                                                       sampleRate, maxDelayMs,
                                                       30.0f, 120.0f, 16384, searchRotator);

        if (! align.valid)
        {
            updateDerivedResultFields (result);
            return result;
        }

        result.valid = true;
        result.enoughSignal = true; 
        
        const auto beforeFull = score (bass, kick, numSamples, sampleRate);
        const auto beforeCrop = score (bass + alignCropStart, kick + alignCropStart, alignCropLength, sampleRate);
        
        PhaseFixRenderSettings bestSettings;
        bestSettings.delayInterpolation = delayInterpolation;
        float bestMatch = beforeFull.match;
        float bestConfidence = beforeCrop.confidence;

        std::vector<float> bassScratch ((size_t) numSamples, 0.0f);
        std::vector<float> kickScratch ((size_t) numSamples, 0.0f);

        auto scoreCandidate = [&] (bool invert, float delayMs, bool useRotator)
        {
            PhaseFixRenderSettings settings;
            settings.bassPolarityInvert = invert;
            settings.bassDelayMs = juce::jlimit (-absoluteManualMaxDelayMs,
                                                 absoluteManualMaxDelayMs,
                                                 delayMs);
            settings.phaseFilterEnabled = useRotator && align.adjustRotator;
            settings.phaseFilterFreqHz = align.rotatorFreqHz;
            settings.phaseFilterQ = align.rotatorQ;
            settings.phaseFilterStages = align.rotatorStages;
            settings.delayInterpolation = delayInterpolation;

            const auto candidate = scoreSettings (bass, kick, numSamples, sampleRate,
                                                  settings, absoluteManualMaxDelayMs,
                                                  bassScratch, kickScratch,
                                                  alignCropStart, alignCropLength);
            return std::pair<PhaseFixRenderSettings, PhaseFixScore> { settings, candidate };
        };

        auto considerCandidate = [&] (const PhaseFixRenderSettings& settings,
                                      const PhaseFixScore& candidate)
        {
            if (candidate.matchPercent > bestMatch + 0.01f)
            {
                bestMatch = candidate.matchPercent;
                bestConfidence = candidate.confidence;
                bestSettings = settings;
            }
        };

        const float candidateDelayMs = (hasTransientOffset && std::abs (transientOffsetMs) > 0.1f)
            ? transientOffsetMs
            : align.delayMs;
            
        const bool useTimingCandidate = std::abs (candidateDelayMs) > 0.1f;
        const bool useAnalyzerDelay = std::abs (align.delayMs - candidateDelayMs) > 0.1f;
        const float delayCandidates[] = { candidateDelayMs,
                                          useAnalyzerDelay ? align.delayMs : candidateDelayMs,
                                          useTimingCandidate ? 0.0f : candidateDelayMs };

        const bool invertPolys[] = { align.invertPolarity, ! align.invertPolarity };
        
        for (const bool invert : invertPolys)
        {
            for (const float delayMs : delayCandidates)
            {
                const auto withoutRotator = scoreCandidate (invert, delayMs, false);
                considerCandidate (withoutRotator.first, withoutRotator.second);

                if (align.adjustRotator)
                {
                    const auto withRotator = scoreCandidate (invert, delayMs, true);
                    if (withRotator.second.matchPercent >= withoutRotator.second.matchPercent + 3.0f)
                        considerCandidate (withRotator.first, withRotator.second);
                }
            }
        }

        result.bassPolarityInvert = bestSettings.bassPolarityInvert;
        result.bassDelayMs = bestSettings.bassDelayMs;
        result.phaseFilterEnabled = bestSettings.phaseFilterEnabled;
        result.phaseFilterFreqHz = bestSettings.phaseFilterFreqHz;
        result.phaseFilterQ = bestSettings.phaseFilterQ;
        result.phaseFilterStages = bestSettings.phaseFilterStages;

        result.beforeMatchPercent = beforeFull.match;
        result.afterMatchPercent = bestMatch;
        result.predictedAfterMatchPercent = bestMatch;
        result.displayBeforeMatchPercent = beforeFull.match;
        result.displayAfterMatchPercent = bestMatch;
        result.improvementPercent = result.afterMatchPercent - result.beforeMatchPercent;
        result.confidence = std::min (beforeCrop.confidence, bestConfidence);

        if ((hasTransientOffset && std::abs (transientOffsetMs) > maxDelayMs + 0.25f)
                 || std::abs (align.delayMs) > maxDelayMs + 0.25f
                 || (align.hitSearchLimit && std::abs (align.delayMs) >= maxDelayMs - 0.25f))
        {
            result.largeTimingOffset = true;
            result.detectedTimingOffsetMs = hasTransientOffset ? std::abs (transientOffsetMs)
                                                               : std::abs (align.delayMs);
            result.bassDelayMs = juce::jlimit (-maxDelayMs, maxDelayMs, result.bassDelayMs);
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

        // P2: only a measured Strong/Partial improvement may be auto-applied.
        // Large-offset and timeline-move advisories are NOT applicable in place
        // (the plugin can't safely delay bass that far, and can't move the kick
        // clip) - they carry a warning message instead.
        result.applyAllowed = result.valid
            && result.enoughSignal
            && ! result.unstableRecommendation
            && (result.quality == PhaseFixQuality::StrongImprovement
                || result.quality == PhaseFixQuality::PartialImprovement);

        // Optional apply: any stable, in-budget analysis can be written to the
        // controls so the Analyze -> Apply button flow always has an auditionable
        // result. Hard failures and out-of-budget advisories stay disabled.
        result.optionalApplyAllowed =
            result.valid
            && result.enoughSignal
            && ! result.unstableRecommendation
            && result.quality != PhaseFixQuality::LargeTimingOffset
            && result.quality != PhaseFixQuality::TimelineMoveRequired
            && result.quality != PhaseFixQuality::NotEnoughSignal;

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

    // P2 classification thresholds. Strong needs a big gain that also lands on a
    // genuinely good match (or a smaller gain onto an excellent final match);
    // Partial is a worthwhile gain regardless of the final absolute; AlreadyGood
    // is a high starting match with little left to add.
    static constexpr float strongImprovementThreshold = 8.0f;
    static constexpr float partialImprovementThreshold = 5.0f;
    static constexpr float alreadyGoodBeforeThreshold = 80.0f;
    static constexpr float strongAfterThreshold = 75.0f;
    static constexpr float excellentAfterThreshold = 90.0f;
    static constexpr float verificationWarningThreshold = 10.0f;

    // P3: score a rendered pair on the ONE canonical ruler (the shared
    // PhaseBands table + low-end-weighted blend), and fill Score.multi so band
    // detail is actually populated rather than a default-initialised struct.
    // scoreSettings() feeds this the processed bass candidate, so the reported
    // before/after percentages and the Apply-time verification share definitions
    // with the live meter.
    static Score score (const float* bass, const float* kick, int numSamples, double sampleRate)
    {
        Score result;
        result.multi = MultiBandCorrelation::analyze (bass, kick, numSamples, sampleRate);
        result.match = result.multi.weightedMatchPercent;
        result.confidence = result.multi.confidence;
        return result;
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
        double meanEnergy = 0.0;
        float peakEnergy = 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            const float energy = kick[i] * kick[i];
            meanEnergy += (double) energy;
            peakEnergy = juce::jmax (peakEnergy, energy);
        }

        meanEnergy /= (double) numSamples;
        const float threshold = juce::jlimit (1.0e-8f, 0.004f,
                                             juce::jmax ((float) meanEnergy * 6.0f,
                                                        peakEnergy * 0.08f));
        const float minimumEnergyGate = juce::jlimit (1.0e-9f, 0.0004f,
                                                     juce::jmax ((float) meanEnergy * 1.5f,
                                                                peakEnergy * 0.01f));
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

    static void renderBassCandidate (const float* bass,
                                     int numSamples,
                                     double sampleRate,
                                     const PhaseFixRenderSettings& settings,
                                     float renderMaxDelayMs,
                                     std::vector<float>& out)
    {
        const float sign = settings.bassPolarityInvert ? -1.0f : 1.0f;
        const float clampedDelayMs = juce::jlimit (-renderMaxDelayMs,
                                                   renderMaxDelayMs,
                                                   settings.bassDelayMs);
        const bool delayActive = std::abs (clampedDelayMs) > 1.0e-6f;

        if (delayActive && clampedDelayMs > 0.0f)
        {
            FractionalDelayLine delay;
            delay.prepare (sampleRate, std::max (renderMaxDelayMs, clampedDelayMs));
            delay.setInterpolationType (settings.delayInterpolation);
            delay.setDelaySamples ((float) (sampleRate * (double) clampedDelayMs / 1000.0));

            for (int i = 0; i < numSamples; ++i)
                out[(size_t) i] = delay.processSample (sign * bass[i]);
        }
        else if (delayActive)
        {
            const float advanceSamples = (float) (sampleRate * (double) -clampedDelayMs / 1000.0);
            const int wholeSamples = (int) std::floor (advanceSamples);
            const float frac = advanceSamples - (float) wholeSamples;

            for (int i = 0; i < numSamples; ++i)
            {
                const int i0 = i + wholeSamples;
                const int i1 = i0 + 1;
                const float x0 = i0 < numSamples ? bass[i0] : 0.0f;
                const float x1 = i1 < numSamples ? bass[i1] : 0.0f;
                out[(size_t) i] = sign * (x0 + frac * (x1 - x0));
            }
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

    static void renderCandidatePair (const float* bass,
                                     const float* kick,
                                     int numSamples,
                                     double sampleRate,
                                     const PhaseFixRenderSettings& settings,
                                     float renderMaxDelayMs,
                                     std::vector<float>& bassOut,
                                     std::vector<float>& kickOut)
    {
        bassOut.assign ((size_t) juce::jmax (0, numSamples), 0.0f);
        kickOut.assign ((size_t) juce::jmax (0, numSamples), 0.0f);
        if (bass == nullptr || kick == nullptr || numSamples <= 0 || sampleRate <= 0.0)
            return;

        const float signedDelayMs = juce::jlimit (-renderMaxDelayMs,
                                                  renderMaxDelayMs,
                                                  settings.bassDelayMs);
        const float bassDelayMs = juce::jmax (0.0f, signedDelayMs);
        const float kickDelayMs = juce::jmax (0.0f, -signedDelayMs);
        const float sign = settings.bassPolarityInvert ? -1.0f : 1.0f;

        FractionalDelayLine bassDelay;
        FractionalDelayLine kickDelay;
        const float maxDelayMs = juce::jmax (renderMaxDelayMs, std::abs (signedDelayMs));
        bassDelay.prepare (sampleRate, maxDelayMs);
        kickDelay.prepare (sampleRate, maxDelayMs);
        bassDelay.setInterpolationType (settings.delayInterpolation);
        kickDelay.setInterpolationType (settings.delayInterpolation);
        bassDelay.setDelaySamples ((float) (sampleRate * (double) bassDelayMs / 1000.0));
        kickDelay.setDelaySamples ((float) (sampleRate * (double) kickDelayMs / 1000.0));

        for (int i = 0; i < numSamples; ++i)
        {
            bassOut[(size_t) i] = bassDelay.processSample (sign * bass[i]);
            kickOut[(size_t) i] = kickDelay.processSample (kick[i]);
        }

        if (settings.phaseFilterEnabled)
        {
            AllpassPhaseRotator rotator;
            rotator.prepare (sampleRate, settings.phaseFilterStages);
            rotator.setParameters (settings.phaseFilterFreqHz, settings.phaseFilterQ);

            for (int i = 0; i < numSamples; ++i)
                bassOut[(size_t) i] = rotator.processSample (bassOut[(size_t) i]);
        }
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

        // Improvement-driven classification (P2). The old logic returned
        // StrongImprovement for ANY polarity flip / any delay >= 0.1 ms / any
        // enabled rotator regardless of measured gain, so users applied fixes
        // that did nothing. Grade purely on the measured before/after numbers:
        //   Strong  - a big gain that also lands on a genuinely good match
        //   Partial - a worthwhile gain, even if the result is still imperfect
        //   AlreadyGood - already well aligned, nothing meaningful to add
        //   NoUsefulChange - anything else
        const float improvement = result.improvementPercent;

        // Strong when there's a big measurable gain to a genuinely good place,
        // OR a smaller-but-real gain that lands on an excellent final match (a
        // fine timing correction is honest even if the numeric jump is modest -
        // what the prompt forbids is calling ANY flip/delay "Strong" with no
        // measured gain at all).
        const bool strongByGain = improvement >= strongImprovementThreshold
                                  && result.afterMatchPercent >= strongAfterThreshold;
        const bool strongByResult = improvement >= partialImprovementThreshold
                                    && result.afterMatchPercent >= excellentAfterThreshold;

        if (strongByGain || strongByResult)
            return PhaseFixQuality::StrongImprovement;

        if (improvement >= partialImprovementThreshold)
            return PhaseFixQuality::PartialImprovement;

        if (result.beforeMatchPercent >= alreadyGoodBeforeThreshold)
            return PhaseFixQuality::AlreadyGood;

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

        if (std::abs (result.bassDelayMs) > 0.02f)
        {
            if (wroteAction)
                summary << " + ";

            if (result.bassDelayMs >= 0.0f)
                summary << "delay bass " << juce::String (result.bassDelayMs, 2) << " ms";
            else
                summary << "advance bass " << juce::String (-result.bassDelayMs, 2) << " ms";

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

                return "Already close. Apply writes the measured low-end alignment so you can A/B it.";

            case PhaseFixQuality::StrongImprovement:
                return "Fix found: Predicted WTD "
                    + juce::String ((int) std::round (result.beforeMatchPercent))
                    + "% -> "
                    + juce::String ((int) std::round (result.afterMatchPercent))
                    + "%.";

            case PhaseFixQuality::PartialImprovement:
                return "Partial fix: Predicted WTD "
                    + juce::String ((int) std::round (result.beforeMatchPercent))
                    + "% -> "
                    + juce::String ((int) std::round (result.afterMatchPercent))
                    + "%. Apply if it sounds better.";

            case PhaseFixQuality::NoUsefulChange:
                return "Best low-end suggestion ready: " + formatActionSummary (result) + ". Apply to audition it.";
        }

        return "No useful bass-path fix found.";
    }
};
