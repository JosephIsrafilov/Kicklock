#pragma once

#include <JuceHeader.h>

// Coherent dark theme: near-black background, teal + orange accents.
// Palette only — no overridden draw methods.
class KickLockLookAndFeel : public juce::LookAndFeel_V4
{
public:
    KickLockLookAndFeel()
    {
        const juce::Colour background { 0xff1a1a1a };
        const juce::Colour teal       { 0xff2dd4bf };
        const juce::Colour orange      { 0xfff97316 };
        const juce::Colour text        { 0xffe5e5e5 };

        setColour (juce::ResizableWindow::backgroundColourId, background);
        setColour (juce::Slider::backgroundColourId,          background);

        setColour (juce::Slider::thumbColourId,               teal);
        setColour (juce::Slider::trackColourId,               teal);
        setColour (juce::Slider::rotarySliderFillColourId,    teal);
        setColour (juce::ToggleButton::tickColourId,          teal);

        setColour (juce::ComboBox::backgroundColourId,        background);
        setColour (juce::ComboBox::outlineColourId,           orange);
        setColour (juce::ComboBox::arrowColourId,             orange);

        setColour (juce::Slider::textBoxTextColourId,         text);
        setColour (juce::Slider::textBoxOutlineColourId,      background);
        setColour (juce::Label::textColourId,                 text);
        setColour (juce::ComboBox::textColourId,              text);
        setColour (juce::ToggleButton::textColourId,          text);
    }
};
