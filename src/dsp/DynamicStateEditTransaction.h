#pragma once

#include "DynamicStateMap.h"

// =============================================================================
// Phase 12: message-thread-only DynamicStateMap edit transactions.
//
// Each function takes a complete map, locates the target State purely by
// stableStateId (never by slot), applies exactly one mutation to a copy, and
// returns either a newly-valid complete map or the *original* map untouched
// plus a specific rejection reason. No JUCE dependency and no audio-thread
// concerns - these are pure value functions, unit-testable exactly like
// DynamicStateMap.h's own validators, and are the only place per-state
// mutation logic lives (PluginProcessor only adds publication/persistence
// around a call to one of these).
//
// All live parameter editing (Delay/Frequency/Q) goes through setManualTrim
// for both Auto and Manual states - manualBasePackage is only ever set once,
// at promotion or creation time, exactly mirroring how learnedPackage is the
// stable Auto base. There is deliberately no separate "set manual base
// package" transaction: it would be an unused abstraction, since nothing in
// the required workflow edits a Manual base package after promotion.
// =============================================================================

enum class DynamicStateEditRejectionReason : uint8_t
{
    None = 0,
    MapNotStructurallyValid,
    StateNotFound,
    InsufficientEvidence,
    AlreadyManual,
    RequiresAutoOrigin,
    RequiresManualOrigin,
    InvalidFingerprint,
    InvalidTrim,
    InvalidGlobalStartingPackage,
    MapValidationFailed,
    NoFreeCapacity,
    // Processor-level gating reasons (Section 10/11): these are never returned
    // by the pure transaction functions in this header, only by the
    // PluginProcessor wrapper that knows about preview/source state that this
    // header intentionally has no knowledge of.
    PreviewNotApplied,
    LegacyMapReadOnly,
    NoMapApplied,
    PublicationBusy
};

struct DynamicStateEditResult
{
    bool success = false;
    DynamicStateMap map;
    DynamicStateEditRejectionReason reason = DynamicStateEditRejectionReason::None;
};

namespace DynamicStateEditDetail
{
    inline int findOccupiedSlotByStableId (const DynamicStateMap& map, uint64_t stableStateId) noexcept
    {
        if (stableStateId == 0)
            return -1;
        for (int i = 0; i < DynamicStateMapContract::kMaxPersistentStates; ++i)
            if (map.states[(size_t) i].occupied && map.states[(size_t) i].stableStateId == stableStateId)
                return i;
        return -1;
    }

    inline int findFreeSlot (const DynamicStateMap& map) noexcept
    {
        for (int i = 0; i < DynamicStateMapContract::kMaxPersistentStates; ++i)
            if (! map.states[(size_t) i].occupied)
                return i;
        return -1;
    }

    inline DynamicStateEditResult reject (const DynamicStateMap& originalMap,
                                          DynamicStateEditRejectionReason reason) noexcept
    {
        return { false, originalMap, reason };
    }

    // Re-validates the whole mutated map before accepting it. On failure,
    // returns the *original* (pre-mutation) map untouched - the atomic
    // rejection contract - never the broken candidate.
    inline DynamicStateEditResult finish (const DynamicStateMap& originalMap,
                                          DynamicStateMap mutatedMap) noexcept
    {
        if (! isStructurallyValidDynamicStateMap (mutatedMap))
            return reject (originalMap, DynamicStateEditRejectionReason::MapValidationFailed);
        return { true, mutatedMap, DynamicStateEditRejectionReason::None };
    }

    inline DynamicZonePackage safeGlobalEquivalentPackage (const DynamicStateMap& map) noexcept
    {
        return { 0.0f, map.globalBase.globalAllpassFreqHz, map.globalBase.globalAllpassQ };
    }
}

// Additive manual trim, applied on top of whichever base package the State's
// origin uses (learnedPackage for Auto, manualBasePackage for Manual). Works
// uniformly for both origins - the only live-editing entry point.
inline DynamicStateEditResult setManualTrim (const DynamicStateMap& map,
                                             uint64_t stableStateId,
                                             const DynamicManualTrim& trim) noexcept
{
    using namespace DynamicStateEditDetail;
    if (! isStructurallyValidDynamicStateMap (map))
        return reject (map, DynamicStateEditRejectionReason::MapNotStructurallyValid);

    const int slot = findOccupiedSlotByStableId (map, stableStateId);
    if (slot < 0)
        return reject (map, DynamicStateEditRejectionReason::StateNotFound);

    if (! isValidDynamicManualTrim (trim))
        return reject (map, DynamicStateEditRejectionReason::InvalidTrim);

    DynamicStateMap mutated = map;
    mutated.states[(size_t) slot].manualTrim = trim;
    return finish (map, mutated);
}

inline DynamicStateEditResult resetManualTrim (const DynamicStateMap& map, uint64_t stableStateId) noexcept
{
    return setManualTrim (map, stableStateId, makeZeroDynamicManualTrim());
}

// Auto States only: zero the trim, keep learnedPackage and identity untouched.
inline DynamicStateEditResult resetToLearned (const DynamicStateMap& map, uint64_t stableStateId) noexcept
{
    using namespace DynamicStateEditDetail;
    if (! isStructurallyValidDynamicStateMap (map))
        return reject (map, DynamicStateEditRejectionReason::MapNotStructurallyValid);

    const int slot = findOccupiedSlotByStableId (map, stableStateId);
    if (slot < 0)
        return reject (map, DynamicStateEditRejectionReason::StateNotFound);
    if (map.states[(size_t) slot].origin != DynamicStateOrigin::Auto)
        return reject (map, DynamicStateEditRejectionReason::RequiresAutoOrigin);

    DynamicStateMap mutated = map;
    mutated.states[(size_t) slot].manualTrim = makeZeroDynamicManualTrim();
    return finish (map, mutated);
}

// Auto States only: clear the learned correction package so the State remains
// recognized (identity/fingerprint/evidence preserved) but resolves through
// Global. Manual States always require a manual base package by the frozen
// validity contract, so they cannot be "reset to Global" this way - the
// equivalent action for a Manual State is Remove Manual State.
inline DynamicStateEditResult resetToGlobal (const DynamicStateMap& map, uint64_t stableStateId) noexcept
{
    using namespace DynamicStateEditDetail;
    if (! isStructurallyValidDynamicStateMap (map))
        return reject (map, DynamicStateEditRejectionReason::MapNotStructurallyValid);

    const int slot = findOccupiedSlotByStableId (map, stableStateId);
    if (slot < 0)
        return reject (map, DynamicStateEditRejectionReason::StateNotFound);
    if (map.states[(size_t) slot].origin != DynamicStateOrigin::Auto)
        return reject (map, DynamicStateEditRejectionReason::RequiresAutoOrigin);

    DynamicStateMap mutated = map;
    auto& state = mutated.states[(size_t) slot];
    state.hasLearnedPackage = false;
    state.learnedPackage = {};
    state.manualTrim = makeZeroDynamicManualTrim();
    return finish (map, mutated);
}

inline DynamicStateEditResult setEnabled (const DynamicStateMap& map, uint64_t stableStateId, bool enabled) noexcept
{
    using namespace DynamicStateEditDetail;
    if (! isStructurallyValidDynamicStateMap (map))
        return reject (map, DynamicStateEditRejectionReason::MapNotStructurallyValid);

    const int slot = findOccupiedSlotByStableId (map, stableStateId);
    if (slot < 0)
        return reject (map, DynamicStateEditRejectionReason::StateNotFound);

    DynamicStateMap mutated = map;
    auto& state = mutated.states[(size_t) slot];
    state.enabled = enabled;
    // Disabled supersedes Bypassed: the frozen validity contract forbids
    // "not enabled && bypassed" (DynamicStateMap.h's isValidDynamicState), and
    // a disabled State has no runtime routing decision left to distinguish.
    if (! enabled)
        state.bypassed = false;
    return finish (map, mutated);
}

inline DynamicStateEditResult setBypassed (const DynamicStateMap& map, uint64_t stableStateId, bool bypassed) noexcept
{
    using namespace DynamicStateEditDetail;
    if (! isStructurallyValidDynamicStateMap (map))
        return reject (map, DynamicStateEditRejectionReason::MapNotStructurallyValid);

    const int slot = findOccupiedSlotByStableId (map, stableStateId);
    if (slot < 0)
        return reject (map, DynamicStateEditRejectionReason::StateNotFound);

    DynamicStateMap mutated = map;
    mutated.states[(size_t) slot].bypassed = bypassed;
    return finish (map, mutated);
}

// Section 10: preserves stableStateId/fingerprint/evidence/hitCount/
// repeatability/ambiguity/enabled/optional pitch metadata. Initializes the new
// manualBasePackage from a safe Global-equivalent package (zero delay delta,
// current Global allpass frequency/Q) and zeros manualTrim. Never invents an
// automatic improvement result; verification restarts as fresh runtime
// evidence (handled by the caller via the normal map-generation bump).
inline DynamicStateEditResult promoteToManual (const DynamicStateMap& map, uint64_t stableStateId) noexcept
{
    using namespace DynamicStateEditDetail;
    if (! isStructurallyValidDynamicStateMap (map))
        return reject (map, DynamicStateEditRejectionReason::MapNotStructurallyValid);

    const int slot = findOccupiedSlotByStableId (map, stableStateId);
    if (slot < 0)
        return reject (map, DynamicStateEditRejectionReason::StateNotFound);

    const auto& existing = map.states[(size_t) slot];
    if (existing.origin == DynamicStateOrigin::Manual)
        return reject (map, DynamicStateEditRejectionReason::AlreadyManual);
    if (existing.hitCount < DynamicStateMapContract::kManualMinimumRepeatableHits)
        return reject (map, DynamicStateEditRejectionReason::InsufficientEvidence);
    if (! isValidDynamicFingerprintPrototype (existing.fingerprint))
        return reject (map, DynamicStateEditRejectionReason::InvalidFingerprint);

    const auto startingPackage = safeGlobalEquivalentPackage (map);
    if (! isValidDynamicZonePackage (startingPackage))
        return reject (map, DynamicStateEditRejectionReason::InvalidGlobalStartingPackage);

    DynamicStateMap mutated = map;
    auto& state = mutated.states[(size_t) slot];
    state.origin = DynamicStateOrigin::Manual;
    state.hasLearnedPackage = false;
    state.learnedPackage = {};
    state.hasManualBasePackage = true;
    state.manualBasePackage = startingPackage;
    state.manualTrim = makeZeroDynamicManualTrim();
    return finish (map, mutated);
}

// Manual States only. Removes the persistent State entirely (frees its slot);
// other State identities are untouched. nextStateId is never decremented, so
// the freed stableStateId is never recycled. The caller (UI) is responsible
// for requiring a deliberate confirmation interaction before calling this.
inline DynamicStateEditResult removeManualState (const DynamicStateMap& map, uint64_t stableStateId) noexcept
{
    using namespace DynamicStateEditDetail;
    if (! isStructurallyValidDynamicStateMap (map))
        return reject (map, DynamicStateEditRejectionReason::MapNotStructurallyValid);

    const int slot = findOccupiedSlotByStableId (map, stableStateId);
    if (slot < 0)
        return reject (map, DynamicStateEditRejectionReason::StateNotFound);
    if (map.states[(size_t) slot].origin != DynamicStateOrigin::Manual)
        return reject (map, DynamicStateEditRejectionReason::RequiresManualOrigin);

    DynamicStateMap mutated = map;
    mutated.states[(size_t) slot] = DynamicState {};
    return finish (map, mutated);
}

// Section 15: creates a brand-new Manual State from a repeatable Unknown
// cluster. Allocates the next monotonic stableStateId (never recycled).
// Initializes from the same safe Global-equivalent package as promotion.
struct DynamicManualStateCreationRequest
{
    DynamicFingerprintPrototype fingerprint;
    DynamicStateEvidence evidence = DynamicStateEvidence::Candidate;
    uint32_t hitCount = 0;
    float repeatability = 0.0f;
    float ambiguity = 0.0f;
    bool hasLikelyMidi = false;
    int likelyMidi = -1;
    bool hasLikelyPitchHz = false;
    float likelyPitchHz = 0.0f;
};

inline DynamicStateEditResult createManualState (const DynamicStateMap& map,
                                                 const DynamicManualStateCreationRequest& request) noexcept
{
    using namespace DynamicStateEditDetail;
    if (! isStructurallyValidDynamicStateMap (map))
        return reject (map, DynamicStateEditRejectionReason::MapNotStructurallyValid);

    const int slot = findFreeSlot (map);
    if (slot < 0)
        return reject (map, DynamicStateEditRejectionReason::NoFreeCapacity);
    if (request.hitCount < DynamicStateMapContract::kManualMinimumRepeatableHits)
        return reject (map, DynamicStateEditRejectionReason::InsufficientEvidence);
    if (! isValidDynamicFingerprintPrototype (request.fingerprint))
        return reject (map, DynamicStateEditRejectionReason::InvalidFingerprint);

    const auto startingPackage = safeGlobalEquivalentPackage (map);
    if (! isValidDynamicZonePackage (startingPackage))
        return reject (map, DynamicStateEditRejectionReason::InvalidGlobalStartingPackage);

    if (map.nextStateId == 0)
        return reject (map, DynamicStateEditRejectionReason::MapValidationFailed);

    DynamicStateMap mutated = map;
    DynamicState state;
    state.occupied = true;
    state.stableStateId = mutated.nextStateId;
    state.fingerprint = request.fingerprint;
    state.hasLearnedPackage = false;
    state.hasManualBasePackage = true;
    state.manualBasePackage = startingPackage;
    state.manualTrim = makeZeroDynamicManualTrim();
    state.origin = DynamicStateOrigin::Manual;
    state.evidence = request.evidence;
    state.enabled = true;
    state.bypassed = false;
    state.hitCount = request.hitCount;
    state.repeatability = request.repeatability;
    state.ambiguity = request.ambiguity;
    state.hasLikelyMidi = request.hasLikelyMidi;
    state.likelyMidi = request.likelyMidi;
    state.hasLikelyPitchHz = request.hasLikelyPitchHz;
    state.likelyPitchHz = request.likelyPitchHz;

    mutated.states[(size_t) slot] = state;
    mutated.nextStateId = mutated.nextStateId + 1;
    return finish (map, mutated);
}
