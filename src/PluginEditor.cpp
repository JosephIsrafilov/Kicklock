#include "PluginEditor.h"

#include <cmath>

#include "ui/StatusHelpers.h"
#include "ui/UiFormatters.h"

namespace
{
    const auto background = juce::Colour (0xff101418);
    const auto panel      = juce::Colour (0xff171d23);
    const auto border     = juce::Colour (0xff2b3540);
    const auto text       = juce::Colour (0xffedf2f7);
    const auto mutedText  = juce::Colour (0xff97a5b2);
    const auto teal       = juce::Colour (0xff2dd4bf);
    const auto orange     = juce::Colour (0xfff97316);
    const auto green      = juce::Colour (0xff22c55e);
    const auto red        = juce::Colour (0xffef4444);
    const auto amber      = juce::Colour (0xfff59e0b);

    constexpr int kTopBarHeight = 62;
    constexpr int kScopeHeight   = 260;
}

KickLockAudioProcessorEditor::KickLockAudioProcessorEditor (KickLockAudioProcessor& p)
    : AudioProcessorEditor (&p),
      audioProcessor (p),
      oscilloscope (p.scopeFifo),
      correlationDisplay (p.liveMultiBandMatchPercent,
                          p.liveLowEndMatchPercent,
                          p.liveBroadbandMatchPercent)
{
    setLookAndFeel (&lookAndFeel);
    sidechainStatusColour = mutedText;

    // --- Top bar controls --------------------------------------------------
    configureCombo (gridCombo, { "1/4", "1/8", "1/16", "1/32", "Bar", "ms" });
    configureCombo (viewCombo, { "Phase Delta", "Overlay", "Separate" });

    analyzeButton.setButtonText ("Analyze");
    analyzeButton.setColour (juce::TextButton::buttonColourId, teal);
    analyzeButton.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
    analyzeButton.setEnabled (false);
    analyzeButton.onClick = [this]
    {
        if (! canStartAnalyze)
            return;

        if (audioProcessor.beginBackgroundAnalyze())
        {
            suggestedText.clear();
            haveResult = false;
            latestResult = {};
            analyzeButton.setButtonText ("Analyzing...");
        }
    };
    addAndMakeVisible (analyzeButton);

    applyFixButton.setButtonText ("Apply Fix");
    applyFixButton.setColour (juce::TextButton::buttonColourId, orange);
    applyFixButton.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
    applyFixButton.setEnabled (false);
    applyFixButton.onClick = [this] { audioProcessor.applyLatestFix(); };
    addAndMakeVisible (applyFixButton);

    // --- Centre scope + live match ----------------------------------------
    addAndMakeVisible (oscilloscope);
    addAndMakeVisible (correlationDisplay);

    // Scope overlays draw on top of the scope (added after it).
    auto configureOverlay = [this] (juce::Label& label, juce::Justification just, juce::Colour colour)
    {
        label.setJustificationType (just);
        label.setFont (juce::Font (juce::FontOptions (12.0f)).boldened());
        label.setColour (juce::Label::textColourId, colour);
        label.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (label);
    };

    configureOverlay (suggestedOverlay, juce::Justification::centredLeft, amber);
    configureOverlay (manualDelayOverlay, juce::Justification::centredRight, teal);
    configureOverlay (noSidechainOverlay, juce::Justification::centred, text);
    noSidechainOverlay.setText ("Route kick to sidechain to compare phase.",
                                juce::dontSendNotification);
    manualDelayOverlay.setText (manualDelayText, juce::dontSendNotification);

    // --- Manual alignment --------------------------------------------------
    configureSectionLabel (manualHeader, "MANUAL ALIGNMENT");

    configureRotary (delaySlider);
    delaySlider.setTextValueSuffix ({});
    delaySlider.textFromValueFunction = [] (double value)
    {
        return formatSignedDelayMs ((float) value).toStdString();
    };
    delaySlider.valueFromTextFunction = [] (const juce::String& t)
    {
        return (double) t.retainCharacters ("-0123456789.").getFloatValue();
    };
    delaySlider.setTooltip ("Moves the bass earlier or later relative to the kick. "
                            "Negative values advance bass using plugin delay compensation.");
    addAndMakeVisible (delaySlider);

    polarityInvertButton.setButtonText ("Invert Polarity");
    polarityInvertButton.setTooltip ("Flips the bass polarity by 180 degrees. "
                                     "Useful when kick and bass cancel each other.");
    addAndMakeVisible (polarityInvertButton);

    phaseFilterButton.setButtonText ("Phase Filter");
    phaseFilterButton.setTooltip ("Rotates phase around the selected frequency "
                                  "without moving the whole signal in time.");
    addAndMakeVisible (phaseFilterButton);

    configureRotary (phaseFreqSlider);
    phaseFreqSlider.setTextValueSuffix (" Hz");
    phaseFreqSlider.setNumDecimalPlacesToDisplay (0);
    phaseFreqSlider.setTooltip ("Centre frequency of the phase filter (20 Hz - 500 Hz).");
    addAndMakeVisible (phaseFreqSlider);

    configureRotary (phaseQSlider);
    phaseQSlider.setNumDecimalPlacesToDisplay (2);
    phaseQSlider.setTooltip ("Q (sharpness) of the phase filter around its centre frequency.");
    addAndMakeVisible (phaseQSlider);

    configureRotary (visualOffsetSlider);
    visualOffsetSlider.setTextValueSuffix (" smp");
    visualOffsetSlider.setNumDecimalPlacesToDisplay (0);
    visualOffsetSlider.setTooltip ("Moves only the waveform display. This does not affect audio.");
    addAndMakeVisible (visualOffsetSlider);

    configureControlLabel (delayLabel, "Delay");
    configureControlLabel (polarityLabel, "Polarity");
    configureControlLabel (phaseFilterLabel, "Phase");
    configureControlLabel (phaseFreqLabel, "Phase Freq");
    configureControlLabel (phaseQLabel, "Q");
    configureControlLabel (visualOffsetLabel, "Visual Offset");

    // --- Advanced ----------------------------------------------------------
    configureSectionLabel (advancedHeader, "ADVANCED");
    advancedHeader.setColour (juce::Label::textColourId, mutedText.withAlpha (0.7f));

    configureCombo (delayInterpCombo, { "Linear", "Allpass" });
    configureCombo (phaseStagesCombo, { "2", "3", "4" });
    configureControlLabel (delayInterpLabel, "Delay Interp");
    configureControlLabel (phaseStagesLabel, "Phase Stages");

    // --- Analyzer explanation panel ---------------------------------------
    analyzerTitle.setText ("ANALYZER", juce::dontSendNotification);
    analyzerTitle.setJustificationType (juce::Justification::centredLeft);
    analyzerTitle.setFont (juce::Font (juce::FontOptions (12.0f)).boldened());
    analyzerTitle.setColour (juce::Label::textColourId, mutedText);
    addAndMakeVisible (analyzerTitle);

    analyzerBody.setJustificationType (juce::Justification::topLeft);
    analyzerBody.setFont (juce::Font (juce::FontOptions (12.5f)));
    analyzerBody.setColour (juce::Label::textColourId, text);
    analyzerBody.setMinimumHorizontalScale (1.0f);
    analyzerBody.setText ("Press Analyze while the loop plays. KickLock will "
                          "recommend a delay, polarity and phase setting and "
                          "explain what it found.", juce::dontSendNotification);
    addAndMakeVisible (analyzerBody);

    // --- Parameter attachments --------------------------------------------
    auto& apvts = audioProcessor.apvts;
    delayAttachment        = std::make_unique<SliderAttachment> (apvts, "delay_ms", delaySlider);
    polarityInvertAttachment = std::make_unique<ButtonAttachment> (apvts, "polarity_invert", polarityInvertButton);
    phaseFilterAttachment  = std::make_unique<ButtonAttachment> (apvts, "allpass_enable", phaseFilterButton);
    phaseFreqAttachment    = std::make_unique<SliderAttachment> (apvts, "allpass_freq", phaseFreqSlider);
    phaseQAttachment       = std::make_unique<SliderAttachment> (apvts, "rotatorQ", phaseQSlider);
    visualOffsetAttachment = std::make_unique<SliderAttachment> (apvts, "visualOffsetSamples", visualOffsetSlider);
    gridAttachment         = std::make_unique<ComboAttachment> (apvts, "gridDivision", gridCombo);
    viewAttachment         = std::make_unique<ComboAttachment> (apvts, "scopeViewMode", viewCombo);
    delayInterpAttachment  = std::make_unique<ComboAttachment> (apvts, "delayInterp", delayInterpCombo);
    phaseStagesAttachment  = std::make_unique<ComboAttachment> (apvts, "rotatorStages", phaseStagesCombo);

    oscilloscope.setTimebase (audioProcessor.getSampleRate(),
                              audioProcessor.getScopeDecimationFactor());
    pushScopeSettings();

    setSize (1000, 680);
    startTimerHz (30);
}

KickLockAudioProcessorEditor::~KickLockAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void KickLockAudioProcessorEditor::configureRotary (juce::Slider& slider)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 84, 20);
    slider.setColour (juce::Slider::rotarySliderFillColourId, teal);
    slider.setColour (juce::Slider::rotarySliderOutlineColourId, border);
    slider.setColour (juce::Slider::thumbColourId, text);
    slider.setColour (juce::Slider::textBoxTextColourId, text);
    slider.setColour (juce::Slider::textBoxOutlineColourId, border);
}

void KickLockAudioProcessorEditor::configureSectionLabel (juce::Label& label, const juce::String& labelText)
{
    label.setText (labelText, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centredLeft);
    label.setFont (juce::Font (juce::FontOptions (13.0f)).boldened());
    label.setColour (juce::Label::textColourId, teal);
    addAndMakeVisible (label);
}

void KickLockAudioProcessorEditor::configureControlLabel (juce::Label& label, const juce::String& labelText)
{
    label.setText (labelText, juce::dontSendNotification);
    label.setJustificationType (juce::Justification::centred);
    label.setFont (juce::Font (juce::FontOptions (11.5f)).boldened());
    label.setColour (juce::Label::textColourId, mutedText);
    addAndMakeVisible (label);
}

void KickLockAudioProcessorEditor::configureCombo (juce::ComboBox& combo, const juce::StringArray& items)
{
    combo.addItemList (items, 1);
    combo.setColour (juce::ComboBox::backgroundColourId, panel);
    combo.setColour (juce::ComboBox::outlineColourId, border);
    combo.setColour (juce::ComboBox::textColourId, text);
    combo.setColour (juce::ComboBox::arrowColourId, orange);
    addAndMakeVisible (combo);
}

void KickLockAudioProcessorEditor::pushScopeSettings()
{
    oscilloscope.setViewMode (scopeViewModeFromChoiceIndex (viewCombo.getSelectedItemIndex()));
    oscilloscope.setGridDivision (gridDivisionFromChoiceIndex (gridCombo.getSelectedItemIndex()));

    if (const auto* offset = audioProcessor.apvts.getRawParameterValue ("visualOffsetSamples"))
        oscilloscope.setVisualOffsetSamples ((int) std::lround (offset->load()));
}

void KickLockAudioProcessorEditor::refreshStatusStrings()
{
    hasSidechain = audioProcessor.hasSidechainReference();

    const auto status = classifyAnalysisMaterialStatus (hasSidechain,
                                                        audioProcessor.isKickActive(),
                                                        audioProcessor.isBassActive(),
                                                        audioProcessor.isAnalysisSignalUsable(),
                                                        audioProcessor.hasEnoughMaterialForAnalysis());
    canStartAnalyze = analysisStatusCanStartAnalyze (status);

    switch (status)
    {
        case AnalysisMaterialStatus::WaitingForSidechain:
            sidechainStatusText = "NO SIDECHAIN"; sidechainStatusColour = mutedText; break;
        case AnalysisMaterialStatus::WaitingForKick:
            sidechainStatusText = "WAITING FOR KICK"; sidechainStatusColour = amber; break;
        case AnalysisMaterialStatus::WaitingForBass:
            sidechainStatusText = "WAITING FOR BASS"; sidechainStatusColour = amber; break;
        case AnalysisMaterialStatus::SignalTooLow:
            sidechainStatusText = "SIGNAL TOO LOW"; sidechainStatusColour = amber; break;
        case AnalysisMaterialStatus::CapturingMaterial:
            sidechainStatusText = "SIDECHAIN ACTIVE"; sidechainStatusColour = teal; break;
        case AnalysisMaterialStatus::ReadyToAnalyze:
            sidechainStatusText = "SIDECHAIN ACTIVE"; sidechainStatusColour = green; break;
    }

    if (audioProcessor.isTempoAvailable())
        bpmText = juce::String (audioProcessor.getLatestBpm(), 1) + " BPM";
    else
        bpmText = "-- BPM";

    const double sr = audioProcessor.getSampleRate();
    const int latencySamples = audioProcessor.getLatencySamples();
    const float latencyMs = sr > 0.0 ? (float) (latencySamples * 1000.0 / sr) : 0.0f;
    pdcText = "PDC " + juce::String (latencySamples) + " smp / "
            + juce::String (latencyMs, 2) + " ms";

    const float delayMs = audioProcessor.apvts.getRawParameterValue ("delay_ms")->load();
    manualDelayText = "Manual Delay: " + formatSignedDelayMs (delayMs);
    manualDelayOverlay.setText (manualDelayText, juce::dontSendNotification);

    noSidechainOverlay.setVisible (! hasSidechain);
}

void KickLockAudioProcessorEditor::refreshAnalyzeWorkflow()
{
    const auto state = audioProcessor.getAnalyzeState();
    const bool busy = analyzeStateIsBusy (state);

    analyzeButton.setButtonText (analyzeStateButtonText (state));
    analyzeButton.setEnabled (! busy && canStartAnalyze);

    if (busy)
    {
        suggestedText.clear();
        haveResult = false;

        if (state != lastAnalyzeState)
            analyzerBody.setText ("Analyzing captured kick and bass...", juce::dontSendNotification);
    }

    if (state != lastAnalyzeState && analyzeStateIsResolved (state))
    {
        latestResult = audioProcessor.getLatestFixResult();
        const bool resultCanApply = latestResult.applyAllowed || latestResult.optionalApplyAllowed;
        haveResult = state == AnalyzeState::ResultReady && resultCanApply;

        if (state == AnalyzeState::ResultReady)
        {
            suggestedText = resultCanApply ? "Suggested: " + formatSignedDelayMs (latestResult.bassDelayMs)
                                           : juce::String();

            juce::String body;
            body << latestResult.message;

            if (resultCanApply)
            {
                body << "\n\nRecommended Delay: " << formatSignedDelayMs (latestResult.bassDelayMs) << "\n"
                     << "Polarity: " << (latestResult.bassPolarityInvert ? "Invert" : "Normal") << "\n";

                if (latestResult.phaseFilterEnabled)
                    body << "Phase Filter: " << juce::String ((int) std::round (latestResult.phaseFilterFreqHz))
                         << " Hz, Q " << juce::String (latestResult.phaseFilterQ, 2) << "\n";
                else
                    body << "Phase Filter: off\n";
            }
            else
            {
                if (latestResult.largeTimingOffset)
                    body << "\n\nDetected offset: " << juce::String (latestResult.detectedTimingOffsetMs, 1)
                         << " ms. Apply Fix is disabled; move the sample/clip in your DAW timeline.";
                else if (latestResult.requiresTimelineMove)
                    body << "\n\nApply Fix is disabled because this correction needs a DAW timeline move.";
                else if (latestResult.unstableRecommendation)
                    body << "\n\nApply Fix is disabled because the captured hits disagree.";
                else if (latestResult.quality == PhaseFixQuality::AlreadyGood)
                    body << "\n\nNo applicable correction is needed.";
                else
                    body << "\n\nNo applicable bass-path change was found.";

                body << "\n";
            }

            body << "Confidence: " << juce::String ((int) std::round (latestResult.confidence * 100.0f)) << "%\n"
                 << "Low-end match: " << juce::String ((int) std::round (latestResult.beforeMatchPercent))
                 << "% -> " << juce::String ((int) std::round (latestResult.predictedAfterMatchPercent)) << "%";

            if (resultCanApply && (latestResult.largeTimingOffset || latestResult.requiresTimelineMove))
                body << "\n\nWarning: Large timing offset detected. Manual DAW movement may sound more natural.";
            else if (resultCanApply && latestResult.unstableRecommendation)
                body << "\n\nWarning: Different hits need different corrections; result is unstable.";

            analyzerBody.setText (body, juce::dontSendNotification);
        }
        else
        {
            suggestedText.clear();
            analyzerBody.setText (latestResult.message.isNotEmpty()
                                      ? latestResult.message
                                      : "Analyze did not find enough usable kick and bass material.",
                                  juce::dontSendNotification);
        }

        audioProcessor.acknowledgeAnalyzeState();
    }

    lastAnalyzeState = state;

    suggestedOverlay.setText (suggestedText, juce::dontSendNotification);

    const bool canApply = applyFixAvailable (hasSidechain, haveResult);
    applyFixButton.setEnabled (canApply);
}

void KickLockAudioProcessorEditor::timerCallback()
{
    oscilloscope.setTimebase (audioProcessor.getSampleRate(),
                              audioProcessor.getScopeDecimationFactor());
    oscilloscope.setTempoInfo (audioProcessor.getLatestBpm(),
                               audioProcessor.isTempoAvailable());
    pushScopeSettings();

    refreshStatusStrings();
    refreshAnalyzeWorkflow();

    repaint();
}

void KickLockAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);

    auto bounds = getLocalBounds();

    // --- Top bar -----------------------------------------------------------
    auto topBar = bounds.removeFromTop (kTopBarHeight);
    g.setColour (panel);
    g.fillRect (topBar);
    g.setColour (border);
    g.drawLine ((float) topBar.getX(), (float) topBar.getBottom(),
                (float) topBar.getRight(), (float) topBar.getBottom(), 1.0f);

    auto barInner = topBar.reduced (14, 8);

    g.setColour (text);
    g.setFont (juce::Font (juce::FontOptions (22.0f)).boldened());
    g.drawText ("KICKLOCK", barInner.removeFromLeft (150),
                juce::Justification::centredLeft);

    g.setColour (mutedText);
    g.setFont (juce::Font (juce::FontOptions (10.5f)));
    g.drawText ("v0.2-dev Visual Manual Build",
                juce::Rectangle<int> (barInner.getX(), topBar.getBottom() - 20, 200, 14),
                juce::Justification::centredLeft);

    // Status block: sidechain state (top), BPM + PDC (bottom).
    auto statusArea = barInner.removeFromLeft (240);
    g.setColour (sidechainStatusColour);
    g.setFont (juce::Font (juce::FontOptions (14.0f)).boldened());
    g.drawText (sidechainStatusText, statusArea.removeFromTop (statusArea.getHeight() / 2),
                juce::Justification::centredLeft);
    g.setColour (mutedText);
    g.setFont (juce::Font (juce::FontOptions (11.0f)));
    g.drawText (bpmText + "   |   " + pdcText, statusArea,
                juce::Justification::centredLeft);

    // --- Lower panel backgrounds (geometry set in resized) -----------------
    auto drawPanel = [&g] (juce::Rectangle<int> r)
    {
        if (r.isEmpty())
            return;
        g.setColour (panel);
        g.fillRoundedRectangle (r.toFloat(), 8.0f);
        g.setColour (border);
        g.drawRoundedRectangle (r.toFloat(), 8.0f, 1.0f);
    };

    drawPanel (manualPanelBounds);
    drawPanel (analyzerPanelBounds);
}

void KickLockAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds();

    // --- Top bar (right side: grid, view, analyze, apply) ------------------
    auto topBar = bounds.removeFromTop (kTopBarHeight).reduced (14, 12);

    applyFixButton.setBounds (topBar.removeFromRight (96).reduced (0, 2));
    topBar.removeFromRight (8);
    analyzeButton.setBounds (topBar.removeFromRight (96).reduced (0, 2));
    topBar.removeFromRight (16);
    viewCombo.setBounds (topBar.removeFromRight (110).reduced (0, 4));
    topBar.removeFromRight (8);
    gridCombo.setBounds (topBar.removeFromRight (80).reduced (0, 4));

    bounds.reduce (14, 12);

    // --- Centre scope + overlays ------------------------------------------
    auto scopeArea = bounds.removeFromTop (kScopeHeight);
    oscilloscope.setBounds (scopeArea);

    noSidechainOverlay.setBounds (scopeArea.withSizeKeepingCentre (scopeArea.getWidth(), 24));

    auto markerRow = scopeArea.removeFromBottom (18);
    suggestedOverlay.setBounds (markerRow.removeFromLeft (markerRow.getWidth() / 2).reduced (6, 0));
    manualDelayOverlay.setBounds (markerRow.reduced (6, 0));

    bounds.removeFromTop (10);

    // --- Lower area: manual controls (left) + analyzer/live (right) -------
    auto lower = bounds;
    auto right = lower.removeFromRight (300);
    lower.removeFromRight (12);
    auto manualArea = lower;

    manualPanelBounds = manualArea;
    analyzerPanelBounds = right;
    manualArea = manualArea.reduced (14, 12);
    right = right.reduced (12, 12);

    // Manual alignment panel.
    manualHeader.setBounds (manualArea.removeFromTop (20));
    manualArea.removeFromTop (4);

    auto row1 = manualArea.removeFromTop (118);
    const int knobW = 110;

    auto delayCell = row1.removeFromLeft (knobW);
    delayLabel.setBounds (delayCell.removeFromTop (16));
    delaySlider.setBounds (delayCell);

    row1.removeFromLeft (8);
    auto freqCell = row1.removeFromLeft (knobW);
    phaseFreqLabel.setBounds (freqCell.removeFromTop (16));
    phaseFreqSlider.setBounds (freqCell);

    row1.removeFromLeft (8);
    auto qCell = row1.removeFromLeft (knobW);
    phaseQLabel.setBounds (qCell.removeFromTop (16));
    phaseQSlider.setBounds (qCell);

    row1.removeFromLeft (8);
    auto visualCell = row1.removeFromLeft (knobW);
    visualOffsetLabel.setBounds (visualCell.removeFromTop (16));
    visualOffsetSlider.setBounds (visualCell);

    manualArea.removeFromTop (8);
    auto row2 = manualArea.removeFromTop (56);
    auto polCell = row2.removeFromLeft (knobW * 2 + 8);
    polarityLabel.setBounds (polCell.removeFromTop (16));
    polarityInvertButton.setBounds (polCell.removeFromTop (28));

    row2.removeFromLeft (8);
    auto phCell = row2.removeFromLeft (knobW * 2);
    phaseFilterLabel.setBounds (phCell.removeFromTop (16));
    phaseFilterButton.setBounds (phCell.removeFromTop (28));

    manualArea.removeFromTop (10);
    advancedHeader.setBounds (manualArea.removeFromTop (18));
    manualArea.removeFromTop (2);
    auto advRow = manualArea.removeFromTop (44);
    auto interpCell = advRow.removeFromLeft (knobW * 2 + 8);
    delayInterpLabel.setBounds (interpCell.removeFromTop (16));
    delayInterpCombo.setBounds (interpCell.removeFromTop (24).reduced (0, 2));
    advRow.removeFromLeft (8);
    auto stagesCell = advRow.removeFromLeft (knobW * 2);
    phaseStagesLabel.setBounds (stagesCell.removeFromTop (16));
    phaseStagesCombo.setBounds (stagesCell.removeFromTop (24).reduced (0, 2));

    // Right column: live match on top, analyzer explanation below.
    correlationDisplay.setBounds (right.removeFromTop (150));
    right.removeFromTop (10);
    analyzerTitle.setBounds (right.removeFromTop (20));
    analyzerBody.setBounds (right);
}
