#include "DynamicFocusStatus.h"

namespace
{
    const auto kPanel = juce::Colour (0xff171d23);
    const auto kBorder = juce::Colour (0xff2b3540);
    const auto kText = juce::Colour (0xffedf2f7);
    const auto kMuted = juce::Colour (0xff97a5b2);
    const auto kTeal = juce::Colour (0xff2dd4bf);
    const auto kOrange = juce::Colour (0xfff97316);
    const auto kAmber = juce::Colour (0xfff59e0b);
}

DynamicFocusStatus::DynamicFocusStatus()
{
    setName ("Dynamic Focus Status");
    setDescription ("Focuses measurement and trace display on the selected State without changing playback.");

    focusToggle.setName ("Focus Selected State");
    focusToggle.setDescription ("Watches the selected State's own detection/verification without soloing or muting audio.");
    focusToggle.onClick = [this]
    {
        if (onFocusToggle) onFocusToggle (focusToggle.getToggleState());
    };
    addAndMakeVisible (focusToggle);

    statusLabel.setColour (juce::Label::textColourId, kMuted);
    statusLabel.setFont (juce::Font (juce::FontOptions (10.0f)));
    addAndMakeVisible (statusLabel);
}

void DynamicFocusStatus::setModel (const DynamicWorkspaceViewModel& newModel)
{
    model = newModel;
    focusToggle.setToggleState (model.focusEnabled, juce::dontSendNotification);
    focusToggle.setEnabled (model.hasSelectedState);
    statusLabel.setText (dynamicFocusStatusText (model), juce::dontSendNotification);
    repaint();
}

void DynamicFocusStatus::resized()
{
    auto area = getLocalBounds().reduced (10, 6);
    auto topRow = area.removeFromTop (20);
    focusToggle.setBounds (topRow.removeFromLeft (170));
    statusLabel.setBounds (topRow);

    area.removeFromTop (4);
    const int gap = 6;
    const int traceWidth = (area.getWidth() - gap * 2) / 3;
    kickTraceArea = area.removeFromLeft (traceWidth);
    area.removeFromLeft (gap);
    beforeTraceArea = area.removeFromLeft (traceWidth);
    area.removeFromLeft (gap);
    afterTraceArea = area;
}

void DynamicFocusStatus::drawTrace (juce::Graphics& g, juce::Rectangle<int> area,
                                    const std::vector<float>& samples, juce::Colour colour,
                                    const juce::String& label) const
{
    g.setColour (kBorder);
    g.drawRect (area, 1);

    auto textArea = area.removeFromTop (12);
    g.setColour (kMuted);
    g.setFont (juce::Font (juce::FontOptions (8.5f)));
    g.drawText (label, textArea, juce::Justification::centred);

    if (samples.empty())
    {
        g.setColour (kMuted);
        g.setFont (juce::Font (juce::FontOptions (9.0f)));
        g.drawText ("no data", area, juce::Justification::centred);
        return;
    }

    float maxAbs = 1.0e-6f;
    for (const float s : samples)
        maxAbs = std::max (maxAbs, std::abs (s));

    juce::Path path;
    const auto bounds = area.toFloat();
    const float midY = bounds.getCentreY();
    const float halfHeight = bounds.getHeight() * 0.5f;
    for (size_t i = 0; i < samples.size(); ++i)
    {
        const float x = bounds.getX() + bounds.getWidth() * (float) i / (float) juce::jmax ((size_t) 1, samples.size() - 1);
        const float y = midY - (samples[i] / maxAbs) * halfHeight;
        if (i == 0) path.startNewSubPath (x, y); else path.lineTo (x, y);
    }
    g.setColour (colour);
    g.strokePath (path, juce::PathStrokeType (1.0f));
}

void DynamicFocusStatus::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour (kPanel);
    g.fillRoundedRectangle (bounds, 7.0f);
    g.setColour (kBorder);
    g.drawRoundedRectangle (bounds, 7.0f, 1.0f);

    if (! model.focusEnabled || ! model.hasSelectedState)
        return;

    const auto& trace = model.focusedTrace;
    if (! trace.available)
    {
        g.setColour (kMuted);
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText ("Waiting for a fresh, settled hit on this State...",
                    getLocalBounds().reduced (10).removeFromBottom (100), juce::Justification::centred);
        return;
    }

    drawTrace (g, kickTraceArea, trace.beforeKick, kOrange, "KICK REF");
    drawTrace (g, beforeTraceArea, trace.beforeBass, kAmber, "BASS BEFORE");
    drawTrace (g, afterTraceArea, trace.afterBass, kTeal, "BASS AFTER");

    auto footer = getLocalBounds().reduced (10, 2).removeFromBottom (12);
    g.setColour (kMuted);
    g.setFont (juce::Font (juce::FontOptions (8.5f)));
    juce::String meta = "ID " + fullDynamicStateId (trace.stableStateId)
        + "  gen " + juce::String ((int64_t) trace.mapGeneration)
        + "  " + dynamicBranchLabel (trace.branchKind) + " (settled)";
    g.drawText (meta, footer, juce::Justification::centredLeft, true);
    juce::ignoreUnused (kText);
}
