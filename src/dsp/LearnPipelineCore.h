#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "FixedTimingRotatorSearch.h"
#include "FractionalDelayLine.h"     // InterpolationType
#include "HitConsensus.h"
#include "MultiBandCorrelation.h"
#include "NoteLearnAccumulator.h"
#include "NotePhaseMap.h"
#include "NoteQuantizer.h"
#include "OfflineFundamentalEstimator.h"
#include "PhaseBands.h"
#include "PhaseFixEngine.h"

// =============================================================================
// LearnPipelineCore (plan v1.2, Phase 3 offline Learn core).
//
// Pure, worker-thread-only, deterministic two-pass finalize. It consumes the
// completed LearnHitWindow data captured by the Phase 2 transport and returns a
// LearnFinalizeResult (note map + global fix + diagnostics). It owns NO threads,
// NO queue lifecycle, NO Apply/Revert, NO APVTS, and never activates any DSP:
// it only computes the map that a later phase will apply on explicit Apply.
//
// Pass A - global timing (one Delay + Polarity for the whole map):
//   * estimate each hit's bass fundamental offline (authoritative);
//   * reject invalid / silent / low-confidence hits and hits with too little
//     signal for the analysis engine;
//   * run the existing analysis engine per accepted hit with searchRotator=false
//     to get a timing/polarity observation, then HitConsensus to pick ONE global
//     Delay + Polarity. The tracked pitch is metadata only; the global timing is
//     never derived from HitObservation::dominantFrequencyHz.
//
// Fixed-timing rotator search + global stage selection:
//   * with the global Delay/Polarity held FIXED, search each accepted hit's
//     allpass frequency/Q/stages (FixedTimingRotatorSearch);
//   * pick ONE global allpass stage count across all hits.
//
// Pass B - per-note map:
//   * quantise each accepted fundamental to MIDI;
//   * aggregate the hits per note deterministically (NoteLearnAccumulator) at the
//     single global stage count, so per-note entries vary only in
//     allpassFreqHz/allpassQ/confidence/spread/hitCount/rotatorHelps;
//   * enforce the section-5 min-hit / confidence / spread / F-Q limits;
//   * build a valid NotePhaseMapSnapshot (global fallback + full base context).
//
// An empty or all-invalid dataset returns a valid==false result with populated
// diagnostics and a fresh empty map - never stale data, never a partial map.
// =============================================================================

struct LearnPipelineConfig
{
    double sampleRate = 48000.0;
    InterpolationType delayInterpolation = InterpolationType::Linear;
    float maxDelayMs = PhaseFixEngine::defaultAutoFixMaxDelayMs;

    // Recorded into the map's base context (not re-derived by Learn).
    bool  crossoverEnabled = true;
    float crossoverHz = 150.0f;

    // Fixed-timing rotator search grids. Empty means "use the production
    // defaults" (FixedTimingRotatorSearch::default*). Tests can supply a small
    // grid to keep the offline search cheap without changing production tuning.
    std::vector<float> rotatorFreqs;
    std::vector<float> rotatorQs;
    std::vector<int>   rotatorStages;

    const std::vector<float>& freqs() const
    {
        return rotatorFreqs.empty() ? FixedTimingRotatorSearch::defaultFrequencies() : rotatorFreqs;
    }
    const std::vector<float>& qs() const
    {
        return rotatorQs.empty() ? FixedTimingRotatorSearch::defaultQs() : rotatorQs;
    }
    const std::vector<int>& stages() const
    {
        return rotatorStages.empty() ? FixedTimingRotatorSearch::defaultStages() : rotatorStages;
    }
};

class LearnPipelineCore
{
public:
    static LearnFinalizeResult finalize (const std::vector<LearnHitWindow>& windows,
                                         const LearnPipelineConfig& config,
                                         const LearnDiagnostics& transportDiagnostics = {})
    {
        LearnFinalizeResult result;
        result.map = NoteMap::makeEmptyNoteMap();

        // Diagnostics passed through from the transport layer.
        LearnDiagnostics& diag = result.diagnostics;
        diag.capturedHits = (int) windows.size();
        diag.droppedQueueHits = transportDiagnostics.droppedQueueHits;
        diag.ignoredOverlappingTriggers = transportDiagnostics.ignoredOverlappingTriggers;

        const double sr = config.sampleRate;
        if (! (sr > 0.0) || ! std::isfinite (sr) || windows.empty())
        {
            diag.warning = "No hits to learn from.";
            result.message = diag.warning;
            return result;
        }

        // ---- Per-hit analysis (shared by Pass A and Pass B) -----------------
        std::vector<AcceptedHit> accepted;
        accepted.reserve (windows.size());
        std::vector<HitObservation> observations;
        observations.reserve (windows.size());

        int rejectedPitch = 0;
        int rejectedSignal = 0;

        for (const auto& w : windows)
        {
            const int n = (int) std::min (w.bass.size(), w.kick.size());
            if (n <= 32)
            {
                ++rejectedSignal;
                continue;
            }

            const float* bass = w.bass.data();
            const float* kick = w.kick.data();

            // Offline fundamental is authoritative for the note identity.
            const auto pitch = OfflineFundamentalEstimator::estimate (
                bass, n, sr, NoteMap::kFundamentalMinHz, NoteMap::kFundamentalMaxHz);

            if (! pitch.valid || pitch.confidence < NoteMap::kMinOfflinePitchConfidence)
            {
                ++rejectedPitch;
                continue;
            }

            // Existing analysis engine, rotator disabled: timing/polarity only.
            const auto timing = PhaseFixEngine::analyze (
                bass, kick, n, sr, config.maxDelayMs, config.delayInterpolation, /*searchRotator*/ false);

            if (! timing.enoughSignal)
            {
                ++rejectedSignal;
                continue;
            }

            // Dominant band frequency computed the SAME way Static Analyze does
            // (strongest low-end band centre) - NOT the tracked/offline pitch.
            const auto multi = MultiBandCorrelation::analyze (bass, kick, n, sr);

            HitObservation obs;
            obs.hitIndex = (int) accepted.size();
            obs.delayMs = timing.bassDelayMs;
            obs.polarityInvert = timing.bassPolarityInvert;
            obs.phaseFilterFreqHz = timing.phaseFilterFreqHz;
            obs.phaseFilterEnabled = false;   // rotator was disabled in Pass A
            obs.matchPercent = timing.afterMatchPercent;
            obs.signalConfidence = timing.confidence;
            obs.energy =
                  PhaseBands::table[0].weight * multi.bands[0].kickEnergy
                + PhaseBands::table[1].weight * multi.bands[1].kickEnergy
                + PhaseBands::table[2].weight * multi.bands[2].kickEnergy
                + PhaseBands::table[3].weight * multi.bands[3].kickEnergy;

            float bestBandEnergy = -1.0f;
            obs.dominantFrequencyHz = 60.0f;
            for (int band = 0; band < PhaseBands::lowEndBandCount; ++band)
            {
                const float e = multi.bands[(size_t) band].kickEnergy;
                if (e > bestBandEnergy)
                {
                    bestBandEnergy = e;
                    obs.dominantFrequencyHz = 0.5f * (PhaseBands::table[(size_t) band].lowHz
                                                    + PhaseBands::table[(size_t) band].highHz);
                }
            }

            AcceptedHit hit;
            hit.bass = bass;
            hit.kick = kick;
            hit.numSamples = n;
            hit.offlineFundamentalHz = pitch.frequencyHz;
            hit.offlinePitchConfidence = pitch.confidence;
            hit.trackedHz = w.trackedHzAtTrigger;
            hit.midi = NoteQuantizer::hzToMidi (pitch.frequencyHz);

            observations.push_back (obs);
            accepted.push_back (hit);
        }

        diag.analyzedHits = (int) accepted.size();
        diag.rejectedPitchHits = rejectedPitch;

        if (accepted.empty())
        {
            diag.warning = rejectedPitch > 0
                ? "No usable hits: pitch could not be confirmed."
                : "No usable hits: not enough signal.";
            result.message = diag.warning;
            return result;
        }

        // ---- Pass A: one global Delay + Polarity from consensus -------------
        const auto consensus = HitConsensus::analyze (observations);
        if (! consensus.hasConsensus || consensus.dominantClusterIndex < 0)
        {
            diag.warning = "No timing consensus across hits.";
            result.message = diag.warning;
            return result;
        }

        const auto& dom = consensus.clusters[(size_t) consensus.dominantClusterIndex];
        const float globalDelayMs = std::isfinite (dom.centroidDelayMs) ? dom.centroidDelayMs : 0.0f;
        const bool  globalPolarity = dom.centroidPolarity;

        diag.dominantClusterShare = consensus.dominantClusterShare;
        diag.multipleTimingFamilies = consensus.clusters.size() > 1
                                       && consensus.dominantClusterShare < 0.85f;

        // ---- Fixed-timing rotator search (global timing held FIXED) ---------
        std::vector<FixedTimingRotatorResult> rotatorResults;
        rotatorResults.reserve (accepted.size());
        std::vector<float> noFixMatch;      // baseline before any correction
        noFixMatch.reserve (accepted.size());

        for (const auto& hit : accepted)
        {
            rotatorResults.push_back (FixedTimingRotatorSearch::search (
                hit.bass, hit.kick, hit.numSamples, sr,
                globalDelayMs, globalPolarity, config.delayInterpolation,
                config.stages(), config.freqs(), config.qs()));

            PhaseFixRenderSettings none;
            none.bassPolarityInvert = false;
            none.bassDelayMs = 0.0f;
            none.phaseFilterEnabled = false;
            none.delayInterpolation = config.delayInterpolation;
            const auto raw = PhaseFixEngine::scoreSettings (
                hit.bass, hit.kick, hit.numSamples, sr, none,
                PhaseFixEngine::absoluteManualMaxDelayMs);
            noFixMatch.push_back (std::isfinite (raw.matchPercent) ? raw.matchPercent : 50.0f);
        }

        // ---- One global allpass stage count ---------------------------------
        const int globalStages = selectGlobalStages (rotatorResults, config.stages());

        // ---- Global fallback entry (aggregate ALL accepted hits) ------------
        NoteLearnAccumulator globalAcc;
        for (size_t i = 0; i < accepted.size(); ++i)
        {
            const auto cand = FixedTimingRotatorSearch::candidateForStage (rotatorResults[i], globalStages);
            globalAcc.add ({ accepted[i].offlineFundamentalHz,
                             cand.allpassFreqHz, cand.allpassQ,
                             accepted[i].offlinePitchConfidence, cand.helps });
        }
        NoteEntry globalEntry = globalAcc.aggregate();

        // ---- Pass B: per-note aggregation at the global stage count ---------
        std::array<NoteLearnAccumulator, (size_t) NotePhaseMapSnapshot::size> perNote;
        std::vector<int> hitNoteIndex ((size_t) accepted.size(), -1);

        for (size_t i = 0; i < accepted.size(); ++i)
        {
            const int idx = NotePhaseMapSnapshot::indexForMidi (accepted[i].midi);
            hitNoteIndex[i] = idx;
            if (idx < 0)
                continue;

            const auto cand = FixedTimingRotatorSearch::candidateForStage (rotatorResults[i], globalStages);
            perNote[(size_t) idx].add ({ accepted[i].offlineFundamentalHz,
                                         cand.allpassFreqHz, cand.allpassQ,
                                         accepted[i].offlinePitchConfidence, cand.helps });
        }

        NotePhaseMapSnapshot map = NoteMap::makeEmptyNoteMap();
        map.schemaVersion = NoteMap::kSchemaVersion;

        std::array<bool, (size_t) NotePhaseMapSnapshot::size> noteLearned {};
        int learnedNotes = 0;
        for (int idx = 0; idx < NotePhaseMapSnapshot::size; ++idx)
        {
            if (perNote[(size_t) idx].count() == 0)
                continue;

            const NoteEntry entry = perNote[(size_t) idx].finalizeEntry();
            if (entry.learned && NoteMap::isValidNoteEntry (entry))
            {
                map.notes[(size_t) idx] = entry;
                noteLearned[(size_t) idx] = true;
                ++learnedNotes;
            }
        }

        // A hit contributed only to the global fallback if its note was not
        // learned (out of range, too few hits, or rejected by spread/confidence).
        int globalOnly = 0;
        for (size_t i = 0; i < accepted.size(); ++i)
        {
            const int idx = hitNoteIndex[i];
            if (idx < 0 || ! noteLearned[(size_t) idx])
                ++globalOnly;
        }
        diag.globalOnlyHits = globalOnly;

        // ---- Base context + global fallback ---------------------------------
        map.base.delayMs = globalDelayMs;
        map.base.polarityInvert = globalPolarity;
        map.base.crossoverEnabled = config.crossoverEnabled;
        map.base.crossoverHz = (std::isfinite (config.crossoverHz) && config.crossoverHz > 0.0f)
                                   ? config.crossoverHz : 150.0f;
        map.base.allpassStages = std::clamp (globalStages, 2, 4);
        map.base.delayInterpolationIndex =
            (config.delayInterpolation == InterpolationType::Linear) ? 0 : 1;
        map.base.learnedSampleRate = sr;

        map.global = globalEntry;

        map.valid = NoteMap::isValidGlobalEntry (map.global)
                 && NoteMap::isValidBaseContext (map.base);

        // ---- Global fix (diagnostic PhaseFixResult) -------------------------
        result.globalFix = makeGlobalFix (globalDelayMs, globalPolarity,
                                          consensus, rotatorResults, noFixMatch);

        result.map = map;
        result.valid = NoteMap::isValidNoteMap (map);

        // ---- Final message --------------------------------------------------
        if (result.valid)
        {
            result.message = "Learned " + juce::String (learnedNotes) + " note"
                           + (learnedNotes == 1 ? "" : "s") + " from "
                           + juce::String (diag.analyzedHits) + " hit"
                           + (diag.analyzedHits == 1 ? "" : "s") + ".";
            if (diag.multipleTimingFamilies)
                diag.warning = "Multiple timing families detected; used the dominant one.";
        }
        else
        {
            diag.warning = "Learned data did not form a valid map.";
            result.message = diag.warning;
        }

        return result;
    }

private:
    struct AcceptedHit
    {
        const float* bass = nullptr;
        const float* kick = nullptr;
        int   numSamples = 0;
        float offlineFundamentalHz = 0.0f;
        float offlinePitchConfidence = 0.0f;
        float trackedHz = 0.0f;
        int   midi = -1;
    };

    // Pick one global allpass stage count: the stage with the largest total
    // helping gain across all hits. Ties (and the no-help case) prefer the
    // fewest stages, so the map stays minimal and deterministic.
    static int selectGlobalStages (const std::vector<FixedTimingRotatorResult>& results,
                                   const std::vector<int>& stageList)
    {
        if (stageList.empty())
            return 2;

        int bestStage = stageList.front();
        double bestGain = -1.0;

        for (int stage : stageList)
        {
            double gain = 0.0;
            for (const auto& r : results)
            {
                if (! r.valid)
                    continue;
                const auto cand = FixedTimingRotatorSearch::candidateForStage (r, stage);
                if (cand.helps)
                    gain += (double) cand.gainPoints;
            }

            // Strictly-greater so the earliest (fewest-stage) winner keeps ties.
            if (gain > bestGain + 1.0e-6)
            {
                bestGain = gain;
                bestStage = stage;
            }
        }

        return std::clamp (bestStage, 2, 4);
    }

    static PhaseFixResult makeGlobalFix (float delayMs,
                                         bool polarityInvert,
                                         const ConsensusResult& consensus,
                                         const std::vector<FixedTimingRotatorResult>& rotatorResults,
                                         const std::vector<float>& noFixMatch)
    {
        PhaseFixResult fix;
        fix.valid = true;
        fix.enoughSignal = true;
        fix.bassPolarityInvert = polarityInvert;
        fix.bassDelayMs = delayMs;
        fix.phaseFilterEnabled = false;
        fix.confidence = std::clamp (consensus.consensusConfidence, 0.0f, 1.0f);
        fix.contributingHits = (int) rotatorResults.size();
        fix.singleHitAnalysis = rotatorResults.size() == 1;

        // Before = mean no-fix match; after = mean global-timing (rotator-off)
        // match. Both are pure diagnostics; they do not gate map validity.
        double before = 0.0;
        double after = 0.0;
        int count = 0;
        for (size_t i = 0; i < rotatorResults.size(); ++i)
        {
            if (! rotatorResults[i].valid)
                continue;
            before += (double) (i < noFixMatch.size() ? noFixMatch[i] : rotatorResults[i].baselineMatchPercent);
            after += (double) rotatorResults[i].baselineMatchPercent;
            ++count;
        }

        if (count > 0)
        {
            fix.beforeMatchPercent = (float) (before / (double) count);
            fix.afterMatchPercent = (float) (after / (double) count);
            fix.predictedAfterMatchPercent = fix.afterMatchPercent;
            fix.displayBeforeMatchPercent = fix.beforeMatchPercent;
            fix.displayAfterMatchPercent = fix.afterMatchPercent;
            fix.improvementPercent = fix.afterMatchPercent - fix.beforeMatchPercent;
        }

        PhaseFixEngine::updateDerivedResultFields (fix);
        return fix;
    }
};
