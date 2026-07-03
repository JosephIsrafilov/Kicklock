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
