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
}

void TransientHealthComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour (juce::Colour (0xff101418));
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (juce::Colour (0xff2b3540));
    g.drawRoundedRectangle (bounds, 6.0f, 1.0f);

    auto inner = getLocalBounds().reduced (8, 6);
    auto title = inner.removeFromTop (16);
    g.setColour (mutedText);
    g.setFont (juce::Font (juce::FontOptions (11.0f)).boldened());
    g.drawText ("TRANSIENT HEALTH", title, juce::Justification::centredLeft);

    auto readout = title.removeFromRight (70);
    g.setColour (healthDb >= 0.0f ? green : red);
    g.drawText ((healthDb >= 0.0f ? "+" : "") + juce::String (healthDb, 1) + " dB",
                readout, juce::Justification::centredRight);

    auto bars = inner.removeFromTop (20).toFloat();
    const float preW = bars.getWidth() * juce::jlimit (0.0f, 1.0f, prePeak * 2.0f);
    const float postW = bars.getWidth() * juce::jlimit (0.0f, 1.0f, postPeak * 2.0f);

    g.setColour (orange.withAlpha (0.22f));
    g.fillRoundedRectangle (bars.withWidth (preW), 3.0f);
    g.setColour (teal.withAlpha (0.85f));
    g.fillRoundedRectangle (bars.withWidth (postW).reduced (0.0f, 4.0f), 3.0f);
}

KickLockAudioProcessorEditor::KickLockAudioProcessorEditor (KickLockAudioProcessor& p)
    : AudioProcessorEditor (&p),
      audioProcessor (p),
      oscilloscope (p.scopeFifo, p.getTriggeredHitCapture()),
      correlationDisplay (p.realtimeLowBandMatchPercent,
                          p.liveLowEndMatchPercent,
                          p.liveBroadbandMatchPercent,
                          p.liveBandMatchPercent,
                          p.latestAppliedBeforePercent)
{
    setLookAndFeel (&lookAndFeel);
    sidechainStatusColour = mutedText;

    // --- Top bar controls --------------------------------------------------
    configureCombo (gridCombo, { "1/4", "1/8", "1/16", "1/32", "Bar", "ms" });
    configureCombo (viewCombo, { "Triggered", "Free-run", "Phase Delta", "Overlay", "Separate" });
    gridCombo.setTooltip ("Sets the scope time grid.");
    viewCombo.setTooltip ("Chooses triggered or scrolling scope display.");

    freezeButton.setButtonText ("Freeze");
    freezeButton.setClickingTogglesState (true);
    freezeButton.setColour (juce::TextButton::buttonColourId, panel);
    freezeButton.setColour (juce::TextButton::textColourOffId, text);
    freezeButton.setColour (juce::TextButton::buttonOnColourId, teal);
    freezeButton.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    freezeButton.setTooltip ("Freezes the oscilloscope display so you can inspect the waveform.");
    freezeButton.onClick = [this] { oscilloscope.setFrozen (freezeButton.getToggleState()); oscilloscope.repaint(); };
    addAndMakeVisible (freezeButton);

    analyzeButton.setButtonText ("Analyze");
    analyzeButton.setColour (juce::TextButton::buttonColourId, teal);
    analyzeButton.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
    analyzeButton.setTooltip ("Captures the current kick and bass loop and recommends a bass-path correction.");
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
    applyFixButton.setTooltip ("Applies the latest analyzer correction to the bass-path controls.");
    addAndMakeVisible (applyFixButton);

    revertButton.setButtonText ("Revert");
    revertButton.setColour (juce::TextButton::buttonColourId, panel);
    revertButton.setColour (juce::TextButton::textColourOffId, text);
    revertButton.setEnabled (false);
    revertButton.onClick = [this] { audioProcessor.revertLatestFix(); };
    revertButton.setTooltip ("Restores the bass-path settings that were active before Apply Fix.");
    addAndMakeVisible (revertButton);

    compareAButton.setButtonText ("A");
    compareAButton.setTooltip ("Switches to compare slot A.");
    compareAButton.onClick = [this] { audioProcessor.selectCompareSlot (0); refreshCompareButtons(); };
    addAndMakeVisible (compareAButton);

    compareBButton.setButtonText ("B");
    compareBButton.setTooltip ("Switches to compare slot B.");
    compareBButton.onClick = [this] { audioProcessor.selectCompareSlot (1); refreshCompareButtons(); };
    addAndMakeVisible (compareBButton);

    compareCopyButton.setButtonText ("Copy");
    compareCopyButton.setTooltip ("Copies the active compare slot to the other slot.");
    compareCopyButton.onClick = [this] { audioProcessor.copyActiveCompareSlotToOther(); refreshCompareButtons(); };
    addAndMakeVisible (compareCopyButton);

    helpButton.setButtonText ("?");
    helpButton.setTooltip ("Shows sidechain routing steps for common DAWs.");
    helpButton.onClick = [this] { helpOverlayVisible = ! helpOverlayVisible; repaint(); };
    addAndMakeVisible (helpButton);

    // --- Centre scope + live match ----------------------------------------
    addAndMakeVisible (oscilloscope);
    oscilloscope.setTooltip ("Triggered mode: drag left/right to nudge Delay. Shift-drag uses fine steps; double-click resets Delay.");
    addAndMakeVisible (correlationDisplay);
    correlationDisplay.setTooltip ("Live match. Click Details to collapse or expand the detailed meters.");

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

    configureRotary (crossoverSlider);
    crossoverSlider.setTextValueSuffix (" Hz");
    crossoverSlider.setNumDecimalPlacesToDisplay (0);
    addAndMakeVisible (crossoverSlider);

    configureRotary (dynEqAmountSlider);
    dynEqAmountSlider.textFromValueFunction = [] (double value)
    {
        return juce::String ((int) std::round (value * 100.0)) + "%";
    };
    dynEqAmountSlider.valueFromTextFunction = [] (const juce::String& input)
    {
        const float value = input.retainCharacters ("0123456789.").getFloatValue();
        return input.containsChar ('%') ? (double) (value / 100.0f) : (double) value;
    };
    dynEqAmountSlider.setTooltip ("Sidechain-triggered high-band transient EQ amount. At 0%, the EQ Freq, EQ Q and EQ Boost controls are intentionally inactive.");
    addAndMakeVisible (dynEqAmountSlider);

    configureRotary (dynEqFreqSlider);
    dynEqFreqSlider.textFromValueFunction = [] (double value)
    {
        return juce::String ((int) std::round (value)) + " Hz";
    };
    dynEqFreqSlider.valueFromTextFunction = [] (const juce::String& input)
    {
        return (double) input.retainCharacters ("0123456789.").getFloatValue();
    };
    dynEqFreqSlider.setTooltip ("Frequency of the transient enhancer. Audible only when Transient Amt and EQ Boost are above zero.");
    addAndMakeVisible (dynEqFreqSlider);

    configureRotary (dynEqQSlider);
    dynEqQSlider.textFromValueFunction = [] (double value)
    {
        return juce::String (value, 2);
    };
    dynEqQSlider.valueFromTextFunction = [] (const juce::String& input)
    {
        return (double) input.retainCharacters ("0123456789.").getFloatValue();
    };
    dynEqQSlider.setTooltip ("Sharpness of the transient enhancer band. Audible only when Transient Amt and EQ Boost are above zero.");
    addAndMakeVisible (dynEqQSlider);

    configureRotary (dynEqBoostSlider);
    dynEqBoostSlider.textFromValueFunction = [] (double value)
    {
        return juce::String (value, 1) + " dB";
    };
    dynEqBoostSlider.valueFromTextFunction = [] (const juce::String& input)
    {
        return (double) input.retainCharacters ("0123456789.").getFloatValue();
    };
    dynEqBoostSlider.setTooltip ("Maximum transient boost. At 0 dB this section is intentionally silent even if Transient Amt is raised.");
    addAndMakeVisible (dynEqBoostSlider);

    configureControlLabel (delayLabel, "Delay");
    configureControlLabel (polarityLabel, "Polarity");
    configureControlLabel (phaseFilterLabel, "Phase");
    configureControlLabel (phaseFreqLabel, "Phase Freq");
    configureControlLabel (phaseQLabel, "Q");
    configureControlLabel (visualOffsetLabel, "Visual Offset");
    configureControlLabel (crossoverLabel, "Crossover");
    configureControlLabel (dynEqAmountLabel, "Trans Amt");
    configureControlLabel (dynEqFreqLabel, "EQ Freq");
    configureControlLabel (dynEqQLabel, "EQ Q");
    configureControlLabel (dynEqBoostLabel, "EQ Boost");

    // --- Advanced ----------------------------------------------------------
    configureSectionLabel (advancedHeader, "ADVANCED");
    advancedHeader.setColour (juce::Label::textColourId, mutedText.withAlpha (0.7f));

    configureCombo (delayInterpCombo, { "Linear", "Allpass" });
    delayInterpCombo.setTooltip ("Chooses the fractional-delay interpolation used by Analyze and manual delay.");
    configureCombo (phaseStagesCombo, { "2", "3", "4" });
    phaseStagesCombo.setTooltip ("Number of cascaded allpass stages used by the phase filter.");
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
    addAndMakeVisible (transientHealth);

    // --- Parameter attachments --------------------------------------------
    auto& apvts = audioProcessor.apvts;
    delayAttachment        = std::make_unique<SliderAttachment> (apvts, "delay_ms", delaySlider);
    polarityInvertAttachment = std::make_unique<ButtonAttachment> (apvts, "polarity_invert", polarityInvertButton);
    phaseFilterAttachment  = std::make_unique<ButtonAttachment> (apvts, "allpass_enable", phaseFilterButton);
    phaseFreqAttachment    = std::make_unique<SliderAttachment> (apvts, "allpass_freq", phaseFreqSlider);
    phaseQAttachment       = std::make_unique<SliderAttachment> (apvts, "rotatorQ", phaseQSlider);
    visualOffsetAttachment = std::make_unique<SliderAttachment> (apvts, "visualOffsetSamples", visualOffsetSlider);
    crossoverAttachment    = std::make_unique<SliderAttachment> (apvts, "crossover_freq", crossoverSlider);
    dynEqAmountAttachment  = std::make_unique<SliderAttachment> (apvts, "dyneq_amount", dynEqAmountSlider);
    dynEqFreqAttachment    = std::make_unique<SliderAttachment> (apvts, "dyneq_freq", dynEqFreqSlider);
    dynEqQAttachment       = std::make_unique<SliderAttachment> (apvts, "dyneq_q", dynEqQSlider);
    dynEqBoostAttachment   = std::make_unique<SliderAttachment> (apvts, "dyneq_boost_db", dynEqBoostSlider);
    gridAttachment         = std::make_unique<ComboAttachment> (apvts, "gridDivision", gridCombo);
    viewAttachment         = std::make_unique<ComboAttachment> (apvts, "scopeViewMode", viewCombo);
    delayInterpAttachment  = std::make_unique<ComboAttachment> (apvts, "delayInterp", delayInterpCombo);
    phaseStagesAttachment  = std::make_unique<ComboAttachment> (apvts, "rotatorStages", phaseStagesCombo);

    oscilloscope.setTimebase (audioProcessor.getSampleRate(),
                              audioProcessor.getScopeDecimationFactor());
    oscilloscope.setDelayParameter (audioProcessor.apvts.getParameter ("delay_ms"));
    pushScopeSettings();

    resizeConstrainer.setSizeLimits (800, 544, 1500, 1020);
    resizeConstrainer.setFixedAspectRatio (1000.0 / 680.0);
    setConstrainer (&resizeConstrainer);
    setResizable (true, true);

    const int savedWidth = (int) audioProcessor.apvts.state.getProperty ("editorWidth", 1000);
    const int savedHeight = (int) audioProcessor.apvts.state.getProperty ("editorHeight", 680);
    setSize (juce::jlimit (800, 1500, savedWidth),
             juce::jlimit (544, 1020, savedHeight));
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

    latestMaterialStatus = classifyAnalysisMaterialStatus (hasSidechain,
                                                           audioProcessor.isKickActive(),
                                                           audioProcessor.isBassActive(),
                                                           audioProcessor.isAnalysisSignalUsable(),
                                                           audioProcessor.hasEnoughMaterialForAnalysis());
    canStartAnalyze = analysisStatusCanStartAnalyze (latestMaterialStatus);

    switch (latestMaterialStatus)
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

    analyzeButton.setButtonText (busy ? analyzeStateButtonText (state)
                                      : juce::String (analyzeButtonTextForStatus (latestMaterialStatus)));
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
                if (latestResult.quality == PhaseFixQuality::AlreadyGood)
                    body << "\n\nNo applicable correction is needed.";
                else
                    body << "\n\nNo applicable bass-path change was found.";

                body << "\n";
            }

            if (resultCanApply)
            {
                body << "Confidence: " << juce::String ((int) std::round (latestResult.confidence * 100.0f)) << "%\n"
                     << "Low-end match: " << juce::String ((int) std::round (latestResult.beforeMatchPercent))
                     << "% -> " << juce::String ((int) std::round (latestResult.predictedAfterMatchPercent)) << "%";
            }
            else
            {
                body << "Signal confidence: " << juce::String ((int) std::round (latestResult.confidence * 100.0f)) << "%\n"
                     << "Current low-end match: " << juce::String ((int) std::round (latestResult.beforeMatchPercent)) << "%";

                if (latestResult.predictedAfterMatchPercent < latestResult.beforeMatchPercent)
                    body << "\nBest tested change would reduce match to "
                         << juce::String ((int) std::round (latestResult.predictedAfterMatchPercent))
                         << "%, so Apply is disabled.";
            }

            if (resultCanApply && latestResult.largeTimingOffset)
                body << "\n\nWarning: Detected offset is " << juce::String (latestResult.detectedTimingOffsetMs, 1) << " ms, which exceeds the max delay of " << juce::String(PhaseFixEngine::defaultAutoFixMaxDelayMs, 1) << " ms. A best-effort delay was calculated, but manual DAW movement may sound more natural.";
            else if (resultCanApply && latestResult.requiresTimelineMove)
                body << "\n\nWarning: This correction conceptually needs a DAW timeline move (" << juce::String(latestResult.suggestedKickMoveMs, 1) << " ms). Delay was clamped to 0 ms for a best-effort fix.";
            else if (resultCanApply && latestResult.unstableRecommendation)
                body << "\n\nWarning: Different hits need different corrections; result is a best-effort consensus.";

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
    revertButton.setEnabled (audioProcessor.hasRevertSnapshot());
}

void KickLockAudioProcessorEditor::refreshCompareButtons()
{
    const int activeSlot = audioProcessor.getActiveCompareSlot();
    auto styleSlot = [] (juce::TextButton& button, bool active)
    {
        button.setColour (juce::TextButton::buttonColourId, active ? teal : panel);
        button.setColour (juce::TextButton::textColourOffId, active ? juce::Colours::black : text);
    };

    styleSlot (compareAButton, activeSlot == 0);
    styleSlot (compareBButton, activeSlot == 1);
    compareCopyButton.setColour (juce::TextButton::buttonColourId, panel);
    compareCopyButton.setColour (juce::TextButton::textColourOffId, text);
    helpButton.setColour (juce::TextButton::buttonColourId, helpOverlayVisible ? orange : panel);
    helpButton.setColour (juce::TextButton::textColourOffId, helpOverlayVisible ? juce::Colours::black : text);
}

void KickLockAudioProcessorEditor::refreshTransientEqControlState()
{
    const auto* amountParam = audioProcessor.apvts.getRawParameterValue ("dyneq_amount");
    const auto* boostParam = audioProcessor.apvts.getRawParameterValue ("dyneq_boost_db");
    const float amount = amountParam != nullptr ? amountParam->load() : 0.0f;
    const float boostDb = boostParam != nullptr ? boostParam->load() : 0.0f;

    const bool active = amount > 0.001f && boostDb > 0.01f;
    const float dependentAlpha = active ? 1.0f : 0.48f;
    const auto dependentColour = active ? mutedText : mutedText.withAlpha (0.55f);

    dynEqFreqSlider.setAlpha (dependentAlpha);
    dynEqQSlider.setAlpha (dependentAlpha);
    dynEqFreqLabel.setColour (juce::Label::textColourId, dependentColour);
    dynEqQLabel.setColour (juce::Label::textColourId, dependentColour);

    dynEqAmountLabel.setColour (juce::Label::textColourId, active ? teal : amber);
    dynEqBoostLabel.setColour (juce::Label::textColourId, amount > 0.001f ? mutedText : mutedText.withAlpha (0.75f));
}

void KickLockAudioProcessorEditor::timerCallback()
{
    oscilloscope.setTimebase (audioProcessor.getSampleRate(),
                              audioProcessor.getScopeDecimationFactor());
    oscilloscope.setTempoInfo (audioProcessor.getLatestBpm(),
                               audioProcessor.isTempoAvailable());
    pushScopeSettings();
    transientHealth.setValues (audioProcessor.getTransientPrePeak(),
                               audioProcessor.getTransientPostPeak(),
                               audioProcessor.getTransientHealthDb());

    refreshStatusStrings();
    refreshAnalyzeWorkflow();
    refreshCompareButtons();
    refreshTransientEqControlState();

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

    auto barInner = topBar.reduced (14, 6);
    auto infoRow = barInner.removeFromTop (20);

    g.setColour (text);
    g.setFont (juce::Font (juce::FontOptions (18.0f)).boldened());
    g.drawText ("KICKLOCK", infoRow.removeFromLeft (124),
                juce::Justification::centredLeft);

    g.setColour (sidechainStatusColour);
    g.setFont (juce::Font (juce::FontOptions (12.0f)).boldened());
    g.drawText (sidechainStatusText, infoRow.removeFromLeft (168),
                juce::Justification::centredLeft);

    g.setColour (mutedText);
    g.setFont (juce::Font (juce::FontOptions (11.0f)));
    g.drawText (bpmText + "   |   " + pdcText, infoRow,
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

void KickLockAudioProcessorEditor::paintOverChildren (juce::Graphics& g)
{
    if (! helpOverlayVisible)
        return;

    auto bounds = getLocalBounds().reduced (70, 64);
    g.setColour (juce::Colours::black.withAlpha (0.58f));
    g.fillAll();

    g.setColour (panel);
    g.fillRoundedRectangle (bounds.toFloat(), 8.0f);
    g.setColour (orange.withAlpha (0.8f));
    g.drawRoundedRectangle (bounds.toFloat(), 8.0f, 1.4f);

    auto area = bounds.reduced (22, 18);
    g.setColour (text);
    g.setFont (juce::Font (juce::FontOptions (20.0f)).boldened());
    g.drawText ("Kick to Sidechain", area.removeFromTop (28), juce::Justification::centredLeft);

    g.setColour (mutedText);
    g.setFont (juce::Font (juce::FontOptions (12.5f)));

    const juce::StringArray lines {
        "Ableton Live: Drop KickLock on the bass track, open Sidechain in the device header, choose the kick track.",
        "FL Studio: Put KickLock on the bass mixer insert, route the kick insert to it with Sidechain to this track, select that input in the wrapper.",
        "Logic Pro: Insert KickLock on bass, enable the plug-in sidechain menu, choose the kick track or bus.",
        "Cubase: Insert KickLock on bass, activate sidechain in the plug-in header, send the kick channel to that sidechain.",
        "Reaper: Put KickLock on bass, route kick channels 1/2 to bass channels 3/4, keep bass audio on 1/2."
    };

    for (const auto& line : lines)
    {
        g.drawFittedText (line, area.removeFromTop (44), juce::Justification::centredLeft, 2);
        area.removeFromTop (4);
    }

    g.setColour (orange);
    g.setFont (juce::Font (juce::FontOptions (12.0f)).boldened());
    g.drawText ("Press ? to close", area.removeFromBottom (18), juce::Justification::centredRight);
}

void KickLockAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds();

    // --- Top bar -----------------------------------------------------------
    auto topBar = bounds.removeFromTop (kTopBarHeight).reduced (14, 6);
    auto controls = topBar.removeFromBottom (30);

    gridCombo.setBounds (controls.removeFromLeft (68).reduced (0, 3));
    controls.removeFromLeft (5);
    viewCombo.setBounds (controls.removeFromLeft (96).reduced (0, 3));
    controls.removeFromLeft (5);
    freezeButton.setBounds (controls.removeFromLeft (56).reduced (0, 2));
    controls.removeFromLeft (8);
    compareAButton.setBounds (controls.removeFromLeft (30).reduced (0, 2));
    controls.removeFromLeft (4);
    compareBButton.setBounds (controls.removeFromLeft (30).reduced (0, 2));
    controls.removeFromLeft (4);
    compareCopyButton.setBounds (controls.removeFromLeft (48).reduced (0, 2));
    controls.removeFromLeft (10);
    analyzeButton.setBounds (controls.removeFromLeft (170).reduced (0, 2));
    controls.removeFromLeft (6);
    applyFixButton.setBounds (controls.removeFromLeft (82).reduced (0, 2));
    controls.removeFromLeft (6);
    revertButton.setBounds (controls.removeFromLeft (64).reduced (0, 2));
    controls.removeFromLeft (6);
    helpButton.setBounds (controls.removeFromLeft (30).reduced (0, 2));

    bounds.reduce (14, 12);

    // --- Live hero + scope overlays ---------------------------------------
    const int scopeBlockHeight = juce::jlimit (190, 292, bounds.getHeight() - 318);
    auto scopeBlock = bounds.removeFromTop (scopeBlockHeight);
    correlationDisplay.setBounds (scopeBlock.removeFromTop (108));
    scopeBlock.removeFromTop (6);
    auto scopeArea = scopeBlock;
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

    auto row1 = manualArea.removeFromTop (88);
    const int knobW = juce::jlimit (74, 98, (manualArea.getWidth() - 32) / 5);

    auto delayCell = row1.removeFromLeft (knobW);
    delayLabel.setBounds (delayCell.removeFromTop (14));
    delaySlider.setBounds (delayCell);

    row1.removeFromLeft (8);
    auto crossoverCell = row1.removeFromLeft (knobW);
    crossoverLabel.setBounds (crossoverCell.removeFromTop (14));
    crossoverSlider.setBounds (crossoverCell);

    row1.removeFromLeft (8);
    auto freqCell = row1.removeFromLeft (knobW);
    phaseFreqLabel.setBounds (freqCell.removeFromTop (14));
    phaseFreqSlider.setBounds (freqCell);

    row1.removeFromLeft (8);
    auto qCell = row1.removeFromLeft (knobW);
    phaseQLabel.setBounds (qCell.removeFromTop (14));
    phaseQSlider.setBounds (qCell);

    row1.removeFromLeft (8);
    auto visualCell = row1.removeFromLeft (knobW);
    visualOffsetLabel.setBounds (visualCell.removeFromTop (14));
    visualOffsetSlider.setBounds (visualCell);

    manualArea.removeFromTop (4);
    auto transientRow = manualArea.removeFromTop (88);
    auto dynAmtCell = transientRow.removeFromLeft (knobW);
    dynEqAmountLabel.setBounds (dynAmtCell.removeFromTop (14));
    dynEqAmountSlider.setBounds (dynAmtCell);

    transientRow.removeFromLeft (8);
    auto dynFreqCell = transientRow.removeFromLeft (knobW);
    dynEqFreqLabel.setBounds (dynFreqCell.removeFromTop (14));
    dynEqFreqSlider.setBounds (dynFreqCell);

    transientRow.removeFromLeft (8);
    auto dynQCell = transientRow.removeFromLeft (knobW);
    dynEqQLabel.setBounds (dynQCell.removeFromTop (14));
    dynEqQSlider.setBounds (dynQCell);

    transientRow.removeFromLeft (8);
    auto dynBoostCell = transientRow.removeFromLeft (knobW);
    dynEqBoostLabel.setBounds (dynBoostCell.removeFromTop (14));
    dynEqBoostSlider.setBounds (dynBoostCell);

    manualArea.removeFromTop (4);
    auto row2 = manualArea.removeFromTop (42);
    auto polCell = row2.removeFromLeft (knobW * 2 + 8);
    polarityLabel.setBounds (polCell.removeFromTop (14));
    polarityInvertButton.setBounds (polCell.removeFromTop (24));

    row2.removeFromLeft (8);
    auto phCell = row2.removeFromLeft (knobW * 2);
    phaseFilterLabel.setBounds (phCell.removeFromTop (14));
    phaseFilterButton.setBounds (phCell.removeFromTop (24));

    manualArea.removeFromTop (4);
    advancedHeader.setBounds (manualArea.removeFromTop (16));
    manualArea.removeFromTop (2);
    auto advRow = manualArea.removeFromTop (34);
    auto interpCell = advRow.removeFromLeft (knobW * 2 + 8);
    delayInterpLabel.setBounds (interpCell.removeFromTop (12));
    delayInterpCombo.setBounds (interpCell.removeFromTop (22).reduced (0, 1));
    advRow.removeFromLeft (8);
    auto stagesCell = advRow.removeFromLeft (knobW * 2);
    phaseStagesLabel.setBounds (stagesCell.removeFromTop (12));
    phaseStagesCombo.setBounds (stagesCell.removeFromTop (22).reduced (0, 1));

    // Right column: analyzer explanation.
    transientHealth.setBounds (right.removeFromTop (54));
    right.removeFromTop (8);
    analyzerTitle.setBounds (right.removeFromTop (20));
    analyzerBody.setBounds (right);

    audioProcessor.apvts.state.setProperty ("editorWidth", getWidth(), nullptr);
    audioProcessor.apvts.state.setProperty ("editorHeight", getHeight(), nullptr);
}
