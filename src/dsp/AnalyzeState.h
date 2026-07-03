#pragma once

// Explicit state machine for the (now background-threaded) Analyze workflow.
// The processor owns a std::atomic<AnalyzeState>; the editor polls it on its
// UI timer to drive the Analyze button text and Apply Fix availability.
//
//   Idle              -> nothing analyzed yet this session
//   Preparing         -> click accepted, snapshot/handoff in progress
//   Analyzing         -> background cross-correlation running
//   ResultReady       -> a usable result is published and readable
//   NotEnoughMaterial -> ran, but the capture had no usable kick+bass
//   Failed            -> analysis could not complete
//
// Preparing/Analyzing are the "busy" states: while busy the Analyze button is
// disabled and shows "Analyzing...", and Apply Fix stays disabled.
enum class AnalyzeState
{
    Idle,
    Preparing,
    Analyzing,
    ResultReady,
    NotEnoughMaterial,
    Failed
};

inline bool analyzeStateIsBusy (AnalyzeState state) noexcept
{
    return state == AnalyzeState::Preparing || state == AnalyzeState::Analyzing;
}

inline bool analyzeStateIsResolved (AnalyzeState state) noexcept
{
    return state == AnalyzeState::ResultReady
        || state == AnalyzeState::NotEnoughMaterial
        || state == AnalyzeState::Failed;
}

inline const char* analyzeStateButtonText (AnalyzeState state) noexcept
{
    return analyzeStateIsBusy (state) ? "Analyzing..." : "Analyze";
}
