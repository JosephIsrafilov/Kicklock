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
