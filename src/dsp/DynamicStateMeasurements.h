#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "DynamicStateMap.h"

// =============================================================================
// Phase 9: fixed measurement value contracts.
//
// Predicted (Learn-side, offline retained windows) and Verified (runtime,
// fresh audible output) share the same score scale and the same categorical
// semantics, but are never fabricated from one another. Every field here is a
// finite, bounded value type: no strings, no vectors, no pointers, safe to
// copy on the audio thread and safe to publish in the fixed UI snapshot.
// =============================================================================

namespace DynamicMeasurementContract
{
    // Canonical score scale: MultiBandCorrelation::weightedMatchPercent, the
    // same low-end-weighted ruler already used by Learn/Analyze/Apply
    // verification. [0, 100].
    inline constexpr float kScoreMin = 0.0f;
    inline constexpr float kScoreMax = 100.0f;

    // Minimum retained/verified evidence gates (Section 10/16).
    inline constexpr int kMinPredictedWindows = 3;
    inline constexpr int kMinVerifiedEvents = 3;

    // Retained Learn window bounds (Section 8).
    inline constexpr int kMaxRetainedStates = DynamicStateMapContract::kMaxPersistentStates;
    inline constexpr int kMaxRetainedWindowsPerState = 12;

    // Fixed rolling verified sample store per State (Section 16).
    inline constexpr int kVerifiedRingCapacity = 16;

    // Final-package cluster verification acceptance constants (Section 10).
    // Reuses the existing FixedTimingRotatorSearch minimum meaningful gain.
    inline constexpr float kMinimumMeaningfulGainPoints = 3.0f; // == FixedTimingRotatorSearch::kMinGainPoints
    inline constexpr float kMaxAcceptableWorstRegressionPoints = 10.0f;
    inline constexpr float kMinImprovedWindowRatio = 0.5f; // strictly-greater majority enforced by caller (> not >=)
    inline constexpr float kMinConfidence = 0.15f;

    inline bool isFiniteScore (float value) noexcept
    {
        return std::isfinite (value) && value >= kScoreMin && value <= kScoreMax;
    }

    inline float sanitizeScore (float value) noexcept
    {
        if (! std::isfinite (value))
            return kScoreMin;
        return std::clamp (value, kScoreMin, kScoreMax);
    }

    inline float sanitizeUnit (float value) noexcept
    {
        if (! std::isfinite (value))
            return 0.0f;
        return std::clamp (value, 0.0f, 1.0f);
    }
}

enum class DynamicMeasurementAvailability : uint8_t
{
    Unavailable = 0,
    Collecting,
    Available,
    InsufficientMaterial,
    Invalid
};

inline constexpr bool isValidDynamicMeasurementAvailability (DynamicMeasurementAvailability value) noexcept
{
    return value == DynamicMeasurementAvailability::Unavailable
        || value == DynamicMeasurementAvailability::Collecting
        || value == DynamicMeasurementAvailability::Available
        || value == DynamicMeasurementAvailability::InsufficientMaterial
        || value == DynamicMeasurementAvailability::Invalid;
}

enum class DynamicCorrectionAssessment : uint8_t
{
    Unknown = 0,
    CandidateOnly,
    PredictedImprovement,
    VerifiedImprovement,
    Neutral,
    Unstable,
    Regressed,
    RecognizedNoCorrection,
    Disabled,
    Bypassed
};

inline constexpr bool isValidDynamicCorrectionAssessment (DynamicCorrectionAssessment value) noexcept
{
    return value >= DynamicCorrectionAssessment::Unknown && value <= DynamicCorrectionAssessment::Bypassed;
}

enum class DynamicCorrectionRejectionReason : uint8_t
{
    None = 0,
    CandidateEvidence,
    InsufficientWindows,
    InvalidFingerprint,
    InvalidPackage,
    NonBeneficial,
    InconsistentAcrossHits,
    ExcessiveWorstRegression,
    OutOfDelayBudget,
    PolarityMismatch,
    Disabled,
    Bypassed,
    RuntimeUnavailable
};

inline constexpr bool isValidDynamicCorrectionRejectionReason (DynamicCorrectionRejectionReason value) noexcept
{
    return value >= DynamicCorrectionRejectionReason::None && value <= DynamicCorrectionRejectionReason::RuntimeUnavailable;
}

// One robust aggregate measurement summary. Shared shape for Predicted
// (Learn-side, offline) and Verified (runtime, audible-output) results; the
// two are populated by entirely separate code paths but read identically.
struct DynamicMeasurementSummary
{
    DynamicMeasurementAvailability availability = DynamicMeasurementAvailability::Unavailable;
    DynamicCorrectionAssessment assessment = DynamicCorrectionAssessment::Unknown;
    DynamicCorrectionRejectionReason rejectionReason = DynamicCorrectionRejectionReason::None;

    uint32_t validWindowCount = 0;
    uint32_t rejectedWindowCount = 0;

    float beforeScore = 0.0f;
    float afterScore = 0.0f;
    float improvementPoints = 0.0f;
    float medianImprovementPoints = 0.0f;
    float lowerQuantileImprovementPoints = 0.0f;
    float worstImprovementPoints = 0.0f;
    float improvedWindowRatio = 0.0f;
    float confidence = 0.0f;

    uint64_t mapGeneration = 0;
    uint64_t lastUpdateSample = 0;
};

inline constexpr DynamicMeasurementSummary makeUnavailableDynamicMeasurementSummary() noexcept
{
    return {};
}

inline bool isValidDynamicMeasurementSummary (const DynamicMeasurementSummary& summary) noexcept
{
    using namespace DynamicMeasurementContract;
    if (! isValidDynamicMeasurementAvailability (summary.availability)
        || ! isValidDynamicCorrectionAssessment (summary.assessment)
        || ! isValidDynamicCorrectionRejectionReason (summary.rejectionReason))
        return false;

    if (! std::isfinite (summary.beforeScore) || summary.beforeScore < kScoreMin || summary.beforeScore > kScoreMax)
        return false;
    if (! std::isfinite (summary.afterScore) || summary.afterScore < kScoreMin || summary.afterScore > kScoreMax)
        return false;

    const float maxImprovement = kScoreMax - kScoreMin;
    for (const float value : { summary.improvementPoints, summary.medianImprovementPoints,
                               summary.lowerQuantileImprovementPoints, summary.worstImprovementPoints })
        if (! std::isfinite (value) || value < -maxImprovement || value > maxImprovement)
            return false;

    if (! std::isfinite (summary.improvedWindowRatio) || summary.improvedWindowRatio < 0.0f || summary.improvedWindowRatio > 1.0f)
        return false;
    if (! std::isfinite (summary.confidence) || summary.confidence < 0.0f || summary.confidence > 1.0f)
        return false;

    if (summary.availability == DynamicMeasurementAvailability::Available
        && summary.validWindowCount == 0)
        return false;

    return true;
}
