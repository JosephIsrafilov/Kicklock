#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

class KickLockAudioProcessorEditor : public juce::AudioProcessorEditor,
                                     private juce::Timer
{
public:
    explicit KickLockAudioProcessorEditor (KickLockAudioProcessor&);
    ~KickLockAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void configureSlider (juce::Slider& slider, const juce::String& suffix, int decimals);
    void configureLabel (juce::Label& label, const juce::String& text);

    KickLockAudioProcessor& audioProcessor;

    juce::Slider delaySlider;
    juce::Slider allpassFreqSlider;
    juce::ToggleButton polarityInvertButton;
    juce::ToggleButton allpassEnableButton;
    juce::TextButton autoAlignButton;

    juce::Label delayLabel;
    juce::Label allpassFreqLabel;
    juce::Label polarityLabel;
    juce::Label allpassEnableLabel;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    std::unique_ptr<SliderAttachment> delayAttachment;
    std::unique_ptr<SliderAttachment> allpassFreqAttachment;
    std::unique_ptr<ButtonAttachment> polarityInvertAttachment;
    std::unique_ptr<ButtonAttachment> allpassEnableAttachment;

    float displayCorrelation = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KickLockAudioProcessorEditor)
};
