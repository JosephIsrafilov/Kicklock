#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "DynamicStateMeasurements.h"
#include "DynamicMeasurementScorer.h"
#include "DynamicFingerprintMatcher.h"
#include "DynamicPackageMorpher.h"
#include "DynamicLearnFormation.h"
#include "../util/LearnHitQueue.h"

// =============================================================================
// Phase 9: predicted State measurement + final-package cluster verification.
//
// Learn-worker-only (offline, allocation allowed). Two responsibilities:
//
//  1. Deterministically retain a bounded set of member bass/kick windows per
//     final stableStateId, re-matched against the already-formed map through
//     the frozen Phase-3 matcher (never a second clustering formula).
//  2. Render the exact final resolved package against every retained window
//     with the canonical DynamicMeasurementScorer, aggregate robustly into a
//     predicted DynamicMeasurementSummary, and (for Auto Stable States only)
//     gate hasLearnedPackage on that aggregate — demoting a package that
//     cannot survive being rendered as one final package even when every
//     per-hit candidate looked individually beneficial.
//
// Retained windows are temporary evidence: this header returns only the
// aggregated summaries (and the possibly-demoted map). No raw audio survives
// past this call.
// =============================================================================

inline constexpr int kDynamicPredictedMeasurementMaxRetainedPerState =
    DynamicMeasurementContract::kMaxRetainedWindowsPerState;

struct DynamicPredictedMeasurementResult
{
    DynamicStateMap map = makeEmptyDynamicStateMap();
    std::array<DynamicMeasurementSummary, DynamicMeasurementContract::kMaxRetainedStates> predicted {};
};

namespace DynamicPredictedMeasurementDetail
{
    struct RetainedCandidate
    {
        int windowIndex = -1;
        int64_t triggerSample = -1;
        float distance = 0.0f;
        DynamicFingerprintPrototype fingerprint;
    };

    inline bool candidateLess (const RetainedCandidate& a, const RetainedCandidate& b) noexcept
    {
        if (a.distance != b.distance)
            return a.distance < b.distance;
        if (a.triggerSample != b.triggerSample)
            return a.triggerSample < b.triggerSample;
        for (int i = 0; i < DynamicFingerprintContract::kFeatureCount; ++i)
            if (a.fingerprint.features[(size_t) i] != b.fingerprint.features[(size_t) i])
                return a.fingerprint.features[(size_t) i] < b.fingerprint.features[(size_t) i];
        return false;
    }

    // Deterministic bounded selection independent of input encounter order:
    // sorts purely on intrinsic per-hit values (distance / trigger / feature
    // vector), then keeps a central half (closest to the prototype) and a
    // spread half (furthest, still within the matcher's own threshold), so
    // the retained set is representative rather than "whichever hits came
    // first".
    inline std::vector<int> selectRetainedIndices (std::vector<RetainedCandidate> candidates, int cap)
    {
        std::vector<int> selected;
        if (candidates.empty())
            return selected;

        std::sort (candidates.begin(), candidates.end(), candidateLess);

        if ((int) candidates.size() <= cap)
        {
            selected.reserve (candidates.size());
            for (const auto& c : candidates)
                selected.push_back (c.windowIndex);
            return selected;
        }

        const int centralCount = (cap + 1) / 2;
        const int edgeCount = cap - centralCount;
        selected.reserve ((size_t) cap);
        for (int i = 0; i < centralCount; ++i)
            selected.push_back (candidates[(size_t) i].windowIndex);
        for (int i = 0; i < edgeCount; ++i)
            selected.push_back (candidates[candidates.size() - 1 - (size_t) i].windowIndex);
        return selected;
    }

    inline float medianOf (std::vector<float> values) noexcept
    {
        if (values.empty())
            return 0.0f;
        std::sort (values.begin(), values.end());
        const size_t mid = values.size() / 2;
        return values.size() % 2 != 0 ? values[mid] : 0.5f * (values[mid - 1] + values[mid]);
    }

    // Lower robust quantile (25th percentile, nearest-rank) as a bounded
    // pessimistic statistic that cannot be hidden by a few very good windows.
    inline float lowerQuantileOf (std::vector<float> values) noexcept
    {
        if (values.empty())
            return 0.0f;
        std::sort (values.begin(), values.end());
        const size_t index = (values.size() - 1) / 4;
        return values[index];
    }
}

// One retained bass/kick pair, keyed by its final matched stableStateId.
// Worker-owned; never touched by the audio thread.
struct DynamicRetainedMeasurementWindow
{
    int64_t triggerSample = -1;
    std::vector<float> bass;
    std::vector<float> kick;
};

// Deterministically retains up to kMaxRetainedWindowsPerState member windows
// per occupied State by re-matching each ORIGINAL captured hit window (real
// audio, never the timing-free synthetic triggers finalizeDynamic() adds for
// fingerprint density) against the already-formed map through the frozen
// Phase-3 matcher. A window whose hit is Ambiguous/Unknown/InvalidInput for
// the final map is excluded, matching "exclude categorically ambiguous
// cluster assignments".
inline std::array<std::vector<DynamicRetainedMeasurementWindow>, DynamicMeasurementContract::kMaxRetainedStates>
    retainDynamicLearnMeasurementWindows (const std::vector<DynamicLearnHit>& hits,
                                          const std::vector<LearnHitWindow>& windows,
                                          const DynamicStateMap& map,
                                          double sampleRate)
{
    using namespace DynamicPredictedMeasurementDetail;
    std::array<std::vector<DynamicRetainedMeasurementWindow>, DynamicMeasurementContract::kMaxRetainedStates> result;
    if (! isStructurallyValidDynamicStateMap (map) || windows.empty() || hits.empty())
        return result;

    // hits[] and windows[] are independently ordered (hits is Phase-8's
    // fingerprint-eligible subset); join on (sequence, triggerSample), which
    // is unique per captured window within one Learn session.
    std::array<std::vector<RetainedCandidate>, DynamicMeasurementContract::kMaxRetainedStates> perSlot;

    for (int windowIndex = 0; windowIndex < (int) windows.size(); ++windowIndex)
    {
        const auto& window = windows[(size_t) windowIndex];
        if (window.bass.empty() || window.kick.empty())
            continue;

        const auto hitIt = std::find_if (hits.begin(), hits.end(), [&] (const DynamicLearnHit& hit)
        {
            return hit.sequence == window.sequence && hit.triggerSample == window.absoluteSampleAtTrigger;
        });
        if (hitIt == hits.end() || hitIt->fingerprintValidity != DynamicFingerprintValidity::Valid)
            continue;

        DynamicFingerprintObservation observation;
        observation.triggerSample = hitIt->triggerSample;
        observation.readySample = hitIt->triggerSample;
        observation.fingerprint.validity = DynamicFingerprintValidity::Valid;
        observation.fingerprint.extractorVersion = DynamicFingerprintContract::kExtractorVersion;
        observation.fingerprint.featureCount = hitIt->fingerprint.featureCount;
        observation.fingerprint.features = hitIt->fingerprint.features;

        const auto match = matchDynamicFingerprint (observation, map);
        if (match.decision != DynamicMatchDecision::Matched)
            continue;

        int slot = -1;
        for (int s = 0; s < DynamicStateMapContract::kMaxPersistentStates; ++s)
            if (map.states[(size_t) s].occupied && map.states[(size_t) s].stableStateId == match.selectedStableStateId)
            {
                slot = s;
                break;
            }
        if (slot < 0)
            continue;

        RetainedCandidate candidate;
        candidate.windowIndex = windowIndex;
        candidate.triggerSample = hitIt->triggerSample;
        candidate.distance = match.nearestDistance;
        candidate.fingerprint = hitIt->fingerprint;
        perSlot[(size_t) slot].push_back (candidate);
    }

    for (int slot = 0; slot < DynamicMeasurementContract::kMaxRetainedStates; ++slot)
    {
        const auto selectedIndices = selectRetainedIndices (
            perSlot[(size_t) slot], kDynamicPredictedMeasurementMaxRetainedPerState);
        result[(size_t) slot].reserve (selectedIndices.size());
        for (const int windowIndex : selectedIndices)
        {
            const auto& window = windows[(size_t) windowIndex];
            DynamicRetainedMeasurementWindow retained;
            retained.triggerSample = window.absoluteSampleAtTrigger;
            retained.bass = window.bass;
            retained.kick = window.kick;
            result[(size_t) slot].push_back (std::move (retained));
        }
    }

    (void) sampleRate;
    return result;
}

// Renders the exact Strength-1 final package against every retained window
// for one State, aggregates robustly, and (for an Auto Stable State only)
// demotes hasLearnedPackage when the final package fails cluster
// verification. Recognized-no-correction and Manual states are measured as
// identities without a demotion gate; Candidate States are measured but
// always report CandidateOnly regardless of the numeric result.
inline DynamicPredictedMeasurementResult computeDynamicPredictedMeasurement (
    const DynamicStateMap& formedMap,
    const std::array<std::vector<DynamicRetainedMeasurementWindow>, DynamicMeasurementContract::kMaxRetainedStates>& retained,
    double sampleRate,
    uint64_t mapGeneration)
{
    using namespace DynamicMeasurementContract;
    DynamicPredictedMeasurementResult out;
    out.map = formedMap;
    if (! isStructurallyValidDynamicStateMap (formedMap) || ! DynamicAllpassPoleDomain::isValidSampleRate (sampleRate))
        return out;

    DynamicMeasurementScorer::Scratch scratch;

    for (int slot = 0; slot < DynamicMeasurementContract::kMaxRetainedStates; ++slot)
    {
        DynamicState& state = out.map.states[(size_t) slot];
        DynamicMeasurementSummary& summary = out.predicted[(size_t) slot];
        summary = makeUnavailableDynamicMeasurementSummary();
        summary.mapGeneration = mapGeneration;

        if (! state.occupied)
            continue;

        if (! state.enabled)
        {
            summary.assessment = DynamicCorrectionAssessment::Disabled;
            summary.rejectionReason = DynamicCorrectionRejectionReason::Disabled;
            continue;
        }
        if (state.bypassed)
        {
            summary.assessment = DynamicCorrectionAssessment::Bypassed;
            summary.rejectionReason = DynamicCorrectionRejectionReason::Bypassed;
            continue;
        }

        const auto& windows = retained[(size_t) slot];
        if ((int) windows.size() < 1)
        {
            summary.availability = DynamicMeasurementAvailability::Unavailable;
            summary.rejectionReason = DynamicCorrectionRejectionReason::InsufficientWindows;
            if (state.origin == DynamicStateOrigin::Auto && state.evidence == DynamicStateEvidence::Candidate)
                summary.assessment = DynamicCorrectionAssessment::CandidateOnly;
            continue;
        }

        const bool isRecognizedNoCorrection = state.origin == DynamicStateOrigin::Auto
            && state.evidence == DynamicStateEvidence::Stable && ! state.hasLearnedPackage;
        const bool isCandidate = state.origin == DynamicStateOrigin::Auto
            && state.evidence == DynamicStateEvidence::Candidate;
        const bool hasCorrectionPackage = (state.origin == DynamicStateOrigin::Auto && state.hasLearnedPackage)
            || (state.origin == DynamicStateOrigin::Manual && state.hasManualBasePackage);

        if (! hasCorrectionPackage && isRecognizedNoCorrection)
        {
            // The ONE place Global's real, shared package is rendered against
            // this identity's actual retained material. A recognized identity
            // with no State-level package always falls through to the Global
            // branch at runtime (DynamicPackageDecision::GlobalRecognizedNoCorrection),
            // so if Global itself is harmful for it, that harm must be caught
            // and gated here rather than only logged: the harmful package
            // must never reach accepted audible output for this identity.
            const auto globalPackage = resolveDynamicPackage (out.map, 0, 1.0, sampleRate);
            if (! globalPackage.valid)
            {
                summary.availability = DynamicMeasurementAvailability::Invalid;
                summary.rejectionReason = DynamicCorrectionRejectionReason::InvalidPackage;
                continue;
            }

            std::vector<float> beforeScores, afterScores, improvements;
            beforeScores.reserve (windows.size());
            afterScores.reserve (windows.size());
            improvements.reserve (windows.size());
            uint32_t rejected = 0;
            for (const auto& w : windows)
            {
                const int n = (int) std::min (w.bass.size(), w.kick.size());
                if (n <= 8) { ++rejected; continue; }
                const auto scored = DynamicMeasurementScorer::score (
                    w.bass.data(), w.kick.data(), n, sampleRate, globalPackage, scratch);
                if (! scored.valid) { ++rejected; continue; }
                beforeScores.push_back (scored.beforeScore);
                afterScores.push_back (scored.afterScore);
                improvements.push_back (scored.improvementPoints);
            }

            if (improvements.empty())
            {
                summary.availability = DynamicMeasurementAvailability::Invalid;
                summary.rejectionReason = DynamicCorrectionRejectionReason::InvalidFingerprint;
                continue;
            }

            const float medianImprovement = DynamicPredictedMeasurementDetail::medianOf (improvements);
            const float worst = *std::min_element (improvements.begin(), improvements.end());
            const int improvedCount = (int) std::count_if (improvements.begin(), improvements.end(),
                [] (float v) { return v > 0.0f; });

            summary.availability = (int) improvements.size() >= kMinPredictedWindows
                ? DynamicMeasurementAvailability::Available : DynamicMeasurementAvailability::InsufficientMaterial;
            summary.validWindowCount = (uint32_t) improvements.size();
            summary.rejectedWindowCount = rejected;
            summary.beforeScore = DynamicPredictedMeasurementDetail::medianOf (beforeScores);
            summary.afterScore = DynamicPredictedMeasurementDetail::medianOf (afterScores);
            summary.improvementPoints = medianImprovement;
            summary.medianImprovementPoints = medianImprovement;
            summary.lowerQuantileImprovementPoints = DynamicPredictedMeasurementDetail::lowerQuantileOf (improvements);
            summary.worstImprovementPoints = worst;
            summary.improvedWindowRatio = (float) improvedCount / (float) improvements.size();
            summary.confidence = sanitizeUnit ((float) improvements.size() / (float) (improvements.size() + rejected + 1u));

            // Dual gate: either the median or the single worst-case regression
            // exceeding the bound is enough - never accepted on average alone.
            const bool globalExcessivelyHarmful = summary.availability == DynamicMeasurementAvailability::Available
                && (medianImprovement < -kMaxAcceptableWorstRegressionPoints
                    || worst < -kMaxAcceptableWorstRegressionPoints);

            if (globalExcessivelyHarmful)
            {
                state.correctionPolicy = DynamicCorrectionPolicy::NeutralSafe;
                state.policyRejectionReason = DynamicPolicyRejectionReason::GlobalPackageExcessiveRegression;
                summary.assessment = DynamicCorrectionAssessment::Regressed;
                summary.rejectionReason = DynamicCorrectionRejectionReason::ExcessiveWorstRegression;
            }
            else
            {
                state.correctionPolicy = DynamicCorrectionPolicy::GlobalFallback;
                state.policyRejectionReason = DynamicPolicyRejectionReason::None;
                summary.assessment = DynamicCorrectionAssessment::RecognizedNoCorrection;
                summary.rejectionReason = DynamicCorrectionRejectionReason::None;
            }
            continue;
        }

        if (! hasCorrectionPackage)
        {
            // Candidate (not yet correction-eligible at runtime): identity-only
            // measurement, before/after are the same score, no fabricated
            // improvement and no policy gating (Candidates are never routed
            // through Global-vs-Neutral at runtime until promoted to Stable).
            std::vector<float> scores;
            scores.reserve (windows.size());
            uint32_t rejected = 0;
            for (const auto& w : windows)
            {
                const int n = (int) std::min (w.bass.size(), w.kick.size());
                if (n <= 8) { ++rejected; continue; }
                const auto before = MultiBandCorrelation::analyze (w.bass.data(), w.kick.data(), n, sampleRate);
                if (! std::isfinite (before.weightedMatchPercent)) { ++rejected; continue; }
                scores.push_back (sanitizeScore (before.weightedMatchPercent));
            }
            if (scores.empty())
            {
                summary.availability = DynamicMeasurementAvailability::Invalid;
                summary.rejectionReason = DynamicCorrectionRejectionReason::InvalidFingerprint;
                continue;
            }
            const float med = DynamicPredictedMeasurementDetail::medianOf (scores);
            summary.availability = DynamicMeasurementAvailability::Available;
            summary.assessment = DynamicCorrectionAssessment::CandidateOnly;
            summary.rejectionReason = DynamicCorrectionRejectionReason::CandidateEvidence;
            summary.validWindowCount = (uint32_t) scores.size();
            summary.rejectedWindowCount = rejected;
            summary.beforeScore = med;
            summary.afterScore = med;
            summary.confidence = sanitizeUnit ((float) scores.size() / (float) (scores.size() + rejected));
            continue;
        }

        // resolveDynamicPackage() gates Auto States on evidence == Stable (a
        // Candidate is never correction-eligible at runtime, by design), so
        // measuring "what this package would do if promoted" needs a
        // Stable-evidence PROBE of the map - never the real map, and never
        // written back. This is scoring-only; state.evidence in the
        // returned map is untouched.
        DynamicStateMap probeMap = out.map;
        if (isCandidate)
            probeMap.states[(size_t) slot].evidence = DynamicStateEvidence::Stable;
        const auto package = resolveDynamicPackage (probeMap, state.stableStateId, 1.0, sampleRate);
        if (! package.valid || package.decision != DynamicPackageDecision::StateCorrection)
        {
            summary.availability = DynamicMeasurementAvailability::Invalid;
            summary.rejectionReason = DynamicCorrectionRejectionReason::InvalidPackage;
            if (isCandidate)
                summary.assessment = DynamicCorrectionAssessment::CandidateOnly;
            continue;
        }

        std::vector<float> beforeScores, afterScores, improvements;
        beforeScores.reserve (windows.size());
        afterScores.reserve (windows.size());
        improvements.reserve (windows.size());
        uint32_t rejected = 0;

        for (const auto& w : windows)
        {
            const int n = (int) std::min (w.bass.size(), w.kick.size());
            if (n <= 8) { ++rejected; continue; }
            const auto scored = DynamicMeasurementScorer::score (
                w.bass.data(), w.kick.data(), n, sampleRate, package, scratch);
            if (! scored.valid) { ++rejected; continue; }
            beforeScores.push_back (scored.beforeScore);
            afterScores.push_back (scored.afterScore);
            improvements.push_back (scored.improvementPoints);
        }

        if (improvements.empty())
        {
            summary.availability = DynamicMeasurementAvailability::Invalid;
            summary.rejectionReason = DynamicCorrectionRejectionReason::InvalidFingerprint;
            if (state.origin == DynamicStateOrigin::Auto && state.evidence == DynamicStateEvidence::Stable)
            {
                state.hasLearnedPackage = false;
                state.manualTrim = makeZeroDynamicManualTrim();
            }
            continue;
        }

        const float medianImprovement = DynamicPredictedMeasurementDetail::medianOf (improvements);
        const float lowerQuantile = DynamicPredictedMeasurementDetail::lowerQuantileOf (improvements);
        const float worst = *std::min_element (improvements.begin(), improvements.end());
        const int improvedCount = (int) std::count_if (improvements.begin(), improvements.end(),
            [] (float v) { return v > 0.0f; });
        const float improvedRatio = (float) improvedCount / (float) improvements.size();
        const float confidence = sanitizeUnit ((float) improvements.size()
            / (float) (improvements.size() + rejected + 1u));

        summary.availability = (int) improvements.size() >= kMinPredictedWindows
            ? DynamicMeasurementAvailability::Available : DynamicMeasurementAvailability::InsufficientMaterial;
        summary.validWindowCount = (uint32_t) improvements.size();
        summary.rejectedWindowCount = rejected;
        summary.beforeScore = DynamicPredictedMeasurementDetail::medianOf (beforeScores);
        summary.afterScore = DynamicPredictedMeasurementDetail::medianOf (afterScores);
        summary.improvementPoints = medianImprovement;
        summary.medianImprovementPoints = medianImprovement;
        summary.lowerQuantileImprovementPoints = lowerQuantile;
        summary.worstImprovementPoints = worst;
        summary.improvedWindowRatio = improvedRatio;
        summary.confidence = confidence;

        if (isCandidate)
        {
            summary.assessment = DynamicCorrectionAssessment::CandidateOnly;
            summary.rejectionReason = DynamicCorrectionRejectionReason::CandidateEvidence;
            continue;
        }

        // The numeric verdict is honest regardless of origin - a Manual
        // State's user-authored package can still measure as Regressed or
        // Unstable. Only the DEMOTION (mutating the map to withdraw a
        // package) is restricted to Auto Stable States below; Manual States
        // are never silently changed.
        const bool measuresAsBeneficial = summary.availability == DynamicMeasurementAvailability::Available
            && medianImprovement >= kMinimumMeaningfulGainPoints
            && improvedRatio > kMinImprovedWindowRatio
            && worst >= -kMaxAcceptableWorstRegressionPoints
            && confidence >= kMinConfidence;

        if (measuresAsBeneficial)
        {
            summary.assessment = DynamicCorrectionAssessment::PredictedImprovement;
            summary.rejectionReason = DynamicCorrectionRejectionReason::None;
            state.correctionPolicy = DynamicCorrectionPolicy::LearnedState;
            state.policyRejectionReason = DynamicPolicyRejectionReason::None;
        }
        else if (state.origin == DynamicStateOrigin::Manual)
        {
            summary.assessment = worst < -kMaxAcceptableWorstRegressionPoints
                ? DynamicCorrectionAssessment::Unstable : DynamicCorrectionAssessment::Neutral;
            summary.rejectionReason = medianImprovement < kMinimumMeaningfulGainPoints
                ? DynamicCorrectionRejectionReason::NonBeneficial
                : DynamicCorrectionRejectionReason::InconsistentAcrossHits;
            // Manual packages are never silently withdrawn (see comment
            // above), so this identity still carries its own package.
            state.correctionPolicy = DynamicCorrectionPolicy::LearnedState;
        }
        else if (state.origin == DynamicStateOrigin::Auto)
        {
            // Demote: the State stays occupied, matchable and recognizable;
            // only the automatic correction is withdrawn.
            state.hasLearnedPackage = false;
            state.manualTrim = makeZeroDynamicManualTrim();
            state.correctionPolicy = DynamicCorrectionPolicy::GlobalFallback;
            state.policyRejectionReason = DynamicPolicyRejectionReason::None;

            summary.assessment = worst < -kMaxAcceptableWorstRegressionPoints
                ? DynamicCorrectionAssessment::Unstable : DynamicCorrectionAssessment::RecognizedNoCorrection;
            summary.rejectionReason = medianImprovement < kMinimumMeaningfulGainPoints
                ? DynamicCorrectionRejectionReason::NonBeneficial
                : (improvedRatio <= kMinImprovedWindowRatio
                       ? DynamicCorrectionRejectionReason::InconsistentAcrossHits
                       : DynamicCorrectionRejectionReason::ExcessiveWorstRegression);
        }
    }

    return out;
}
