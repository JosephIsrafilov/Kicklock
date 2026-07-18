#pragma once

#include <array>
#include <cstdint>

#include "DynamicStateMeasurements.h"
#include "DynamicStateMap.h"
#include "DynamicFingerprintMatcher.h"
#include "DynamicSelectorTypes.h"
#include "DynamicMapSourceResolver.h"

// =============================================================================
// Phase 9: fixed, UI-ready Dynamic runtime snapshot.
//
// A complete, self-contained value: no dynamic strings, no vectors, no
// pointers into map/audio storage, no ValueTree. Safe to copy on the audio
// thread and safe for a UI/message-thread reader to hold after the next
// audio block. This is data only - Phase 10 owns the UI that reads it.
// =============================================================================

struct DynamicStateCard
{
    bool occupied = false;
    uint64_t stableStateId = 0;
    int slot = -1;
    DynamicStateOrigin origin = DynamicStateOrigin::Auto;
    DynamicStateEvidence evidence = DynamicStateEvidence::Candidate;
    bool enabled = true;
    bool bypassed = false;
    uint32_t hitCount = 0;
    float repeatability = 0.0f;
    float ambiguity = 0.0f;
    bool hasCorrection = false;
    bool hasLikelyMidi = false;
    int likelyMidi = -1;
    bool hasLikelyPitchHz = false;
    float likelyPitchHz = 0.0f;

    // Live selection (Phase 6/7 mirror; slot alone is never identity).
    bool selected = false;
    bool active = false;
    DynamicSelectorBranchKind activeBranchKind = DynamicSelectorBranchKind::Global;
    bool hasCurrentMatchDistance = false;
    float currentMatchDistance = 0.0f;

    DynamicMeasurementSummary predicted;
    DynamicMeasurementSummary verified;
    DynamicCorrectionAssessment assessment = DynamicCorrectionAssessment::Unknown;
    DynamicCorrectionRejectionReason rejectionReason = DynamicCorrectionRejectionReason::None;
};

inline constexpr DynamicStateCard makeEmptyDynamicStateCard() noexcept { return {}; }

struct DynamicRuntimeSnapshot
{
    uint64_t sequence = 0;
    uint64_t mapGeneration = 0;
    DynamicMapSource source = DynamicMapSource::None;
    bool mapValid = false;
    int stateCount = 0;

    uint64_t activeSemanticStateId = 0;
    uint64_t selectedSemanticStateId = 0;
    DynamicSelectorBranchKind activeBranchKind = DynamicSelectorBranchKind::Global;

    DynamicMatchDecision matcherDecision = DynamicMatchDecision::InvalidInput;
    DynamicSelectorDiagnostic selectorDiagnostic = DynamicSelectorDiagnostic::None;
    bool holdActive = false;
    bool fallbackActive = true;

    bool sidechainPresent = false;
    bool bypassActive = false;

    uint64_t captureExhaustedCount = 0;
    uint64_t captureDroppedForTransportCount = 0;
    uint64_t measurementCaptureQueueDroppedCount = 0;
    uint64_t measurementScoreQueueDroppedCount = 0;

    int64_t timestampSample = 0;

    std::array<DynamicStateCard, DynamicMeasurementContract::kMaxRetainedStates> states {};
};

inline constexpr DynamicRuntimeSnapshot makeEmptyDynamicRuntimeSnapshot() noexcept { return {}; }
