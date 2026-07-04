#pragma once

enum class SidechainSignalStatus
{
    WaitingForSidechain,
    SignalTooLow,
    SidechainActive
};

enum class ProcessingWorkflowStatus
{
    MonitoringDryInput,
    FixReady,
    ProcessingBass
};

inline SidechainSignalStatus classifySidechainSignalStatus (bool hasSidechain,
                                                            float bassSignalRms,
                                                            float kickSignalRms,
                                                            float minimumSignalRms = 1.0e-4f) noexcept
{
    if (! hasSidechain)
        return SidechainSignalStatus::WaitingForSidechain;

    if (bassSignalRms < minimumSignalRms || kickSignalRms < minimumSignalRms)
        return SidechainSignalStatus::SignalTooLow;

    return SidechainSignalStatus::SidechainActive;
}

inline ProcessingWorkflowStatus classifyProcessingWorkflowStatus (bool bassProcessingNeutral,
                                                                 bool fixReady) noexcept
{
    if (! bassProcessingNeutral)
        return ProcessingWorkflowStatus::ProcessingBass;

    return fixReady ? ProcessingWorkflowStatus::FixReady
                    : ProcessingWorkflowStatus::MonitoringDryInput;
}

// -----------------------------------------------------------------------------
// P3: musically-aware material/signal status.
//
// The old classifySidechainSignalStatus above uses instant per-block RMS, which
// treats the normal silence between kick transients as "no signal" and flickers
// to SIGNAL TOO LOW on every real beat. This richer classifier is driven by
// HELD activity flags (see SignalActivityTracker) plus a captured-material
// count, so a steady loop reads as continuously active and progresses
// Capturing -> Ready without flicker.
//
// Inputs (all computed off held/envelope-smoothed signals, not one block):
//   hasSidechain    - sidechain bus connected and carrying channels
//   kickActive      - kick crossed its threshold within the hold window
//   bassActive      - bass crossed its threshold within the hold window
//   materialUsable  - both signals' recent level is above a usable floor
//                     (distinguishes "present but far too quiet to phase-read"
//                      from "genuinely playing")
//   enoughMaterial  - enough samples captured for a meaningful analysis window
enum class AnalysisMaterialStatus
{
    WaitingForSidechain,
    WaitingForKick,
    WaitingForBass,
    SignalTooLow,
    CapturingMaterial,
    ReadyToAnalyze
};

inline AnalysisMaterialStatus classifyAnalysisMaterialStatus (bool hasSidechain,
                                                              bool kickActive,
                                                              bool bassActive,
                                                              bool materialUsable,
                                                              bool enoughMaterial) noexcept
{
    if (! hasSidechain)
        return AnalysisMaterialStatus::WaitingForSidechain;

    if (! kickActive)
        return AnalysisMaterialStatus::WaitingForKick;

    if (! bassActive)
        return AnalysisMaterialStatus::WaitingForBass;

    // Both kick and bass are present within the hold window from here on, so we
    // never fall back to a "waiting"/"too low" state just because this instant
    // sits between two kick transients.
    if (! materialUsable)
        return AnalysisMaterialStatus::SignalTooLow;

    return enoughMaterial ? AnalysisMaterialStatus::ReadyToAnalyze
                          : AnalysisMaterialStatus::CapturingMaterial;
}

// True for any state where the sidechain is connected and both kick and bass
// are within their hold windows (i.e. the top-line reads "SIDECHAIN ACTIVE").
inline bool analysisStatusHasLiveSidechain (AnalysisMaterialStatus status) noexcept
{
    return status == AnalysisMaterialStatus::SignalTooLow
        || status == AnalysisMaterialStatus::CapturingMaterial
        || status == AnalysisMaterialStatus::ReadyToAnalyze;
}

inline bool analysisStatusCanStartAnalyze (AnalysisMaterialStatus status) noexcept
{
    return status == AnalysisMaterialStatus::ReadyToAnalyze;
}

inline const char* analyzeButtonTextForStatus (AnalysisMaterialStatus status) noexcept
{
    switch (status)
    {
        case AnalysisMaterialStatus::WaitingForSidechain: return "Analyze - no sidechain";
        case AnalysisMaterialStatus::WaitingForKick:      return "Analyze - waiting for kick";
        case AnalysisMaterialStatus::WaitingForBass:      return "Analyze - waiting for bass";
        case AnalysisMaterialStatus::SignalTooLow:        return "Analyze - signal too low";
        case AnalysisMaterialStatus::CapturingMaterial:   return "Analyze - capturing";
        case AnalysisMaterialStatus::ReadyToAnalyze:      return "Analyze";
    }

    return "Analyze";
}

// Apply Fix must depend on the latest valid analysis result, not on whether the
// current instant happens to be between kicks. This gate only requires the
// sidechain to still be connected — never the instantaneous level.
inline bool applyFixAvailable (bool hasSidechain, bool fixReady) noexcept
{
    return hasSidechain && fixReady;
}
