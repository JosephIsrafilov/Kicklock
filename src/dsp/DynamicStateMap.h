#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <limits>

namespace DynamicStateMapContract
{
    inline constexpr uint32_t kSchemaVersion = 1;
    inline constexpr uint32_t kExtractorVersion = 1;
    inline constexpr int kMaxPersistentStates = 8;
    inline constexpr int kMaxFingerprintFeatures = 8;

    inline constexpr float kReportedLatencyMs = 20.0f;
    inline constexpr float kFingerprintWindowMs = 4.0f;
    inline constexpr float kMaximumTransitionMs = 8.0f;
    inline constexpr float kSafetyMarginMs = 1.0f;

    inline constexpr float kGlobalBaseDelayMinMs = -4.0f;
    inline constexpr float kGlobalBaseDelayMaxMs = 17.0f;
    inline constexpr float kStateDelayDeltaMinMs = -3.0f;
    inline constexpr float kStateDelayDeltaMaxMs = 3.0f;
    inline constexpr float kAllpassFrequencyMinHz = 30.0f;
    inline constexpr float kAllpassFrequencyMaxHz = 220.0f;
    inline constexpr float kAllpassQMin = 0.35f;
    inline constexpr float kAllpassQMax = 2.0f;
    inline constexpr float kCrossoverMinHz = 40.0f;
    inline constexpr float kCrossoverMaxHz = 500.0f;

    inline constexpr float kManualDelayTrimMinMs = -3.0f;
    inline constexpr float kManualDelayTrimMaxMs = 3.0f;
    inline constexpr float kManualFrequencyTrimMinSemitones = -24.0f;
    inline constexpr float kManualFrequencyTrimMaxSemitones = 24.0f;
    inline constexpr float kManualLogPoleDampingTrimMin = -2.0f;
    inline constexpr float kManualLogPoleDampingTrimMax = 2.0f;

    inline constexpr uint32_t kCandidateMinimumRepeatableHits = 3;
    inline constexpr uint32_t kStableAutoMinimumRepeatableHits = 5;
    inline constexpr uint32_t kManualMinimumRepeatableHits = 3;
    inline constexpr uint32_t kMaximumHoldEvents = 2;
    inline constexpr float kMaximumHoldTimeMs = 250.0f;

    inline bool isFinite (float value) noexcept
    {
        return std::isfinite (value);
    }

    inline bool isInRange (float value, float minimum, float maximum) noexcept
    {
        return isFinite (value) && value >= minimum && value <= maximum;
    }
}

struct DynamicZonePackage
{
    float delayDeltaMs = 0.0f;
    float allpassFreqHz = 0.0f;
    float allpassQ = 0.0f;
};

inline bool isValidDynamicZonePackage (const DynamicZonePackage& package) noexcept
{
    using namespace DynamicStateMapContract;
    return isInRange (package.delayDeltaMs, kStateDelayDeltaMinMs, kStateDelayDeltaMaxMs)
        && isInRange (package.allpassFreqHz, kAllpassFrequencyMinHz, kAllpassFrequencyMaxHz)
        && isInRange (package.allpassQ, kAllpassQMin, kAllpassQMax);
}

struct DynamicManualTrim
{
    float delayTrimMs = 0.0f;
    float frequencyTrimSemitones = 0.0f;
    float logPoleDampingTrim = 0.0f;
};

inline constexpr DynamicManualTrim makeZeroDynamicManualTrim() noexcept
{
    return {};
}

inline constexpr bool isZeroDynamicManualTrim (const DynamicManualTrim& trim) noexcept
{
    return trim.delayTrimMs == 0.0f
        && trim.frequencyTrimSemitones == 0.0f
        && trim.logPoleDampingTrim == 0.0f;
}

inline bool isValidDynamicManualTrim (const DynamicManualTrim& trim) noexcept
{
    using namespace DynamicStateMapContract;
    return isInRange (trim.delayTrimMs, kManualDelayTrimMinMs, kManualDelayTrimMaxMs)
        && isInRange (trim.frequencyTrimSemitones,
                      kManualFrequencyTrimMinSemitones,
                      kManualFrequencyTrimMaxSemitones)
        && isInRange (trim.logPoleDampingTrim,
                      kManualLogPoleDampingTrimMin,
                      kManualLogPoleDampingTrimMax);
}

struct DynamicFingerprintPrototype
{
    bool valid = false;
    uint8_t featureCount = 0;
    std::array<float, DynamicStateMapContract::kMaxFingerprintFeatures> features {};
};

inline bool isValidDynamicFingerprintPrototype (const DynamicFingerprintPrototype& fingerprint) noexcept
{
    using namespace DynamicStateMapContract;
    if (! fingerprint.valid || fingerprint.featureCount == 0
        || fingerprint.featureCount > kMaxFingerprintFeatures)
        return false;

    for (int i = 0; i < kMaxFingerprintFeatures; ++i)
    {
        const float value = fingerprint.features[(size_t) i];
        if (! isFinite (value))
            return false;
        if (i < fingerprint.featureCount)
        {
            if (value < -1.0f || value > 1.0f)
                return false;
        }
        else if (value != 0.0f)
        {
            return false;
        }
    }
    return true;
}

struct DynamicMatchCalibration
{
    bool valid = false;
    float absoluteDistanceThreshold = 0.0f;
    float ambiguityMargin = 0.0f;
};

inline bool isValidDynamicMatchCalibration (const DynamicMatchCalibration& calibration) noexcept
{
    // The v1 fingerprint distance is normalized to [0, 1], so both calibration
    // bounds are constrained to that same range in addition to the ordering
    // rule. A threshold or margin above 1 can never be reached by a real match
    // and is rejected at validation (and therefore at XML/binary activation).
    return calibration.valid
        && std::isfinite (calibration.absoluteDistanceThreshold)
        && std::isfinite (calibration.ambiguityMargin)
        && calibration.absoluteDistanceThreshold > 0.0f
        && calibration.absoluteDistanceThreshold <= 1.0f
        && calibration.ambiguityMargin > 0.0f
        && calibration.ambiguityMargin <= 1.0f
        && calibration.ambiguityMargin <= calibration.absoluteDistanceThreshold;
}

struct DynamicGlobalBase
{
    float globalBaseDelayMs = 0.0f;
    bool polarityInvert = false;
    bool crossoverEnabled = true;
    float crossoverHz = 150.0f;
    bool allpassEnabled = true;
    float globalAllpassFreqHz = 100.0f;
    float globalAllpassQ = 0.7f;
    int allpassStages = 2;
    int delayInterpolationIndex = 0;
    double learnedSampleRate = 0.0;
};

inline bool isValidDynamicGlobalBase (const DynamicGlobalBase& base) noexcept
{
    using namespace DynamicStateMapContract;
    return isInRange (base.globalBaseDelayMs, kGlobalBaseDelayMinMs, kGlobalBaseDelayMaxMs)
        && isInRange (base.crossoverHz, kCrossoverMinHz, kCrossoverMaxHz)
        && isInRange (base.globalAllpassFreqHz, kAllpassFrequencyMinHz, kAllpassFrequencyMaxHz)
        && isInRange (base.globalAllpassQ, kAllpassQMin, kAllpassQMax)
        && base.allpassStages >= 2 && base.allpassStages <= 4
        && (base.delayInterpolationIndex == 0 || base.delayInterpolationIndex == 1)
        && std::isfinite (base.learnedSampleRate) && base.learnedSampleRate >= 0.0;
}

enum class DynamicStateOrigin : uint8_t
{
    Auto = 0,
    Manual = 1
};

enum class DynamicStateEvidence : uint8_t
{
    Candidate = 0,
    Stable = 1
};

enum class DynamicMapDiagnostic : uint8_t
{
    None = 0,
    NonRepeatablePhaseRelationship = 1,
    InsufficientMaterial = 2,
    NoConfidentAutoFix = 3
};

struct DynamicMapDiagnostics
{
    DynamicMapDiagnostic diagnostic = DynamicMapDiagnostic::None;
    uint32_t analyzedHitCount = 0;
    uint32_t rejectedHitCount = 0;
    uint32_t unstableHitCount = 0;
    uint32_t repeatableClusterCount = 0;
};

inline constexpr bool isValidDynamicStateOrigin (DynamicStateOrigin origin) noexcept
{
    return origin == DynamicStateOrigin::Auto || origin == DynamicStateOrigin::Manual;
}

inline constexpr bool isValidDynamicStateEvidence (DynamicStateEvidence evidence) noexcept
{
    return evidence == DynamicStateEvidence::Candidate || evidence == DynamicStateEvidence::Stable;
}

inline constexpr bool isValidDynamicMapDiagnostic (DynamicMapDiagnostic diagnostic) noexcept
{
    return diagnostic == DynamicMapDiagnostic::None
        || diagnostic == DynamicMapDiagnostic::NonRepeatablePhaseRelationship
        || diagnostic == DynamicMapDiagnostic::InsufficientMaterial
        || diagnostic == DynamicMapDiagnostic::NoConfidentAutoFix;
}

// Per-identity persisted correction policy (product safety contract).
// GlobalFallback is the historical default: the identity is recognized but has
// no State-level package, so the shared Global package (delay + optional
// allpass) is applied to it, exactly as before this policy field existed.
// LearnedState means a real State package (learned or manual) is applied.
// NeutralSafe is assigned only when the measurement gate proves the Global
// package itself causes excessive degradation for a recognized, no-correction
// identity: that identity is routed to a shared, always-hot, zero-delay,
// no-allpass Neutral branch instead of Global, so the harmful package never
// reaches audible output for it.
enum class DynamicCorrectionPolicy : uint8_t
{
    LearnedState = 0,
    GlobalFallback = 1,
    NeutralSafe = 2
};

inline constexpr bool isValidDynamicCorrectionPolicy (DynamicCorrectionPolicy policy) noexcept
{
    return policy == DynamicCorrectionPolicy::LearnedState
        || policy == DynamicCorrectionPolicy::GlobalFallback
        || policy == DynamicCorrectionPolicy::NeutralSafe;
}

// Precise, persisted reason a policy other than the identity's natural choice
// was assigned. Currently only populated when NeutralSafe was forced by the
// measurement gate; None otherwise.
enum class DynamicPolicyRejectionReason : uint8_t
{
    None = 0,
    GlobalPackageExcessiveRegression = 1
};

inline constexpr bool isValidDynamicPolicyRejectionReason (DynamicPolicyRejectionReason reason) noexcept
{
    return reason == DynamicPolicyRejectionReason::None
        || reason == DynamicPolicyRejectionReason::GlobalPackageExcessiveRegression;
}

struct DynamicState
{
    bool occupied = false;
    uint64_t stableStateId = 0;
    DynamicFingerprintPrototype fingerprint;
    bool hasLearnedPackage = false;
    DynamicZonePackage learnedPackage;
    bool hasManualBasePackage = false;
    DynamicZonePackage manualBasePackage;
    DynamicManualTrim manualTrim;
    DynamicStateOrigin origin = DynamicStateOrigin::Auto;
    DynamicStateEvidence evidence = DynamicStateEvidence::Candidate;
    bool enabled = true;
    bool bypassed = false;
    uint32_t hitCount = 0;
    float repeatability = 0.0f;
    float ambiguity = 0.0f;
    bool hasLikelyMidi = false;
    int likelyMidi = 0;
    bool hasLikelyPitchHz = false;
    float likelyPitchHz = 0.0f;
    DynamicCorrectionPolicy correctionPolicy = DynamicCorrectionPolicy::GlobalFallback;
    DynamicPolicyRejectionReason policyRejectionReason = DynamicPolicyRejectionReason::None;
};

inline bool getEffectiveStoredDynamicStateDelayDeltaMs (const DynamicState& state,
                                                        float& effectiveDelayDeltaMs) noexcept
{
    using namespace DynamicStateMapContract;
    const DynamicZonePackage* sourcePackage = nullptr;

    if (state.origin == DynamicStateOrigin::Auto)
    {
        if (! state.hasLearnedPackage)
        {
            if (! isZeroDynamicManualTrim (state.manualTrim))
                return false;
            effectiveDelayDeltaMs = 0.0f;
            return true;
        }

        sourcePackage = &state.learnedPackage;
    }
    else if (state.origin == DynamicStateOrigin::Manual)
    {
        if (! state.hasManualBasePackage)
            return false;

        sourcePackage = &state.manualBasePackage;
    }
    else
    {
        return false;
    }

    if (! isInRange (sourcePackage->delayDeltaMs, kStateDelayDeltaMinMs, kStateDelayDeltaMaxMs)
        || ! isFinite (state.manualTrim.delayTrimMs))
        return false;

    const float effective = sourcePackage->delayDeltaMs + state.manualTrim.delayTrimMs;
    if (! isInRange (effective, kStateDelayDeltaMinMs, kStateDelayDeltaMaxMs))
        return false;

    effectiveDelayDeltaMs = effective;
    return true;
}

inline bool isValidDynamicState (const DynamicState& state) noexcept
{
    using namespace DynamicStateMapContract;
    if (! state.occupied)
        return true;
    if (state.stableStateId == 0 || ! isValidDynamicFingerprintPrototype (state.fingerprint)
        || ! isValidDynamicStateOrigin (state.origin)
        || ! isValidDynamicStateEvidence (state.evidence)
        || ! isValidDynamicManualTrim (state.manualTrim)
        || ! isInRange (state.repeatability, 0.0f, 1.0f)
        || ! isInRange (state.ambiguity, 0.0f, 1.0f)
        || (! state.enabled && state.bypassed))
        return false;

    if (! isValidDynamicCorrectionPolicy (state.correctionPolicy)
        || ! isValidDynamicPolicyRejectionReason (state.policyRejectionReason))
        return false;
    // NeutralSafe exists only to shield a no-correction identity from a
    // harmful Global package; a State that already carries its own package
    // should use it (LearnedState), never be routed around it as Neutral.
    if (state.correctionPolicy == DynamicCorrectionPolicy::NeutralSafe && state.hasLearnedPackage)
        return false;
    if (state.correctionPolicy != DynamicCorrectionPolicy::NeutralSafe
        && state.policyRejectionReason != DynamicPolicyRejectionReason::None)
        return false;
    if (state.correctionPolicy == DynamicCorrectionPolicy::LearnedState
        && ! state.hasLearnedPackage && ! state.hasManualBasePackage)
        return false;

    if (state.hasLearnedPackage && ! isValidDynamicZonePackage (state.learnedPackage))
        return false;
    if (state.hasManualBasePackage && ! isValidDynamicZonePackage (state.manualBasePackage))
        return false;
    if (state.hasLikelyMidi && (state.likelyMidi < 0 || state.likelyMidi > 127))
        return false;
    if (state.hasLikelyPitchHz && (! isFinite (state.likelyPitchHz) || state.likelyPitchHz <= 0.0f))
        return false;

    const uint32_t minimumHits = state.origin == DynamicStateOrigin::Manual
        ? kManualMinimumRepeatableHits : kCandidateMinimumRepeatableHits;
    if (state.hitCount < minimumHits)
        return false;
    if (state.origin == DynamicStateOrigin::Auto
        && state.evidence == DynamicStateEvidence::Stable
        && state.hitCount < kStableAutoMinimumRepeatableHits)
        return false;

    if (state.origin == DynamicStateOrigin::Auto)
    {
        if (state.hasManualBasePackage)
            return false;
        if (! state.hasLearnedPackage && ! isZeroDynamicManualTrim (state.manualTrim))
            return false;
    }
    else
    {
        if (! state.hasManualBasePackage || state.hasLearnedPackage)
            return false;
    }

    float effectiveDelayDeltaMs = 0.0f;
    return getEffectiveStoredDynamicStateDelayDeltaMs (state, effectiveDelayDeltaMs);
}

// This remains pure so map validation and future runtime scheduling share one
// boundary rule without depending on global constants or processor state.
inline bool hasDynamicLookAheadBudget (float reportedLatencyMs,
                                       float globalBaseDelayMs,
                                       float stateDelayDeltaMs,
                                       float fingerprintWindowMs,
                                       float transitionMs,
                                       float safetyMarginMs) noexcept
{
    return std::isfinite (reportedLatencyMs)
        && std::isfinite (globalBaseDelayMs)
        && std::isfinite (stateDelayDeltaMs)
        && std::isfinite (fingerprintWindowMs)
        && std::isfinite (transitionMs)
        && std::isfinite (safetyMarginMs)
        && reportedLatencyMs >= 0.0f
        && fingerprintWindowMs >= 0.0f
        && transitionMs >= 0.0f
        && safetyMarginMs >= 0.0f
        && reportedLatencyMs + globalBaseDelayMs + stateDelayDeltaMs
            >= fingerprintWindowMs + transitionMs + safetyMarginMs;
}

struct DynamicStateMap
{
    bool valid = false;
    uint32_t schemaVersion = DynamicStateMapContract::kSchemaVersion;
    uint32_t fingerprintExtractorVersion = DynamicStateMapContract::kExtractorVersion;
    DynamicGlobalBase globalBase;
    DynamicMatchCalibration calibration;
    std::array<DynamicState, DynamicStateMapContract::kMaxPersistentStates> states {};
    uint64_t nextStateId = 1;
    DynamicMapDiagnostics diagnostics;
};

inline constexpr DynamicStateMap makeEmptyDynamicStateMap() noexcept
{
    return {};
}

inline int getOccupiedDynamicStateCount (const DynamicStateMap& map) noexcept
{
    int count = 0;
    for (const auto& state : map.states)
        count += state.occupied ? 1 : 0;
    return count;
}

inline bool isStructurallyValidDynamicStateMap (const DynamicStateMap& map) noexcept
{
    using namespace DynamicStateMapContract;
    if (! map.valid || map.schemaVersion != kSchemaVersion
        || map.fingerprintExtractorVersion != kExtractorVersion
        || map.nextStateId == 0 || ! isValidDynamicGlobalBase (map.globalBase)
        || ! isValidDynamicMapDiagnostic (map.diagnostics.diagnostic))
        return false;

    uint64_t maximumStateId = 0;
    int occupiedCount = 0;
    for (int i = 0; i < kMaxPersistentStates; ++i)
    {
        const auto& state = map.states[(size_t) i];
        if (! state.occupied)
            continue;
        ++occupiedCount;
        if (! isValidDynamicState (state))
            return false;
        for (int previous = 0; previous < i; ++previous)
            if (map.states[(size_t) previous].occupied
                && map.states[(size_t) previous].stableStateId == state.stableStateId)
                return false;
        maximumStateId = std::max (maximumStateId, state.stableStateId);
    }

    if (occupiedCount > kMaxPersistentStates || map.nextStateId <= maximumStateId)
        return false;
    if (occupiedCount > 0 && ! isValidDynamicMatchCalibration (map.calibration))
        return false;
    return true;
}

inline bool isRuntimeEligibleDynamicStateMap (const DynamicStateMap& map) noexcept
{
    using namespace DynamicStateMapContract;
    if (! isStructurallyValidDynamicStateMap (map) || getOccupiedDynamicStateCount (map) == 0
        || ! isValidDynamicMatchCalibration (map.calibration))
        return false;

    for (const auto& state : map.states)
    {
        if (! state.occupied)
            continue;
        float delayDelta = 0.0f;
        if (! getEffectiveStoredDynamicStateDelayDeltaMs (state, delayDelta))
            return false;
        if (! hasDynamicLookAheadBudget (kReportedLatencyMs,
                                         map.globalBase.globalBaseDelayMs,
                                         delayDelta,
                                         kFingerprintWindowMs,
                                         kMaximumTransitionMs,
                                         kSafetyMarginMs))
            return false;
    }
    return true;
}
