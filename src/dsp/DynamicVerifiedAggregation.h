#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

#include "DynamicStateMeasurements.h"
#include "DynamicStateMap.h"
#include "../util/DynamicMeasurementResultQueue.h"

// =============================================================================
// Phase 9: fixed rolling verified aggregation.
//
// Audio-thread-owned (drained results are folded in at the block boundary,
// never from the worker directly). One fixed ring of the most recent
// kVerifiedRingCapacity fresh runtime events per occupied State, keyed by
// stableStateId AND the map generation that produced the correction being
// measured, so a stale worker result can never update a State it no longer
// describes. No growth, no allocation after construction.
// =============================================================================

class DynamicVerifiedAggregation
{
public:
    void reset() noexcept
    {
        for (auto& state : perState)
            state = PerState {};
    }

    // Reconciles the fixed per-slot rolling stores against a newly-activated
    // map generation. A slot whose occupant stable ID changed (or is no
    // longer occupied) is cleared to Unavailable. A slot whose stableStateId
    // is UNCHANGED is also cleared whenever the generation itself changed:
    // DynamicPackageResolution/DynamicZonePackage/DynamicState carry no
    // cheap package-identity/version stamp, so there is no way to cheaply
    // prove the State's resolved correction package (delay/polarity/allpass/
    // crossover) is still the exact one the retained ring's evidence was
    // measured against. Old evidence must never be relabelled as belonging
    // to a new generation on the strength of an unproven assumption -
    // prefer the simple safe clear. Only a reconcile() call repeating the
    // SAME generation (e.g. a redundant call) preserves the ring.
    void reconcile (const DynamicStateMap& map, uint64_t newGeneration) noexcept
    {
        for (int slot = 0; slot < DynamicMeasurementContract::kMaxRetainedStates; ++slot)
        {
            auto& state = perState[(size_t) slot];
            const auto& mapState = map.states[(size_t) slot];
            if (! mapState.occupied)
            {
                state = PerState {};
                continue;
            }
            if (! state.occupied || state.stableStateId != mapState.stableStateId
                || state.mapGeneration != newGeneration)
                state = PerState {};

            state.occupied = true;
            state.stableStateId = mapState.stableStateId;
            state.mapGeneration = newGeneration;
        }
    }

    // Folds one worker-scored result into its State's ring. Silently ignored
    // when the identity/generation no longer matches any live slot (stale
    // result) - this is the audio-thread half of the stale-generation guard.
    void addResult (const DynamicMeasurementScoredCapture& scored) noexcept
    {
        if (! scored.valid)
            return;
        for (auto& state : perState)
        {
            if (! state.occupied || state.stableStateId != scored.stableStateId
                || state.mapGeneration != scored.mapGeneration)
                continue;

            if (! scored.score.valid)
            {
                ++state.rejectedEvents;
                return;
            }

            state.beforeRing[(size_t) state.writeIndex] = DynamicMeasurementContract::sanitizeScore (scored.score.beforeScore);
            state.afterRing[(size_t) state.writeIndex] = DynamicMeasurementContract::sanitizeScore (scored.score.afterScore);
            state.improvementRing[(size_t) state.writeIndex] = scored.score.improvementPoints;
            state.writeIndex = (state.writeIndex + 1) % DynamicMeasurementContract::kVerifiedRingCapacity;
            state.count = std::min (state.count + 1, DynamicMeasurementContract::kVerifiedRingCapacity);
            ++state.totalEvents;
            state.lastUpdateSample = scored.triggerSample;
            return;
        }
    }

    DynamicMeasurementSummary summaryFor (int slot) const noexcept
    {
        using namespace DynamicMeasurementContract;
        DynamicMeasurementSummary summary = makeUnavailableDynamicMeasurementSummary();
        if (slot < 0 || slot >= kMaxRetainedStates)
            return summary;
        const auto& state = perState[(size_t) slot];
        summary.mapGeneration = state.mapGeneration;
        summary.lastUpdateSample = (uint64_t) std::max<int64_t> (0, state.lastUpdateSample);
        summary.rejectedWindowCount = state.rejectedEvents;
        if (! state.occupied)
            return summary;

        if (state.count == 0)
        {
            summary.availability = DynamicMeasurementAvailability::Unavailable;
            return summary;
        }

        std::array<float, kVerifiedRingCapacity> before {};
        std::array<float, kVerifiedRingCapacity> after {};
        std::array<float, kVerifiedRingCapacity> improvement {};
        for (int i = 0; i < state.count; ++i)
        {
            before[(size_t) i] = state.beforeRing[(size_t) i];
            after[(size_t) i] = state.afterRing[(size_t) i];
            improvement[(size_t) i] = state.improvementRing[(size_t) i];
        }

        summary.validWindowCount = (uint32_t) state.count;
        summary.beforeScore = medianOfPrefix (before, state.count);
        summary.afterScore = medianOfPrefix (after, state.count);
        const float medianImprovement = medianOfPrefix (improvement, state.count);
        const float worst = *std::min_element (improvement.begin(), improvement.begin() + state.count);
        const int improvedCount = (int) std::count_if (improvement.begin(), improvement.begin() + state.count,
            [] (float v) { return v > 0.0f; });
        const float improvedRatio = (float) improvedCount / (float) state.count;

        summary.improvementPoints = medianImprovement;
        summary.medianImprovementPoints = medianImprovement;
        summary.lowerQuantileImprovementPoints = lowerQuantileOfPrefix (improvement, state.count);
        summary.worstImprovementPoints = worst;
        summary.improvedWindowRatio = improvedRatio;
        summary.confidence = sanitizeUnit ((float) state.count / (float) (state.count + state.rejectedEvents + 1u));

        if (state.count < kMinVerifiedEvents)
        {
            summary.availability = DynamicMeasurementAvailability::Collecting;
            summary.assessment = DynamicCorrectionAssessment::Unknown;
            return summary;
        }

        summary.availability = DynamicMeasurementAvailability::Available;
        if (medianImprovement >= kMinimumMeaningfulGainPoints && improvedRatio > kMinImprovedWindowRatio)
            summary.assessment = DynamicCorrectionAssessment::VerifiedImprovement;
        else if (medianImprovement <= -kMinimumMeaningfulGainPoints || worst < -kMaxAcceptableWorstRegressionPoints)
            summary.assessment = worst < -kMaxAcceptableWorstRegressionPoints && medianImprovement > -kMinimumMeaningfulGainPoints
                ? DynamicCorrectionAssessment::Unstable : DynamicCorrectionAssessment::Regressed;
        else
            summary.assessment = DynamicCorrectionAssessment::Neutral;

        return summary;
    }

private:
    static float medianOfPrefix (const std::array<float, DynamicMeasurementContract::kVerifiedRingCapacity>& values, int count) noexcept
    {
        if (count <= 0)
            return 0.0f;
        std::array<float, DynamicMeasurementContract::kVerifiedRingCapacity> sorted {};
        for (int i = 0; i < count; ++i)
            sorted[(size_t) i] = values[(size_t) i];
        std::sort (sorted.begin(), sorted.begin() + count);
        const int mid = count / 2;
        return count % 2 != 0 ? sorted[(size_t) mid] : 0.5f * (sorted[(size_t) (mid - 1)] + sorted[(size_t) mid]);
    }

    static float lowerQuantileOfPrefix (const std::array<float, DynamicMeasurementContract::kVerifiedRingCapacity>& values, int count) noexcept
    {
        if (count <= 0)
            return 0.0f;
        std::array<float, DynamicMeasurementContract::kVerifiedRingCapacity> sorted {};
        for (int i = 0; i < count; ++i)
            sorted[(size_t) i] = values[(size_t) i];
        std::sort (sorted.begin(), sorted.begin() + count);
        const int index = (count - 1) / 4;
        return sorted[(size_t) index];
    }

    struct PerState
    {
        bool occupied = false;
        uint64_t stableStateId = 0;
        uint64_t mapGeneration = 0;
        int count = 0;
        int writeIndex = 0;
        uint32_t totalEvents = 0;
        uint32_t rejectedEvents = 0;
        int64_t lastUpdateSample = 0;
        std::array<float, DynamicMeasurementContract::kVerifiedRingCapacity> beforeRing {};
        std::array<float, DynamicMeasurementContract::kVerifiedRingCapacity> afterRing {};
        std::array<float, DynamicMeasurementContract::kVerifiedRingCapacity> improvementRing {};
    };

    std::array<PerState, DynamicMeasurementContract::kMaxRetainedStates> perState {};
};
