#pragma once

#include <array>

#include <JuceHeader.h>

#include "../dsp/DynamicRuntimeSnapshot.h"
#include "../dsp/DynamicRuntimeSelector.h"
#include "../dsp/LearnState.h"

// Complete message-thread input for DynamicWorkspace. The editor owns processor
// polling and workflow actions; the workspace only renders this stored value.
struct DynamicWorkspaceViewModel
{
    CorrectionMode mode = CorrectionMode::Static;
    LearnState learnState = LearnState::Idle;

    bool previewActive = false;
    bool previewApplyAvailable = false;
    bool previewApplyBlocked = false;
    juce::String previewBlockedReason;
    uint64_t previewSessionId = 0;
    DynamicStateMap previewMap = makeEmptyDynamicStateMap();
    std::array<DynamicMeasurementSummary, DynamicMeasurementContract::kMaxRetainedStates> previewPredicted {};

    DynamicRuntimeSnapshot runtime = makeEmptyDynamicRuntimeSnapshot();

    int capturedHits = 0;
    int processedHits = 0;
    bool clearAvailable = false;
    bool revertAvailable = false;
};
