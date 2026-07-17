#pragma once

#include <cmath>
#include <cstdint>

#include "DynamicAllpassPoleDomain.h"

enum class DynamicPackageDecision : uint8_t
{
    StateCorrection = 0,
    GlobalNoSelection,
    GlobalRecognizedNoCorrection,
    GlobalBypassed,
    InvalidInput
};

struct DynamicPackageResolution
{
    DynamicPackageDecision decision = DynamicPackageDecision::InvalidInput;
    bool valid = false;
    uint64_t selectedStableStateId = 0;
    bool correctionAvailable = false;
    bool correctionApplied = false;
    bool selectedBypassed = false;
    bool sourceWasManual = false;
    double requestedStrength = 0.0;
    double effectiveStrength = 0.0;
    double sourceDelayDeltaMs = 0.0;
    double trimmedStateDelayDeltaMs = 0.0;
    double morphedDelayDeltaMs = 0.0;
    double effectiveAbsoluteDelayMs = 0.0;
    double effectiveAllpassFreqHz = 0.0;
    double effectiveAllpassQ = 0.0;
    DynamicAllpassCoefficients allpassCoefficients;
    bool polarityInvert = false;
    int allpassStages = 0;
    bool crossoverEnabled = false;
    double crossoverHz = 0.0;
    bool allpassEnabled = false;
    int delayInterpolationIndex = 0;
    bool frequencyTrimClamped = false;
    bool dampingTrimClamped = false;
};

inline const DynamicState* findDynamicStateByStableId (const DynamicStateMap& map,
                                                        uint64_t stableStateId) noexcept
{
    if (stableStateId == 0)
        return nullptr;

    for (const auto& state : map.states)
        if (state.occupied && state.stableStateId == stableStateId)
            return &state;
    return nullptr;
}

namespace DynamicPackageMorpherDetail
{
    inline double clamp (double value, double minimum, double maximum) noexcept
    {
        return value < minimum ? minimum : (value > maximum ? maximum : value);
    }

    inline bool setGlobalPackage (DynamicPackageResolution& result,
                                  const DynamicGlobalBase& globalBase,
                                  double sampleRate) noexcept
    {
        result.polarityInvert = globalBase.polarityInvert;
        result.allpassStages = globalBase.allpassStages;
        result.crossoverEnabled = globalBase.crossoverEnabled;
        result.crossoverHz = (double) globalBase.crossoverHz;
        result.allpassEnabled = globalBase.allpassEnabled;
        result.delayInterpolationIndex = globalBase.delayInterpolationIndex;
        result.effectiveAbsoluteDelayMs = (double) globalBase.globalBaseDelayMs;
        result.effectiveAllpassFreqHz = (double) globalBase.globalAllpassFreqHz;
        result.effectiveAllpassQ = (double) globalBase.globalAllpassQ;
        return DynamicAllpassPoleDomain::makeCoefficients (result.effectiveAllpassFreqHz,
                                                            result.effectiveAllpassQ,
                                                            sampleRate,
                                                            result.allpassCoefficients);
    }
}

// Resolves one runtime-only correction package. The map and all stored packages
// remain read-only; the result is a fixed-size value and is never serialized.
inline DynamicPackageResolution resolveDynamicPackage (const DynamicStateMap& map,
                                                       uint64_t stableStateId,
                                                       double requestedStrength,
                                                       double sampleRate) noexcept
{
    using namespace DynamicPackageMorpherDetail;

    DynamicPackageResolution result;
    result.requestedStrength = requestedStrength;

    if (! std::isfinite (requestedStrength) || ! DynamicAllpassPoleDomain::isValidSampleRate (sampleRate)
        || ! isStructurallyValidDynamicStateMap (map))
        return result;

    result.effectiveStrength = clamp (requestedStrength, 0.0, 1.0);
    if (! setGlobalPackage (result, map.globalBase, sampleRate)
        || ! std::isfinite (result.effectiveAbsoluteDelayMs))
        return result;

    const DynamicState* state = findDynamicStateByStableId (map, stableStateId);
    if (state == nullptr || ! state->enabled
        || (state->origin == DynamicStateOrigin::Auto && state->evidence != DynamicStateEvidence::Stable))
    {
        result.decision = DynamicPackageDecision::GlobalNoSelection;
        result.valid = true;
        return result;
    }

    result.selectedStableStateId = state->stableStateId;
    result.sourceWasManual = state->origin == DynamicStateOrigin::Manual;

    const DynamicZonePackage* sourcePackage = nullptr;
    if (state->origin == DynamicStateOrigin::Auto)
    {
        if (! state->hasLearnedPackage)
        {
            result.decision = DynamicPackageDecision::GlobalRecognizedNoCorrection;
            result.valid = true;
            return result;
        }
        sourcePackage = &state->learnedPackage;
    }
    else if (state->origin == DynamicStateOrigin::Manual)
    {
        if (! state->hasManualBasePackage)
            return result;
        sourcePackage = &state->manualBasePackage;
    }
    else
    {
        return result;
    }

    result.correctionAvailable = true;
    if (state->bypassed)
    {
        result.decision = DynamicPackageDecision::GlobalBypassed;
        result.selectedBypassed = true;
        result.valid = true;
        return result;
    }

    float storedDelayDeltaMs = 0.0f;
    if (! getEffectiveStoredDynamicStateDelayDeltaMs (*state, storedDelayDeltaMs))
        return result;

    result.sourceDelayDeltaMs = (double) sourcePackage->delayDeltaMs;
    result.trimmedStateDelayDeltaMs = (double) storedDelayDeltaMs;
    if (! hasDynamicLookAheadBudget (DynamicStateMapContract::kReportedLatencyMs,
                                     map.globalBase.globalBaseDelayMs,
                                     storedDelayDeltaMs,
                                     DynamicStateMapContract::kFingerprintWindowMs,
                                     DynamicStateMapContract::kMaximumTransitionMs,
                                     DynamicStateMapContract::kSafetyMarginMs))
        return result;

    DynamicAllpassPoleCoordinates globalCoordinates;
    DynamicAllpassPoleCoordinates sourceCoordinates;
    if (! DynamicAllpassPoleDomain::frequencyQToPoleCoordinates (
            (double) map.globalBase.globalAllpassFreqHz,
            (double) map.globalBase.globalAllpassQ,
            sampleRate,
            globalCoordinates)
        || ! DynamicAllpassPoleDomain::frequencyQToPoleCoordinates (
            (double) sourcePackage->allpassFreqHz,
            (double) sourcePackage->allpassQ,
            sampleRate,
            sourceCoordinates))
        return result;

    const double requestedTargetFrequency = std::exp2 (
        sourceCoordinates.log2FrequencyHz + (double) state->manualTrim.frequencyTrimSemitones / 12.0);
    if (! std::isfinite (requestedTargetFrequency))
        return result;
    const double trimmedFrequency = clamp (requestedTargetFrequency,
                                           (double) DynamicStateMapContract::kAllpassFrequencyMinHz,
                                           (double) DynamicStateMapContract::kAllpassFrequencyMaxHz);
    result.frequencyTrimClamped = trimmedFrequency != requestedTargetFrequency;
    const double trimmedLog2Frequency = std::log2 (trimmedFrequency);
    if (! std::isfinite (trimmedLog2Frequency))
        return result;

    double minimumLogPoleDamping = 0.0;
    double maximumLogPoleDamping = 0.0;
    const double requestedTrimmedDamping = sourceCoordinates.logPoleDamping
        + (double) state->manualTrim.logPoleDampingTrim;
    if (! std::isfinite (requestedTrimmedDamping)
        || ! DynamicAllpassPoleDomain::getLogPoleDampingEnvelope (trimmedFrequency,
                                                                    sampleRate,
                                                                    minimumLogPoleDamping,
                                                                    maximumLogPoleDamping))
        return result;
    const double trimmedDamping = clamp (requestedTrimmedDamping,
                                         minimumLogPoleDamping,
                                         maximumLogPoleDamping);
    result.dampingTrimClamped = trimmedDamping != requestedTrimmedDamping;

    const double strength = result.effectiveStrength;
    result.morphedDelayDeltaMs = strength * result.trimmedStateDelayDeltaMs;
    result.effectiveAbsoluteDelayMs = (double) map.globalBase.globalBaseDelayMs + result.morphedDelayDeltaMs;
    if (! std::isfinite (result.effectiveAbsoluteDelayMs))
        return DynamicPackageResolution {};

    if (strength == 0.0)
    {
        result.effectiveAllpassFreqHz = (double) map.globalBase.globalAllpassFreqHz;
        result.effectiveAllpassQ = (double) map.globalBase.globalAllpassQ;
    }
    else if (strength == 1.0 && isZeroDynamicManualTrim (state->manualTrim))
    {
        // Preserve exact stored source endpoints without an unnecessary inverse round trip.
        result.effectiveAllpassFreqHz = (double) sourcePackage->allpassFreqHz;
        result.effectiveAllpassQ = (double) sourcePackage->allpassQ;
    }
    else if (strength == 1.0)
    {
        // Exact clamped Manual target after production bounds.
        DynamicAllpassFrequencyQ trimmedFrequencyQ;
        if (! DynamicAllpassPoleDomain::poleCoordinatesToFrequencyQ (trimmedLog2Frequency,
                                                                      trimmedDamping,
                                                                      sampleRate,
                                                                      trimmedFrequencyQ))
            return DynamicPackageResolution {};
        result.effectiveAllpassFreqHz = trimmedFrequency;
        result.effectiveAllpassQ = trimmedFrequencyQ.q;
    }
    else
    {
        const double finalLog2Frequency = globalCoordinates.log2FrequencyHz
            + (trimmedLog2Frequency - globalCoordinates.log2FrequencyHz) * strength;
        const double finalFrequency = std::exp2 (finalLog2Frequency);
        double finalMinimumLogPoleDamping = 0.0;
        double finalMaximumLogPoleDamping = 0.0;
        if (! DynamicAllpassPoleDomain::isValidFrequency (finalFrequency)
            || ! DynamicAllpassPoleDomain::getLogPoleDampingEnvelope (finalFrequency,
                                                                        sampleRate,
                                                                        finalMinimumLogPoleDamping,
                                                                        finalMaximumLogPoleDamping))
            return DynamicPackageResolution {};

        const double finalLogPoleDamping = clamp (globalCoordinates.logPoleDamping
            + (trimmedDamping - globalCoordinates.logPoleDamping) * strength,
            finalMinimumLogPoleDamping,
            finalMaximumLogPoleDamping);
        DynamicAllpassFrequencyQ finalFrequencyQ;
        if (! DynamicAllpassPoleDomain::poleCoordinatesToFrequencyQ (finalLog2Frequency,
                                                                      finalLogPoleDamping,
                                                                      sampleRate,
                                                                      finalFrequencyQ))
            return DynamicPackageResolution {};
        result.effectiveAllpassFreqHz = finalFrequencyQ.frequencyHz;
        result.effectiveAllpassQ = finalFrequencyQ.q;
    }

    if (! DynamicAllpassPoleDomain::makeCoefficients (result.effectiveAllpassFreqHz,
                                                       result.effectiveAllpassQ,
                                                       sampleRate,
                                                       result.allpassCoefficients))
        return DynamicPackageResolution {};

    result.decision = DynamicPackageDecision::StateCorrection;
    result.valid = true;
    result.correctionApplied = strength > 0.0;
    return result;
}
