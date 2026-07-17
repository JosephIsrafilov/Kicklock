#pragma once

#include <array>
#include <cstdint>

#include "DynamicFingerprintExtractor.h"
#include "DynamicFingerprintMatcher.h"
#include "DynamicHotBranchEngine.h"

// =============================================================================
// Phase 6: shared value contracts for the standalone selector scheduler and
// continuity mixer.
//
// This header defines only fixed-size, allocation-free data. It reads
// DynamicHotBranchInfo (Phase 5) and DynamicMatchResult (Phase 3) but does not
// extract fingerprints, compute distances or configure DSP packages.
// =============================================================================

namespace DynamicSelectorContract
{
    inline constexpr int kStateSlotCount = DynamicStateMapContract::kMaxPersistentStates;
    // Fixed ten low-band branch positions: Global, eight State slots, Service.
    inline constexpr int kBranchCount = kStateSlotCount + 2;
    inline constexpr int kGlobalBranchIndex = 0;
    inline constexpr int kFirstStateBranchIndex = 1;
    inline constexpr int kServiceBranchIndex = kBranchCount - 1;

    inline constexpr int kEventQueueCapacity =
        DynamicFingerprintContract::kMaxConcurrentDynamicFingerprintCaptures;

    inline constexpr uint32_t kMaximumHoldEvents = DynamicStateMapContract::kMaximumHoldEvents;

    inline constexpr float kGainEpsilon = 1.0e-6f;
}

enum class DynamicSelectorBranchKind : uint8_t
{
    Global = 0,
    State = 1,
    Service = 2
};

struct DynamicSelectorBranchRef
{
    DynamicSelectorBranchKind kind = DynamicSelectorBranchKind::Global;
    int stateSlot = -1;
    uint64_t stableStateId = 0;
};

inline constexpr DynamicSelectorBranchRef makeGlobalSelectorBranchRef() noexcept
{
    return DynamicSelectorBranchRef { DynamicSelectorBranchKind::Global, -1, 0 };
}

inline bool dynamicSelectorBranchRefsEqual (const DynamicSelectorBranchRef& a,
                                            const DynamicSelectorBranchRef& b) noexcept
{
    return a.kind == b.kind && a.stateSlot == b.stateSlot && a.stableStateId == b.stableStateId;
}

// Maps a branch reference to its fixed gain-vector index, or -1 when invalid.
inline int dynamicSelectorBranchIndex (const DynamicSelectorBranchRef& ref) noexcept
{
    using namespace DynamicSelectorContract;
    switch (ref.kind)
    {
        case DynamicSelectorBranchKind::Global:
            return kGlobalBranchIndex;
        case DynamicSelectorBranchKind::State:
            return (ref.stateSlot >= 0 && ref.stateSlot < kStateSlotCount)
                ? (kFirstStateBranchIndex + ref.stateSlot) : -1;
        case DynamicSelectorBranchKind::Service:
            return kServiceBranchIndex;
    }
    return -1;
}

// Fixed, read-only snapshot of Phase-5 branch availability and identity. The
// caller supplies this every render call; Phase 6 never mutates the persistent
// State map or the hot-branch engine from this snapshot.
struct DynamicSelectorBranchRoster
{
    DynamicHotBranchInfo global;
    std::array<DynamicHotBranchInfo, DynamicSelectorContract::kStateSlotCount> states {};
    DynamicHotBranchInfo service;

    // Explicit semantic binding: which stable State identity Service currently
    // represents. Phase 5 always stores 0 as Service's internal stableStateId;
    // this is Phase 6's own bookkeeping, supplied by the caller.
    uint64_t serviceBoundStableStateId = 0;
    bool serviceBindingValid = false;
};

inline bool isValidDynamicSelectorBranchRoster (const DynamicSelectorBranchRoster& roster) noexcept
{
    using namespace DynamicSelectorContract;

    if (roster.global.kind != DynamicHotBranchKind::Global || roster.global.stableStateId != 0)
        return false;

    std::array<uint64_t, kStateSlotCount> seenIds {};
    int seenCount = 0;
    for (int i = 0; i < kStateSlotCount; ++i)
    {
        const auto& state = roster.states[(size_t) i];
        if (! state.active)
            continue;
        if (state.kind != DynamicHotBranchKind::State || state.stableStateId == 0)
            return false;
        for (int j = 0; j < seenCount; ++j)
            if (seenIds[(size_t) j] == state.stableStateId)
                return false;
        seenIds[(size_t) seenCount++] = state.stableStateId;
    }

    if (roster.service.active
        && (roster.service.kind != DynamicHotBranchKind::Service || roster.service.stableStateId != 0))
        return false;

    if (roster.serviceBindingValid && roster.serviceBoundStableStateId == 0)
        return false;

    return true;
}

// Returns the slot of an active AND warm persistent State branch carrying this
// stable identity, or -1. Slot alone is never treated as identity.
inline int findWarmActiveStateSlotByStableId (const DynamicSelectorBranchRoster& roster,
                                              uint64_t stableStateId) noexcept
{
    if (stableStateId == 0)
        return -1;
    for (int i = 0; i < DynamicSelectorContract::kStateSlotCount; ++i)
    {
        const auto& state = roster.states[(size_t) i];
        if (state.active && state.warm && state.stableStateId == stableStateId)
            return i;
    }
    return -1;
}

// Returns the slot of an active (not necessarily warm) persistent State branch
// carrying this stable identity, or -1. Used to distinguish "does not exist"
// from "exists but not yet warm".
inline int findActiveStateSlotByStableId (const DynamicSelectorBranchRoster& roster,
                                          uint64_t stableStateId) noexcept
{
    if (stableStateId == 0)
        return -1;
    for (int i = 0; i < DynamicSelectorContract::kStateSlotCount; ++i)
    {
        const auto& state = roster.states[(size_t) i];
        if (state.active && state.stableStateId == stableStateId)
            return i;
    }
    return -1;
}

enum class DynamicSelectorDiagnostic : uint8_t
{
    None = 0,
    MatchedPersistentState,
    MatchedService,
    MatchedGlobalNoCorrection,
    MatchedGlobalBypassed,
    HeldAmbiguous,
    HeldUnknown,
    FallbackAmbiguous,
    FallbackUnknown,
    FallbackInvalidInput,
    FallbackNoEligibleStates,
    FallbackTargetUnavailable,
    FallbackTargetNotWarm,
    LateDecisionRejected,
    InvalidEvent,
    QueueExhausted,
    StaleBranchReference,
    TransportReset
};

enum class DynamicSelectorTransportReason : uint8_t
{
    Seek = 0,
    LoopWrap,
    StopStart,
    HostReset
};

// One scheduled selector observation. Timestamps are absolute stream sample
// positions, identical in meaning to DynamicFingerprintObservation.
struct DynamicSelectorEvent
{
    int64_t triggerSample = 0;
    int64_t readySample = 0;
    DynamicMatchResult match;
};

struct DynamicSelectorDiagnostics
{
    DynamicSelectorDiagnostic lastDecision = DynamicSelectorDiagnostic::None;
    int64_t triggerSample = 0;
    int64_t readySample = 0;
    uint64_t recognizedStableStateId = 0;
    uint64_t selectedSemanticStateId = 0;
    DynamicSelectorBranchKind selectedBranchKind = DynamicSelectorBranchKind::Global;
    int selectedStateSlot = -1;
    uint32_t holdEventCount = 0;
    int64_t holdAgeSamples = 0;
    bool fadeActive = false;
    int64_t fadeStartSample = 0;
    int64_t fadePosition = 0;
    int64_t fadeLength = 0;
    int64_t targetDeadlineSample = 0;
    int64_t safetySamples = 0;
    int queuedEventCount = 0;
    uint64_t staleReferenceCount = 0;
    uint64_t lateDecisionCount = 0;
    uint64_t invalidEventCount = 0;
    uint64_t nonFiniteGainRecoveryCount = 0;
};
