#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include "ui/LookAndFeel.h"
#include "ui/Oscilloscope.h"
#include "ui/SpectrumAnalyzer.h"
#include "ui/CorrelationDisplay.h"
#include "ui/StatusHelpers.h"
#include "ui/DynamicUiHelpers.h"
#include "ui/DynamicWorkspace.h"
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
        if (punchDb != punchDbIn || valid != validIn || hasSidechain != hasSidechainIn ||
            hasReference != hasReferenceIn || referenceDb != referenceDbIn ||
            kickPeak != kickPeakIn || sumPeak != sumPeakIn)
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

class HorizontalSplitter : public juce::Component
{
public:
    HorizontalSplitter (std::function<void(int)> onDragCallback)
        : onDrag (std::move (onDragCallback))
    {
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
    }

    void mouseDown (const juce::MouseEvent&) override { dragStartHeight = currentBottomHeight; }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        const int deltaY = e.getDistanceFromDragStartY();
        // Negative deltaY means moving UP, which should INCREASE the bottom panel height
        onDrag (dragStartHeight - deltaY);
    }

    void setBottomHeightBase (int height) { currentBottomHeight = height; }

    void paint (juce::Graphics& g) override
    {
        // Simple drag handle visual
        g.setColour (juce::Colour (0xff32363e));
        g.fillAll();
        g.setColour (juce::Colours::white.withAlpha (0.15f));
        const float midY = (float) getHeight() / 2.0f;
        g.drawHorizontalLine ((int) midY - 1, (float) getWidth() / 2.0f - 15.0f, (float) getWidth() / 2.0f + 15.0f);
        g.drawHorizontalLine ((int) midY + 1, (float) getWidth() / 2.0f - 15.0f, (float) getWidth() / 2.0f + 15.0f);
    }

private:
    std::function<void(int)> onDrag;
    int dragStartHeight = 0;
    int currentBottomHeight = 0;
};

class SegmentedModeSelector : public juce::Component,
                              private juce::ComboBox::Listener
{
public:
    SegmentedModeSelector();
    ~SegmentedModeSelector() override;

    juce::ComboBox& parameterCombo() noexcept { return parameterChoice; }
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void comboBoxChanged (juce::ComboBox*) override;
    void syncButtons();

    juce::TextButton staticButton { "Static" };
    juce::TextButton dynamicButton { "Dynamic" };
    juce::ComboBox parameterChoice;
};

class LearnProgressComponent : public juce::Component
{
public:
    void setModel (const LearnProgressSnapshot&, const NotePhaseMapSnapshot&, int activeMidiNote,
                   int selectedMidiNote = -1);
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

    std::function<void (int midi)> onNoteSelected;
    int getSelectedMidi() const noexcept { return selectedMidi; }

private:
    LearnProgressSnapshot progress;
    std::array<int, NotePhaseMapSnapshot::size> noteCounts {};
    std::array<bool, NotePhaseMapSnapshot::size> learnedNotes {};
    std::array<juce::String, NotePhaseMapSnapshot::size> noteTexts {};
    std::array<juce::Rectangle<int>, 6> chipBounds {};
    std::array<int, 6> chipMidi {};
    int chipCount = 0;
    int activeMidi = -1;
    int selectedMidi = -1;
};

// Full visual + manual phase-alignment editor. The oscilloscope is the visual
// centre; a top status bar reports sidechain/BPM/PDC state and hosts the grid,
// view, Analyze and Apply Fix controls; a MANUAL ALIGNMENT section drives the
// signed delay, polarity, phase filter and (display-only) visual offset; and an
// analyzer panel explains what Analyze recommends. All heavy DSP lives in the
// processor — the editor only wires parameters and polls published state.
class KickLockAudioProcessorEditor : public juce::AudioProcessorEditor,
                                     private juce::Timer,
                                     private juce::AudioProcessorValueTreeState::Listener
{
public:
    explicit KickLockAudioProcessorEditor (KickLockAudioProcessor&);
    ~KickLockAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void parameterChanged (const juce::String& parameterID, float newValue) override;

    void configureRotary (juce::Slider& slider);
    void configureSectionLabel (juce::Label& label, const juce::String& text);
    void configureControlLabel (juce::Label& label, const juce::String& text);
    void configureCombo (juce::ComboBox& combo, const juce::StringArray& items);

    void refreshStatusStrings();
    void refreshAnalyzeWorkflow();
    void refreshDynamicWorkflow();
    void refreshCompareButtons();
    void pushScopeSettings();
    void updateVisualOffsetAvailability (ScopeViewMode mode);
    void setChromeVisible (bool shouldBeVisible);

    // Single edge-triggered owner for Static <-> Dynamic presentation.
    // Covers user click, host automation, preset restore, and construction.
    void handleCorrectionModeChanged (CorrectionMode newMode, bool force = false);
    CorrectionMode readCorrectionMode() const noexcept;
    void applyModeTransitionSideEffects (const ModeTransitionActions& actions);
    void applyWorkflowChromeForMode (CorrectionMode mode, bool layoutMayChange);

    KickLockAudioProcessor& audioProcessor;
    KickLockLookAndFeel lookAndFeel;
    juce::TooltipWindow tooltipWindow { this, 500 };

    // --- Top bar -----------------------------------------------------------
    juce::ComboBox gridCombo;
    juce::ComboBox viewCombo;
    SegmentedModeSelector correctionModeSelector;

    juce::TextButton freezeButton;
    juce::TextButton relockKickButton;
    juce::TextButton analyzeButton;
    juce::TextButton applyFixButton;
    juce::TextButton discardButton;
    juce::TextButton revertButton;
    juce::TextButton compareAButton;
    juce::TextButton compareBButton;
    juce::TextButton compareCopyButton;
    juce::TextButton helpButton;

    // --- Centre scope ------------------------------------------------------
    Oscilloscope oscilloscope;
    SpectrumAnalyzer spectrumAnalyzer;
    CorrelationDisplay correlationDisplay;

    // Scope overlays. Added after the scope so they draw on top of it.
    juce::Label manualDelayOverlay;
    juce::Label noSidechainOverlay;

    // --- Analyzer explanation panel ---------------------------------------
    juce::Label analyzerTitle;
    juce::Label analyzerBody;
    TransientPunchComponent transientPunch;
    LearnProgressComponent learnProgressDisplay;
    juce::TextButton setRefButton;

    // --- Manual alignment --------------------------------------------------
    juce::Label manualHeader;
    juce::Slider delaySlider;
    juce::ToggleButton polarityInvertButton;
    juce::ToggleButton phaseFilterButton;
    juce::ToggleButton pitchTrackButton;
    juce::Slider phaseFreqSlider;
    juce::Slider phaseQSlider;
    juce::Slider visualOffsetSlider;
    juce::Slider dynamicStrengthSlider;
    juce::ToggleButton crossoverEnableButton;
    juce::Slider crossoverSlider;

    juce::Label delayLabel;
    juce::Label polarityLabel;
    juce::Label phaseFilterLabel;
    juce::Label pitchTrackLabel;
    juce::Label phaseFreqLabel;
    juce::Label phaseQLabel;
    juce::Label visualOffsetLabel;
    juce::Label dynamicStrengthLabel;
    juce::Label crossoverEnableLabel;
    juce::Label crossoverLabel;
    DynamicWorkspace dynamicWorkspace;



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
    std::unique_ptr<ButtonAttachment> pitchTrackAttachment;
    std::unique_ptr<SliderAttachment> phaseFreqAttachment;
    std::unique_ptr<SliderAttachment> phaseQAttachment;
    std::unique_ptr<SliderAttachment> visualOffsetAttachment;
    std::unique_ptr<SliderAttachment> dynamicStrengthAttachment;
    std::unique_ptr<ButtonAttachment> crossoverEnableAttachment;
    std::unique_ptr<SliderAttachment> crossoverAttachment;
    std::unique_ptr<ComboAttachment>  gridAttachment;
    std::unique_ptr<ComboAttachment>  delayInterpAttachment;
    std::unique_ptr<ComboAttachment>  phaseStagesAttachment;
    std::unique_ptr<ComboAttachment>  correctionModeAttachment;

    // Cached status/analyzer strings, recomputed on the UI timer and drawn in
    // paint(). Kept as members so paint() never touches processor internals.
    juce::String sidechainStatusText { "NO SIDECHAIN" };
    juce::Colour  sidechainStatusColour;
    juce::String bpmText { "-- BPM" };
    juce::String pdcText { "PDC --" };
    juce::String manualDelayText { "Manual Delay: 0.00 ms" };
    bool hasSidechain = false;
    bool canStartAnalyze = false;
    bool polarityHintVisible = false;
    AnalysisMaterialStatus latestMaterialStatus = AnalysisMaterialStatus::WaitingForSidechain;
    bool helpOverlayVisible = false;
    bool cleanScopeMode = false;

    // Latest analyzer result snapshot for the explanation panel.
    PhaseFixResult latestResult;
    AnalyzeState lastAnalyzeState = AnalyzeState::Idle;
    bool haveResult = false;
    bool latestResultAutoApplied = false;
    bool showingLearnWorkflow = false;
    bool dynamicModeSelected = false;
    bool hasRenderedCorrectionMode = false;
    CorrectionMode renderedCorrectionMode = CorrectionMode::Static;
    uint64_t lastLearnBodySessionId = 0;
    LearnState lastLearnBodyState = LearnState::Idle;
    juce::String lastLearnBodyText;
    int lastLearnBodySelectedMidi = -1;
    juce::String lastPrimaryButtonText;
    juce::String lastApplyButtonText;
    LearnProgressSnapshot latestLearnProgress;
    NotePhaseMapSnapshot latestNoteMap;
    int selectedLearnMidi = -1;

    // Phase 12: last per-state edit-transaction outcome, surfaced to the
    // Inspector so a rejected edit explains why rather than failing silently.
    bool lastDynamicEditFailed = false;
    juce::String lastDynamicEditFailureReason;

    // Panel backgrounds computed in resized(), painted behind the child
    // controls in paint() so the geometry lives in one place.
    juce::Rectangle<int> manualPanelBounds;
    juce::Rectangle<int> analyzerPanelBounds;
    juce::ComponentBoundsConstrainer resizeConstrainer;
    
    int bottomPanelHeight = 252;
    HorizontalSplitter splitter;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KickLockAudioProcessorEditor)
};
