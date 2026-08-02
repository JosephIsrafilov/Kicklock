#pragma once

#include <algorithm>
#include <cmath>

#include "DynamicAllpassPoleDomain.h"
#include "DynamicStateMap.h"

// =============================================================================
// Phase 12, Section 8.2/8.3: canonical, bidirectional conversion between the
// producer-facing effective Frequency (Hz) / Q and the internally-stored
// manualTrim (frequencyTrimSemitones / logPoleDampingTrim). Both directions
// reuse the exact same pole-domain math DynamicPackageMorpher.h's
// resolveDynamicPackage() already uses for the forward (trim -> effective)
// direction at strength == 1.0 - there is only one formula, used both ways.
//
// "Effective" here is independent of Dynamic Strength: Strength morphs
// between Global and this effective target elsewhere (DynamicPackageMorpher.h);
// the Inspector always shows/edits this fully-resolved target, matching
// Section 7D's "effective ... value" wording.
// =============================================================================

namespace DynamicManualTrimConversion
{
    inline bool effectiveFrequencyQ (const DynamicZonePackage& sourcePackage,
                                     const DynamicManualTrim& trim,
                                     double sampleRate,
                                     double& outFrequencyHz,
                                     double& outQ) noexcept
    {
        DynamicAllpassPoleCoordinates sourceCoordinates;
        if (! DynamicAllpassPoleDomain::frequencyQToPoleCoordinates (
                (double) sourcePackage.allpassFreqHz, (double) sourcePackage.allpassQ,
                sampleRate, sourceCoordinates))
            return false;

        const double targetLog2Frequency = sourceCoordinates.log2FrequencyHz
            + (double) trim.frequencyTrimSemitones / 12.0;
        const double targetFrequency = std::exp2 (targetLog2Frequency);
        if (! std::isfinite (targetFrequency))
            return false;
        const double clampedFrequency = std::clamp (targetFrequency,
            (double) DynamicStateMapContract::kAllpassFrequencyMinHz,
            (double) DynamicStateMapContract::kAllpassFrequencyMaxHz);
        const double clampedLog2Frequency = std::log2 (clampedFrequency);

        double minimumDamping = 0.0;
        double maximumDamping = 0.0;
        if (! DynamicAllpassPoleDomain::getLogPoleDampingEnvelope (
                clampedFrequency, sampleRate, minimumDamping, maximumDamping))
            return false;
        const double targetDamping = std::clamp (
            sourceCoordinates.logPoleDamping + (double) trim.logPoleDampingTrim,
            minimumDamping, maximumDamping);

        DynamicAllpassFrequencyQ result;
        if (! DynamicAllpassPoleDomain::poleCoordinatesToFrequencyQ (
                clampedLog2Frequency, targetDamping, sampleRate, result))
            return false;

        outFrequencyHz = result.frequencyHz;
        outQ = result.q;
        return true;
    }

    // Inverse of effectiveFrequencyQ(): given a desired effective Hz/Q (what a
    // UI slider is being dragged to), computes the manualTrim fields that
    // would produce it from this source package. Result is clamped to the
    // frozen manual-trim bounds; callers must still run the result through
    // isValidDynamicManualTrim() (and the whole edit through
    // DynamicStateEditTransaction::setManualTrim()) before publishing - this
    // function only performs the unit conversion, not policy validation.
    inline bool manualTrimForEffectiveFrequencyQ (const DynamicZonePackage& sourcePackage,
                                                  double desiredFrequencyHz,
                                                  double desiredQ,
                                                  double sampleRate,
                                                  float& outFrequencyTrimSemitones,
                                                  float& outLogPoleDampingTrim) noexcept
    {
        DynamicAllpassPoleCoordinates sourceCoordinates;
        DynamicAllpassPoleCoordinates targetCoordinates;
        if (! DynamicAllpassPoleDomain::frequencyQToPoleCoordinates (
                (double) sourcePackage.allpassFreqHz, (double) sourcePackage.allpassQ,
                sampleRate, sourceCoordinates)
            || ! DynamicAllpassPoleDomain::frequencyQToPoleCoordinates (
                desiredFrequencyHz, desiredQ, sampleRate, targetCoordinates))
            return false;

        const double semitones = (targetCoordinates.log2FrequencyHz - sourceCoordinates.log2FrequencyHz) * 12.0;
        const double dampingTrim = targetCoordinates.logPoleDamping - sourceCoordinates.logPoleDamping;
        if (! std::isfinite (semitones) || ! std::isfinite (dampingTrim))
            return false;

        outFrequencyTrimSemitones = (float) std::clamp (semitones,
            (double) DynamicStateMapContract::kManualFrequencyTrimMinSemitones,
            (double) DynamicStateMapContract::kManualFrequencyTrimMaxSemitones);
        outLogPoleDampingTrim = (float) std::clamp (dampingTrim,
            (double) DynamicStateMapContract::kManualLogPoleDampingTrimMin,
            (double) DynamicStateMapContract::kManualLogPoleDampingTrimMax);
        return true;
    }
}
