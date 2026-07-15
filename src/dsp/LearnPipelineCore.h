#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

#include "FixedTimingRotatorSearch.h"
#include "ConstrainedDtwRefiner.h"
#include "FrozenCrossoverPhaseSimulation.h"
#include "HitConsensus.h"
#include "../util/LearnHitQueue.h"
#include "MultiBandCorrelation.h"
#include "NoteLearnAccumulator.h"
#include "NoteQuantizer.h"
#include "OfflineFundamentalEstimator.h"
#include "OfflineNoteSegmenter.h"
#include "PhaseBands.h"

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
            const float segmentHz = resolveTrackedPitch (
                window, noteSegments, useOfflineSegments, segmentForwardSearch);
            hit.analysis.trackedFundamentalHz = segmentHz;
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
            if (config.crossoverEnabled && ! FrozenCrossoverPhaseSimulation::apply (hit.bass, config.sampleRate, frozenCrossoverHz (config)))
            {
                ++diag.unusableSignalHits;
                hits.push_back (std::move (hit));
                continue;
            }
            if (cancelled (shouldCancel))
                return fail (result, "Learn cancelled.");

            auto timing = PhaseFixEngine::analyze (hit.bass.data(), hit.kick.data(), n, config.sampleRate,
                                                    config.maxDelayMs, config.delayInterpolation, false);
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
            if (useOfflineSegments && segmentHz > 0.0f
                && window.absoluteSampleAtTrigger >= 0)
            {
                const float conf = OfflineNoteSegmenter::confidenceAt (
                    noteSegments, window.absoluteSampleAtTrigger, segmentForwardSearch);
                hit.analysis.offlineFundamentalHz = segmentHz;
                hit.analysis.offlinePitchConfidence = conf;

                double energy = 0.0;
                for (int i = 0; i < n; ++i)
                    energy += (double) hit.bass[(size_t) i] * (double) hit.bass[(size_t) i];
                const double rms = std::sqrt (energy / (double) juce::jmax (1, n));
                // Slightly above OfflineFundamentalEstimator::kMinRms so near-empty
                // post-roll tails (bass just at the 150 ms edge) do not count.
                constexpr float kMinHitRms = 5.0e-3f;
                FundamentalEstimate segPitch;
                segPitch.valid = conf >= NoteMap::kMinOfflinePitchConfidence
                                 && rms >= (double) kMinHitRms;
                segPitch.frequencyHz = segmentHz;
                segPitch.confidence = conf;
                classifyPitch (hit.analysis, segPitch);
            }
            else
            {
                const auto pitch = OfflineFundamentalEstimator::estimate (
                    hit.bass.data(), n, config.sampleRate,
                    NoteMap::kFundamentalMinHz, NoteMap::kFundamentalMaxHz);
                if (cancelled (shouldCancel))
                    return fail (result, "Learn cancelled.");
                hit.analysis.offlineFundamentalHz = pitch.frequencyHz;
                hit.analysis.offlinePitchConfidence = pitch.confidence;
                classifyPitch (hit.analysis, pitch);
            }

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

        const auto consensus = HitConsensus::analyze (observations);
        if (cancelled (shouldCancel))
            return fail (result, "Learn cancelled.");
        if (! consensus.hasConsensus || consensus.dominantClusterIndex < 0)
            return fail (result, "No timing consensus across hits.");
        const auto& dominant = consensus.clusters[(size_t) consensus.dominantClusterIndex];
        diag.dominantClusterHitCount = (int) dominant.memberIndices.size();
        diag.dominantClusterShare = consensus.dominantClusterShare;
        diag.timingOutlierHits = (int) observations.size() - diag.dominantClusterHitCount;
        diag.multipleTimingFamilies = consensus.clusters.size() > 1;
        if ((int) dominant.memberIndices.size() < LearnPipelineConfig::kMinDominantClusterMembers
            || consensus.dominantClusterShare < LearnPipelineConfig::kMinDominantClusterShare
            || consensus.consensusConfidence < LearnPipelineConfig::kMinConsensusConfidence)
            return fail (result, "Timing consensus is too weak for Learn.");

        std::vector<int> dominantHits;
        dominantHits.reserve (dominant.memberIndices.size());
        for (const int observationIndex : dominant.memberIndices)
            if (observationIndex >= 0 && observationIndex < (int) observationToHit.size())
                dominantHits.push_back (observationToHit[(size_t) observationIndex]);
        if ((int) dominantHits.size() < LearnPipelineConfig::kMinDominantClusterMembers)
            return fail (result, "Dominant timing cluster is incomplete.");
        for (const int index : dominantHits)
            hits[(size_t) index].analysis.dominantTimingClusterMember = true;

        const float globalDelay = dominant.centroidDelayMs;
        const bool globalPolarity = dominant.centroidPolarity;
        PhaseFixResult timingFix = timingDiagnostic (globalDelay, globalPolarity, consensus, hits, dominantHits);
        if (! std::isfinite (globalDelay) || std::abs (globalDelay) > config.maxDelayMs + 1.0e-4f
            || timingFix.largeTimingOffset || timingFix.requiresTimelineMove || timingFix.unstableRecommendation)
        {
            result.globalFix = timingFix;
            return fail (result, "Dominant timing solution is not supported by the delay budget.");
        }

        std::vector<FixedTimingRotatorInput> globalInputs;
        globalInputs.reserve (dominantHits.size());
        for (const int index : dominantHits)
        {
            const auto& hit = hits[(size_t) index];
            globalInputs.push_back ({ hit.bass.data(), hit.kick.data(), (int) hit.bass.size(), hit.analysis.lowBandEnergy });
        }
        const auto globalSearch = FixedTimingRotatorSearch::searchCombined (
            globalInputs, config.sampleRate, globalDelay, globalPolarity, config.delayInterpolation,
            config.stages(), config.freqs(), config.qs(), 0, shouldCancel);
        if (cancelled (shouldCancel))
            return fail (result, "Learn cancelled.");
        if (! globalSearch.valid)
        {
            result.globalFix = timingFix;
            return fail (result, "Fixed-timing global rotator baseline is invalid.");
        }
        const int globalStages = chooseStage (globalSearch);
        const auto globalCandidate = FixedTimingRotatorSearch::candidateForStage (globalSearch, globalStages);
        const float fallbackFreq = sanitizeFallbackFreq (config.preLearnAllpassFreqHz);
        const float fallbackQ = sanitizeFallbackQ (config.preLearnAllpassQ);
        const bool globalHelps = globalCandidate.valid && globalCandidate.helps;

        NotePhaseMapSnapshot map = NoteMap::makeEmptyNoteMap();
        map.schemaVersion = NoteMap::kSchemaVersion;
        map.base.delayMs = globalDelay;
        map.base.polarityInvert = globalPolarity;
        map.base.crossoverEnabled = config.crossoverEnabled;
        map.base.crossoverHz = frozenCrossoverHz (config);
        map.base.allpassStages = globalStages;
        map.base.delayInterpolationIndex = config.delayInterpolation == InterpolationType::Linear ? 0 : 1;
        map.base.learnedSampleRate = config.sampleRate;
        map.global = makeGlobalEntry (hits, dominantHits, globalHelps ? globalCandidate.allpassFreqHz : fallbackFreq,
                                       globalHelps ? globalCandidate.allpassQ : fallbackQ, globalHelps,
                                       consensus.consensusConfidence, globalDelay, globalPolarity);

        std::array<NoteLearnAccumulator, (size_t) NotePhaseMapSnapshot::size> buckets;
        for (const int index : dominantHits)
        {
            if (cancelled (shouldCancel))
                return fail (result, "Learn cancelled.");
            auto& hit = hits[(size_t) index];
            if (! hit.analysis.pitchAccepted)
                continue;
            if (hit.analysis.timingAmbiguous
                || std::abs (hit.timing.bassDelayMs) > config.maxDelayMs + 1.0e-4f)
                continue;
            const auto perHit = FixedTimingRotatorSearch::search (hit.bass.data(), hit.kick.data(), (int) hit.bass.size(),
                config.sampleRate, globalDelay, globalPolarity, config.delayInterpolation,
                config.stages(), config.freqs(), config.qs(), globalStages, shouldCancel);
            if (cancelled (shouldCancel))
                return fail (result, "Learn cancelled.");
            const auto candidate = FixedTimingRotatorSearch::candidateForStage (perHit, globalStages);
            hit.analysis.rotatorHelps = candidate.helps;
            hit.analysis.allpassFreqHz = candidate.allpassFreqHz;
            hit.analysis.allpassQ = candidate.allpassQ;
            hit.analysis.rotatorGainPoints = candidate.gainPoints;
            hit.analysis.rotatorConfidence = candidate.confidence;
            const int note = NotePhaseMapSnapshot::indexForMidi (NoteQuantizer::hzToMidi (hit.analysis.trackedFundamentalHz));
            if (note >= 0)
            {
                buckets[(size_t) note].add ({ hit.analysis.trackedFundamentalHz, candidate.allpassFreqHz,
                    candidate.allpassQ, hit.timing.bassDelayMs, hit.timing.bassPolarityInvert,
                    hit.analysis.timingConfidence,
                    std::min (hit.analysis.offlinePitchConfidence, candidate.confidence), candidate.helps });
            }
        }

        int learnedNotes = 0;
        for (int i = 0; i < NotePhaseMapSnapshot::size; ++i)
        {
            if (cancelled (shouldCancel))
                return fail (result, "Learn cancelled.");
            const auto entry = buckets[(size_t) i].finalizeEntry();
            if (entry.learned) { map.notes[(size_t) i] = entry; ++learnedNotes; }
        }
        diag.globalOnlyHits = 0;
        for (size_t i = 0; i < hits.size(); ++i)
        {
            const auto& hit = hits[i];
            if (! hit.analysis.timingUsable) continue;
            if (! hit.analysis.pitchAccepted) { ++diag.globalOnlyHits; continue; }
            const int note = NotePhaseMapSnapshot::indexForMidi (NoteQuantizer::hzToMidi (hit.analysis.trackedFundamentalHz));
            if (note < 0 || ! map.notes[(size_t) note].learned) ++diag.globalOnlyHits;
        }

        fillNoteReports (result.noteReports, hits, map, config.maxDelayMs, noteSegments);

        timingFix.phaseFilterEnabled = globalHelps;
        timingFix.phaseFilterFreqHz = map.global.allpassFreqHz;
        timingFix.phaseFilterQ = map.global.allpassQ;
        timingFix.phaseFilterStages = globalStages;
        timingFix.confidence = consensus.consensusConfidence;
        timingFix.contributingHits = (int) dominantHits.size();
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
        if (diag.multipleTimingFamilies)
            diag.warning = "Multiple timing families detected; used the dominant one.";
        result.message = "Learned " + juce::String (learnedNotes) + " notes from "
                       + juce::String ((int) dominantHits.size()) + " dominant timing hits.";
        if (cancelled (shouldCancel))
            return fail (result, "Learn cancelled.");
        return result;
    }

private:
    struct AnalyzedHit { std::vector<float> bass, kick; LearnHitAnalysis analysis; PhaseFixResult timing; };

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

        bool anyDominantAssigned = false;
        for (const auto& hit : hits)
            if (hit.analysis.dominantTimingClusterMember)
            {
                anyDominantAssigned = true;
                break;
            }

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

            const bool timingOk = hit.analysis.timingUsable
                && std::isfinite (hit.timing.bassDelayMs)
                && std::abs (hit.timing.bassDelayMs) <= maxDelayMs + 1.0e-4f;
            // timingAmbiguous still counts as an overlap for UI reasons (note was
            // present with kick); only out-of-budget / unusable timing is "window".
            const bool inCluster = ! anyDominantAssigned || hit.analysis.dominantTimingClusterMember;
            const bool overlapOk = hit.analysis.pitchAccepted && timingOk && inCluster;

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
            if (r.outOfWindowHits > 0 && r.acceptedHits == 0)
            {
                r.outcome = LearnNoteOutcome::OutOfCorrectionWindow;
                continue;
            }
            if (r.acceptedHits >= NoteMap::kMinHitsPerNote)
            {
                // Enough overlaps but aggregator/rotator did not accept the note.
                r.outcome = LearnNoteOutcome::NotEnoughOverlap;
                continue;
            }
            if (r.recognizedHits > 0)
                r.outcome = LearnNoteOutcome::NotEnoughOverlap;
            else
                r.outcome = LearnNoteOutcome::None;
        }
    }

    // Prefer offline full-loop segment pitch when available; fall back to the
    // live trigger snapshot only when no full loop was supplied (unit fixtures).
    static float resolveTrackedPitch (const LearnHitWindow& window,
                                      const std::vector<OfflineNoteSegment>& segments,
                                      bool useOfflineSegments,
                                      int segmentForwardSearch) noexcept
    {
        if (useOfflineSegments && window.absoluteSampleAtTrigger >= 0)
        {
            const float offlineHz = OfflineNoteSegmenter::frequencyAt (
                segments, window.absoluteSampleAtTrigger, segmentForwardSearch);
            if (offlineHz > 0.0f && std::isfinite (offlineHz))
                return offlineHz;
            return 0.0f;
        }
        return window.trackedHzAtTrigger;
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
    static void classifyPitch (LearnHitAnalysis& analysis, const FundamentalEstimate& offline)
    {
        const float tracked = analysis.trackedFundamentalHz;
        if (! (tracked > 0.0f) || ! std::isfinite (tracked) || ! offline.valid || ! std::isfinite (offline.frequencyHz)
            || offline.confidence < NoteMap::kMinOfflinePitchConfidence) { analysis.pitchInvalid = true; return; }
        const float cents = std::abs (1200.0f * std::log2 (tracked / offline.frequencyHz));
        analysis.pitchCentsDifference = cents;
        if (std::abs (cents - 1200.0f) <= NoteMap::kPitchAgreementCents) { analysis.octaveAmbiguous = true; return; }
        if (cents > NoteMap::kPitchAgreementCents) { analysis.pitchDisagrees = true; return; }
        analysis.pitchAccepted = true;
    }
    static void countPitchDiagnostic (LearnDiagnostics& d, const LearnHitAnalysis& a)
    { if (a.pitchInvalid) ++d.pitchInvalidHits; if (a.pitchDisagrees) ++d.pitchDisagreementHits; if (a.octaveAmbiguous) ++d.octaveAmbiguousHits; d.rejectedPitchHits = d.pitchInvalidHits + d.pitchDisagreementHits + d.octaveAmbiguousHits; }
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
