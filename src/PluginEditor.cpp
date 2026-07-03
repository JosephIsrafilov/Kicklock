#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <cmath>

namespace
{
    const auto appBackground   = juce::Colour (0xff0c1014);
    const auto panelBackground = juce::Colour (0xff141a20);
    const auto panelBorder     = juce::Colour (0xff27313a);
    const auto accentTeal      = juce::Colour (0xff2dd4bf);
    const auto accentOrange    = juce::Colour (0xfff97316);
    const auto softText        = juce::Colour (0xff93a1ad);
    const auto strongText      = juce::Colour (0xffeef2f7);

    void styleCaptionLabel (juce::Label& label, const juce::String& text)
    {
        label.setText (text, juce::dontSendNotification);
        label.setFont (juce::Font (juce::FontOptions (11.0f)).boldened());
        label.setColour (juce::Label::textColourId, softText);
    }

    void styleValueLabel (juce::Label& label, float size, juce::Colour colour, bool bold = false)
    {
        auto font = juce::Font (juce::FontOptions (size));
        if (bold)
            font = font.boldened();

        label.setFont (font);
        label.setColour (juce::Label::textColourId, colour);
        label.setJustificationType (juce::Justification::centredLeft);
    }

    juce::String qualityLabel (PhaseFixQuality quality)
    {
        switch (quality)
        {
            case PhaseFixQuality::StrongImprovement:   return "Strong Fix";
            case PhaseFixQuality::PartialImprovement:  return "Partial Fix";
            case PhaseFixQuality::AlreadyGood:         return "Already Good";
            case PhaseFixQuality::LargeTimingOffset:   return "Large Timing Offset";
            case PhaseFixQuality::TimelineMoveRequired:return "Timeline Move";
            case PhaseFixQuality::Unstable:            return "Unstable";
            case PhaseFixQuality::NotEnoughSignal:     return "Waiting For Signal";
            case PhaseFixQuality::NoUsefulChange:      return "No Useful Fix";
        }

        return "No Useful Fix";
    }

    bool hasApplyAction (const PhaseFixResult& result)
    {
        return result.applyAllowed || result.optionalApplyAllowed;
    }
}

KickLockAudioProcessorEditor::KickLockAudioProcessorEditor (KickLockAudioProcessor& p)
    : AudioProcessorEditor (&p),
      audioProcessor (p),
      oscilloscope (p.scopeFifo),
      correlationDisplay (p.correlationPercent)
{
    setLookAndFeel (&lookAndFeel);

    addAndMakeVisible (oscilloscope);
    addAndMakeVisible (correlationDisplay);

    analysisHeaderLabel.setText ("ANALYZER", juce::dontSendNotification);
    analysisHeaderLabel.setFont (juce::Font (juce::FontOptions (12.0f)).boldened());
    analysisHeaderLabel.setColour (juce::Label::textColourId, softText);
    addAndMakeVisible (analysisHeaderLabel);

    styleValueLabel (processingStatusLabel, 14.0f, accentTeal, true);
    styleValueLabel (processingDetailLabel, 12.0f, strongText);
    styleValueLabel (analyzeResultLabel, 13.0f, strongText, true);
    styleValueLabel (fixActionLabel, 12.0f, strongText);
    styleValueLabel (fixStatsLabel, 12.0f, strongText);
    styleValueLabel (fixConfidenceLabel, 12.0f, strongText);
    styleValueLabel (fixSettingsLabel, 12.0f, strongText);
    styleValueLabel (fixTimelineLabel, 12.0f, strongText);
    styleValueLabel (sidechainStatusLabel, 13.0f, accentTeal, true);
    styleValueLabel (bpmValueLabel, 13.0f, strongText, true);

    // Multi-line read-outs anchor at the top-left so their wrapped lines stack
    // cleanly instead of centering oddly inside a tall box.
    for (auto* label : { &analyzeResultLabel, &fixStatsLabel, &fixSettingsLabel, &fixTimelineLabel })
        label->setJustificationType (juce::Justification::topLeft);

    for (auto* label : { &processingStatusLabel, &processingDetailLabel, &analyzeResultLabel,
                         &fixActionLabel, &fixStatsLabel, &fixConfidenceLabel,
                         &fixSettingsLabel, &fixTimelineLabel, &sidechainStatusLabel,
                         &bpmValueLabel })
        addAndMakeVisible (*label);

    styleCaptionLabel (gridDivisionLabel, "GRID");
    styleCaptionLabel (viewModeLabel, "VIEW");
    styleCaptionLabel (visualOffsetLabel, "PDC");
    styleCaptionLabel (delayMsLabel, "DELAY");
    styleCaptionLabel (delayInterpLabel, "INTERP");
    styleCaptionLabel (phaseSectionLabel, "PHASE");
    styleCaptionLabel (rotatorFreqLabel, "FREQ");
    styleCaptionLabel (rotatorQLabel, "Q");
    styleCaptionLabel (rotatorStagesLabel, "STAGES");
    styleCaptionLabel (timeZoomLabel, "TIME");
    styleCaptionLabel (ampZoomLabel, "AMP");
    styleCaptionLabel (bpmLabel, "BPM");

    for (auto* label : { &gridDivisionLabel, &viewModeLabel, &visualOffsetLabel, &delayMsLabel,
                         &phaseSectionLabel,
                         &delayInterpLabel, &rotatorFreqLabel, &rotatorQLabel,
                         &rotatorStagesLabel, &timeZoomLabel, &ampZoomLabel, &bpmLabel })
        addAndMakeVisible (*label);

    freezeButton.setButtonText ("Freeze");
    freezeButton.setClickingTogglesState (true);
    freezeButton.setColour (juce::TextButton::buttonOnColourId, accentTeal);
    freezeButton.onClick = [this] { oscilloscope.setFrozen (freezeButton.getToggleState()); };
    addAndMakeVisible (freezeButton);

    analyzeButton.setButtonText ("Analyze");
    analyzeButton.setColour (juce::TextButton::buttonColourId, accentOrange);
    analyzeButton.setTooltip ("Measure kick against bass and prepare a safe bass-path fix.");
    analyzeButton.onClick = [this]
    {
        // Non-blocking: kicks off analysis on a background worker. The UI timer
        // polls getAnalyzeState() and picks up the result when it's ready.
        if (audioProcessor.beginBackgroundAnalyze())
        {
            hasFixReady = false;
            analyzeButton.setButtonText ("Analyzing...");
            analyzeButton.setEnabled (false);
            applyFixButton.setEnabled (false);
            analyzeResultLabel.setText ("Analyzing current window...", juce::dontSendNotification);
            updateStatusLabels();
        }
    };
    addAndMakeVisible (analyzeButton);

    applyFixButton.setButtonText ("Apply Fix");
    applyFixButton.setColour (juce::TextButton::buttonColourId, accentTeal);
    applyFixButton.setTooltip ("Apply the latest safe bass correction. This never moves the kick reference.");
    applyFixButton.setEnabled (false);
    applyFixButton.onClick = [this]
    {
        if (audioProcessor.applyLatestFix())
        {
            hasFixReady = false;
            const auto latest = audioProcessor.getLatestFixResult();
            analyzeResultLabel.setText (
                latest.verificationWarning
                    ? "Applied fix, but verified match is lower than predicted. Re-analyze or adjust manually."
                    : latest.quality == PhaseFixQuality::AlreadyGood && latest.phaseFilterEnabled
                    ? "Applied optional phase refinement to the bass path."
                    : latest.quality == PhaseFixQuality::PartialImprovement
                        ? "Applied partial fix to the bass path. Listen and decide."
                        : "Applied fix to the bass path.",
                juce::dontSendNotification);
        }
        else
        {
            analyzeResultLabel.setText ("No bass-path fix or refinement is ready. Run Analyze again.",
                                        juce::dontSendNotification);
        }

        updateStatusLabels();
    };
    addAndMakeVisible (applyFixButton);

    gridDivisionBox.addItem ("1/4", 1);
    gridDivisionBox.addItem ("1/8", 2);
    gridDivisionBox.addItem ("1/16", 3);
    gridDivisionBox.addItem ("1/32", 4);
    gridDivisionBox.addItem ("Bar", 5);
    gridDivisionBox.addItem ("ms", 6);
    gridDivisionBox.setTooltip ("Beat-synced grid when tempo is available, or a fixed millisecond grid as fallback.");
    gridDivisionBox.onChange = [this] { syncScopeSettings(); };
    addAndMakeVisible (gridDivisionBox);

    viewModeBox.addItem ("Phase Delta", 1);
    viewModeBox.addItem ("Overlay", 2);
    viewModeBox.addItem ("Separate", 3);
    viewModeBox.setTooltip ("Choose the scope display style.");
    viewModeBox.onChange = [this] { syncScopeSettings(); };
    addAndMakeVisible (viewModeBox);

    visualOffsetSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    visualOffsetSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 68, 22);
    visualOffsetSlider.setNumDecimalPlacesToDisplay (0);
    visualOffsetSlider.setTextValueSuffix (" smp");
    visualOffsetSlider.setTooltip ("Visual waveform offset for compensating DAW/plugin latency. Does not process audio.");
    visualOffsetSlider.onValueChange = [this] { syncScopeSettings(); };
    addAndMakeVisible (visualOffsetSlider);

    delayMsSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    delayMsSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 68, 22);
    delayMsSlider.setNumDecimalPlacesToDisplay (2);
    delayMsSlider.setTextValueSuffix (" ms");
    delayMsSlider.setTooltip ("Bass-path delay only. Kick movement remains a DAW-side recommendation.");
    delayMsSlider.textFromValueFunction = [] (double value)
    {
        return formatBassDelayMs ((float) value);
    };
    delayMsSlider.valueFromTextFunction = [] (const juce::String& text)
    {
        auto cleaned = text.upToFirstOccurrenceOf ("ms", false, false).trim();
        return (double) normaliseBassDelayMs ((float) cleaned.getFloatValue());
    };
    addAndMakeVisible (delayMsSlider);

    delayInterpBox.addItem ("Linear", 1);
    delayInterpBox.addItem ("Allpass", 2);
    delayInterpBox.setTooltip ("Interpolation used by the bass delay line.");
    addAndMakeVisible (delayInterpBox);

    polarityInvertButton.setButtonText ("Invert Polarity");
    polarityInvertButton.setTooltip ("Flip the bass polarity by 180 degrees.");
    addAndMakeVisible (polarityInvertButton);

    phaseFilterEnabledButton.setButtonText ("Enable");
    phaseFilterEnabledButton.setTooltip ("Enables the bass phase rotator. When off, Hz/Q/stages do not affect audio.");
    addAndMakeVisible (phaseFilterEnabledButton);

    rotatorFreqSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    rotatorFreqSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 68, 22);
    rotatorFreqSlider.setTextValueSuffix (" Hz");
    rotatorFreqSlider.setNumDecimalPlacesToDisplay (0);
    rotatorFreqSlider.setTooltip ("Phase rotator frequency. Only affects audio when Phase Filter is enabled.");
    addAndMakeVisible (rotatorFreqSlider);

    rotatorQSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    rotatorQSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 22);
    rotatorQSlider.setNumDecimalPlacesToDisplay (2);
    rotatorQSlider.setTooltip ("Phase rotator Q. Only affects audio when Phase Filter is enabled.");
    addAndMakeVisible (rotatorQSlider);

    rotatorStagesBox.addItem ("2", 1);
    rotatorStagesBox.addItem ("3", 2);
    rotatorStagesBox.addItem ("4", 3);
    rotatorStagesBox.setTooltip ("Phase rotator stage count. Only affects audio when Phase Filter is enabled.");
    addAndMakeVisible (rotatorStagesBox);

    timeZoomSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    timeZoomSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 46, 20);
    timeZoomSlider.setRange (1.0, 16.0, 0.1);
    timeZoomSlider.setValue (1.0, juce::dontSendNotification);
    timeZoomSlider.setTooltip ("Horizontal zoom for the scope. Mouse wheel also works over the scope.");
    timeZoomSlider.onValueChange = [this] { oscilloscope.setTimeZoom ((float) timeZoomSlider.getValue()); };
    addAndMakeVisible (timeZoomSlider);

    ampZoomSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    ampZoomSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 46, 20);
    ampZoomSlider.setRange (1.0, 8.0, 0.1);
    ampZoomSlider.setValue (1.0, juce::dontSendNotification);
    ampZoomSlider.setTooltip ("Vertical scope zoom. Shift+wheel also works over the scope.");
    ampZoomSlider.onValueChange = [this] { oscilloscope.setAmpZoom ((float) ampZoomSlider.getValue()); };
    addAndMakeVisible (ampZoomSlider);

    oscilloscope.onZoomChanged = [this]
    {
        timeZoomSlider.setValue (oscilloscope.getTimeZoom(), juce::dontSendNotification);
        ampZoomSlider.setValue (oscilloscope.getAmpZoom(), juce::dontSendNotification);
    };

    delayMsAttachment = std::make_unique<SliderAttachment> (audioProcessor.apvts, "delayMs", delayMsSlider);
    delayInterpAttachment = std::make_unique<ComboBoxAttachment> (audioProcessor.apvts, "delayInterp", delayInterpBox);
    gridDivisionAttachment = std::make_unique<ComboBoxAttachment> (audioProcessor.apvts, "gridDivision", gridDivisionBox);
    viewModeAttachment = std::make_unique<ComboBoxAttachment> (audioProcessor.apvts, "scopeViewMode", viewModeBox);
    visualOffsetAttachment = std::make_unique<SliderAttachment> (audioProcessor.apvts, "visualOffsetSamples", visualOffsetSlider);
    polarityInvertAttachment = std::make_unique<ButtonAttachment> (audioProcessor.apvts, "polarityInvert", polarityInvertButton);
    phaseFilterEnabledAttachment = std::make_unique<ButtonAttachment> (audioProcessor.apvts, "phaseFilterEnabled", phaseFilterEnabledButton);
    rotatorFreqAttachment = std::make_unique<SliderAttachment> (audioProcessor.apvts, "rotatorFreq", rotatorFreqSlider);
    rotatorQAttachment = std::make_unique<SliderAttachment> (audioProcessor.apvts, "rotatorQ", rotatorQSlider);
    rotatorStagesAttachment = std::make_unique<ComboBoxAttachment> (audioProcessor.apvts, "rotatorStages", rotatorStagesBox);

    setSize (1120, 720);
    syncScopeSettings();
    updateStatusLabels();
    startTimerHz (12);
}

KickLockAudioProcessorEditor::~KickLockAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void KickLockAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (appBackground);

    auto bounds = getLocalBounds().reduced (16);
    const auto topBar = bounds.removeFromTop (112).toFloat();
    bounds.removeFromTop (12);
    auto content = bounds.removeFromTop (430);
    const auto sidePanel = content.removeFromRight (300).toFloat();
    content.removeFromRight (12);
    const auto scopePanel = content.toFloat();
    bounds.removeFromTop (12);
    const auto footerPanel = bounds.removeFromTop (96).toFloat();

    for (auto panel : { topBar, scopePanel, sidePanel, footerPanel })
    {
        g.setColour (panelBackground);
        g.fillRoundedRectangle (panel, 14.0f);
        g.setColour (panelBorder);
        g.drawRoundedRectangle (panel, 14.0f, 1.2f);
    }

    g.setColour (accentTeal);
    g.setFont (juce::Font (juce::FontOptions (28.0f)).boldened());
    g.drawText ("KICKLOCK",
                juce::Rectangle<int> ((int) topBar.getX() + 18, (int) topBar.getY() + 16, 220, 28),
                juce::Justification::centredLeft);

    g.setColour (softText);
    g.setFont (juce::Font (juce::FontOptions (12.0f)));
    g.drawText ("Phase Alignment Tool",
                juce::Rectangle<int> ((int) topBar.getX() + 20, (int) topBar.getY() + 48, 180, 16),
                juce::Justification::centredLeft);
}

void KickLockAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds().reduced (16);

    auto topBar = bounds.removeFromTop (112);
    bounds.removeFromTop (12);
    auto content = bounds.removeFromTop (430);
    auto sidePanel = content.removeFromRight (300);
    content.removeFromRight (12);
    auto scopePanel = content;
    bounds.removeFromTop (12);
    auto footerPanel = bounds.removeFromTop (96);

    auto layoutCompact = [] (juce::Rectangle<int>& row, juce::Label& label, juce::Component& control,
                             int labelWidth, int controlWidth)
    {
        label.setBounds (row.removeFromLeft (labelWidth));
        control.setBounds (row.removeFromLeft (controlWidth));
    };

    // --- Top bar: brand/status on the left, controls + action buttons right --
    auto brandArea = topBar.removeFromLeft (250);
    topBar.removeFromLeft (16);
    auto controlsArea = topBar.reduced (0, 6);

    brandArea.removeFromTop (58);
    processingDetailLabel.setBounds (brandArea.removeFromTop (18));
    sidechainStatusLabel.setBounds (brandArea.removeFromTop (18));
    processingStatusLabel.setBounds (brandArea.removeFromTop (20));

    auto row1 = controlsArea.removeFromTop (34);
    controlsArea.removeFromTop (6);
    auto row2 = controlsArea.removeFromTop (34);

    // Action buttons live at the right end of row 1 so they never get pushed
    // out of the visible area by the left-hand controls (P4: Freeze stays put).
    applyFixButton.setBounds (row1.removeFromRight (100).reduced (0, 2));
    row1.removeFromRight (8);
    analyzeButton.setBounds (row1.removeFromRight (100).reduced (0, 2));
    row1.removeFromRight (8);
    freezeButton.setBounds (row1.removeFromRight (84).reduced (0, 2));
    row1.removeFromRight (16);

    layoutCompact (row1, gridDivisionLabel, gridDivisionBox, 34, 82);
    row1.removeFromLeft (10);
    layoutCompact (row1, viewModeLabel, viewModeBox, 36, 110);
    row1.removeFromLeft (12);
    layoutCompact (row1, bpmLabel, bpmValueLabel, 28, 86);

    layoutCompact (row2, visualOffsetLabel, visualOffsetSlider, 34, 300);

    // --- Scope ---------------------------------------------------------------
    auto scopeArea = scopePanel.reduced (10);
    oscilloscope.setBounds (scopeArea);

    // --- Side panel: meter + analyzer read-out ------------------------------
    // Vertical budget is tight (406 px); the score/timeline labels render as
    // two lines each, so heights below are sized for wrapped text, not clipped.
    auto rightArea = sidePanel.reduced (12);
    correlationDisplay.setBounds (rightArea.removeFromTop (156));
    rightArea.removeFromTop (10);

    analysisHeaderLabel.setBounds (rightArea.removeFromTop (18));
    rightArea.removeFromTop (2);
    analyzeResultLabel.setBounds (rightArea.removeFromTop (38));
    fixActionLabel.setBounds (rightArea.removeFromTop (22));
    fixStatsLabel.setBounds (rightArea.removeFromTop (40));
    fixConfidenceLabel.setBounds (rightArea.removeFromTop (22));
    fixSettingsLabel.setBounds (rightArea.removeFromTop (40));
    fixTimelineLabel.setBounds (rightArea.removeFromTop (40));

    // --- Footer: two rows of bass-path controls -----------------------------
    auto footerArea = footerPanel.reduced (12, 8);
    auto footerRowA = footerArea.removeFromTop (34);
    footerArea.removeFromTop (6);
    auto footerRowB = footerArea.removeFromTop (34);

    layoutCompact (footerRowA, phaseSectionLabel, phaseFilterEnabledButton, 54, 84);
    footerRowA.removeFromLeft (12);
    layoutCompact (footerRowA, rotatorFreqLabel, rotatorFreqSlider, 36, 170);
    footerRowA.removeFromLeft (12);
    layoutCompact (footerRowA, rotatorQLabel, rotatorQSlider, 18, 150);
    footerRowA.removeFromLeft (12);
    layoutCompact (footerRowA, rotatorStagesLabel, rotatorStagesBox, 52, 64);
    footerRowA.removeFromLeft (12);
    layoutCompact (footerRowA, delayInterpLabel, delayInterpBox, 42, 96);

    layoutCompact (footerRowB, delayMsLabel, delayMsSlider, 40, 240);
    footerRowB.removeFromLeft (12);
    polarityInvertButton.setBounds (footerRowB.removeFromLeft (128));
    footerRowB.removeFromLeft (24);
    layoutCompact (footerRowB, timeZoomLabel, timeZoomSlider, 38, 150);
    footerRowB.removeFromLeft (12);
    layoutCompact (footerRowB, ampZoomLabel, ampZoomSlider, 34, 150);
}

void KickLockAudioProcessorEditor::timerCallback()
{
    pollAnalyzeState();
    syncScopeSettings();
    updateStatusLabels();
}

void KickLockAudioProcessorEditor::pollAnalyzeState()
{
    const auto state = audioProcessor.getAnalyzeState();
    if (state == lastAnalyzeState)
        return;

    lastAnalyzeState = state;

    switch (state)
    {
        case AnalyzeState::Preparing:
        case AnalyzeState::Analyzing:
            analyzeBusy = true;
            analyzeButton.setButtonText ("Analyzing...");
            analyzeButton.setEnabled (false);
            hasFixReady = false;
            analyzeResultLabel.setText ("Analyzing the captured window...", juce::dontSendNotification);
            break;

        case AnalyzeState::ResultReady:
        {
            analyzeBusy = false;
            analyzeButton.setButtonText ("Analyze");
            analyzeButton.setEnabled (true);
            const auto fix = audioProcessor.getLatestFixResult();
            hasAnalyzedResult = true;
            hasFixReady = hasApplyAction (fix);
            analyzeResultLabel.setText (fix.message, juce::dontSendNotification);
            audioProcessor.acknowledgeAnalyzeState();
            lastAnalyzeState = AnalyzeState::Idle;
            break;
        }

        case AnalyzeState::NotEnoughMaterial:
        {
            analyzeBusy = false;
            analyzeButton.setButtonText ("Analyze");
            analyzeButton.setEnabled (true);
            const auto fix = audioProcessor.getLatestFixResult();
            hasAnalyzedResult = true;
            hasFixReady = false;
            analyzeResultLabel.setText (fix.message.isNotEmpty()
                                            ? fix.message
                                            : "Not enough kick + bass material captured yet. Keep the loop playing and analyze again.",
                                        juce::dontSendNotification);
            audioProcessor.acknowledgeAnalyzeState();
            lastAnalyzeState = AnalyzeState::Idle;
            break;
        }

        case AnalyzeState::Failed:
            analyzeBusy = false;
            analyzeButton.setButtonText ("Analyze");
            analyzeButton.setEnabled (true);
            hasFixReady = false;
            analyzeResultLabel.setText ("Analysis could not complete. Try again.", juce::dontSendNotification);
            audioProcessor.acknowledgeAnalyzeState();
            lastAnalyzeState = AnalyzeState::Idle;
            break;

        case AnalyzeState::Idle:
            analyzeBusy = false;
            analyzeButton.setButtonText ("Analyze");
            analyzeButton.setEnabled (true);
            break;
    }

    updateStatusLabels();
}

void KickLockAudioProcessorEditor::syncScopeSettings()
{
    oscilloscope.setTimebase (audioProcessor.getSampleRate(), audioProcessor.getScopeDecimationFactor());

    if (const auto* gridParam = audioProcessor.apvts.getRawParameterValue ("gridDivision"))
        oscilloscope.setGridDivision (gridDivisionFromChoiceIndex ((int) std::lround (gridParam->load())));

    oscilloscope.setTempoInfo (audioProcessor.getLatestBpm(), audioProcessor.isTempoAvailable());

    if (const auto* viewModeParam = audioProcessor.apvts.getRawParameterValue ("scopeViewMode"))
        oscilloscope.setViewMode (scopeViewModeFromChoiceIndex ((int) std::lround (viewModeParam->load())));

    if (const auto* offsetParam = audioProcessor.apvts.getRawParameterValue ("visualOffsetSamples"))
        oscilloscope.setVisualOffsetSamples ((int) std::lround (offsetParam->load()));
}

void KickLockAudioProcessorEditor::updateStatusLabels()
{
    const bool neutral = audioProcessor.isBassProcessingNeutral();
    const bool hasSidechain = audioProcessor.hasSidechainReference();
    const auto latest = audioProcessor.getLatestFixResult();

    // P3: musically-aware status driven by HELD activity, not instant RMS, so a
    // normal beat does not flicker to SIGNAL TOO LOW between kick transients.
    const auto materialStatus = classifyAnalysisMaterialStatus (hasSidechain,
                                                               audioProcessor.isKickActive(),
                                                               audioProcessor.isBassActive(),
                                                               audioProcessor.isAnalysisSignalUsable(),
                                                               audioProcessor.hasEnoughMaterialForAnalysis());
    const bool sidechainActive = analysisStatusHasLiveSidechain (materialStatus);

    const auto workflowStatus = classifyProcessingWorkflowStatus (neutral,
                                                                  hasFixReady && sidechainActive);

    const auto* delayParam = audioProcessor.apvts.getRawParameterValue ("delayMs");
    const auto* polarityParam = audioProcessor.apvts.getRawParameterValue ("polarityInvert");
    const auto* phaseParam = audioProcessor.apvts.getRawParameterValue ("phaseFilterEnabled");

    const float delayMs = delayParam != nullptr ? delayParam->load() : 0.0f;
    const bool polarity = polarityParam != nullptr && polarityParam->load() > 0.5f;
    const bool phase = phaseParam != nullptr && phaseParam->load() > 0.5f;
    const float liveMultiBand = audioProcessor.liveMultiBandMatchPercent.load();
    const float liveLowEnd = audioProcessor.liveLowEndMatchPercent.load();
    const float liveBroadband = audioProcessor.liveBroadbandMatchPercent.load();

    switch (materialStatus)
    {
        case AnalysisMaterialStatus::WaitingForSidechain:
            sidechainStatusLabel.setText ("WAITING FOR SIDECHAIN", juce::dontSendNotification);
            sidechainStatusLabel.setColour (juce::Label::textColourId, accentOrange);
            processingDetailLabel.setText ("Connect the kick reference to the sidechain input.",
                                           juce::dontSendNotification);
            break;

        case AnalysisMaterialStatus::WaitingForKick:
            sidechainStatusLabel.setText ("WAITING FOR KICK", juce::dontSendNotification);
            sidechainStatusLabel.setColour (juce::Label::textColourId, accentOrange);
            processingDetailLabel.setText ("Sidechain connected. Waiting for a kick transient.",
                                           juce::dontSendNotification);
            break;

        case AnalysisMaterialStatus::WaitingForBass:
            sidechainStatusLabel.setText ("WAITING FOR BASS", juce::dontSendNotification);
            sidechainStatusLabel.setColour (juce::Label::textColourId, accentOrange);
            processingDetailLabel.setText ("Kick detected. Waiting for bass on the main input.",
                                           juce::dontSendNotification);
            break;

        case AnalysisMaterialStatus::SignalTooLow:
            sidechainStatusLabel.setText ("SIGNAL TOO LOW", juce::dontSendNotification);
            sidechainStatusLabel.setColour (juce::Label::textColourId, accentOrange);
            processingDetailLabel.setText ("Kick or bass is too quiet for a reliable low-end phase read.",
                                           juce::dontSendNotification);
            break;

        case AnalysisMaterialStatus::CapturingMaterial:
            sidechainStatusLabel.setText ("CAPTURING MATERIAL", juce::dontSendNotification);
            sidechainStatusLabel.setColour (juce::Label::textColourId, accentTeal);
            break;

        case AnalysisMaterialStatus::ReadyToAnalyze:
            sidechainStatusLabel.setText ("READY TO ANALYZE", juce::dontSendNotification);
            sidechainStatusLabel.setColour (juce::Label::textColourId, accentTeal);
            break;
    }

    if (audioProcessor.isTempoAvailable())
        bpmValueLabel.setText (juce::String ((int) std::round (audioProcessor.getLatestBpm())) + " BPM",
                               juce::dontSendNotification);
    else
        bpmValueLabel.setText ("BPM --", juce::dontSendNotification);

    switch (workflowStatus)
    {
        case ProcessingWorkflowStatus::ProcessingBass:
        {
            processingStatusLabel.setText ("PROCESSING BASS", juce::dontSendNotification);
            processingStatusLabel.setColour (juce::Label::textColourId, accentTeal);

            if (sidechainActive)
            {
                juce::String detail;
                detail << "Delay " << formatBassDelayMs (delayMs)
                       << "  |  Polarity " << (polarity ? "Inverted" : "Normal")
                       << "  |  Phase " << (phase ? "On" : "Off");
                processingDetailLabel.setText (detail, juce::dontSendNotification);
            }
            break;
        }

        case ProcessingWorkflowStatus::FixReady:
            processingStatusLabel.setText ("FIX READY", juce::dontSendNotification);
            processingStatusLabel.setColour (juce::Label::textColourId, accentOrange);
            if (sidechainActive)
            {
                processingDetailLabel.setText (
                    latest.quality == PhaseFixQuality::PartialImprovement
                        ? "Analyze found a partial bass-path improvement. Audio stays dry until Apply Fix."
                        : latest.quality == PhaseFixQuality::AlreadyGood && latest.phaseFilterEnabled
                            ? "Analyze found an optional phase refinement. Audio stays dry until Apply Fix."
                            : "Analyze found a bass-path fix. Audio stays dry until Apply Fix.",
                    juce::dontSendNotification);
            }
            break;

        case ProcessingWorkflowStatus::MonitoringDryInput:
            processingStatusLabel.setText ("MONITORING DRY INPUT", juce::dontSendNotification);
            processingStatusLabel.setColour (juce::Label::textColourId, accentTeal);
            if (sidechainActive)
                processingDetailLabel.setText ("Bass path is dry. Scope and meter are reading the live low-end relationship.",
                                               juce::dontSendNotification);
            break;
    }

    if (! hasAnalyzedResult)
    {
        if (! analyzeBusy)
            analyzeResultLabel.setText (materialStatus == AnalysisMaterialStatus::WaitingForSidechain
                                            ? "Waiting for kick sidechain."
                                            : materialStatus == AnalysisMaterialStatus::WaitingForKick
                                                ? "Waiting for a kick transient on the sidechain."
                                                : materialStatus == AnalysisMaterialStatus::WaitingForBass
                                                    ? "Waiting for bass on the main input."
                                                    : materialStatus == AnalysisMaterialStatus::SignalTooLow
                                                        ? "Needs stronger kick and bass signal before analysis."
                                                        : "Analyze the current window to prepare a bass-path fix.",
                                        juce::dontSendNotification);
        fixActionLabel.setText (sidechainActive ? "Quality: Monitoring"
                                                : "Quality: Waiting For Signal",
                                juce::dontSendNotification);
        fixStatsLabel.setText ("Analyze Multi-Band: -- -> --\nLive Multi-Band "
                               + juce::String ((int) std::round (liveMultiBand)) + "%   |   Low-End "
                               + juce::String ((int) std::round (liveLowEnd)) + "%   |   20-2k "
                               + juce::String ((int) std::round (liveBroadband)) + "%",
                               juce::dontSendNotification);
        fixConfidenceLabel.setText ("Confidence --", juce::dontSendNotification);
        fixSettingsLabel.setText ("Bass Delay: 0.00 ms   Polarity: Normal   Phase: OFF",
                                  juce::dontSendNotification);
        fixTimelineLabel.setText ("Timeline note: Apply Fix stays disabled until Analyze finds a usable bass-path result.",
                                  juce::dontSendNotification);
    }
    else
    {
        if (analyzeResultLabel.getText().isEmpty())
            analyzeResultLabel.setText (latest.message, juce::dontSendNotification);

        fixActionLabel.setText ("Quality: " + qualityLabel (latest.quality),
                                juce::dontSendNotification);

        juce::String matches;
        matches << "Analyze Multi-Band "
                << juce::String ((int) std::round (audioProcessor.latestAnalyzedBeforePercent.load())) << "% -> "
                << juce::String ((int) std::round (audioProcessor.latestAnalyzedAfterPercent.load())) << "%";
        const auto verified = audioProcessor.latestVerifiedAfterPercent.load();
        if (verified >= 0.0f)
            matches << "   |   Verified Offline " << juce::String ((int) std::round (verified)) << "%";
        else
            matches << "   |   Verified Offline --";
        // Second line: the live meters, clearly separated from the offline scores.
        matches << "\nLive Multi-Band " << juce::String ((int) std::round (liveMultiBand)) << "%"
                << "   |   Low-End " << juce::String ((int) std::round (liveLowEnd)) << "%"
                << "   |   20-2k " << juce::String ((int) std::round (liveBroadband)) << "%";
        fixStatsLabel.setText (matches, juce::dontSendNotification);

        juce::String confidenceText;
        confidenceText << "Confidence "
                       << juce::String ((int) std::round (audioProcessor.latestFixConfidence.load())) << "%";
        if (latest.singleHitAnalysis)
            confidenceText << "   Single-hit analysis";

        // P8: predicted-after vs verified-offline divergence > 10% is a real
        // "don't trust this number" signal, so make it visible (orange + words),
        // not just a delta figure buried in grey text.
        if (latest.verificationWarning)
        {
            confidenceText << "   WARNING verified "
                           << juce::String ((int) std::round (audioProcessor.latestVerificationDeltaPercent.load()))
                           << "% off predicted";
            fixConfidenceLabel.setColour (juce::Label::textColourId, accentOrange);
        }
        else
        {
            fixConfidenceLabel.setColour (juce::Label::textColourId, strongText);
        }
        fixConfidenceLabel.setText (confidenceText,
                                    juce::dontSendNotification);

        juce::String settings;
        settings << "Bass Delay: " << formatBassDelayMs (latest.bassDelayMs)
                 << "   Polarity: " << (latest.bassPolarityInvert ? "Flip" : "Normal")
                 << "   Phase: ";
        if (latest.phaseFilterEnabled)
            settings << "ON"
                     << (latest.quality == PhaseFixQuality::AlreadyGood ? " (Optional refinement)" : "")
                     << " | " << juce::String ((int) std::round (latest.phaseFilterFreqHz)) << " Hz / Q "
                     << juce::String (latest.phaseFilterQ, 1) << " / "
                     << juce::String (latest.phaseFilterStages) << " st";
        else
            settings << "OFF";
        fixSettingsLabel.setText (settings, juce::dontSendNotification);

        juce::String timeline;
        timeline << "Timeline note: ";
        if (phase && ! latest.phaseFilterEnabled)
        {
            timeline << "New fix disables previous phase filter. ";

            if (latest.requiresTimelineMove)
                timeline << "Move kick later by " << juce::String (latest.suggestedKickMoveMs, 2) << " ms in the DAW.";
            else if (hasApplyAction (latest))
                timeline << "Apply Fix to clear it.";
            else
                timeline << "New analysis does not recommend phase filter. Turn Phase off manually or re-analyze when the signal is clearer.";
        }
        else if (latest.requiresTimelineMove)
            timeline << "Move kick later by " << juce::String (latest.suggestedKickMoveMs, 2) << " ms in the DAW.";
        else if (latest.quality == PhaseFixQuality::LargeTimingOffset)
            timeline << "Large timing offset. Move the clip/sample in the DAW timeline instead of auto-delaying bass.";
        else if (latest.quality == PhaseFixQuality::Unstable)
            timeline << "Different hits disagree. Use a shorter loop or adjust the arrangement before applying a fix.";
        else if (latest.quality == PhaseFixQuality::AlreadyGood && latest.phaseFilterEnabled)
            timeline << (hasFixReady
                            ? "Already close overall. Apply Fix only if you want the optional phase refinement."
                            : "Optional phase refinement has already been applied.");
        else if (latest.verificationWarning)
            timeline << "Prediction mismatch after apply. Re-analyze or adjust manually.";
        else if (PhaseFixEngine::canApply (latest))
            timeline << (latest.quality == PhaseFixQuality::PartialImprovement
                            ? (hasFixReady
                                ? "Apply Fix for a partial improvement, then listen and decide."
                                : "Partial fix has already been applied. Listen and decide.")
                            : (hasFixReady
                                ? "Apply Fix to change bass."
                                : "Latest fix has already been applied."));
        else if (latest.quality == PhaseFixQuality::AlreadyGood)
            timeline << "Already close.";
        else
            timeline << "No useful bass-path fix is available yet.";
        fixTimelineLabel.setText (timeline, juce::dontSendNotification);
    }

    if (latest.quality == PhaseFixQuality::PartialImprovement)
    {
        applyFixButton.setButtonText ("Apply Partial");
        applyFixButton.setTooltip ("Applies a partial improvement to the bass path. Listen and decide.");
    }
    else if (latest.quality == PhaseFixQuality::AlreadyGood && latest.phaseFilterEnabled)
    {
        applyFixButton.setButtonText ("Apply Refinement");
        applyFixButton.setTooltip ("Applies the optional phase refinement to the bass path.");
    }
    else
    {
        applyFixButton.setButtonText ("Apply Fix");
        applyFixButton.setTooltip ("Apply the latest safe bass correction. This never moves the kick reference.");
    }

    // Apply Fix availability depends on a usable analysis result plus a still-
    // connected sidechain -- deliberately NOT on the instantaneous level, so it
    // does not disable just because the current block sits between two kicks.
    applyFixButton.setEnabled (! analyzeBusy
                               && applyFixAvailable (hasSidechain, hasFixReady));

    for (auto* c : { static_cast<juce::Component*> (&rotatorFreqSlider),
                     static_cast<juce::Component*> (&rotatorFreqLabel),
                     static_cast<juce::Component*> (&rotatorQSlider),
                     static_cast<juce::Component*> (&rotatorQLabel),
                     static_cast<juce::Component*> (&rotatorStagesBox),
                     static_cast<juce::Component*> (&rotatorStagesLabel) })
    {
        c->setEnabled (phase);
    }
}
