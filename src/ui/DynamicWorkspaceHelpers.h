#pragma once

#include <array>
#include <algorithm>
#include <cmath>
#include <string>

#include "DynamicWorkspaceModel.h"
#include "../dsp/DynamicStateEditTransaction.h"
#include "../dsp/DynamicManualTrimConversion.h"

enum class DynamicWorkspaceColourRole
{
    Muted,
    Teal,
    Green,
    Amber,
    Red
};

inline juce::String dynamicMapSourceLabel (DynamicMapSource source)
{
    switch (source)
    {
        case DynamicMapSource::NewDynamicStateMap:      return "NEW MAP";
        case DynamicMapSource::LegacyDynamicCompatibility:return "LEGACY MAP";
        case DynamicMapSource::None:                    return "NO MAP";
    }
    return "NO MAP";
}

inline juce::String dynamicBranchLabel (DynamicSelectorBranchKind branch)
{
    switch (branch)
    {
        case DynamicSelectorBranchKind::Global:  return "GLOBAL";
        case DynamicSelectorBranchKind::State:   return "STATE";
        case DynamicSelectorBranchKind::Service: return "SERVICE";
    }
    return "GLOBAL";
}

inline juce::String dynamicStateOriginLabel (DynamicStateOrigin origin)
{
    return origin == DynamicStateOrigin::Manual ? "MANUAL" : "AUTO";
}

inline juce::String dynamicStateEvidenceLabel (DynamicStateEvidence evidence)
{
    return evidence == DynamicStateEvidence::Stable ? "STABLE" : "CANDIDATE";
}

inline juce::String dynamicMeasurementAvailabilityLabel (DynamicMeasurementAvailability availability)
{
    switch (availability)
    {
        case DynamicMeasurementAvailability::Unavailable:          return "Unavailable";
        case DynamicMeasurementAvailability::Collecting:           return "Collecting";
        case DynamicMeasurementAvailability::Available:            return "Available";
        case DynamicMeasurementAvailability::InsufficientMaterial: return "Insufficient Material";
        case DynamicMeasurementAvailability::Invalid:              return "Invalid";
    }
    return "Unavailable";
}

inline juce::String formatDynamicMeasurementAvailability (const DynamicMeasurementSummary& summary,
                                                          bool verified)
{
    if (summary.availability != DynamicMeasurementAvailability::Collecting)
        return dynamicMeasurementAvailabilityLabel (summary.availability);

    const int minimum = verified ? DynamicMeasurementContract::kMinVerifiedEvents
                                 : DynamicMeasurementContract::kMinPredictedWindows;
    return "Collecting " + juce::String (summary.validWindowCount) + "/" + juce::String (minimum);
}

inline juce::String dynamicCorrectionAssessmentLabel (DynamicCorrectionAssessment assessment)
{
    switch (assessment)
    {
        case DynamicCorrectionAssessment::CandidateOnly:        return "Candidate Only";
        case DynamicCorrectionAssessment::PredictedImprovement: return "Predicted Improvement";
        case DynamicCorrectionAssessment::VerifiedImprovement:  return "Verified Improvement";
        case DynamicCorrectionAssessment::Neutral:              return "Neutral";
        case DynamicCorrectionAssessment::Unstable:             return "Unstable";
        case DynamicCorrectionAssessment::Regressed:            return "Regressed";
        case DynamicCorrectionAssessment::RecognizedNoCorrection:return "Recognized - Global Only";
        case DynamicCorrectionAssessment::Disabled:             return "Disabled";
        case DynamicCorrectionAssessment::Bypassed:             return "Bypassed";
        case DynamicCorrectionAssessment::Unknown:              return "Unknown";
    }
    return "Unknown";
}

inline juce::String dynamicCorrectionRejectionLabel (DynamicCorrectionRejectionReason reason)
{
    switch (reason)
    {
        case DynamicCorrectionRejectionReason::None:                    return {};
        case DynamicCorrectionRejectionReason::CandidateEvidence:       return "Candidate evidence";
        case DynamicCorrectionRejectionReason::InsufficientWindows:     return "Insufficient windows";
        case DynamicCorrectionRejectionReason::InvalidFingerprint:      return "Invalid fingerprint";
        case DynamicCorrectionRejectionReason::InvalidPackage:          return "Invalid package";
        case DynamicCorrectionRejectionReason::NonBeneficial:           return "Not beneficial";
        case DynamicCorrectionRejectionReason::InconsistentAcrossHits:  return "Inconsistent across hits";
        case DynamicCorrectionRejectionReason::ExcessiveWorstRegression:return "Excessive worst regression";
        case DynamicCorrectionRejectionReason::OutOfDelayBudget:        return "Outside delay budget";
        case DynamicCorrectionRejectionReason::PolarityMismatch:        return "Polarity mismatch";
        case DynamicCorrectionRejectionReason::Disabled:                return "Disabled";
        case DynamicCorrectionRejectionReason::Bypassed:                return "Bypassed";
        case DynamicCorrectionRejectionReason::RuntimeUnavailable:      return "Runtime unavailable";
    }
    return {};
}

inline juce::String shortDynamicStateId (uint64_t stableStateId)
{
    constexpr char digits[] = "0123456789ABCDEF";
    char text[17] {};
    int position = 16;
    do
    {
        text[--position] = digits[stableStateId & 0x0f];
        stableStateId >>= 4;
    }
    while (stableStateId != 0);

    auto shortText = juce::String (text + position);
    if (shortText.length() > 6)
        shortText = shortText.substring (shortText.length() - 6);
    return shortText;
}

inline juce::String fullDynamicStateId (uint64_t stableStateId)
{
    return juce::String (std::to_string (stableStateId));
}

inline juce::String dynamicStateCardTitle (const DynamicStateCard& card)
{
    if (card.hasLikelyMidi)
    {
        auto title = juce::MidiMessage::getMidiNoteName (card.likelyMidi, true, true, 4);
        if (card.hasLikelyPitchHz)
            title << "  " << juce::String ((int) std::lround (card.likelyPitchHz)) << " Hz";
        return title;
    }
    if (card.hasLikelyPitchHz)
        return "State " + shortDynamicStateId (card.stableStateId) + "  "
             + juce::String ((int) std::lround (card.likelyPitchHz)) + " Hz";
    return "State " + shortDynamicStateId (card.stableStateId);
}

inline bool dynamicMeasurementShowsScores (DynamicMeasurementAvailability availability) noexcept
{
    return availability == DynamicMeasurementAvailability::Available
        || availability == DynamicMeasurementAvailability::InsufficientMaterial;
}

inline juce::String formatDynamicImprovementPoints (float points)
{
    const float cleaned = std::abs (points) < 0.05f ? 0.0f : points;
    return juce::String (cleaned >= 0.0f ? "+" : "") + juce::String (cleaned, 1) + " pts";
}

inline juce::String formatDynamicBeforeAfter (const DynamicMeasurementSummary& summary)
{
    if (! dynamicMeasurementShowsScores (summary.availability))
        return "--";
    return juce::String (juce::jlimit (0.0f, 100.0f, summary.beforeScore), 1) + " -> "
         + juce::String (juce::jlimit (0.0f, 100.0f, summary.afterScore), 1);
}

inline DynamicWorkspaceColourRole dynamicWorkspaceColourRole (const DynamicStateCard& card) noexcept
{
    if (! card.enabled || card.bypassed)
        return DynamicWorkspaceColourRole::Muted;
    switch (card.assessment)
    {
        case DynamicCorrectionAssessment::VerifiedImprovement:
        case DynamicCorrectionAssessment::PredictedImprovement:
            return DynamicWorkspaceColourRole::Green;
        case DynamicCorrectionAssessment::Regressed:
            return DynamicWorkspaceColourRole::Red;
        case DynamicCorrectionAssessment::CandidateOnly:
        case DynamicCorrectionAssessment::Unstable:
            return DynamicWorkspaceColourRole::Amber;
        default:
            return card.hasCorrection ? DynamicWorkspaceColourRole::Teal : DynamicWorkspaceColourRole::Muted;
    }
}

inline DynamicWorkspaceColourRole dynamicMeasurementColourRole (
    const DynamicMeasurementSummary& summary) noexcept
{
    switch (summary.availability)
    {
        case DynamicMeasurementAvailability::Unavailable:
            return DynamicWorkspaceColourRole::Muted;
        case DynamicMeasurementAvailability::Collecting:
            return DynamicWorkspaceColourRole::Amber;
        case DynamicMeasurementAvailability::InsufficientMaterial:
            return DynamicWorkspaceColourRole::Amber;
        case DynamicMeasurementAvailability::Invalid:
            return DynamicWorkspaceColourRole::Red;
        case DynamicMeasurementAvailability::Available:
            break;
    }

    switch (summary.assessment)
    {
        case DynamicCorrectionAssessment::PredictedImprovement:
        case DynamicCorrectionAssessment::VerifiedImprovement:
            return DynamicWorkspaceColourRole::Green;
        case DynamicCorrectionAssessment::Neutral:
        case DynamicCorrectionAssessment::RecognizedNoCorrection:
            return DynamicWorkspaceColourRole::Teal;
        case DynamicCorrectionAssessment::CandidateOnly:
        case DynamicCorrectionAssessment::Unstable:
            return DynamicWorkspaceColourRole::Amber;
        case DynamicCorrectionAssessment::Regressed:
            return DynamicWorkspaceColourRole::Red;
        case DynamicCorrectionAssessment::Disabled:
        case DynamicCorrectionAssessment::Bypassed:
        case DynamicCorrectionAssessment::Unknown:
            return DynamicWorkspaceColourRole::Muted;
    }
    return DynamicWorkspaceColourRole::Muted;
}

inline DynamicStateCard makePreviewDynamicStateCard (
    const DynamicState& state,
    int slot,
    const DynamicMeasurementSummary& predicted)
{
    DynamicStateCard card;
    card.occupied = state.occupied;
    if (! card.occupied)
        return card;
    card.stableStateId = state.stableStateId;
    card.slot = slot;
    card.origin = state.origin;
    card.evidence = state.evidence;
    card.enabled = state.enabled;
    card.bypassed = state.bypassed;
    card.hitCount = state.hitCount;
    card.repeatability = state.repeatability;
    card.ambiguity = state.ambiguity;
    card.hasCorrection = (state.origin == DynamicStateOrigin::Auto
                          && state.evidence == DynamicStateEvidence::Stable
                          && state.hasLearnedPackage)
        || (state.origin == DynamicStateOrigin::Manual && state.hasManualBasePackage);
    card.hasLikelyMidi = state.hasLikelyMidi;
    card.likelyMidi = state.hasLikelyMidi ? state.likelyMidi : -1;
    card.hasLikelyPitchHz = state.hasLikelyPitchHz;
    card.likelyPitchHz = state.likelyPitchHz;
    card.predicted = predicted;
    card.verified = makeUnavailableDynamicMeasurementSummary();
    card.assessment = ! state.enabled ? DynamicCorrectionAssessment::Disabled
        : state.bypassed ? DynamicCorrectionAssessment::Bypassed
        : predicted.assessment;
    card.rejectionReason = predicted.rejectionReason;
    return card;
}

inline std::array<DynamicStateCard, DynamicMeasurementContract::kMaxRetainedStates>
    dynamicWorkspaceCards (const DynamicWorkspaceViewModel& model)
{
    if (! model.previewActive)
    {
        if (model.runtime.source == DynamicMapSource::NewDynamicStateMap)
            return model.runtime.states;
        return {};
    }

    std::array<DynamicStateCard, DynamicMeasurementContract::kMaxRetainedStates> cards {};
    for (int slot = 0; slot < DynamicMeasurementContract::kMaxRetainedStates; ++slot)
        cards[(size_t) slot] = makePreviewDynamicStateCard (
            model.previewMap.states[(size_t) slot], slot, model.previewPredicted[(size_t) slot]);
    return cards;
}

inline bool dynamicCardIsSelected (const DynamicStateCard& card,
                                   const DynamicRuntimeSnapshot& runtime) noexcept
{
    return card.occupied && runtime.selectedSemanticStateId != 0
        && card.stableStateId == runtime.selectedSemanticStateId;
}

inline bool dynamicCardIsActive (const DynamicStateCard& card,
                                 const DynamicRuntimeSnapshot& runtime) noexcept
{
    return card.occupied && runtime.activeSemanticStateId != 0
        && card.stableStateId == runtime.activeSemanticStateId;
}

inline std::array<int, DynamicMeasurementContract::kMaxRetainedStates>
    orderedDynamicWorkspaceSlots (
        const std::array<DynamicStateCard, DynamicMeasurementContract::kMaxRetainedStates>& cards)
{
    std::array<int, DynamicMeasurementContract::kMaxRetainedStates> slots {};
    for (int i = 0; i < (int) slots.size(); ++i)
        slots[(size_t) i] = i;

    auto rank = [&cards] (int slot)
    {
        const auto& card = cards[(size_t) slot];
        if (! card.occupied) return 4;
        if (card.active) return 0;
        if (card.selected) return 1;
        if (card.evidence == DynamicStateEvidence::Stable) return 2;
        return 3;
    };
    std::stable_sort (slots.begin(), slots.end(), [&] (int left, int right)
    {
        const int leftRank = rank (left);
        const int rightRank = rank (right);
        return leftRank != rightRank ? leftRank < rightRank : left < right;
    });
    return slots;
}

inline juce::String dynamicWorkspaceRuntimeStatus (const DynamicRuntimeSnapshot& runtime)
{
    if (runtime.source == DynamicMapSource::None)
        return "NO MAP";
    if (runtime.source == DynamicMapSource::LegacyDynamicCompatibility)
        return "LEGACY COMPATIBILITY";
    if (runtime.bypassActive)
        return "BYPASSED";
    if (! runtime.sidechainPresent)
        return "NO SIDECHAIN";
    if (runtime.holdActive)
        return "HOLD";
    if (runtime.activeBranchKind == DynamicSelectorBranchKind::Service)
        return "ACTIVE SERVICE";
    if (runtime.activeBranchKind == DynamicSelectorBranchKind::State && runtime.activeSemanticStateId != 0)
        return "ACTIVE STATE";
    return runtime.fallbackActive ? "GLOBAL FALLBACK" : "GLOBAL";
}

struct DynamicWorkspaceHeaderPresentation
{
    juce::String source;
    juce::String status;
    juce::String detail;
};

inline DynamicWorkspaceHeaderPresentation dynamicWorkspaceHeaderPresentation (
    const DynamicWorkspaceViewModel& model)
{
    DynamicWorkspaceHeaderPresentation presentation;
    if (model.previewActive)
    {
        presentation.source = "PREVIEW - NOT APPLIED";
        presentation.status = "PREDICTED ONLY";
        presentation.detail = model.previewApplyBlocked ? model.previewBlockedReason
            : model.previewApplyAvailable ? "Apply Learn is available. Verified remains unavailable until fresh runtime output."
                                         : "Pending Learn result is not applicable.";
        return presentation;
    }

    presentation.source = dynamicMapSourceLabel (model.runtime.source);
    presentation.status = dynamicWorkspaceRuntimeStatus (model.runtime);
    if (learnStateIsBusy (model.learnState))
    {
        presentation.detail = model.learnStatusMessage;
        return presentation;
    }

    if (model.learnStatusMessage.isNotEmpty())
    {
        presentation.source = "LEARN RESULT";
        presentation.status = model.learnState == LearnState::NotEnoughMaterial ? "NOT ENOUGH MATERIAL"
            : model.learnState == LearnState::Failed ? "LEARN FAILED"
                                                     : "NO PREVIEW";
        presentation.detail = model.learnStatusMessage;
        return presentation;
    }

    if (model.runtime.source == DynamicMapSource::LegacyDynamicCompatibility)
        presentation.detail = "Legacy map compatibility is active. New State cards and Phase 9 measurements are unavailable.";
    else if (model.runtime.source == DynamicMapSource::None)
        presentation.detail = "Learn repeatable conflict States. Unknown or ambiguous conflicts use the safe Global path.";
    else
        presentation.detail = "Selected and active States are distinct runtime identities.";
    return presentation;
}

struct DynamicWorkspaceWorkflowVisibility
{
    bool applyLearn = false;
    bool discard = false;
    bool workspace = false;
    bool workspaceMapActions = false;
    bool dynamicStrength = false;
};

inline DynamicWorkspaceWorkflowVisibility dynamicWorkspaceWorkflowVisibility (
    bool resultReady, LearnState learnState, bool cleanScopeMode) noexcept
{
    const bool controlsVisible = ! cleanScopeMode;
    const bool resolved = learnState == LearnState::ResultReady
        || learnState == LearnState::NotEnoughMaterial
        || learnState == LearnState::Failed;
    return { resultReady && controlsVisible, resolved && controlsVisible,
             controlsVisible, controlsVisible, controlsVisible };
}

// =============================================================================
// Phase 12: State Inspector / Status Header / actions support (Sections 5-10).
// =============================================================================

inline juce::String dynamicStateEditRejectionLabel (DynamicStateEditRejectionReason reason)
{
    switch (reason)
    {
        case DynamicStateEditRejectionReason::None:                        return {};
        case DynamicStateEditRejectionReason::MapNotStructurallyValid:     return "The current map is not valid.";
        case DynamicStateEditRejectionReason::StateNotFound:               return "That State no longer exists.";
        case DynamicStateEditRejectionReason::InsufficientEvidence:        return "Need at least 3 repeatable hits.";
        case DynamicStateEditRejectionReason::AlreadyManual:               return "State is already Manual.";
        case DynamicStateEditRejectionReason::RequiresAutoOrigin:          return "Only available for Auto States.";
        case DynamicStateEditRejectionReason::RequiresManualOrigin:        return "Only available for Manual States.";
        case DynamicStateEditRejectionReason::InvalidFingerprint:          return "Invalid fingerprint.";
        case DynamicStateEditRejectionReason::InvalidTrim:                 return "That value is outside the allowed range.";
        case DynamicStateEditRejectionReason::InvalidGlobalStartingPackage:return "Invalid Global starting package.";
        case DynamicStateEditRejectionReason::MapValidationFailed:         return "That change would produce an invalid map.";
        case DynamicStateEditRejectionReason::NoFreeCapacity:              return "No free capacity (maximum 8 States).";
        case DynamicStateEditRejectionReason::PreviewNotApplied:           return "Apply the preview first.";
        case DynamicStateEditRejectionReason::LegacyMapReadOnly:           return "Legacy map is read-only.";
        case DynamicStateEditRejectionReason::NoMapApplied:                return "No Dynamic map is applied yet.";
        case DynamicStateEditRejectionReason::PublicationBusy:             return "Busy publishing the last change - try again.";
    }
    return {};
}

struct DynamicStateControlEligibility
{
    bool controlsEnabled = false;
    juce::String disabledReason;
    bool canPromote = false;
    juce::String promoteDisabledReason;
    bool canRemoveManual = false;
};

// Pure control-eligibility policy (Section 8.4): Preview/Legacy/No-map/no-
// selection disable everything with an explicit reason; otherwise controls
// are enabled and promotion/removal are gated on origin and evidence.
inline DynamicStateControlEligibility dynamicStateControlEligibility (
    const DynamicWorkspaceViewModel& model) noexcept
{
    DynamicStateControlEligibility result;
    if (model.previewActive)
    {
        result.disabledReason = "Preview is not applied yet. Apply the result to edit States.";
        result.promoteDisabledReason = result.disabledReason;
        return result;
    }
    if (model.runtime.source == DynamicMapSource::LegacyDynamicCompatibility)
    {
        result.disabledReason = "Legacy map is read-only.";
        result.promoteDisabledReason = result.disabledReason;
        return result;
    }
    if (model.runtime.source == DynamicMapSource::None)
    {
        result.disabledReason = "No Dynamic map is applied yet.";
        result.promoteDisabledReason = result.disabledReason;
        return result;
    }
    if (! model.hasSelectedState)
    {
        result.disabledReason = "Select a State to inspect or edit it.";
        result.promoteDisabledReason = result.disabledReason;
        return result;
    }

    result.controlsEnabled = true;
    const auto& state = model.selectedState;
    if (state.origin == DynamicStateOrigin::Manual)
    {
        result.promoteDisabledReason = "State is already Manual.";
        result.canRemoveManual = true;
    }
    else if (state.hitCount < DynamicStateMapContract::kManualMinimumRepeatableHits)
    {
        result.promoteDisabledReason = dynamicStateEditRejectionLabel (
            DynamicStateEditRejectionReason::InsufficientEvidence);
    }
    else
    {
        result.canPromote = true;
    }
    return result;
}

// Section 7C: correction status for the selected State (distinct from the
// per-card assessment badge - this is the full producer-facing sentence).
inline juce::String dynamicStateCorrectionStatusLine (const DynamicState& state) noexcept
{
    if (! state.occupied)
        return "No State selected.";
    if (! state.enabled)
        return "Disabled - not eligible for matching.";
    if (state.bypassed)
        return "Bypassed - using Global.";
    if (state.origin == DynamicStateOrigin::Manual)
        return state.hasManualBasePackage ? "Manual correction." : "Manual State - invalid package.";
    if (state.hasLearnedPackage)
        return "Automatic correction accepted.";
    return "Recognized - no automatic correction. Using Global. Manual editing is available.";
}

// Section 7D: effective package values for display, using the canonical
// conversion (never a second, duplicated formula).
struct DynamicEffectivePackageDisplay
{
    bool valid = false;
    float delayDeltaMs = 0.0f;
    double frequencyHz = 0.0;
    double q = 0.0;
};

inline DynamicEffectivePackageDisplay dynamicEffectivePackageDisplay (
    const DynamicState& state, double sampleRate) noexcept
{
    DynamicEffectivePackageDisplay result;
    const DynamicZonePackage* source = nullptr;
    if (state.origin == DynamicStateOrigin::Auto && state.hasLearnedPackage)
        source = &state.learnedPackage;
    else if (state.origin == DynamicStateOrigin::Manual && state.hasManualBasePackage)
        source = &state.manualBasePackage;
    if (source == nullptr)
        return result;

    float effectiveDelay = 0.0f;
    if (! getEffectiveStoredDynamicStateDelayDeltaMs (state, effectiveDelay))
        return result;

    double freq = 0.0, q = 0.0;
    if (! DynamicManualTrimConversion::effectiveFrequencyQ (*source, state.manualTrim, sampleRate, freq, q))
        return result;

    result.valid = true;
    result.delayDeltaMs = effectiveDelay;
    result.frequencyHz = freq;
    result.q = q;
    return result;
}

// Section 5: header summary counts.
struct DynamicWorkspaceSummary
{
    int recognizedStateCount = 0;
    int correctionCount = 0;
    int manualCount = 0;
    int globalOnlyCount = 0;
};

inline DynamicWorkspaceSummary dynamicWorkspaceSummary (const DynamicWorkspaceViewModel& model) noexcept
{
    DynamicWorkspaceSummary summary;
    if (model.previewActive)
    {
        for (int slot = 0; slot < DynamicMeasurementContract::kMaxRetainedStates; ++slot)
        {
            const auto& state = model.previewMap.states[(size_t) slot];
            if (! state.occupied)
                continue;
            ++summary.recognizedStateCount;
            const bool hasCorrection = (state.origin == DynamicStateOrigin::Auto
                                        && state.evidence == DynamicStateEvidence::Stable
                                        && state.hasLearnedPackage)
                || (state.origin == DynamicStateOrigin::Manual && state.hasManualBasePackage);
            if (hasCorrection) ++summary.correctionCount; else ++summary.globalOnlyCount;
            if (state.origin == DynamicStateOrigin::Manual) ++summary.manualCount;
        }
        return summary;
    }

    if (model.runtime.source != DynamicMapSource::NewDynamicStateMap)
        return summary;
    for (const auto& card : model.runtime.states)
    {
        if (! card.occupied)
            continue;
        ++summary.recognizedStateCount;
        if (card.hasCorrection) ++summary.correctionCount; else ++summary.globalOnlyCount;
        if (card.origin == DynamicStateOrigin::Manual) ++summary.manualCount;
    }
    return summary;
}

// Section 13: Focus status text. Never called "Solo" - audio is never soloed.
inline juce::String dynamicFocusStatusText (const DynamicWorkspaceViewModel& model) noexcept
{
    if (! model.focusEnabled)
        return {};
    if (model.previewActive || model.runtime.source != DynamicMapSource::NewDynamicStateMap)
        return "Focus unavailable in Preview/Legacy/No Map.";
    if (model.focusedStableStateId == 0)
        return "Select a State to focus.";

    const DynamicStateCard* card = nullptr;
    for (const auto& c : model.runtime.states)
        if (c.occupied && c.stableStateId == model.focusedStableStateId) { card = &c; break; }
    if (card == nullptr)
        return "Selected State not detected recently.";
    if (! card->enabled)
        return "Selected State disabled.";
    if (card->bypassed)
        return "Selected State bypassed.";

    const bool isActive = card->active && card->stableStateId == model.runtime.activeSemanticStateId;
    if (! isActive)
        return "Waiting for selected State...";

    const auto& verified = card->verified;
    if (verified.availability == DynamicMeasurementAvailability::Collecting)
        return "Collecting verification " + juce::String (verified.validWindowCount) + "/"
             + juce::String (DynamicMeasurementContract::kMinVerifiedEvents);
    if (verified.availability == DynamicMeasurementAvailability::Available)
        return "Verified.";
    return "Selected State detected.";
}
