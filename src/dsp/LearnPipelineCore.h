#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

#include "FixedTimingRotatorSearch.h"
#include "ConstrainedDtwRefiner.h"
#include "ConflictRegionLocalizer.h"
#include "FrozenCrossoverPhaseSimulation.h"
#include "HitConsensus.h"
#include "../util/LearnHitQueue.h"
#include "MultiBandCorrelation.h"
#include "NoteLearnAccumulator.h"
#include "ConflictStateClusterer.h"
#include "ConflictFingerprint.h"
#include "NoteQuantizer.h"
#include "OfflineFundamentalEstimator.h"
#include "OfflineNoteSegmenter.h"
#include "PhaseBands.h"
#include "DynamicLearnFingerprintExtraction.h"
#include "DynamicLearnFormation.h"
#include "DynamicFingerprintTrigger.h"
#include "DynamicPredictedMeasurement.h"

struct LearnPipelineConfig
{
    static constexpr int kMinUsableTimingObservations = 3;
    static constexpr int kMinDominantClusterMembers = 3;
    static constexpr float kMinDominantClusterShare = 0.60f;
    static constexpr float kMinConsensusConfidence = 0.35f;

    double sampleRate = 48000.0;
    InterpolationType delayInterpolation = InterpolationType::Linear;
    float maxDelayMs = PhaseFixEngine::defaultAutoFixMaxDelayMs;
    bool crossoverEnabled = true;
    float crossoverHz = 150.0f;

    // Frozen manual fallback. These are captured before the worker starts and
    // are never read from a live processor parameter.
    float preLearnAllpassFreqHz = 200.0f;
    float preLearnAllpassQ = 0.7f;
    bool preLearnAllpassEnabled = false;
    int preLearnAllpassStages = 2;
    std::vector<float> rotatorFreqs;
    std::vector<float> rotatorQs;
    std::vector<int> rotatorStages;
    const std::vector<float>& freqs() const { return rotatorFreqs.empty() ? FixedTimingRotatorSearch::defaultFrequencies() : rotatorFreqs; }
    const std::vector<float>& qs() const { return rotatorQs.empty() ? FixedTimingRotatorSearch::defaultQs() : rotatorQs; }
    const std::vector<int>& stages() const { return rotatorStages.empty() ? FixedTimingRotatorSearch::defaultStages() : rotatorStages; }
};

class LearnPipelineCore
{
public:
    static LearnFinalizeResult finalizeDynamic (const std::vector<LearnHitWindow>& windows,
                                                const LearnPipelineConfig& config,
                                                const DynamicStateMap& previousMap,
                                                const float* rawLoopBass,
                                                const float* rawLoopKick,
                                                int rawLoopSamples,
                                                const LearnDiagnostics& transportDiagnostics = {},
                                                const std::function<bool()>& shouldCancel = {},
                                                std::array<DynamicMeasurementSummary,
                                                    DynamicMeasurementContract::kMaxRetainedStates>* outPredictedMeasurements = nullptr)
    {
        LearnFinalizeResult result;
        result.map = NoteMap::makeEmptyNoteMap();
        result.diagnostics.capturedHits = (int) windows.size();
        result.diagnostics.droppedQueueHits = transportDiagnostics.droppedQueueHits;
        result.diagnostics.ignoredOverlappingTriggers = transportDiagnostics.ignoredOverlappingTriggers;
        if (windows.empty() || rawLoopBass == nullptr || rawLoopKick == nullptr || rawLoopSamples <= 0
            || ! std::isfinite (config.sampleRate) || config.sampleRate <= 0.0)
        {
            result.message = "No usable Dynamic Learn material.";
            return result;
        }

        // The timing queue intentionally serializes long analysis windows, but
        // its overlap rule must not change the fingerprint trigger contract.
        // Re-run the shared raw detector offline and create timing-free hits for
        // dense valid triggers that did not have a queue window.
        std::vector<LearnHitWindow> capturedWindows = windows;
        int nextSyntheticSequence = 0;
        for (const auto& window : capturedWindows)
            nextSyntheticSequence = std::max (nextSyntheticSequence, window.sequence + 1);
        TransientDetector triggerDetector;
        configureDynamicFingerprintTrigger (triggerDetector, config.sampleRate);
        for (int sample = 0; sample < rawLoopSamples; ++sample)
        {
            if ((sample & 1023) == 0 && cancelled (shouldCancel))
            {
                result.message = "Learn cancelled.";
                return result;
            }
            if (! triggerDetector.processSample (rawLoopKick[sample]))
                continue;
            const auto existing = std::find_if (capturedWindows.begin(), capturedWindows.end(), [sample] (const auto& window)
            {
                return window.absoluteSampleAtTrigger == sample;
            });
            if (existing == capturedWindows.end())
            {
                LearnHitWindow timingFree;
                timingFree.sequence = nextSyntheticSequence++;
                timingFree.absoluteSampleAtTrigger = sample;
                capturedWindows.push_back (std::move (timingFree));
            }
        }
        std::vector<std::pair<int, int64_t>> triggers;
        triggers.reserve (capturedWindows.size());
        for (const auto& window : capturedWindows)
            if (window.absoluteSampleAtTrigger >= 0 && window.absoluteSampleAtTrigger < rawLoopSamples)
                triggers.emplace_back (window.sequence, window.absoluteSampleAtTrigger);
        const auto fingerprints = extractDynamicLearnFingerprints (rawLoopBass, rawLoopKick, rawLoopSamples,
                                                                    config.sampleRate, triggers);
        if (cancelled (shouldCancel))
        {
            result.message = "Learn cancelled.";
            return result;
        }

        std::vector<DynamicLearnHit> hits;
        hits.reserve (capturedWindows.size());
        for (const auto& window : capturedWindows)
        {
            if (cancelled (shouldCancel))
            {
                result.message = "Learn cancelled.";
                return result;
            }
            DynamicLearnHit hit;
            hit.sequence = window.sequence;
            hit.triggerSample = window.absoluteSampleAtTrigger;
            const auto fingerprint = std::find_if (fingerprints.begin(), fingerprints.end(), [&] (const auto& sample)
            {
                return sample.sequence == window.sequence && sample.triggerSample == window.absoluteSampleAtTrigger;
            });
            if (fingerprint != fingerprints.end())
            {
                hit.fingerprintValidity = fingerprint->fingerprint.validity;
                hit.fingerprint = fingerprint->fingerprint.toPrototype();
            }

            const int n = (int) std::min (window.bass.size(), window.kick.size());
            if (n > 32 && finiteSignal (window.bass, n, shouldCancel) && finiteSignal (window.kick, n, shouldCancel))
            {
                const auto timing = PhaseFixEngine::analyze (window.bass.data(), window.kick.data(), n,
                                                              config.sampleRate, config.maxDelayMs,
                                                              config.delayInterpolation, false);
                hit.timingEligible = timing.valid && timing.enoughSignal
                    && std::isfinite (timing.bassDelayMs) && std::isfinite (timing.confidence);
                hit.absoluteDelayMs = timing.bassDelayMs;
                hit.polarityInvert = timing.bassPolarityInvert;
                hit.timingConfidence = std::clamp (timing.confidence, 0.0f, 1.0f);
                const auto multi = MultiBandCorrelation::analyze (window.bass.data(), window.kick.data(), n,
                                                                   config.sampleRate);
                hit.lowBandEnergy = weightedEnergy (multi);
                if (hit.timingEligible && ! timing.largeTimingOffset && ! timing.requiresTimelineMove)
                {
                    const int frozenStages = std::clamp (config.preLearnAllpassStages, 2, 4);
                    const auto search = FixedTimingRotatorSearch::search (
                        window.bass.data(), window.kick.data(), n, config.sampleRate, timing.bassDelayMs,
                        timing.bassPolarityInvert, config.delayInterpolation, config.stages(), config.freqs(),
                        config.qs(), frozenStages, shouldCancel);
                    const auto candidate = FixedTimingRotatorSearch::candidateForStage (search, frozenStages);
                    hit.correctionBeneficial = candidate.valid && candidate.helps;
                    hit.allpassFreqHz = candidate.allpassFreqHz;
                    hit.allpassQ = candidate.allpassQ;
                }
                const auto pitch = OfflineFundamentalEstimator::estimate (window.bass.data(), n, config.sampleRate,
                                                                            NoteMap::kFundamentalMinHz,
                                                                            NoteMap::kFundamentalMaxHz);
                if (pitch.valid && std::isfinite (pitch.frequencyHz) && pitch.frequencyHz > 0.0f)
                {
                    hit.hasLikelyPitchHz = true;
                    hit.likelyPitchHz = pitch.frequencyHz;
                    const int midi = NoteQuantizer::hzToMidi (pitch.frequencyHz);
                    hit.hasLikelyMidi = midi >= 0;
                    hit.likelyMidi = midi;
                }
            }
            hits.push_back (hit);
        }

        DynamicLearnFormationContext context;
        context.sampleRate = config.sampleRate;
        context.crossoverEnabled = config.crossoverEnabled;
        context.crossoverHz = config.crossoverHz;
        context.allpassEnabled = config.preLearnAllpassEnabled;
        context.fallbackAllpassFreqHz = config.preLearnAllpassFreqHz;
        context.fallbackAllpassQ = config.preLearnAllpassQ;
        context.allpassStages = config.preLearnAllpassStages;
        context.delayInterpolationIndex = config.delayInterpolation == InterpolationType::Linear ? 0 : 1;
        context.previousMap = previousMap;
        const auto formation = formDynamicStateMap (hits, context, shouldCancel);
        if (formation.diagnostics.cancelled)
        {
            result.message = "Learn cancelled.";
            return result;
        }
        result.dynamicMap = formation.map;
        result.hasDynamicStateMap = formation.valid;
        result.valid = formation.valid;
        result.message = formation.valid ? "Dynamic Learn formed State clusters."
                                         : "Dynamic Learn found no repeatable State clusters.";

        // Phase 9: predicted measurement + final-package cluster verification.
        // Retention re-matches the ORIGINAL captured windows (real audio; the
        // timing-free synthetic entries above have no bass/kick and are
        // skipped) against the just-formed map through the frozen Phase-3
        // matcher, then renders the exact Strength-1 final package against
        // every retained window. A State whose combined final package fails
        // verification is demoted here (hasLearnedPackage cleared, trim
        // reset to zero) before the map is ever returned to the caller, so
        // Apply can never publish a package Phase 9 has already rejected.
        if (formation.valid)
        {
            const auto retained = retainDynamicLearnMeasurementWindows (
                hits, capturedWindows, formation.map, config.sampleRate);
            const auto predictedResult = computeDynamicPredictedMeasurement (
                formation.map, retained, config.sampleRate, 0);
            result.dynamicMap = predictedResult.map;
            result.hasDynamicStateMap = isStructurallyValidDynamicStateMap (result.dynamicMap);
            result.valid = result.hasDynamicStateMap;
            if (outPredictedMeasurements != nullptr)
                *outPredictedMeasurements = predictedResult.predicted;
        }
        return result;
    }

    // fullLoopBass: optional full Learn-session bass (same timeline as
    // LearnHitWindow::absoluteSampleAtTrigger). When present, note bucketing
    // uses offline non-causal segmentation over the whole loop instead of the
    // live PitchTracker value frozen at the kick trigger. PitchTracker is
    // intentionally left alone for DynamicRuntimeSelector.
    static LearnFinalizeResult finalize (const std::vector<LearnHitWindow>& windows,
                                         const LearnPipelineConfig& config,
                                         const LearnDiagnostics& transportDiagnostics = {},
                                         const std::function<bool()>& shouldCancel = {},
                                         const float* fullLoopBass = nullptr,
                                         int fullLoopBassSamples = 0)
    {
        LearnFinalizeResult result;
        result.map = NoteMap::makeEmptyNoteMap();
        auto& diag = result.diagnostics;
        diag.capturedHits = (int) windows.size();
        diag.droppedQueueHits = transportDiagnostics.droppedQueueHits;
        diag.ignoredOverlappingTriggers = transportDiagnostics.ignoredOverlappingTriggers;
        if (cancelled (shouldCancel))
            return fail (result, "Learn cancelled.");
        if (windows.empty() || ! (config.sampleRate > 0.0) || ! std::isfinite (config.sampleRate))
            return fail (result, "No hits to learn from.");

        // Offline note map over the full captured loop (worker thread only).
        // Forward search is capped at the Learn hit post-roll (~150 ms) so a
        // true late-arrangement bass outside the capture/correction window is
        // not rescued by segmentation.
        const bool useOfflineSegments = fullLoopBass != nullptr && fullLoopBassSamples > 0;
        const auto noteSegments = useOfflineSegments
            ? OfflineNoteSegmenter::segment (fullLoopBass, fullLoopBassSamples, config.sampleRate,
                                            OfflineNoteSegmenter::kDefaultWindowMs,
                                            OfflineNoteSegmenter::kDefaultHopMs,
                                            shouldCancel)
            : std::vector<OfflineNoteSegment> {};
        if (cancelled (shouldCancel))
            return fail (result, "Learn cancelled.");
        const int segmentForwardSearch = juce::jmax (
            1, (int) std::lround (0.150 * config.sampleRate));

        std::vector<AnalyzedHit> hits;
        std::vector<HitObservation> observations;
        std::vector<int> observationToHit;
        hits.reserve (windows.size()); observations.reserve (windows.size()); observationToHit.reserve (windows.size());
        ConstrainedDtwRefiner::Scratch dtwScratch;

        for (const auto& window : windows)
        {
            if (cancelled (shouldCancel))
                return fail (result, "Learn cancelled.");
            AnalyzedHit hit;
            hit.analysis.sequence = window.sequence;
            hit.analysis.liveTrackedFundamentalHz = window.trackedHzAtTrigger;
            const OfflineNoteSegment* segment = useOfflineSegments
                ? OfflineNoteSegmenter::segmentAt (noteSegments, window.absoluteSampleAtTrigger,
                                                   segmentForwardSearch)
                : nullptr;
            const int n = (int) std::min (window.bass.size(), window.kick.size());
            if (n <= 32 || ! finiteSignal (window.bass, n, shouldCancel)
                || ! finiteSignal (window.kick, n, shouldCancel))
            {
                hit.analysis.signalUsable = false;
                ++diag.unusableSignalHits;
                hits.push_back (std::move (hit));
                continue;
            }

            hit.bass.assign (window.bass.begin(), window.bass.begin() + n);
            hit.kick.assign (window.kick.begin(), window.kick.begin() + n);
            const auto fingerprintBounds = ConflictRegionLocalizer::regionBoundaries (
                hit.kick.data(), n, config.sampleRate);
            hit.fingerprint = makeConflictFingerprint (hit.bass.data(), hit.kick.data(), n,
                                                       config.sampleRate, fingerprintBounds.onsetSample);
            if (config.crossoverEnabled && ! FrozenCrossoverPhaseSimulation::apply (hit.bass, config.sampleRate, frozenCrossoverHz (config)))
            {
                ++diag.unusableSignalHits;
                hits.push_back (std::move (hit));
                continue;
            }
            if (cancelled (shouldCancel))
                return fail (result, "Learn cancelled.");

            // Layer A is Dynamic Learn worker-only.  The shared engine and its
            // Static-path semantics are untouched; it receives only the
            // localized attack/body/tail conflict slice.
            ConflictLocalizationResult localization;
            auto timing = ConflictLocalizedPhaseAnalyzer::analyze (
                hit.bass.data(), hit.kick.data(), n, config.sampleRate,
                config.maxDelayMs, config.delayInterpolation, false, &localization);
            // Keep the shared full-window timing contract as the canonical
            // timing axis. The wrapper above supplies the adaptive region and
            // localized conflict score; this fallback preserves the existing
            // Static-path timing behaviour for real material whose localized
            // crop is too short for a stable delay estimate.
            const auto canonicalTiming = PhaseFixEngine::analyze (
                hit.bass.data(), hit.kick.data(), n, config.sampleRate,
                config.maxDelayMs, config.delayInterpolation, false);
            if (canonicalTiming.valid && canonicalTiming.enoughSignal)
                timing = canonicalTiming;
            hit.analysis.regionType = localization.region;
            hit.analysis.conflictSeverity = localization.severity;
            if (cancelled (shouldCancel))
                return fail (result, "Learn cancelled.");
            if (timing.valid && timing.enoughSignal && ! timing.largeTimingOffset
                && ! timing.requiresTimelineMove)
            {
                const auto refined = ConstrainedDtwRefiner::refine (
                    hit.bass.data(), hit.kick.data(), n, config.sampleRate,
                    timing.bassDelayMs * (float) config.sampleRate / 1000.0f,
                    timing.bassPolarityInvert, dtwScratch, shouldCancel);
                if (refined.cancelled || cancelled (shouldCancel))
                    return fail (result, "Learn cancelled.");
                if (refined.valid)
                {
                    timing.bassDelayMs = refined.delaySamples * 1000.0f / (float) config.sampleRate;
                    timing.confidence = std::min (timing.confidence, refined.confidence);
                }
                else if (refined.ambiguous)
                {
                    timing.unstableRecommendation = true;
                }
            }
            hit.timing = timing;
            hit.analysis.signalUsable = timing.valid && timing.enoughSignal;
            hit.analysis.timingUsable = hit.analysis.signalUsable && std::isfinite (timing.bassDelayMs)
                                       && std::isfinite (timing.confidence);
            hit.analysis.coarseLagMs = timing.detectedTimingOffsetMs;
            hit.analysis.fractionalLagMs = timing.bassDelayMs;
            hit.analysis.timingConfidence = std::clamp (timing.confidence, 0.0f, 1.0f);
            hit.analysis.timingAmbiguous = timing.unstableRecommendation
                                         || timing.largeTimingOffset
                                         || timing.requiresTimelineMove;

            // Pitch is deliberately independent of timing eligibility.  A bad
            // pitch is global-only, not a reason to discard timing evidence.
            //
            // With a full-loop segment map, the segment pitch is the authority
            // for bucketing (non-causal). Classification still requires real
            // bass energy inside the hit capture window so true late-arrangement
            // material outside post-roll is not rescued by the loop map alone.
            FundamentalEstimate pitch;
            if (segment != nullptr)
            {
                hit.analysis.offlineFundamentalHz = segment->frequencyHz;
                hit.analysis.offlinePitchConfidence = segment->confidence;

                double energy = 0.0;
                for (int i = 0; i < n; ++i)
                    energy += (double) hit.bass[(size_t) i] * (double) hit.bass[(size_t) i];
                const double rms = std::sqrt (energy / (double) juce::jmax (1, n));
                // Slightly above OfflineFundamentalEstimator::kMinRms so near-empty
                // post-roll tails (bass just at the 150 ms edge) do not count.
                constexpr float kMinHitRms = 5.0e-3f;
                pitch.valid = segment->confidence >= NoteMap::kMinOfflinePitchConfidence
                                 && rms >= (double) kMinHitRms;
                pitch.frequencyHz = segment->frequencyHz;
                pitch.confidence = segment->confidence;
                pitch.octaveCorrected = segment->octaveCorrected;
            }
            else
            {
                pitch = OfflineFundamentalEstimator::estimate (
                    hit.bass.data(), n, config.sampleRate,
                    NoteMap::kFundamentalMinHz, NoteMap::kFundamentalMaxHz);
                if (cancelled (shouldCancel))
                    return fail (result, "Learn cancelled.");
                hit.analysis.offlineFundamentalHz = pitch.frequencyHz;
                hit.analysis.offlinePitchConfidence = pitch.confidence;
            }
            // Offline segmentation is the authoritative non-causal Learn
            // source. In no-loop fixtures the local worker estimate takes the
            // same role. The raw live value remains a separate cross-check.
            hit.analysis.trackedFundamentalHz = segment != nullptr
                ? pitch.frequencyHz
                : (pitch.octaveCorrected ? pitch.frequencyHz
                                         : hit.analysis.liveTrackedFundamentalHz);
            classifyPitch (hit.analysis, pitch, segment != nullptr);

            if (hit.analysis.timingUsable)
            {
                const auto multi = MultiBandCorrelation::analyze (hit.bass.data(), hit.kick.data(), n, config.sampleRate);
                if (cancelled (shouldCancel))
                    return fail (result, "Learn cancelled.");
                hit.analysis.lowBandEnergy = weightedEnergy (multi);
                hit.analysis.timingObservation = makeObservation (timing, multi, hit.analysis.lowBandEnergy);
                hit.analysis.timingObservation.hitIndex = (int) hits.size();
                observations.push_back (hit.analysis.timingObservation);
                observationToHit.push_back ((int) hits.size());
                ++diag.analyzedHits;
            }
            else
            {
                ++diag.unusableSignalHits;
            }

            countPitchDiagnostic (diag, hit.analysis);
            hits.push_back (std::move (hit));
        }

        result.hitAnalyses.reserve (hits.size());
        for (const auto& hit : hits)
        {
            if (cancelled (shouldCancel))
                return fail (result, "Learn cancelled.");
            result.hitAnalyses.push_back (hit.analysis);
        }
        // Provisional reports from hits alone (empty map). Refined after buckets if map forms.
        fillNoteReports (result.noteReports, hits, NoteMap::makeEmptyNoteMap(), config.maxDelayMs,
                         noteSegments);

        if ((int) observations.size() < LearnPipelineConfig::kMinUsableTimingObservations)
            return fail (result, "Too few usable timing observations.");

        // Global HitConsensus stays informational / base-fallback only.
        // Multi-note loops split into pitch-dependent delay clusters, so a weak
        // global share must not abort Learn when individual notes still agree.
        const auto consensus = HitConsensus::analyze (observations);
        if (cancelled (shouldCancel))
            return fail (result, "Learn cancelled.");

        std::vector<int> dominantHits;
        bool strongGlobal = false;
        float globalDelay = 0.0f;
        bool globalPolarity = false;
        float globalConfidence = 0.0f;
        PhaseFixResult timingFix;

        if (consensus.hasConsensus && consensus.dominantClusterIndex >= 0)
        {
            const auto& dominant = consensus.clusters[(size_t) consensus.dominantClusterIndex];
            diag.dominantClusterHitCount = (int) dominant.memberIndices.size();
            diag.dominantClusterShare = consensus.dominantClusterShare;
            diag.timingOutlierHits = (int) observations.size() - diag.dominantClusterHitCount;
            diag.multipleTimingFamilies = consensus.clusters.size() > 1;

            const bool shareOk = (int) dominant.memberIndices.size() >= LearnPipelineConfig::kMinDominantClusterMembers
                && consensus.dominantClusterShare >= LearnPipelineConfig::kMinDominantClusterShare
                && consensus.consensusConfidence >= LearnPipelineConfig::kMinConsensusConfidence;

            if (shareOk)
            {
                dominantHits.reserve (dominant.memberIndices.size());
                for (const int observationIndex : dominant.memberIndices)
                    if (observationIndex >= 0 && observationIndex < (int) observationToHit.size())
                        dominantHits.push_back (observationToHit[(size_t) observationIndex]);

                if ((int) dominantHits.size() >= LearnPipelineConfig::kMinDominantClusterMembers)
                {
                    const float candidateDelay = dominant.centroidDelayMs;
                    const bool candidatePolarity = dominant.centroidPolarity;
                    PhaseFixResult candidateFix = timingDiagnostic (
                        candidateDelay, candidatePolarity, consensus, hits, dominantHits);
                    // Strong global is optional. Over-budget / unstable dominant
                    // must not abort Learn — per-note path can still form a map.
                    if (std::isfinite (candidateDelay)
                        && std::abs (candidateDelay) <= config.maxDelayMs + 1.0e-4f
                        && ! candidateFix.largeTimingOffset
                        && ! candidateFix.requiresTimelineMove
                        && ! candidateFix.unstableRecommendation)
                    {
                        strongGlobal = true;
                        globalDelay = candidateDelay;
                        globalPolarity = candidatePolarity;
                        globalConfidence = consensus.consensusConfidence;
                        timingFix = candidateFix;
                        for (const int index : dominantHits)
                            hits[(size_t) index].analysis.dominantTimingClusterMember = true;
                    }
                    else
                    {
                        result.globalFix = candidateFix;
                        // Keep dominantHits empty for strongGlobal=false path.
                        dominantHits.clear();
                    }
                }
            }
        }

        // Timing-eligible hits: timing-usable, in-budget. A disagreeing or
        // octave-ambiguous pitch label is deliberately NOT required to match
        // (per the pitch/timing independence policy above) — this list feeds
        // the global fallback delay, which only needs agreement on WHEN, not
        // WHICH note. But pitchInvalid means the capture window had no usable
        // bass energy at all (e.g. bass arriving right at the post-roll edge);
        // that hit carries no real timing evidence either and must still be
        // hard-rejected, or a truly-empty hit could fabricate a "consensus".
        // Soft DTW ambiguity (unstableRecommendation alone) still allows
        // consensus when the measured delay is finite and inside the budget.
        // Hard rejects: pitchInvalid / largeTimingOffset / requiresTimelineMove
        // / out-of-budget.
        std::vector<int> eligibleHits;
        eligibleHits.reserve (hits.size());
        for (int i = 0; i < (int) hits.size(); ++i)
        {
            const auto& hit = hits[(size_t) i];
            if (! hit.analysis.timingUsable || hit.analysis.pitchInvalid)
                continue;
            if (hit.timing.largeTimingOffset || hit.timing.requiresTimelineMove)
                continue;
            if (! std::isfinite (hit.timing.bassDelayMs)
                || std::abs (hit.timing.bassDelayMs) > config.maxDelayMs + 1.0e-4f)
                continue;
            eligibleHits.push_back (i);
        }

        if (! strongGlobal && eligibleHits.empty())
        {
            // Distinguish true out-of-budget timing from empty consensus.
            bool anyOutOfBudget = false;
            for (const auto& hit : hits)
            {
                if (! hit.analysis.timingUsable)
                    continue;
                if (hit.timing.largeTimingOffset || hit.timing.requiresTimelineMove
                    || (std::isfinite (hit.timing.bassDelayMs)
                        && std::abs (hit.timing.bassDelayMs) > config.maxDelayMs + 1.0e-4f))
                {
                    anyOutOfBudget = true;
                    break;
                }
            }
            if (anyOutOfBudget || result.globalFix.largeTimingOffset
                || result.globalFix.requiresTimelineMove)
                return fail (result, "Dominant timing solution is not supported by the delay budget.");
            return fail (result, "No timing consensus across hits.");
        }

        // Fallback global delay from eligible hits when the dominant cluster is weak.
        if (! strongGlobal)
        {
            std::vector<float> delays;
            delays.reserve (eligibleHits.size());
            int inverted = 0;
            double confSum = 0.0;
            for (const int index : eligibleHits)
            {
                const auto& hit = hits[(size_t) index];
                delays.push_back (hit.timing.bassDelayMs);
                inverted += hit.timing.bassPolarityInvert ? 1 : 0;
                confSum += std::clamp ((double) hit.analysis.timingConfidence, 0.0, 1.0);
            }
            std::sort (delays.begin(), delays.end());
            const size_t n = delays.size();
            globalDelay = n % 2 ? delays[n / 2] : 0.5f * (delays[n / 2 - 1] + delays[n / 2]);
            globalPolarity = inverted * 2 > (int) eligibleHits.size();
            globalConfidence = (float) (confSum / (double) eligibleHits.size());
            if (! std::isfinite (globalDelay) || std::abs (globalDelay) > config.maxDelayMs + 1.0e-4f)
                return fail (result, "Dominant timing solution is not supported by the delay budget.");
            timingFix = timingDiagnostic (globalDelay, globalPolarity, consensus, hits, eligibleHits);
        }

        const std::vector<int>& globalSourceHits = strongGlobal ? dominantHits : eligibleHits;
        std::vector<FixedTimingRotatorInput> globalInputs;
        globalInputs.reserve (globalSourceHits.size());
        for (const int index : globalSourceHits)
        {
            const auto& hit = hits[(size_t) index];
            globalInputs.push_back ({ hit.bass.data(), hit.kick.data(), (int) hit.bass.size(), hit.analysis.lowBandEnergy });
        }
        const auto globalSearch = FixedTimingRotatorSearch::searchCombined (
            globalInputs, config.sampleRate, globalDelay, globalPolarity, config.delayInterpolation,
            config.stages(), config.freqs(), config.qs(), 0, shouldCancel);
        if (cancelled (shouldCancel))
            return fail (result, "Learn cancelled.");

        const float fallbackFreq = sanitizeFallbackFreq (config.preLearnAllpassFreqHz);
        const float fallbackQ = sanitizeFallbackQ (config.preLearnAllpassQ);
        int globalStages = 2;
        bool globalHelps = false;
        float globalAllpassFreq = fallbackFreq;
        float globalAllpassQ = fallbackQ;

        if (globalSearch.valid)
        {
            globalStages = chooseStage (globalSearch);
            const auto globalCandidate = FixedTimingRotatorSearch::candidateForStage (globalSearch, globalStages);
            globalHelps = globalCandidate.valid && globalCandidate.helps;
            if (globalHelps)
            {
                globalAllpassFreq = globalCandidate.allpassFreqHz;
                globalAllpassQ = globalCandidate.allpassQ;
                // The winning rotator was evaluated with its own residual
                // alignment, so carry that jointly-selected delay forward to
                // the map/global fix instead of retaining the pre-rotator fit.
                globalDelay = globalCandidate.delayMs;
            }
        }
        else if (strongGlobal)
        {
            result.globalFix = timingFix;
            return fail (result, "Fixed-timing global rotator baseline is invalid.");
        }
        // Weak global: rotator may fail across mixed-pitch hits; per-note path still runs.

        // Layer B state axis: every timing-eligible hit participates. Pitch is
        // used only for the informational majority noteLabel.
        std::vector<ConflictStateSample> stateSamples;
        stateSamples.reserve (eligibleHits.size());
        for (const int index : eligibleHits)
        {
            if (cancelled (shouldCancel))
                return fail (result, "Learn cancelled.");
            auto& hit = hits[(size_t) index];
            // Seed the state signature from the measured timing and the
            // already-computed global joint-search centre. The definitive
            // per-state search below refines F/Q once, keeping Learn's worker
            // cost bounded while still clustering on measured correction axes.
            const float freq = globalHelps ? globalAllpassFreq : fallbackFreq;
            const float q = globalHelps ? globalAllpassQ : fallbackQ;
            // fractionalLagMs is the pre-rotator timing observation. Do not
            // use timing.bassDelayMs here after a previous candidate may have
            // folded allpass group delay into its local residual.
            const float observedDelay = hit.analysis.fractionalLagMs;
            const float measuredDelay = observedDelay;
            const bool measuredPolarity = hit.timing.bassPolarityInvert;
            stateSamples.push_back ({ index, measuredDelay, measuredPolarity,
                                      freq, q, globalStages, hit.analysis.regionType,
                                      std::clamp (hit.analysis.timingConfidence, 0.0f, 1.0f),
                                      0.0f, 0.0f,
                                      NoteQuantizer::hzToMidi (hit.analysis.trackedFundamentalHz) });
        }

        auto stateClusters = ConflictStateClusterer::cluster (stateSamples, NotePhaseMapSnapshot::kMaxStates);
        std::vector<ConflictStateEntry> learnedStates;
        learnedStates.reserve (stateClusters.size());
        auto medianFloat = [] (std::vector<float> v)
        {
            if (v.empty()) return 0.0f;
            std::sort (v.begin(), v.end());
            return v.size() % 2 ? v[v.size() / 2] : 0.5f * (v[v.size() / 2 - 1] + v[v.size() / 2]);
        };
        for (auto& cluster : stateClusters)
        {
            if (cancelled (shouldCancel))
                return fail (result, "Learn cancelled.");
            if (cluster.samples.empty()) continue;
            std::vector<float> delays, freqs, qs;
            int inverted = 0;
            for (const auto& s : cluster.samples)
            {
                delays.push_back (s.delayMs); freqs.push_back (s.allpassFreqHz); qs.push_back (s.allpassQ);
                inverted += s.polarityInvert ? 1 : 0;
            }
            const float delay = medianFloat (delays);
            const bool polarity = inverted * 2 > (int) cluster.samples.size();
            std::vector<FixedTimingRotatorInput> inputs;
            inputs.reserve (cluster.samples.size());
            for (const auto& s : cluster.samples)
            {
                const auto& h = hits[(size_t) s.hitIndex];
                inputs.push_back ({ h.bass.data(), h.kick.data(), (int) h.bass.size(), h.analysis.lowBandEnergy });
            }
            const auto search = FixedTimingRotatorSearch::searchCombined (
                inputs, config.sampleRate, delay, polarity, config.delayInterpolation,
                config.stages(), config.freqs(), config.qs(), 0, shouldCancel);
            const int stages = search.valid ? chooseStage (search) : globalStages;
            const auto combined = search.valid ? FixedTimingRotatorSearch::candidateForStage (search, stages)
                                               : RotatorCandidate {};
            float confidenceSum = 0.0f, matchSum = 0.0f, gainSum = 0.0f;
            std::vector<float> finalDelays;
            finalDelays.reserve (cluster.samples.size());
            std::array<std::vector<float>, ConflictFingerprint::kFeatureCount> fingerprintFeatures;
            std::array<int, NotePhaseMapSnapshot::size> noteCounts {};
            ConflictRegion region = cluster.samples.front().regionType;
            for (const auto& s : cluster.samples)
            {
                auto& h = hits[(size_t) s.hitIndex];
                const auto perHit = FixedTimingRotatorSearch::search (
                    h.bass.data(), h.kick.data(), (int) h.bass.size(), config.sampleRate,
                    delay, polarity, config.delayInterpolation, config.stages(), config.freqs(), config.qs(), stages, shouldCancel);
                const auto c = FixedTimingRotatorSearch::candidateForStage (perHit, stages);
                h.analysis.rotatorHelps = c.helps;
                h.analysis.allpassFreqHz = c.allpassFreqHz;
                h.analysis.allpassQ = c.allpassQ;
                h.analysis.rotatorGainPoints = c.gainPoints;
                h.analysis.rotatorConfidence = c.confidence;
                if (c.helps) { h.timing.bassDelayMs = c.delayMs; h.analysis.fractionalLagMs = c.delayMs; }
                if (c.valid && std::isfinite (c.delayMs)) finalDelays.push_back (c.delayMs);
                confidenceSum += std::clamp (c.valid ? c.confidence : h.analysis.timingConfidence, 0.0f, 1.0f);
                matchSum += c.valid ? c.matchPercent : 0.0f;
                gainSum += c.valid ? c.gainPoints : 0.0f;
                if (h.fingerprint.valid)
                    for (int feature = 0; feature < ConflictFingerprint::kFeatureCount; ++feature)
                        fingerprintFeatures[(size_t) feature].push_back (h.fingerprint.values[(size_t) feature]);
                const int midi = NotePhaseMapSnapshot::containsMidi (s.noteLabel) ? s.noteLabel : -1;
                if (midi >= NotePhaseMapSnapshot::minMidi && midi <= NotePhaseMapSnapshot::maxMidi)
                    ++noteCounts[(size_t) NotePhaseMapSnapshot::indexForMidi (midi)];
            }
            ConflictStateEntry e;
            // Store the measured timing axis, not the allpass candidate's
            // internal group-delay residual. The latter is an implementation
            // detail of the joint search and would create a false timing shift
            // in the compatibility mirror and future state selector.
            e.delayMs = finalDelays.empty() ? delay : medianFloat (finalDelays);
            e.polarityInvert = polarity;
            e.allpassFreqHz = combined.valid ? combined.allpassFreqHz : medianFloat (freqs);
            e.allpassQ = combined.valid ? combined.allpassQ : medianFloat (qs);
            e.stages = stages; e.regionType = region;
            ConflictFingerprint fingerprint;
            fingerprint.valid = fingerprintFeatures[0].size() == cluster.samples.size();
            if (fingerprint.valid)
                for (int feature = 0; feature < ConflictFingerprint::kFeatureCount; ++feature)
                    fingerprint.values[(size_t) feature] = medianFloat (fingerprintFeatures[(size_t) feature]);
            e.fingerprint = fingerprint.pack();
            e.hitCount = (int) cluster.samples.size();
            e.confidence = confidenceSum / (float) std::max (1, e.hitCount);
            e.matchPercent = matchSum / (float) std::max (1, e.hitCount);
            e.improvementPoints = gainSum / (float) std::max (1, e.hitCount);
            e.applied = e.hitCount >= NoteMap::kMinHitsPerNote
                     && e.confidence >= NoteMap::kMinRuntimeConfidence
                     && e.regionType != ConflictRegion::none
                     && e.fingerprint != 0;
            int bestNote = -1, bestCount = 0;
            for (int n = 0; n < NotePhaseMapSnapshot::size; ++n)
                if (noteCounts[(size_t) n] > bestCount) { bestCount = noteCounts[(size_t) n]; bestNote = NotePhaseMapSnapshot::midiForIndex (n); }
            e.noteLabel = bestNote;
            if (e.applied)
                learnedStates.push_back (e);
        }

        NotePhaseMapSnapshot map = NoteMap::makeEmptyNoteMap();
        map.schemaVersion = NoteMap::kSchemaVersion;
        map.base.delayMs = globalDelay;
        map.base.polarityInvert = globalPolarity;
        map.base.crossoverEnabled = config.crossoverEnabled;
        map.base.crossoverHz = frozenCrossoverHz (config);
        map.base.allpassStages = globalStages;
        map.base.delayInterpolationIndex = config.delayInterpolation == InterpolationType::Linear ? 0 : 1;
        map.base.learnedSampleRate = config.sampleRate;

        int learnedNotes = 0;
        for (size_t i = 0; i < learnedStates.size() && i < (size_t) NotePhaseMapSnapshot::kMaxStates; ++i)
        {
            if (cancelled (shouldCancel))
                return fail (result, "Learn cancelled.");
            map.states[i] = learnedStates[i];
            ++learnedNotes;
            const int note = map.states[i].noteLabel;
            if (NotePhaseMapSnapshot::containsMidi (note))
            {
                const int ni = NotePhaseMapSnapshot::indexForMidi (note);
                NoteEntry mirror;
                mirror.learned = true;
                mirror.fundamentalHz = NoteQuantizer::midiToHz (note);
                mirror.allpassFreqHz = map.states[i].allpassFreqHz;
                mirror.allpassQ = map.states[i].allpassQ;
                mirror.delayMs = map.states[i].delayMs;
                mirror.polarityInvert = map.states[i].polarityInvert;
                mirror.timingConfidence = map.states[i].confidence;
                mirror.confidence = map.states[i].confidence;
                mirror.hitCount = map.states[i].hitCount;
                mirror.rotatorHelps = map.states[i].improvementPoints >= FixedTimingRotatorSearch::kMinGainPoints;
                if (! map.notes[(size_t) ni].learned || mirror.confidence > map.notes[(size_t) ni].confidence)
                    map.notes[(size_t) ni] = mirror;
            }
        }

        // Populate the legacy/UI mirror for every observed pitch that belongs
        // to a learned state. This is informational compatibility only; state
        // clustering above never used MIDI as a gate or key.
        for (const int index : eligibleHits)
        {
            const auto& hit = hits[(size_t) index];
            const int midi = NoteQuantizer::hzToMidi (hit.analysis.trackedFundamentalHz);
            const int ni = NotePhaseMapSnapshot::indexForMidi (midi);
            if (ni < 0 || ! hit.analysis.pitchAccepted || learnedStates.empty()) continue;
            const ConflictStateEntry* state = nullptr;
            for (const auto& candidate : learnedStates)
                if (candidate.regionType == hit.analysis.regionType
                    && candidate.polarityInvert == hit.timing.bassPolarityInvert)
                { state = &candidate; break; }
            if (state == nullptr) state = &learnedStates.front();
            NoteEntry mirror;
            mirror.learned = true;
            mirror.fundamentalHz = NoteQuantizer::midiToHz (midi);
            mirror.allpassFreqHz = state->allpassFreqHz;
            mirror.allpassQ = state->allpassQ;
            mirror.delayMs = state->delayMs;
            mirror.polarityInvert = state->polarityInvert;
            mirror.timingConfidence = state->confidence;
            mirror.confidence = state->confidence;
            mirror.hitCount = state->hitCount;
            mirror.rotatorHelps = state->improvementPoints >= FixedTimingRotatorSearch::kMinGainPoints;
            map.notes[(size_t) ni] = mirror;
        }

        map.global = makeGlobalEntry (hits, globalSourceHits, globalAllpassFreq, globalAllpassQ, globalHelps,
                                      globalConfidence, globalDelay, globalPolarity);
        // Need either a strong global cluster, at least one learned note, or a
        // usable global fallback from eligible hits with a valid rotator baseline.
        if (! strongGlobal && learnedNotes == 0 && ! globalSearch.valid)
            return fail (result, "No confident timing solution for any note.");

        diag.globalOnlyHits = 0;
        for (size_t i = 0; i < hits.size(); ++i)
        {
            const auto& hit = hits[i];
            if (! hit.analysis.timingUsable) continue;
            if (! hit.analysis.timingUsable) continue;
            const int note = NotePhaseMapSnapshot::indexForMidi (NoteQuantizer::hzToMidi (hit.analysis.trackedFundamentalHz));
            if (note < 0 || ! map.notes[(size_t) note].learned) ++diag.globalOnlyHits;
        }

        fillNoteReports (result.noteReports, hits, map, config.maxDelayMs, noteSegments);

        timingFix.phaseFilterEnabled = globalHelps;
        timingFix.phaseFilterFreqHz = map.global.allpassFreqHz;
        timingFix.phaseFilterQ = map.global.allpassQ;
        timingFix.phaseFilterStages = globalStages;
        timingFix.confidence = globalConfidence;
        timingFix.contributingHits = strongGlobal ? (int) dominantHits.size() : (int) eligibleHits.size();
        timingFix.bassDelayMs = globalDelay;
        timingFix.bassPolarityInvert = globalPolarity;
        PhaseFixEngine::updateDerivedResultFields (timingFix);
        result.globalFix = timingFix;
        result.hitAnalyses.clear();
        for (const auto& hit : hits)
        {
            if (cancelled (shouldCancel))
                return fail (result, "Learn cancelled.");
            result.hitAnalyses.push_back (hit.analysis);
        }
        map.valid = NoteMap::isValidGlobalEntry (map.global) && NoteMap::isValidBaseContext (map.base);
        result.map = map;
        result.valid = NoteMap::isValidNoteMap (map);
        if (! result.valid) return fail (result, "Learned data did not form a valid map.");
        if (diag.multipleTimingFamilies && strongGlobal)
            diag.warning = "Multiple timing families detected; used the dominant one.";
        else if (diag.multipleTimingFamilies && learnedNotes > 0)
            diag.warning = "Multiple timing families detected; learned per-note solutions.";
        result.message = "Learned " + juce::String (learnedNotes) + " notes from "
                       + juce::String ((int) eligibleHits.size()) + " usable timing hits.";
        if (cancelled (shouldCancel))
            return fail (result, "Learn cancelled.");
        return result;
    }

private:
    struct AnalyzedHit { std::vector<float> bass, kick; ConflictFingerprint fingerprint; LearnHitAnalysis analysis; PhaseFixResult timing; };

    // Runtime-only per-note outcomes for UI. Deterministic from hit analyses + map.
    // noteSegments (optional): full-loop offline map used to flag notes that exist
    // in the loop but never earned an in-window overlap (true late arrangement).
    static void fillNoteReports (std::array<LearnNoteReport, NotePhaseMapSnapshot::size>& reports,
                                 const std::vector<AnalyzedHit>& hits,
                                 const NotePhaseMapSnapshot& map,
                                 float maxDelayMs,
                                 const std::vector<OfflineNoteSegment>& noteSegments = {}) noexcept
    {
        reports = {};
        for (int i = 0; i < NotePhaseMapSnapshot::size; ++i)
            reports[(size_t) i].midi = NotePhaseMapSnapshot::midiForIndex (i);

        for (const auto& hit : hits)
        {
            float hz = hit.analysis.trackedFundamentalHz;
            if (! (hz > 0.0f) || ! std::isfinite (hz))
                hz = hit.analysis.offlineFundamentalHz;
            if (! (hz > 0.0f) || ! std::isfinite (hz))
                continue;
            const int note = NotePhaseMapSnapshot::indexForMidi (NoteQuantizer::hzToMidi (hz));
            if (note < 0)
                continue;

            auto& r = reports[(size_t) note];
            ++r.recognizedHits;
            if (hit.analysis.octaveAmbiguous)
                ++r.ambiguousPitchHits;

            const bool timingOk = hit.analysis.timingUsable
                && std::isfinite (hit.timing.bassDelayMs)
                && std::abs (hit.timing.bassDelayMs) <= maxDelayMs + 1.0e-4f;
            // Per-note Learn counts every pitch-accepted in-budget overlap.
            // Dominant-cluster membership is diagnostics-only (global fallback).
            const bool overlapOk = hit.analysis.pitchAccepted && timingOk;

            if (overlapOk)
                ++r.acceptedHits;
            else
                ++r.outOfWindowHits;
        }

        // Notes present in the full-loop segment map but never overlap-accepted
        // (e.g. bass only after post-roll) count as out-of-window recognition.
        for (const auto& seg : noteSegments)
        {
            if (! (seg.frequencyHz > 0.0f) || ! std::isfinite (seg.frequencyHz))
                continue;
            const int note = NotePhaseMapSnapshot::indexForMidi (
                NoteQuantizer::hzToMidi (seg.frequencyHz));
            if (note < 0)
                continue;
            auto& r = reports[(size_t) note];
            if (r.acceptedHits == 0 && r.recognizedHits == 0)
            {
                ++r.recognizedHits;
                ++r.outOfWindowHits;
            }
        }

        for (int i = 0; i < NotePhaseMapSnapshot::size; ++i)
        {
            auto& r = reports[(size_t) i];
            if (map.notes[(size_t) i].learned)
            {
                r.outcome = LearnNoteOutcome::Learned;
                if (r.acceptedHits <= 0)
                    r.acceptedHits = map.notes[(size_t) i].hitCount;
                continue;
            }
            if (r.recognizedHits <= 0 && r.acceptedHits <= 0 && r.outOfWindowHits <= 0)
            {
                r.outcome = LearnNoteOutcome::None;
                continue;
            }
            if (r.acceptedHits > 0 && r.acceptedHits < NoteMap::kMinHitsPerNote)
            {
                r.outcome = LearnNoteOutcome::NotEnoughOverlap;
                continue;
            }
            if (r.ambiguousPitchHits > 0 && r.acceptedHits == 0)
            {
                r.outcome = LearnNoteOutcome::PitchAmbiguous;
                continue;
            }
            if (r.outOfWindowHits > 0 && r.acceptedHits == 0)
            {
                r.outcome = LearnNoteOutcome::OutOfCorrectionWindow;
                continue;
            }
            if (r.acceptedHits >= NoteMap::kMinHitsPerNote)
            {
                // Enough overlaps but aggregator/rotator did not accept the note.
                r.outcome = LearnNoteOutcome::CorrectionNotConfident;
                continue;
            }
            if (r.recognizedHits > 0)
                r.outcome = LearnNoteOutcome::NotEnoughOverlap;
            else
                r.outcome = LearnNoteOutcome::None;
        }
    }

    static bool cancelled (const std::function<bool()>& shouldCancel)
    { return shouldCancel && shouldCancel(); }

    static LearnFinalizeResult fail (LearnFinalizeResult& result, const juce::String& message)
    { result.valid = false; result.map = NoteMap::makeEmptyNoteMap(); result.message = message; result.diagnostics.warning = message; return result; }
    static bool finiteSignal (const std::vector<float>& values, int n, const std::function<bool()>& shouldCancel)
    {
        for (int i = 0; i < n; ++i)
        {
            if ((i & 255) == 0 && cancelled (shouldCancel))
                return false;
            if (! std::isfinite (values[(size_t) i]))
                return false;
        }
        return true;
    }
    static float frozenCrossoverHz (const LearnPipelineConfig& c)
    { return std::isfinite (c.crossoverHz) ? juce::jlimit (20.0f, (float) (c.sampleRate * 0.45), c.crossoverHz) : 150.0f; }
    static float sanitizeFallbackFreq (float value)
    { return std::isfinite (value) && value >= NoteMap::kAllpassFreqMinHz && value <= NoteMap::kAllpassFreqMaxHz ? value : 200.0f; }
    static float sanitizeFallbackQ (float value)
    { return std::isfinite (value) && value >= NoteMap::kAllpassQMin && value <= NoteMap::kAllpassQMax ? value : 0.7f; }
    static float weightedEnergy (const MultiBandCorrelationResult& multi)
    { float sum = 0.0f; for (int i = 0; i < PhaseBands::lowEndBandCount; ++i) sum += PhaseBands::table[(size_t)i].weight * multi.bands[(size_t)i].kickEnergy; return std::max (sum, 1.0e-6f); }
    static HitObservation makeObservation (const PhaseFixResult& timing, const MultiBandCorrelationResult& multi, float energy)
    {
        HitObservation obs; obs.delayMs = timing.bassDelayMs; obs.polarityInvert = timing.bassPolarityInvert;
        obs.matchPercent = timing.afterMatchPercent; obs.signalConfidence = timing.confidence; obs.energy = energy;
        float strongest = -1.0f;
        for (int i = 0; i < PhaseBands::lowEndBandCount; ++i) if (multi.bands[(size_t)i].kickEnergy > strongest)
        { strongest = multi.bands[(size_t)i].kickEnergy; obs.dominantFrequencyHz = 0.5f * (PhaseBands::table[(size_t)i].lowHz + PhaseBands::table[(size_t)i].highHz); }
        return obs;
    }
    static void classifyPitch (LearnHitAnalysis& analysis, const FundamentalEstimate& offline,
                               bool offlineSegmentIsAuthoritative)
    {
        const float selected = analysis.trackedFundamentalHz;
        if (! (selected > 0.0f) || ! std::isfinite (selected) || ! offline.valid
            || ! std::isfinite (offline.frequencyHz)
            || offline.confidence < NoteMap::kMinOfflinePitchConfidence)
        {
            analysis.pitchInvalid = true;
            return;
        }

        // The live tracker is a genuine independent cross-check only when it
        // published a value. Full-loop segmentation must not overwrite this
        // raw evidence before the comparison.
        const float live = analysis.liveTrackedFundamentalHz;
        if (live > 0.0f && std::isfinite (live))
        {
            const float cents = std::abs (1200.0f * std::log2 (live / offline.frequencyHz));
            analysis.pitchCentsDifference = cents;
            // Segment selection is non-causal and intentionally sees a note
            // change before the audio-thread tracker settles. Keep this value
            // as diagnostic evidence, but do not reject the authoritative
            // segment because a causal tracker is still reporting the prior
            // note at the kick trigger.
            if (offlineSegmentIsAuthoritative)
            {
                analysis.octaveCorrected = offline.octaveCorrected;
                analysis.pitchAccepted = true;
                return;
            }
            // A completely unrelated live pitch is a stronger, separate
            // safety signal than an octave decision inside the worker window.
            // Preserve the established global-only route for that case.
            if (std::abs (cents - 1200.0f) > NoteMap::kPitchAgreementCents
                && cents > NoteMap::kPitchAgreementCents)
            {
                analysis.pitchDisagrees = true;
                return;
            }
            if (offline.octaveAmbiguous)
            {
                analysis.octaveAmbiguous = true;
                return;
            }
            if (std::abs (cents - 1200.0f) <= NoteMap::kPitchAgreementCents)
            {
                // The worker estimator has independently demonstrated the
                // higher octave; retain that correction rather than rejecting
                // a live F/2 lock as if it were unresolved.
                if (! offline.octaveCorrected)
                {
                    analysis.octaveAmbiguous = true;
                    return;
                }
            }
        }
        else if (offline.octaveAmbiguous)
        {
            analysis.octaveAmbiguous = true;
            return;
        }
        analysis.octaveCorrected = offline.octaveCorrected;
        analysis.pitchAccepted = true;
    }
    static void countPitchDiagnostic (LearnDiagnostics& d, const LearnHitAnalysis& a)
    {
        if (a.pitchInvalid) ++d.pitchInvalidHits;
        if (a.pitchDisagrees) ++d.pitchDisagreementHits;
        if (a.octaveAmbiguous) ++d.octaveAmbiguousHits;
        if (a.octaveCorrected) ++d.octaveCorrectedHits;
        d.rejectedPitchHits = d.pitchInvalidHits + d.pitchDisagreementHits + d.octaveAmbiguousHits;
    }
    static int chooseStage (const FixedTimingRotatorResult& search)
    { int chosen = 2; float gain = -std::numeric_limits<float>::infinity(); for (const auto& c : search.perStage) if (c.helps && (c.gainPoints > gain + 1.0e-4f || (std::abs (c.gainPoints-gain)<=1.0e-4f && c.stages < chosen))) { gain = c.gainPoints; chosen = c.stages; } return chosen; }
    static NoteEntry makeGlobalEntry (const std::vector<AnalyzedHit>& hits, const std::vector<int>& indexes,
                                      float f, float q, bool helps, float confidence,
                                      float delayMs, bool polarityInvert)
    {
        NoteEntry e;
        std::vector<float> tracks;
        for (int i : indexes)
            if (hits[(size_t) i].analysis.pitchAccepted)
                tracks.push_back (hits[(size_t) i].analysis.trackedFundamentalHz);
        std::sort (tracks.begin(), tracks.end());
        e.fundamentalHz = tracks.empty() ? 0.0f : tracks[tracks.size() / 2];
        e.allpassFreqHz = f;
        e.allpassQ = q;
        e.delayMs = delayMs;
        e.polarityInvert = polarityInvert;
        e.timingConfidence = std::clamp (confidence, 0.0f, 1.0f);
        e.confidence = e.timingConfidence;
        e.hitCount = (int) indexes.size();
        e.rotatorHelps = helps;
        e.learned = helps;
        return e;
    }
    static PhaseFixResult timingDiagnostic (float delay, bool polarity, const ConsensusResult& consensus, const std::vector<AnalyzedHit>& hits, const std::vector<int>& indexes)
    { PhaseFixResult fix; fix.valid=true; fix.enoughSignal=true; fix.bassDelayMs=delay; fix.bassPolarityInvert=polarity; fix.confidence=consensus.consensusConfidence; for (int i:indexes) { const auto& t=hits[(size_t)i].timing; fix.largeTimingOffset |= t.largeTimingOffset; fix.requiresTimelineMove |= t.requiresTimelineMove; fix.unstableRecommendation |= t.unstableRecommendation; fix.detectedTimingOffsetMs=std::max(fix.detectedTimingOffsetMs,t.detectedTimingOffsetMs); } return fix; }
};
