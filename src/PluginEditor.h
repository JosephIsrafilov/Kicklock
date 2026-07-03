#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "ui/LookAndFeel.h"
#include "ui/Oscilloscope.h"
#include "ui/CorrelationDisplay.h"
#include "ui/StatusHelpers.h"
#include "ui/UiFormatters.h"

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
    void updateStatusLabels();
    void syncScopeSettings();

    KickLockAudioProcessor& audioProcessor;

    KickLockLookAndFeel lookAndFeel;
    Oscilloscope oscilloscope;
    CorrelationDisplay correlationDisplay;

    juce::TextButton freezeButton;
    juce::TextButton analyzeButton;
    juce::TextButton applyFixButton;

    juce::Label analysisHeaderLabel;
    juce::Label analyzeResultLabel;
    juce::Label fixActionLabel;
    juce::Label fixStatsLabel;
    juce::Label fixConfidenceLabel;
    juce::Label fixSettingsLabel;
    juce::Label fixTimelineLabel;
    juce::Label sidechainStatusLabel;
    juce::Label processingStatusLabel;
    juce::Label processingDetailLabel;
    juce::Label bpmLabel;
    juce::Label bpmValueLabel;

    juce::ComboBox gridDivisionBox;
    juce::Label gridDivisionLabel;
    juce::ComboBox viewModeBox;
    juce::Label viewModeLabel;
    juce::Slider visualOffsetSlider;
    juce::Label visualOffsetLabel;

    juce::Slider timeZoomSlider;
    juce::Label timeZoomLabel;
    juce::Slider ampZoomSlider;
    juce::Label ampZoomLabel;

    juce::Label phaseSectionLabel;
    juce::Slider delayMsSlider;
    juce::Label delayMsLabel;
    juce::ComboBox delayInterpBox;
    juce::Label delayInterpLabel;
    juce::ToggleButton polarityInvertButton;
    juce::ToggleButton phaseFilterEnabledButton;
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
    std::unique_ptr<ComboBoxAttachment> gridDivisionAttachment;
    std::unique_ptr<ComboBoxAttachment> viewModeAttachment;
    std::unique_ptr<SliderAttachment> visualOffsetAttachment;
    std::unique_ptr<ButtonAttachment> polarityInvertAttachment;
    std::unique_ptr<ButtonAttachment> phaseFilterEnabledAttachment;
    std::unique_ptr<SliderAttachment> rotatorFreqAttachment;
    std::unique_ptr<SliderAttachment> rotatorQAttachment;
    std::unique_ptr<ComboBoxAttachment> rotatorStagesAttachment;

    bool hasFixReady = false;
    bool hasAnalyzedResult = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KickLockAudioProcessorEditor)
};
