#pragma once

#include <functional>
#include <memory>

#include "DynamicWorkspaceHelpers.h"
#include "DynamicStateInspector.h"
#include "DynamicFocusStatus.h"
#include "DynamicRecentUnknownsPanel.h"

class DynamicWorkspace : public juce::Component
{
public:
    DynamicWorkspace();
    ~DynamicWorkspace() override;

    void setModel (const DynamicWorkspaceViewModel& model);
    void setDynamicStrengthControls (juce::Label* label, juce::Slider* slider);

    void resized() override;
    void paint (juce::Graphics&) override;

    std::function<void()> onClear;
    std::function<void()> onRevert;
    std::function<void()> onApply;
    std::function<void (const DynamicStateInspectorEditRequest&)> onEdit;
    std::function<void (bool)> onFocusToggle;
    std::function<void (uint64_t)> onCreateManualState;
    std::function<void (uint64_t)> onIgnoreRecentUnknown;
    std::function<void()> onClearRecentUnknowns;

    int getVisibleCardCount() const noexcept;
    int getCardColumnCount() const noexcept { return cardColumns; }
    void selectDetailState (uint64_t stableStateId);
    const std::array<juce::Rectangle<int>, DynamicMeasurementContract::kMaxRetainedStates>& getCardBoundsForTesting() const noexcept
    {
        return cardBounds;
    }
    uint64_t getSelectedDetailStableStateId() const noexcept { return selectedDetailStableStateId; }

private:
    class StateCardComponent;

    void setSelectedDetailStableStateId (uint64_t stableStateId);
    bool containsStableStateId (uint64_t stableStateId) const noexcept;

    DynamicWorkspaceViewModel model;
    std::array<std::unique_ptr<StateCardComponent>, DynamicMeasurementContract::kMaxRetainedStates> cards;
    std::array<juce::Rectangle<int>, DynamicMeasurementContract::kMaxRetainedStates> cardBounds {};
    std::array<int, DynamicMeasurementContract::kMaxRetainedStates> orderedSlots {};
    juce::TextButton clearButton { "Clear Map" };
    juce::TextButton revertButton { "Revert" };
    juce::Label* dynamicStrengthLabel = nullptr;
    juce::Slider* dynamicStrengthSlider = nullptr;
    uint64_t selectedDetailStableStateId = 0;
    int cardColumns = 4;
    juce::String headerSource;
    juce::String headerStatus;
    juce::String headerDetail;
    juce::String headerSummary;
    DynamicStateInspector inspector;
    DynamicFocusStatus focusStatus;
    DynamicRecentUnknownsPanel recentUnknownsPanel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DynamicWorkspace)
};
