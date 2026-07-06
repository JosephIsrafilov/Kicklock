#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "ui/LookAndFeel.h"
#include "ui/Oscilloscope.h"
#include "ui/CorrelationDisplay.h"
#include "ui/StatusHelpers.h"
#include "dsp/AnalyzeState.h"

// Kick-punch transient integrity readout. Shows the signed PUNCH dB (how much
// the bass reinforces or cancels the kick's low end), a one-line verdict, a
// diverging bar centred on the kick-alone baseline, and — once a reference is
// captured — the delta against it. Falls back to a neutral placeholder when the
// meter has no recent kick to measure.
class TransientPunchComponent : public juce::Component
{
public:
    // hasSidechainIn / validIn are reported separately so the placeholder text
    // can tell "route kick to sidechain" (no sidechain routed at all) apart
    // from "waiting for kick" (sidechain routed, just no recent hit) instead of
    // collapsing both into one generic message.
    void setValues (float punchDbIn, bool validIn, bool hasSidechainIn, bool hasReferenceIn, float referenceDbIn,
                    float kickPeakIn, float sumPeakIn) noexcept
    {
        punchDb = punchDbIn;
        valid = validIn;
        hasSidechain = hasSidechainIn;
        hasReference = hasReferenceIn;
        referenceDb = referenceDbIn;
        kickPeak = kickPeakIn;
        sumPeak = sumPeakIn;
        repaint();
    }

    void paint (juce::Graphics&) override;

private:
    float punchDb = 0.0f;
    bool valid = false;
    bool hasSidechain = false;
    bool hasReference = false;
    float referenceDb = 0.0f;
    float kickPeak = 0.0f;
    float sumPeak = 0.0f;
};

// Full visual + manual phase-alignment editor. The oscilloscope is the visual
// centre; a top status bar reports sidechain/BPM/PDC state and hosts the grid,
// view, Analyze and Apply Fix controls; a MANUAL ALIGNMENT section drives the
// signed delay, polarity, phase filter and (display-only) visual offset; and an
// analyzer panel explains what Analyze recommends. All heavy DSP lives in the
// processor — the editor only wires parameters and polls published state.
class KickLockAudioProcessorEditor : public juce::AudioProcessorEditor,
                                     private juce::Timer
{
public:
    explicit KickLockAudioProcessorEditor (KickLockAudioProcessor&);
    ~KickLockAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    void configureRotary (juce::Slider& slider);
    void configureSectionLabel (juce::Label& label, const juce::String& text);
    void configureControlLabel (juce::Label& label, const juce::String& text);
    void configureCombo (juce::ComboBox& combo, const juce::StringArray& items);

    void refreshStatusStrings();
    void refreshAnalyzeWorkflow();
    void refreshCompareButtons();
    void pushScopeSettings();

    KickLockAudioProcessor& audioProcessor;
    KickLockLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltipWindow { this, 500 };

    // --- Top bar -----------------------------------------------------------
    juce::ComboBox gridCombo;
    juce::ComboBox viewCombo;
    juce::TextButton freezeButton;
    juce::TextButton relockKickButton;
    juce::TextButton analyzeButton;
    juce::TextButton applyFixButton;
    juce::TextButton revertButton;
    juce::TextButton compareAButton;
    juce::TextButton compareBButton;
    juce::TextButton compareCopyButton;
    juce::TextButton helpButton;

    // --- Centre scope ------------------------------------------------------
    Oscilloscope oscilloscope;
    CorrelationDisplay correlationDisplay;

    // Scope overlays. Added after the scope so they draw on top of it.
    juce::Label suggestedOverlay;
    juce::Label manualDelayOverlay;
    juce::Label noSidechainOverlay;

    // --- Analyzer explanation panel ---------------------------------------
    juce::Label analyzerTitle;
    juce::Label analyzerBody;
    TransientPunchComponent transientPunch;
    juce::TextButton setRefButton;

    // --- Manual alignment --------------------------------------------------
    juce::Label manualHeader;
    juce::Slider delaySlider;
    juce::ToggleButton polarityInvertButton;
    juce::ToggleButton phaseFilterButton;
    juce::Slider phaseFreqSlider;
    juce::Slider phaseQSlider;
    juce::Slider visualOffsetSlider;
    juce::Slider crossoverSlider;

    juce::Label delayLabel;
    juce::Label polarityLabel;
    juce::Label phaseFilterLabel;
    juce::Label phaseFreqLabel;
    juce::Label phaseQLabel;
    juce::Label visualOffsetLabel;
    juce::Label crossoverLabel;

    // --- Advanced ----------------------------------------------------------
    juce::Label advancedHeader;
    juce::ComboBox delayInterpCombo;
    juce::ComboBox phaseStagesCombo;
    juce::Label delayInterpLabel;
    juce::Label phaseStagesLabel;

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using ComboAttachment  = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    std::unique_ptr<SliderAttachment> delayAttachment;
    std::unique_ptr<ButtonAttachment> polarityInvertAttachment;
    std::unique_ptr<ButtonAttachment> phaseFilterAttachment;
    std::unique_ptr<SliderAttachment> phaseFreqAttachment;
    std::unique_ptr<SliderAttachment> phaseQAttachment;
    std::unique_ptr<SliderAttachment> visualOffsetAttachment;
    std::unique_ptr<SliderAttachment> crossoverAttachment;
    std::unique_ptr<ComboAttachment>  gridAttachment;
    std::unique_ptr<ComboAttachment>  viewAttachment;
    std::unique_ptr<ComboAttachment>  delayInterpAttachment;
    std::unique_ptr<ComboAttachment>  phaseStagesAttachment;

    // Cached status/analyzer strings, recomputed on the UI timer and drawn in
    // paint(). Kept as members so paint() never touches processor internals.
    juce::String sidechainStatusText { "NO SIDECHAIN" };
    juce::Colour  sidechainStatusColour;
    juce::String bpmText { "-- BPM" };
    juce::String pdcText { "PDC --" };
    juce::String suggestedText;
    juce::String manualDelayText { "Manual Delay: 0.00 ms" };
    bool hasSidechain = false;
    bool canStartAnalyze = false;
    AnalysisMaterialStatus latestMaterialStatus = AnalysisMaterialStatus::WaitingForSidechain;
    bool helpOverlayVisible = false;

    // Latest analyzer result snapshot for the explanation panel.
    PhaseFixResult latestResult;
    AnalyzeState lastAnalyzeState = AnalyzeState::Idle;
    bool haveResult = false;
    bool latestResultAutoApplied = false;

    // Panel backgrounds computed in resized(), painted behind the child
    // controls in paint() so the geometry lives in one place.
    juce::Rectangle<int> manualPanelBounds;
    juce::Rectangle<int> analyzerPanelBounds;
    juce::ComponentBoundsConstrainer resizeConstrainer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KickLockAudioProcessorEditor)
};
