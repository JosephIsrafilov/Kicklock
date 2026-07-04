#include "PluginEditor.h"

#include <cmath>

namespace
{
    const auto background = juce::Colour (0xff101418);
    const auto panel = juce::Colour (0xff171d23);
    const auto border = juce::Colour (0xff2b3540);
    const auto text = juce::Colour (0xffedf2f7);
    const auto mutedText = juce::Colour (0xff97a5b2);
    const auto green = juce::Colour (0xff22c55e);
    const auto red = juce::Colour (0xffef4444);
    const auto amber = juce::Colour (0xfff59e0b);

    juce::Colour colourForCorrelation (float value)
    {
        const auto clamped = juce::jlimit (-1.0f, 1.0f, value);
        return clamped >= 0.0f ? amber.interpolatedWith (green, clamped)
                               : amber.interpolatedWith (red, -clamped);
    }
}

KickLockAudioProcessorEditor::KickLockAudioProcessorEditor (KickLockAudioProcessor& p)
    : AudioProcessorEditor (&p), audioProcessor (p)
{
    configureSlider (delaySlider, " ms", 2);
    delaySlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    delaySlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 92, 24);
    addAndMakeVisible (delaySlider);

    configureSlider (allpassFreqSlider, " Hz", 0);
    allpassFreqSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    allpassFreqSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 92, 24);
    addAndMakeVisible (allpassFreqSlider);

    polarityInvertButton.setButtonText ("Invert");
    allpassEnableButton.setButtonText ("Enable");
    addAndMakeVisible (polarityInvertButton);
    addAndMakeVisible (allpassEnableButton);

    autoAlignButton.setButtonText ("AUTO-ALIGN");
    autoAlignButton.setColour (juce::TextButton::buttonColourId, amber);
    autoAlignButton.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
    autoAlignButton.onClick = [this]
    {
        audioProcessor.requestAutoAlign();
        autoAlignButton.setButtonText ("ARMED");
    };
    addAndMakeVisible (autoAlignButton);

    configureLabel (delayLabel, "DELAY");
    configureLabel (allpassFreqLabel, "ALLPASS FREQ");
    configureLabel (polarityLabel, "POLARITY");
    configureLabel (allpassEnableLabel, "ALLPASS");

    delayAttachment = std::make_unique<SliderAttachment> (audioProcessor.apvts, "delay_ms", delaySlider);
    allpassFreqAttachment = std::make_unique<SliderAttachment> (audioProcessor.apvts, "allpass_freq", allpassFreqSlider);
    polarityInvertAttachment = std::make_unique<ButtonAttachment> (audioProcessor.apvts, "polarity_invert", polarityInvertButton);
    allpassEnableAttachment = std::make_unique<ButtonAttachment> (audioProcessor.apvts, "allpass_enable", allpassEnableButton);

    setSize (720, 460);
    startTimerHz (60);
}

KickLockAudioProcessorEditor::~KickLockAudioProcessorEditor()
{
    stopTimer();
}

void KickLockAudioProcessorEditor::configureSlider (juce::Slider& slider, const juce::String& suffix, int decimals)
{
    slider.setNumDecimalPlacesToDisplay (decimals);
    slider.setTextValueSuffix (suffix);
    slider.setColour (juce::Slider::rotarySliderFillColourId, green);
    slider.setColour (juce::Slider::rotarySliderOutlineColourId, border);
    slider.setColour (juce::Slider::thumbColourId, text);
    slider.setColour (juce::Slider::textBoxTextColourId, text);
    slider.setColour (juce::Slider::textBoxOutlineColourId, border);
}

void KickLockAudioProcessorEditor::configureLabel (juce::Label& label, const juce::String& labelText)
{
    label.setText (labelText, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.setFont (juce::Font (juce::FontOptions (12.0f)).boldened());
    label.setColour (juce::Label::textColourId, mutedText);
    addAndMakeVisible (label);
}

void KickLockAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);

    auto bounds = getLocalBounds().reduced (18);
    auto titleArea = bounds.removeFromTop (54);

    g.setColour (text);
    g.setFont (juce::Font (juce::FontOptions (28.0f)).boldened());
    g.drawText ("KICKLOCK", titleArea.removeFromLeft (220), juce::Justification::centredLeft);

    g.setColour (mutedText);
    g.setFont (juce::Font (juce::FontOptions (13.0f)));
    g.drawText ("Bass-to-kick phase alignment", titleArea, juce::Justification::centredRight);

    bounds.removeFromTop (8);
    auto meterArea = bounds.removeFromTop (190).toFloat();
    g.setColour (panel);
    g.fillRoundedRectangle (meterArea, 8.0f);
    g.setColour (border);
    g.drawRoundedRectangle (meterArea, 8.0f, 1.0f);

    const auto meterColour = colourForCorrelation (displayCorrelation);
    auto inner = meterArea.reduced (24.0f, 20.0f);

    g.setColour (mutedText);
    g.setFont (juce::Font (juce::FontOptions (12.0f)).boldened());
    g.drawText ("CORRELATION", inner.removeFromTop (18.0f).toNearestInt(), juce::Justification::centred);

    g.setColour (meterColour);
    g.setFont (juce::Font (juce::FontOptions (58.0f)).boldened());
    g.drawText (juce::String (displayCorrelation, 2),
                inner.removeFromTop (70.0f).toNearestInt(),
                juce::Justification::centred);

    auto bar = inner.removeFromTop (24.0f).reduced (8.0f, 7.0f);
    g.setColour (juce::Colour (0xff26303a));
    g.fillRoundedRectangle (bar, 4.0f);

    const auto centerX = bar.getCentreX();
    const auto fillWidth = (bar.getWidth() * 0.5f) * std::abs (displayCorrelation);
    auto fill = displayCorrelation >= 0.0f
        ? juce::Rectangle<float> (centerX, bar.getY(), fillWidth, bar.getHeight())
        : juce::Rectangle<float> (centerX - fillWidth, bar.getY(), fillWidth, bar.getHeight());

    g.setColour (meterColour);
    g.fillRoundedRectangle (fill, 4.0f);

    g.setColour (meterColour);
    g.setFont (juce::Font (juce::FontOptions (15.0f)).boldened());
    g.drawText (displayCorrelation > 0.15f ? "IN PHASE"
                : displayCorrelation < -0.15f ? "CANCELING"
                                               : "LOW OR MIXED SIGNAL",
                inner.removeFromTop (26.0f).toNearestInt(),
                juce::Justification::centred);

    bounds.removeFromTop (18);
    auto controlPanel = bounds.toFloat();
    g.setColour (panel);
    g.fillRoundedRectangle (controlPanel, 8.0f);
    g.setColour (border);
    g.drawRoundedRectangle (controlPanel, 8.0f, 1.0f);
}

void KickLockAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds().reduced (18);
    bounds.removeFromTop (54);
    bounds.removeFromTop (8);
    bounds.removeFromTop (190);
    bounds.removeFromTop (18);

    auto controls = bounds.reduced (18, 16);
    auto top = controls.removeFromTop (150);

    auto delayArea = top.removeFromLeft (170);
    delayLabel.setBounds (delayArea.removeFromTop (20));
    delaySlider.setBounds (delayArea);

    top.removeFromLeft (16);

    auto freqArea = top.removeFromLeft (170);
    allpassFreqLabel.setBounds (freqArea.removeFromTop (20));
    allpassFreqSlider.setBounds (freqArea);

    top.removeFromLeft (20);

    auto toggles = top.removeFromLeft (150);
    polarityLabel.setBounds (toggles.removeFromTop (20));
    polarityInvertButton.setBounds (toggles.removeFromTop (36));
    toggles.removeFromTop (12);
    allpassEnableLabel.setBounds (toggles.removeFromTop (20));
    allpassEnableButton.setBounds (toggles.removeFromTop (36));

    auto buttonArea = controls.removeFromBottom (54);
    autoAlignButton.setBounds (buttonArea.removeFromRight (220).reduced (0, 5));
}

void KickLockAudioProcessorEditor::timerCallback()
{
    const auto target = audioProcessor.realtimeCorrelation.load (std::memory_order_relaxed);
    displayCorrelation += 0.25f * (target - displayCorrelation);

    if (autoAlignButton.getButtonText() == "ARMED"
        && std::abs (target) > 0.02f)
    {
        autoAlignButton.setButtonText ("AUTO-ALIGN");
    }

    repaint();
}
