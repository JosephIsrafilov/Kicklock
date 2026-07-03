#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "ui/LookAndFeel.h"
#include "ui/Oscilloscope.h"
#include "ui/CorrelationDisplay.h"

class KickLockAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit KickLockAudioProcessorEditor (KickLockAudioProcessor&);
    ~KickLockAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    KickLockAudioProcessor& audioProcessor;

    KickLockLookAndFeel lookAndFeel;
    Oscilloscope oscilloscope;
    CorrelationDisplay correlationDisplay;
    juce::TextButton freezeButton;
    juce::TextButton analyzeButton;
    juce::Label analyzeResultLabel;

    juce::Slider timeZoomSlider;
    juce::Label timeZoomLabel;
    juce::Slider ampZoomSlider;
    juce::Label ampZoomLabel;

    juce::Label alignmentHeader;
    juce::Label rotatorHeader;

    juce::Slider delayMsSlider;
    juce::Label delayMsLabel;
    juce::ComboBox delayInterpBox;
    juce::Label delayInterpLabel;
    juce::ToggleButton polarityInvertButton;
    juce::Slider rotatorFreqSlider;
    juce::Label rotatorFreqLabel;
    juce::Slider rotatorQSlider;
    juce::Label rotatorQLabel;
    juce::ComboBox rotatorStagesBox;
    juce::Label rotatorStagesLabel;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    std::unique_ptr<SliderAttachment> delayMsAttachment;
    std::unique_ptr<ComboBoxAttachment> delayInterpAttachment;
    std::unique_ptr<ButtonAttachment> polarityInvertAttachment;
    std::unique_ptr<SliderAttachment> rotatorFreqAttachment;
    std::unique_ptr<SliderAttachment> rotatorQAttachment;
    std::unique_ptr<ComboBoxAttachment> rotatorStagesAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KickLockAudioProcessorEditor)
};
