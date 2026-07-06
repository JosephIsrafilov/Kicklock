#pragma once

#include <JuceHeader.h>

// Coherent dark theme: near-black background, teal + orange accents, and a
// flat arc-style rotary (ring track + teal value arc + thumb dot) in place of
// the stock LookAndFeel_V4 knob.
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

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider& slider) override
    {
        const auto bounds = juce::Rectangle<float> ((float) x, (float) y,
                                                    (float) width, (float) height).reduced (6.0f);
        const float radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f - 2.0f;
        if (radius <= 4.0f)
        {
            juce::LookAndFeel_V4::drawRotarySlider (g, x, y, width, height, sliderPosProportional,
                                                    rotaryStartAngle, rotaryEndAngle, slider);
            return;
        }

        const auto centre = bounds.getCentre();
        const float angle = rotaryStartAngle
                          + sliderPosProportional * (rotaryEndAngle - rotaryStartAngle);
        const float arcWidth = juce::jlimit (2.0f, 3.2f, radius * 0.12f);

        juce::Path track;
        track.addCentredArc (centre.x, centre.y, radius, radius, 0.0f,
                             rotaryStartAngle, rotaryEndAngle, true);
        g.setColour (slider.findColour (juce::Slider::rotarySliderOutlineColourId));
        g.strokePath (track, { arcWidth, juce::PathStrokeType::curved,
                               juce::PathStrokeType::rounded });

        juce::Path value;
        value.addCentredArc (centre.x, centre.y, radius, radius, 0.0f,
                             rotaryStartAngle, angle, true);
        const auto fill = slider.findColour (juce::Slider::rotarySliderFillColourId)
                                .withAlpha (slider.isEnabled() ? 1.0f : 0.4f);
        g.setColour (fill);
        g.strokePath (value, { arcWidth, juce::PathStrokeType::curved,
                               juce::PathStrokeType::rounded });

        // Pointer: a thumb dot just inside the arc at the current angle.
        const float dotRadius = juce::jlimit (2.0f, 4.0f, radius * 0.14f);
        const auto dotCentre = centre.getPointOnCircumference (radius - arcWidth - dotRadius - 1.0f,
                                                               angle);
        g.setColour (slider.findColour (juce::Slider::thumbColourId)
                           .withAlpha (slider.isEnabled() ? 1.0f : 0.4f));
        g.fillEllipse (juce::Rectangle<float> (dotRadius * 2.0f, dotRadius * 2.0f)
                           .withCentre (dotCentre));
    }
};
