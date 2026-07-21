#pragma once

#include <array>
#include <vector>

#include <JuceHeader.h>

#include "../dsp/DynamicRuntimeSnapshot.h"
#include "../dsp/DynamicRuntimeSelector.h"
#include "../dsp/LearnState.h"
#include "../dsp/DynamicRecentUnknownEvents.h"

// Phase 12, Section 13: mirrors PluginProcessor::DynamicFocusedTraceForUi by
// value (same reasoning as every other field in this view model - the editor
// copies processor-owned data in, the workspace/inspector never reaches back
// into the processor itself). Kept as a separate, small struct here rather
// than shared with PluginProcessor.h to avoid a ui-layer header depending on
// the full processor class definition.
struct DynamicFocusedTraceViewModel
{
    bool available = false;
    uint64_t stableStateId = 0;
    uint64_t mapGeneration = 0;
    DynamicSelectorBranchKind branchKind = DynamicSelectorBranchKind::Global;
    double sampleRate = 0.0;
    int windowSamples = 0;
    std::vector<float> beforeBass;
    std::vector<float> beforeKick;
    std::vector<float> afterBass;
    std::vector<float> afterKick;
};

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
    double sampleRate = 48000.0;

    int capturedHits = 0;
    int processedHits = 0;
    // Fully formatted by the editor from the processor-owned finalized result.
    juce::String learnStatusMessage;
    bool clearAvailable = false;
    bool revertAvailable = false;

    // Phase 12, Section 7: the selected State's full raw data (package/trim
    // fields DynamicStateCard does not carry). hasSelectedState mirrors
    // whether selectedStableStateId currently resolves to an occupied State.
    uint64_t selectedStableStateId = 0;
    bool hasSelectedState = false;
    DynamicState selectedState;

    // Phase 12, Section 13: Focus.
    bool focusEnabled = false;
    uint64_t focusedStableStateId = 0;
    DynamicFocusedTraceViewModel focusedTrace;

    // Phase 12, Section 15: Recent Unknown Events (bounded, non-persistent).
    std::vector<DynamicRecentUnknownCluster> recentUnknownClusters;
    uint64_t recentUnknownOverflowCount = 0;

    // Last edit-transaction outcome, for producer-facing "why did that fail"
    // feedback (Section 10/16 - "no silent failure").
    bool lastEditFailed = false;
    juce::String lastEditFailureReason;
};
