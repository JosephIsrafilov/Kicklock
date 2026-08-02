#pragma once

#include <functional>

#include "DynamicWorkspaceHelpers.h"

// Phase 12, Section 13/14: Focus status and the bounded selected-State trace.
// Never called "Solo" - the full mix keeps playing normally; this component
// only gates which State's already-happening measurement captures get drawn.
// Shows real data only (kick reference / bass before / actual audible bass
// after, stableStateId, map generation, settled branch status) - never a
// fabricated waveform when data is unavailable.
class DynamicFocusStatus : public juce::Component
{
public:
    DynamicFocusStatus();

    void setModel (const DynamicWorkspaceViewModel& model);

    void paint (juce::Graphics&) override;
    void resized() override;

    std::function<void (bool)> onFocusToggle;

private:
    void drawTrace (juce::Graphics& g, juce::Rectangle<int> area,
                    const std::vector<float>& samples, juce::Colour colour,
                    const juce::String& label) const;

    DynamicWorkspaceViewModel model;
    juce::ToggleButton focusToggle { "Focus Selected State" };
    juce::Label statusLabel;
    juce::Rectangle<int> kickTraceArea, beforeTraceArea, afterTraceArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DynamicFocusStatus)
};
