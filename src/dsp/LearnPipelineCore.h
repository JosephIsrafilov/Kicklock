#pragma once

#include <algorithm>
#include <array>
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

        // Note-eligible hits: the timing-eligible subset that also carries a
        // trustworthy pitch, since per-note bucketing must know WHICH note a
        // hit belongs to. This is strictly narrower than eligibleHits.
        std::vector<int> noteEligibleHits;
        noteEligibleHits.reserve (eligibleHits.size());
        for (const int index : eligibleHits)
            if (hits[(size_t) index].analysis.pitchAccepted)
                noteEligibleHits.push_back (index);

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

        // Per-note delay medians (within-note consensus) for rotator search.
        std::array<std::vector<int>, (size_t) NotePhaseMapSnapshot::size> noteHitIndexes;
        for (const int index : noteEligibleHits)
        {
            const auto& hit = hits[(size_t) index];
            const int note = NotePhaseMapSnapshot::indexForMidi (
                NoteQuantizer::hzToMidi (hit.analysis.trackedFundamentalHz));
            if (note >= 0)
                noteHitIndexes[(size_t) note].push_back (index);
        }

        std::array<float, (size_t) NotePhaseMapSnapshot::size> noteDelayMs {};
        std::array<bool, (size_t) NotePhaseMapSnapshot::size> notePolarity {};
        std::array<bool, (size_t) NotePhaseMapSnapshot::size> noteTimingReady {};
        for (int n = 0; n < NotePhaseMapSnapshot::size; ++n)
        {
            const auto& indexes = noteHitIndexes[(size_t) n];
            if ((int) indexes.size() < NoteMap::kMinHitsPerNote)
                continue;
            std::vector<float> delays;
            delays.reserve (indexes.size());
            int inverted = 0;
            for (const int index : indexes)
            {
                delays.push_back (hits[(size_t) index].timing.bassDelayMs);
                inverted += hits[(size_t) index].timing.bassPolarityInvert ? 1 : 0;
            }
            std::sort (delays.begin(), delays.end());
            const size_t dn = delays.size();
            const float medianDelay = dn % 2 ? delays[dn / 2]
                                             : 0.5f * (delays[dn / 2 - 1] + delays[dn / 2]);
            // Within-note agreement: MAD-style spread must stay tight.
            double spreadSum = 0.0;
            for (const float d : delays)
            {
                const double diff = (double) d - (double) medianDelay;
                spreadSum += diff * diff;
            }
            const float spread = (float) std::sqrt (spreadSum / (double) delays.size());
            if (! std::isfinite (medianDelay) || ! std::isfinite (spread)
                || std::abs (medianDelay) > config.maxDelayMs + 1.0e-4f
                || spread > 1.0f) // ponytail: 1 ms within-note ceiling; raise if real gear needs it
                continue;
            noteDelayMs[(size_t) n] = medianDelay;
            notePolarity[(size_t) n] = inverted * 2 > (int) indexes.size();
            noteTimingReady[(size_t) n] = true;
        }

        std::array<NoteLearnAccumulator, (size_t) NotePhaseMapSnapshot::size> buckets;
        for (int n = 0; n < NotePhaseMapSnapshot::size; ++n)
        {
            if (! noteTimingReady[(size_t) n])
                continue;
            const float noteDelay = noteDelayMs[(size_t) n];
            const bool notePol = notePolarity[(size_t) n];
            const auto& indexes = noteHitIndexes[(size_t) n];

            // Choose this note's own best stage count from its own hits.
            // Reusing globalStages (picked from the mixed/global hit set) can
            // silently discard a note whose ideal stage differs from the
            // global pick, even though its own combined search would clear
            // the gain threshold.
            std::vector<FixedTimingRotatorInput> noteInputs;
            noteInputs.reserve (indexes.size());
            for (const int index : indexes)
            {
                const auto& hit = hits[(size_t) index];
                noteInputs.push_back ({ hit.bass.data(), hit.kick.data(), (int) hit.bass.size(), hit.analysis.lowBandEnergy });
            }
            const auto noteSearch = FixedTimingRotatorSearch::searchCombined (
                noteInputs, config.sampleRate, noteDelay, notePol, config.delayInterpolation,
                config.stages(), config.freqs(), config.qs(), 0, shouldCancel);
            if (cancelled (shouldCancel))
                return fail (result, "Learn cancelled.");
            const int noteStages = noteSearch.valid ? chooseStage (noteSearch) : globalStages;

            for (const int index : indexes)
            {
                if (cancelled (shouldCancel))
                    return fail (result, "Learn cancelled.");
                auto& hit = hits[(size_t) index];
                const auto perHit = FixedTimingRotatorSearch::search (
                    hit.bass.data(), hit.kick.data(), (int) hit.bass.size(),
                    config.sampleRate, noteDelay, notePol, config.delayInterpolation,
                    config.stages(), config.freqs(), config.qs(), noteStages, shouldCancel);
                if (cancelled (shouldCancel))
                    return fail (result, "Learn cancelled.");
                const auto candidate = FixedTimingRotatorSearch::candidateForStage (perHit, noteStages);
                hit.analysis.rotatorHelps = candidate.helps;
                hit.analysis.allpassFreqHz = candidate.allpassFreqHz;
                hit.analysis.allpassQ = candidate.allpassQ;
                hit.analysis.rotatorGainPoints = candidate.gainPoints;
                hit.analysis.rotatorConfidence = candidate.confidence;
                if (candidate.helps)
                {
                    // Persist the same candidate-local alignment that earned
                    // this allpass gain.  The original timing estimate may
                    // include this allpass's group delay.
                    hit.timing.bassDelayMs = candidate.delayMs;
                    hit.analysis.fractionalLagMs = candidate.delayMs;
                }
                buckets[(size_t) n].add ({ hit.analysis.trackedFundamentalHz, candidate.allpassFreqHz,
                    candidate.allpassQ, hit.timing.bassDelayMs, hit.timing.bassPolarityInvert,
                    hit.analysis.timingConfidence,
                    std::min (hit.analysis.offlinePitchConfidence, candidate.confidence), candidate.helps });
            }
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
        std::vector<float> learnedDelays;
        int learnedInverted = 0;
        for (int i = 0; i < NotePhaseMapSnapshot::size; ++i)
        {
            if (cancelled (shouldCancel))
                return fail (result, "Learn cancelled.");
            const auto entry = buckets[(size_t) i].finalizeEntry();
            if (entry.learned)
            {
                map.notes[(size_t) i] = entry;
                ++learnedNotes;
                learnedDelays.push_back (entry.delayMs);
                learnedInverted += entry.polarityInvert ? 1 : 0;
            }
        }

        // Prefer robust median of learned per-note delays when global cluster was weak.
        if (! strongGlobal && ! learnedDelays.empty())
        {
            std::sort (learnedDelays.begin(), learnedDelays.end());
            const size_t ln = learnedDelays.size();
            globalDelay = ln % 2 ? learnedDelays[ln / 2]
                                 : 0.5f * (learnedDelays[ln / 2 - 1] + learnedDelays[ln / 2]);
            globalPolarity = learnedInverted * 2 > learnedNotes;
            map.base.delayMs = globalDelay;
            map.base.polarityInvert = globalPolarity;
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
            if (! hit.analysis.pitchAccepted) { ++diag.globalOnlyHits; continue; }
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
