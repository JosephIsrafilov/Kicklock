#pragma once

#include <JuceHeader.h>

#include "../dsp/LearnState.h"

enum class PrimaryWorkflowAction
{
    StartAnalyze,
    StartLearn,
    StopLearn,
    LearnAgain,
    None
};

struct PrimaryWorkflowPresentation
{
    const char* text = "Analyze";
    bool enabled = false;
    PrimaryWorkflowAction action = PrimaryWorkflowAction::None;
};

inline PrimaryWorkflowPresentation primaryWorkflowPresentation (bool dynamicMode,
                                                                LearnState learnState,
                                                                bool canStartAnalyze,
                                                                bool analyzeBusy) noexcept
{
    // An active or unresolved Learn always owns the primary workflow, even if
    // automation changes the selected correction mode underneath it.
    if (! dynamicMode && learnState == LearnState::Idle)
        return { analyzeBusy ? "Analyzing..." : "Analyze", ! analyzeBusy && canStartAnalyze,
                 PrimaryWorkflowAction::StartAnalyze };

    switch (learnState)
    {
        case LearnState::Idle:              return { "Learn", ! analyzeBusy, PrimaryWorkflowAction::StartLearn };
        case LearnState::Preparing:         return { "Preparing...", false, PrimaryWorkflowAction::None };
        case LearnState::Capturing:         return { "Stop Learn (" , true, PrimaryWorkflowAction::StopLearn };
        case LearnState::Stopping:
        case LearnState::Draining:
        case LearnState::Finalizing:        return { "Finishing...", false, PrimaryWorkflowAction::None };
        case LearnState::Cancelling:        return { "Cancelling...", false, PrimaryWorkflowAction::None };
        case LearnState::ResultReady:
        case LearnState::NotEnoughMaterial:
        case LearnState::Failed:            return { "Learn Again", true, PrimaryWorkflowAction::LearnAgain };
    }

    return {};
}

inline juce::String primaryWorkflowText (const PrimaryWorkflowPresentation& presentation,
                                         int capturedHits)
{
    if (presentation.action == PrimaryWorkflowAction::StopLearn)
        return "Stop Learn (" + juce::String (juce::jmax (0, capturedHits)) + " hits)";

    return presentation.text;
}

enum class DynamicRuntimeStatus
{
    Hidden,
    MapStale,
    NoMap,
    PhaseFilterOff,
    Fallback,
    LearnedNote
};

inline DynamicRuntimeStatus dynamicRuntimeStatus (bool dynamicMode,
                                                   bool mapStale,
                                                   bool hasMap,
                                                   bool phaseFilterEnabled,
                                                   bool fallbackActive) noexcept
{
    if (! dynamicMode)
        return DynamicRuntimeStatus::Hidden;
    if (mapStale)
        return DynamicRuntimeStatus::MapStale;
    if (! hasMap)
        return DynamicRuntimeStatus::NoMap;
    if (! phaseFilterEnabled)
        return DynamicRuntimeStatus::PhaseFilterOff;
    return fallbackActive ? DynamicRuntimeStatus::Fallback : DynamicRuntimeStatus::LearnedNote;
}

inline juce::String dynamicRuntimeStatusText (DynamicRuntimeStatus status, int activeMidiNote)
{
    switch (status)
    {
        case DynamicRuntimeStatus::MapStale:      return "MAP STALE - RE-LEARN";
        case DynamicRuntimeStatus::NoMap:         return "NO MAP";
        case DynamicRuntimeStatus::PhaseFilterOff:return "PHASE FILTER OFF";
        case DynamicRuntimeStatus::Fallback:      return "FALLBACK";
        case DynamicRuntimeStatus::LearnedNote:
            return activeMidiNote >= 0 ? "LEARNED " + juce::MidiMessage::getMidiNoteName (activeMidiNote, true, true, 4)
                                       : "LEARNED MAP";
        case DynamicRuntimeStatus::Hidden:        return {};
    }

    return {};
}

inline juce::String formatDynamicStrength (float value)
{
    return juce::String ((int) std::lround (juce::jlimit (0.0f, 1.0f, value) * 100.0f)) + "%";
}

inline const char* dynamicStrengthTooltip() noexcept
{
    return "0% uses the global phase filter setting for every note. 100% uses each note's learned Freq/Q; Delay and Polarity stay global.";
}

inline juce::String formatLearnNoteChip (int midi, int acceptedHits)
{
    return juce::MidiMessage::getMidiNoteName (midi, true, true, 4)
         + " x" + juce::String (juce::jmax (0, acceptedHits));
}

inline bool learnNoteHasEnoughMaterial (int acceptedHits) noexcept
{
    return acceptedHits >= NoteMap::kMinHitsPerNote;
}

// Honest Learn progress copy. CAPTURED = completed capture windows,
// PROCESSED = drained queue windows (not "successfully analyzed"),
// PITCH OK / REJECTED / TIMING OK come from post-finalize diagnostics.
inline juce::String formatLearnProgressSummaryLine1 (const LearnProgressSnapshot& progress)
{
    return "CAPTURED " + juce::String (juce::jmax (0, progress.capturedHits))
         + "   PROCESSED " + juce::String (juce::jmax (0, progress.drainedHits))
         + "   QUEUE " + juce::String (juce::jmax (0, progress.pendingQueueHits));
}

inline juce::String formatLearnProgressSummaryLine2 (const LearnProgressSnapshot& progress)
{
    return "PITCH OK " + juce::String (juce::jmax (0, progress.pitchAcceptedHits))
         + "   REJECTED " + juce::String (juce::jmax (0, progress.rejectedPitchHits))
         + "   TIMING OK " + juce::String (juce::jmax (0, progress.timingUsableHits));
}

inline int countPitchAcceptedHits (const LearnFinalizeResult& result) noexcept
{
    int count = 0;
    for (const auto& hit : result.hitAnalyses)
        if (hit.pitchAccepted)
            ++count;
    return count;
}

// Body text for NotEnoughMaterial / Failed. present==false must not hide
// result.message or diagnostics — only Apply Learn is gated by present.
inline juce::String formatLearnFailureBody (const LearnFinalizeResult& result)
{
    juce::String body = result.message.isNotEmpty()
                            ? result.message
                            : juce::String ("Learn needs more usable kick and bass hits. Play the loop, then try again.");

    const int pitchOk = countPitchAcceptedHits (result);
    const int pitchRejected = juce::jmax (0, result.diagnostics.rejectedPitchHits);
    const int timingOk = juce::jmax (0, result.diagnostics.analyzedHits);
    const int captured = juce::jmax (0, result.diagnostics.capturedHits);

    body << "\n\nCaptured: " << captured
         << "\nPitch accepted: " << pitchOk
         << "\nPitch rejected: " << pitchRejected
         << "\nTiming usable: " << timingOk;
    return body;
}

// Apply Learn is enabled only for ResultReady with a present/applicable candidate.
inline bool learnApplyEnabled (LearnState learnState, bool hasPendingApplicableResult) noexcept
{
    return learnState == LearnState::ResultReady && hasPendingApplicableResult;
}
