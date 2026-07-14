#pragma once

#include <JuceHeader.h>

#include "../dsp/DynamicRuntimeSelector.h"
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

// Denormalize the raw APVTS choice (0 = Static, 1 = Dynamic).
inline CorrectionMode correctionModeFromRaw (float raw) noexcept
{
    return raw > 0.5f ? CorrectionMode::Dynamic : CorrectionMode::Static;
}

// Strict workflow ownership: only Dynamic mode owns Learn UI.
// Unresolved Learn under Static must not steal Analyze chrome.
inline bool workflowUsesLearnUi (CorrectionMode mode) noexcept
{
    return mode == CorrectionMode::Dynamic;
}

inline bool workflowUsesLearnUi (bool dynamicMode) noexcept
{
    return dynamicMode;
}

// Processor-side transition side effects. Pure; editor applies them once per real edge.
struct ModeTransitionActions
{
    bool cancelActiveLearn = false;
    bool discardPendingLearn = false;
    bool resetAnalyzePresentation = false;
    bool clearLearnPresentation = false;
};

// from == to → empty (idempotent). first open uses applyInitialModeActions instead.
inline ModeTransitionActions modeTransitionActions (CorrectionMode from, CorrectionMode to) noexcept
{
    ModeTransitionActions actions;
    if (from == to)
        return actions;

    if (to == CorrectionMode::Static)
    {
        actions.cancelActiveLearn = true;
        actions.discardPendingLearn = true;
        actions.clearLearnPresentation = true;
    }
    else
    {
        actions.resetAnalyzePresentation = true;
    }

    return actions;
}

// Editor construction / first paint: only chrome ownership. Do NOT cancel or
// discard processor-owned Learn — editor close/reopen must leave Learn intact.
// Real Dynamic->Static edges use modeTransitionActions for cancel/discard.
inline ModeTransitionActions initialModeActions (CorrectionMode mode) noexcept
{
    ModeTransitionActions actions;
    if (mode == CorrectionMode::Static)
        actions.clearLearnPresentation = true;
    else
        actions.resetAnalyzePresentation = true;
    return actions;
}

// Impossible UI combos (message-thread presentation checks).
inline bool isImpossibleWorkflowCombo (CorrectionMode mode,
                                       bool primaryIsLearnFamily,
                                       bool applyIsLearn,
                                       bool discardVisible,
                                       bool learnProgressVisible,
                                       bool noteChipsVisible) noexcept
{
    if (mode == CorrectionMode::Static)
    {
        if (primaryIsLearnFamily)
            return true;
        if (applyIsLearn)
            return true;
        if (discardVisible)
            return true;
        if (learnProgressVisible)
            return true;
        if (noteChipsVisible)
            return true;
    }
    else
    {
        // Dynamic selected + Analyze primary / Apply Fix as the Dynamic action.
        if (! primaryIsLearnFamily)
            return true;
        // Apply Fix text is only OK while no Apply Learn is offered; the flag
        // applyIsLearn means the button is currently labeled Apply Learn.
        // Showing Apply Fix label while Dynamic is allowed when no pending apply
        // (button may be hidden). Impossible is: Apply Fix *shown as Dynamic action*
        // — callers pass applyFixVisibleWithStaticLabel.
    }

    return false;
}

inline bool isImpossibleDynamicApplyFixAsAction (CorrectionMode mode,
                                                 bool applyVisible,
                                                 bool applyLabeledApplyFix,
                                                 bool learnApplyAvailable) noexcept
{
    // Dynamic must not present Apply Fix as the Dynamic action when Learn apply is available.
    // When Learn apply is not available, Apply is hidden (not shown as Apply Fix).
    if (mode != CorrectionMode::Dynamic)
        return false;
    if (applyVisible && applyLabeledApplyFix)
        return true;
    if (applyVisible && ! learnApplyAvailable && applyLabeledApplyFix)
        return true;
    return applyVisible && applyLabeledApplyFix;
}

inline PrimaryWorkflowPresentation primaryWorkflowPresentation (bool dynamicMode,
                                                                LearnState learnState,
                                                                bool canStartAnalyze,
                                                                bool analyzeBusy) noexcept
{
    // Strict ownership: Static always owns Analyze, even if Learn state is non-Idle
    // (e.g. host automation raced a cancel). Dynamic owns all Learn chrome.
    if (! dynamicMode)
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

inline bool primaryWorkflowIsLearnFamily (const PrimaryWorkflowPresentation& presentation) noexcept
{
    switch (presentation.action)
    {
        case PrimaryWorkflowAction::StartLearn:
        case PrimaryWorkflowAction::StopLearn:
        case PrimaryWorkflowAction::LearnAgain:
            return true;
        case PrimaryWorkflowAction::StartAnalyze:
        case PrimaryWorkflowAction::None:
            return false;
    }
    return false;
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

// --- Layout / bounds helpers (deterministic, scale-aware) --------------------

inline float uiScaleFactor (int percent) noexcept
{
    return (float) percent / 100.0f;
}

inline int scaledPx (int px, float scale) noexcept
{
    return juce::jmax (1, (int) std::lround ((float) px * scale));
}

inline int measureUiTextWidth (const juce::String& text, float fontSizePt) noexcept
{
    // Brace-init avoids most-vexing-parse treating this as a function declaration.
    const juce::Font font { juce::FontOptions (fontSizePt) };
    return juce::GlyphArrangement::getStringWidthInt (font, text);
}

// Primary / Apply / Discard labels that must fit the top bar without relying
// on platform accidental sizing.
inline juce::StringArray workflowButtonLabelsToFit()
{
    return {
        "Analyze",
        "Analyzing...",
        "Apply Fix",
        "Learn",
        "Stop Learn (999 hits)",
        "Learn Again",
        "Apply Learn",
        "Discard",
        "Preparing...",
        "Finishing...",
        "Cancelling..."
    };
}

// Top-bar button widths used by resized(). Keep in one place for tests.
inline constexpr int kPrimaryWorkflowButtonWidth = 168;
inline constexpr int kApplyWorkflowButtonWidth = 92;
inline constexpr int kDiscardWorkflowButtonWidth = 72;

// Learn progress panel: two metric lines then chips. Prefer bounds over tiny fonts.
inline constexpr int kLearnProgressMetricsLineHeight = 14;
inline constexpr int kLearnProgressMetricsBlockHeight = 30; // two lines + gap
inline constexpr int kLearnProgressChipRowTop = 34;
inline constexpr int kLearnProgressPreferredHeight = 84;
inline constexpr int kLearnProgressMinWidthForThreeDigitCounters = 280;
inline constexpr float kLearnProgressMetricsFontPt = 10.0f;

inline bool learnProgressMetricsFitWidth (int panelWidth, const LearnProgressSnapshot& progress) noexcept
{
    const auto line1 = formatLearnProgressSummaryLine1 (progress);
    const auto line2 = formatLearnProgressSummaryLine2 (progress);
    const int w1 = measureUiTextWidth (line1, kLearnProgressMetricsFontPt);
    const int w2 = measureUiTextWidth (line2, kLearnProgressMetricsFontPt);
    // 8 px padding each side (matches paint reduced (8, ...)).
    return w1 <= panelWidth - 16 && w2 <= panelWidth - 16;
}

inline int learnProgressChipOriginY() noexcept
{
    return kLearnProgressChipRowTop;
}

inline bool learnProgressChipsBeginBelowMetrics() noexcept
{
    return learnProgressChipOriginY() >= kLearnProgressMetricsBlockHeight;
}

// Failure body: reason first, then counters. Count lines for bounds checks.
inline int countTextLines (const juce::String& text) noexcept
{
    if (text.isEmpty())
        return 0;
    int lines = 1;
    for (int i = 0; i < text.length(); ++i)
        if (text[i] == '\n')
            ++lines;
    return lines;
}

inline int estimateFailureBodyHeight (const juce::String& body, float fontPt) noexcept
{
    // Conservative: one line-height per newline-separated row (no wrap estimate).
    const float lineH = fontPt + 3.0f;
    return (int) std::ceil (lineH * (float) juce::jmax (1, countTextLines (body)));
}

inline bool failureBodyFitsBounds (const juce::String& body,
                                   int boundsWidth,
                                   int boundsHeight,
                                   float fontPt) noexcept
{
    if (boundsWidth <= 0 || boundsHeight <= 0)
        return false;
    // Important reason is the first paragraph (before blank line + counters).
    const int reasonEnd = body.indexOf ("\n\n");
    const juce::String reason = reasonEnd >= 0 ? body.substring (0, reasonEnd) : body;
    if (reason.isEmpty())
        return false;
    // Multi-line counters always present for formatLearnFailureBody.
    if (! body.contains ("Captured:") || ! body.contains ("Pitch accepted:"))
        return false;
    return estimateFailureBodyHeight (body, fontPt) <= boundsHeight;
}
