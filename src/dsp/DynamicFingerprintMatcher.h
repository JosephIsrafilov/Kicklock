#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

#include "DynamicStateMap.h"
#include "DynamicFingerprintExtractor.h"

// =============================================================================
// Pure v1 State matcher (Phase 3).
//
// Stateless: it maps one completed observation against one DynamicStateMap and
// returns a fixed diagnostic result. No Hold, debounce, transport, crossfade,
// allocation, locks, exceptions or dynamic strings. Stable State ID is identity;
// the array slot is never identity.
// =============================================================================

enum class DynamicMatchDecision : uint8_t
{
    Matched = 0,
    Ambiguous,
    Unknown,
    InvalidInput,
    NoEligibleStates
};

struct DynamicMatchResult
{
    DynamicMatchDecision decision = DynamicMatchDecision::InvalidInput;

    uint64_t selectedStableStateId = 0;
    int selectedSlot = -1; // array index of the nearest state, or -1

    uint64_t nearestStableStateId = 0;
    float nearestDistance = std::numeric_limits<float>::infinity();

    uint64_t secondStableStateId = 0;
    float secondDistance = std::numeric_limits<float>::infinity();

    bool correctionAvailable = false;
    bool selectedBypassed = false;
    // True iff the matched identity's persisted policy is NeutralSafe: the
    // scheduler must route it to the shared Neutral branch regardless of
    // correctionAvailable, since a NeutralSafe identity's own Global fallback
    // was proven harmful and must never reach audible output for it.
    bool selectedNeutralSafe = false;
    int eligibleStateCount = 0;
};

namespace DynamicFingerprintMatcherDetail
{
    // Candidates are recognizable identities but never correction-eligible.
    // This lets a three-hit State route deterministically to Global while the
    // five-hit Stable gate continues to protect automatic correction.
    inline bool isEligibleState (const DynamicState& state) noexcept
    {
        if (! state.occupied || ! state.enabled)
            return false;
        if (! isRuntimeEligibleDynamicFingerprintV1 (state.fingerprint,
                                                     DynamicFingerprintContract::kExtractorVersion))
            return false;

        if (state.origin == DynamicStateOrigin::Auto)
            return isValidDynamicStateEvidence (state.evidence);
        if (state.origin == DynamicStateOrigin::Manual)
            return isValidDynamicStateEvidence (state.evidence);
        return false;
    }

    inline bool stateHasCorrection (const DynamicState& state) noexcept
    {
        return (state.origin != DynamicStateOrigin::Auto
                || state.evidence == DynamicStateEvidence::Stable)
            && (state.hasLearnedPackage || state.hasManualBasePackage);
    }
}

// Matches one completed observation against a DynamicStateMap. The decision
// policy is: InvalidInput (bad map/calibration/extractor/observation) ->
// NoEligibleStates -> Unknown (nearest beyond threshold) -> Ambiguous
// (second-best too close) -> Matched. Boundaries are inclusive on the threshold
// and exclusive on the ambiguity margin.
inline DynamicMatchResult matchDynamicFingerprint (const DynamicFingerprintObservation& observation,
                                                   const DynamicStateMap& map) noexcept
{
    using namespace DynamicFingerprintMatcherDetail;

    DynamicMatchResult result;

    const DynamicFingerprintPrototype prototype = observation.fingerprint.toPrototype();
    const bool observationEligible =
        observation.fingerprint.isValid()
        && observation.fingerprint.extractorVersion == DynamicFingerprintContract::kExtractorVersion
        && isRuntimeEligibleDynamicFingerprintV1 (prototype, observation.fingerprint.extractorVersion);

    const bool mapEligible =
        isStructurallyValidDynamicStateMap (map)
        && map.fingerprintExtractorVersion == DynamicFingerprintContract::kExtractorVersion
        && isValidDynamicMatchCalibration (map.calibration);

    if (! observationEligible || ! mapEligible)
    {
        result.decision = DynamicMatchDecision::InvalidInput;
        return result;
    }

    const float threshold = map.calibration.absoluteDistanceThreshold;
    const float margin = map.calibration.ambiguityMargin;

    int nearestSlot = -1;
    int secondSlot = -1;
    float nearestDistance = std::numeric_limits<float>::infinity();
    float secondDistance = std::numeric_limits<float>::infinity();
    uint64_t nearestId = 0;
    uint64_t secondId = 0;
    int eligibleCount = 0;

    for (int i = 0; i < DynamicStateMapContract::kMaxPersistentStates; ++i)
    {
        const DynamicState& state = map.states[(size_t) i];
        if (! isEligibleState (state))
            continue;
        ++eligibleCount;

        const float distance = dynamicFingerprintDistanceV1 (prototype, state.fingerprint);
        if (! std::isfinite (distance))
            continue;

        // Deterministic ordering: smaller distance wins; ties break on the lower
        // stable State ID so the selected identity is independent of slot order.
        const bool beatsNearest = distance < nearestDistance
            || (distance == nearestDistance && (nearestSlot < 0 || state.stableStateId < nearestId));
        if (beatsNearest)
        {
            secondSlot = nearestSlot;
            secondDistance = nearestDistance;
            secondId = nearestId;

            nearestSlot = i;
            nearestDistance = distance;
            nearestId = state.stableStateId;
        }
        else
        {
            const bool beatsSecond = distance < secondDistance
                || (distance == secondDistance && (secondSlot < 0 || state.stableStateId < secondId));
            if (beatsSecond)
            {
                secondSlot = i;
                secondDistance = distance;
                secondId = state.stableStateId;
            }
        }
    }

    result.eligibleStateCount = eligibleCount;

    if (eligibleCount == 0 || nearestSlot < 0)
    {
        result.decision = DynamicMatchDecision::NoEligibleStates;
        return result;
    }

    result.nearestStableStateId = nearestId;
    result.nearestDistance = nearestDistance;
    result.secondStableStateId = secondId;
    result.secondDistance = secondDistance;

    const DynamicState& nearestState = map.states[(size_t) nearestSlot];
    result.selectedStableStateId = nearestId;
    result.selectedSlot = nearestSlot;
    result.correctionAvailable = stateHasCorrection (nearestState);
    result.selectedBypassed = nearestState.bypassed;
    result.selectedNeutralSafe = nearestState.correctionPolicy == DynamicCorrectionPolicy::NeutralSafe;

    if (nearestDistance > threshold)
    {
        result.decision = DynamicMatchDecision::Unknown;
        return result;
    }

    if (secondSlot >= 0 && (secondDistance - nearestDistance) < margin)
    {
        result.decision = DynamicMatchDecision::Ambiguous;
        return result;
    }

    result.decision = DynamicMatchDecision::Matched;
    return result;
}
