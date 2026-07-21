#pragma once

#include <functional>

#include "DynamicStateEditThrottle.h"
#include "DynamicWorkspaceHelpers.h"

// Phase 12, Section 7/8/9 (Checkpoint 1 scope: Identity/Recognition/
// Correction/Package/Measurements + Enable/Disable/Bypass/Reset actions.
// Promote/Remove Manual/Focus live in DynamicWorkspace's Checkpoint-2 pieces).
//
// Displays and edits the single State identified by
// DynamicWorkspaceViewModel::selectedStableStateId. Never mutates the
// processor directly - only emits onEdit() requests, which PluginEditor
// translates into KickLockAudioProcessor::applyDynamicStateEdit() calls, and
// never assumes the edit succeeded (the caller feeds the outcome back through
// the next setModel() via lastEditFailed/lastEditFailureReason).
enum class DynamicStateInspectorAction
{
    SetManualTrim,
    ResetManualTrim,
    ResetToLearned,
    ResetToGlobal,
    SetEnabled,
    SetBypassed
};

struct DynamicStateInspectorEditRequest
{
    DynamicStateInspectorAction action = DynamicStateInspectorAction::SetManualTrim;
    uint64_t stableStateId = 0;
    DynamicManualTrim trim;
    bool boolValue = false;
};

class DynamicStateInspector : public juce::Component,
                              private juce::Timer
{
public:
    DynamicStateInspector();
    ~DynamicStateInspector() override;

    void setModel (const DynamicWorkspaceViewModel& model);

    void paint (juce::Graphics&) override;
    void resized() override;

    std::function<void (const DynamicStateInspectorEditRequest&)> onEdit;

private:
    void timerCallback() override;
    void publishTrimIfPending();
    void commitTrimNow();
    DynamicManualTrim currentTrimFromSliders() const noexcept;
    void updateSlidersFromModel();
    void updateControlEnablement();

    DynamicWorkspaceViewModel model;
    DynamicEffectivePackageDisplay effective;
    DynamicStateControlEligibility eligibility;

    juce::Label identityLabel;
    juce::Label recognitionLabel;
    juce::Label correctionLabel;
    juce::Label measurementLabel;
    juce::Label delayLabel { {}, "Delay (ms)" };
    juce::Label freqLabel { {}, "Frequency (Hz)" };
    juce::Label qLabel { {}, "Q" };
    juce::Slider delaySlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::Slider freqSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    juce::Slider qSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };

    juce::TextButton enableButton { "Enable" };
    juce::TextButton bypassButton { "Bypass" };
    juce::TextButton resetTrimButton { "Reset Trim" };
    juce::TextButton resetLearnedButton { "Reset to Learned" };
    juce::TextButton resetGlobalButton { "Reset to Global" };

    // One throttle for the whole trim: setManualTrim() publishes all three
    // fields atomically, so a Delay drag and a Frequency drag both update the
    // SAME pending draft (whichever slider last moved wins for that field;
    // "newest wins" applies per edit, not per parameter).
    DynamicStateEditThrottle trimThrottle;
    bool updatingSlidersProgrammatically = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DynamicStateInspector)
};
