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
    constexpr int kDefaultEditorWidth = 1180;
    constexpr int kDefaultEditorHeight = 820;
    constexpr int kMinEditorWidth = 900;
    constexpr int kMinEditorHeight = 680;
    constexpr int kMaxEditorWidth = 2800;
    constexpr int kMaxEditorHeight = 1900;
}

SegmentedModeSelector::SegmentedModeSelector()
{
    setName ("Correction Mode");
    setDescription ("Selects Static or Dynamic phase correction.");
    parameterChoice.addItem ("Static", 1);
    parameterChoice.addItem ("Dynamic", 2);
    parameterChoice.setSelectedItemIndex (0, juce::dontSendNotification);
    parameterChoice.addListener (this);
    addChildComponent (parameterChoice);

    for (auto* button : { &staticButton, &dynamicButton })
    {
        button->setWantsKeyboardFocus (true);
        addAndMakeVisible (*button);
    }
    staticButton.setTooltip ("Static: uses one fixed correction for the whole bass part. Best for basslines that stay mostly on one pitch.");
    dynamicButton.setTooltip ("Dynamic: learns a separate Freq/Q correction for each trusted bass note. Delay and Polarity always remain global.");
    staticButton.setName ("Static correction mode");
    staticButton.setDescription ("Uses the manual and Analyze correction workflow.");
    dynamicButton.setName ("Dynamic correction mode");
    dynamicButton.setDescription ("Uses the learned global and per-note correction workflow.");
    staticButton.onClick = [this] { parameterChoice.setSelectedItemIndex (0, juce::sendNotification); };
    dynamicButton.onClick = [this] { parameterChoice.setSelectedItemIndex (1, juce::sendNotification); };
    syncButtons();
}

SegmentedModeSelector::~SegmentedModeSelector()
{
    parameterChoice.removeListener (this);
}

void SegmentedModeSelector::comboBoxChanged (juce::ComboBox*)
{
    syncButtons();
}

void SegmentedModeSelector::syncButtons()
{
    const bool dynamic = parameterChoice.getSelectedItemIndex() == 1;
    auto style = [] (juce::TextButton& button, bool selected)
    {
        button.setToggleState (selected, juce::dontSendNotification);
        button.setColour (juce::TextButton::buttonColourId, selected ? teal : panel);
        button.setColour (juce::TextButton::textColourOffId, selected ? juce::Colours::black : text);
    };
    style (staticButton, ! dynamic);
    style (dynamicButton, dynamic);
    repaint();
}

void SegmentedModeSelector::resized()
{
    auto bounds = getLocalBounds().reduced (1);
    staticButton.setBounds (bounds.removeFromLeft (bounds.getWidth() / 2));
    dynamicButton.setBounds (bounds);
    parameterChoice.setBounds (getLocalBounds());
}

void SegmentedModeSelector::paint (juce::Graphics& g)
{
    g.setColour (border);
    g.drawRoundedRectangle (getLocalBounds().toFloat(), 4.0f, 1.0f);
}

void LearnProgressComponent::setModel (const LearnProgressSnapshot& nextProgress,
                                       const NotePhaseMapSnapshot& map,
                                       int nextActiveMidi)
{
    std::array<int, NotePhaseMapSnapshot::size> nextCounts {};
    std::array<bool, NotePhaseMapSnapshot::size> nextLearned {};
    std::array<juce::String, NotePhaseMapSnapshot::size> nextTexts {};
    for (int i = 0; i < NotePhaseMapSnapshot::size; ++i)
    {
        const auto& entry = map.notes[(size_t) i];
        nextCounts[(size_t) i] = juce::jmax (nextProgress.trackedNoteHitCounts[(size_t) i], entry.hitCount);
        nextLearned[(size_t) i] = NoteMap::isValidNoteEntry (entry)
                                   || learnNoteHasEnoughMaterial (nextCounts[(size_t) i]);
        if (nextCounts[(size_t) i] > 0)
            nextTexts[(size_t) i] = formatLearnNoteChip (NotePhaseMapSnapshot::midiForIndex (i), nextCounts[(size_t) i]);
    }

    const bool changed = progress.state != nextProgress.state
                      || progress.capturedHits != nextProgress.capturedHits
                      || progress.drainedHits != nextProgress.drainedHits
                      || progress.pendingQueueHits != nextProgress.pendingQueueHits
                      || progress.droppedQueueHits != nextProgress.droppedQueueHits
                      || progress.rejectedPitchHits != nextProgress.rejectedPitchHits
                      || progress.unusableSignalHits != nextProgress.unusableSignalHits
                      || progress.ignoredOverlappingTriggers != nextProgress.ignoredOverlappingTriggers
                      || noteCounts != nextCounts || learnedNotes != nextLearned || activeMidi != nextActiveMidi;
    if (! changed)
        return;

    progress = nextProgress;
    noteCounts = std::move (nextCounts);
    learnedNotes = std::move (nextLearned);
    noteTexts = std::move (nextTexts);
    activeMidi = nextActiveMidi;
    repaint();
}

void LearnProgressComponent::paint (juce::Graphics& g)
{
    auto area = getLocalBounds();
    g.setColour (background);
    g.fillRoundedRectangle (area.toFloat(), 6.0f);
    g.setColour (border);
    g.drawRoundedRectangle (area.toFloat(), 6.0f, 1.0f);

    auto summary = area.reduced (8, 6).removeFromTop (15);
    g.setColour (mutedText);
    g.setFont (juce::Font (juce::FontOptions (10.5f)).boldened());
    g.drawText ("HITS " + juce::String (progress.capturedHits)
                    + "  ANALYZED " + juce::String (progress.drainedHits)
                    + "  QUEUE " + juce::String (progress.pendingQueueHits),
                summary, juce::Justification::centredLeft);

    auto chips = area.reduced (8, 4);
    chips.removeFromTop (20);
    int shown = 0;
    for (int i = 0; i < NotePhaseMapSnapshot::size && shown < 6; ++i)
    {
        if (noteCounts[(size_t) i] == 0)
            continue;
        const int midi = NotePhaseMapSnapshot::midiForIndex (i);
        const int width = juce::jlimit (46, 74, 12 + (int) noteTexts[(size_t) i].length() * 7);
        if (chips.getWidth() < width)
            break;
        auto chip = chips.removeFromLeft (width).reduced (1, 2);
        const auto colour = midi == activeMidi && learnedNotes[(size_t) i] ? teal
                           : learnedNotes[(size_t) i] ? green : amber;
        g.setColour (colour.withAlpha (0.22f));
        g.fillRoundedRectangle (chip.toFloat(), 4.0f);
        g.setColour (colour);
        g.drawRoundedRectangle (chip.toFloat(), 4.0f, 1.0f);
        g.setFont (juce::Font (juce::FontOptions (10.0f)).boldened());
        g.drawText (noteTexts[(size_t) i], chip, juce::Justification::centred);
        chips.removeFromLeft (3);
        ++shown;
    }
    if (shown == 0)
    {
        g.setColour (mutedText.withAlpha (0.8f));
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.drawText ("Waiting for accepted note hits", chips, juce::Justification::centredLeft);
    }
}

void TransientPunchComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour (juce::Colour (0xff101418));
    g.fillRoundedRectangle (bounds, 6.0f);
    g.setColour (juce::Colour (0xff2b3540));
    g.drawRoundedRectangle (bounds, 6.0f, 1.0f);

    auto inner = getLocalBounds().reduced (10, 8);

    auto titleRow = inner.removeFromTop (16);
    g.setColour (mutedText);
    g.setFont (juce::Font (juce::FontOptions (11.0f)).boldened());
    g.drawText ("KICK PUNCH", titleRow, juce::Justification::centredLeft);

    // Two distinct placeholders: no sidechain routed at all (nothing to
    // measure, ever) versus a sidechain that just hasn't seen a recent kick
    // (the meter will resume the moment one arrives). Collapsing these into
    // one generic "waiting" message was what made the meter read as broken
    // when it was actually just accurately reporting no signal.
    if (! valid)
    {
        auto value = inner.removeFromTop (30);
        g.setColour (mutedText);
        g.setFont (juce::Font (juce::FontOptions (26.0f)).boldened());
        g.drawText ("--", value, juce::Justification::centred);

        g.setColour (mutedText.withAlpha (0.8f));
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawText (hasSidechain ? "Waiting for kick" : "Route kick to sidechain",
                    inner.removeFromTop (16), juce::Justification::centred);
        return;
    }

    constexpr float neutralZone = 0.3f;
    const bool reinforcing = punchDb > neutralZone;
    const bool cancelling = punchDb < -neutralZone;
    const auto valueColour = cancelling ? red : (reinforcing ? green : mutedText);

    auto value = inner.removeFromTop (32);
    g.setColour (valueColour);
    g.setFont (juce::Font (juce::FontOptions (26.0f)).boldened());
    g.drawText ((punchDb >= 0.0f ? "+" : "") + juce::String (punchDb, 1) + " dB",
                value, juce::Justification::centred);

    g.setColour (mutedText);
    g.setFont (juce::Font (juce::FontOptions (12.0f)));
    const juce::String verdict = reinforcing ? "Bass reinforces kick"
                               : cancelling  ? "Bass cancels kick"
                                             : "Neutral";
    g.drawText (verdict, inner.removeFromTop (16), juce::Justification::centred);

    inner.removeFromTop (6);

    // Diverging bar centred on the kick-alone baseline: right/green when the
    // bass reinforces, left/red when it cancels. Range is clamped to +/-12 dB.
    auto barArea = inner.removeFromTop (12).toFloat();
    const float centreX = barArea.getCentreX();
    g.setColour (border);
    g.fillRoundedRectangle (barArea, 3.0f);

    constexpr float barRangeDb = 12.0f;
    const float frac = juce::jlimit (-1.0f, 1.0f, punchDb / barRangeDb);
    const float halfW = barArea.getWidth() * 0.5f;
    if (frac >= 0.0f)
    {
        juce::Rectangle<float> fill (centreX, barArea.getY(), halfW * frac, barArea.getHeight());
        g.setColour (green.withAlpha (0.85f));
        g.fillRoundedRectangle (fill, 3.0f);
    }
    else
    {
        const float w = halfW * -frac;
        juce::Rectangle<float> fill (centreX - w, barArea.getY(), w, barArea.getHeight());
        g.setColour (red.withAlpha (0.85f));
        g.fillRoundedRectangle (fill, 3.0f);
    }

    // Centre tick.
    g.setColour (mutedText.withAlpha (0.9f));
    g.fillRect (juce::Rectangle<float> (centreX - 0.5f, barArea.getY() - 2.0f, 1.0f, barArea.getHeight() + 4.0f));

    inner.removeFromTop (8);

    // Transient health: kick-alone vs combined (kick+bass) low-end peak, in
    // dBFS. These are the meter's per-hit finalized peaks, so the bars hold
    // steady between hits instead of dancing per block.
    g.setColour (mutedText);
    g.setFont (juce::Font (juce::FontOptions (10.0f)).boldened());
    g.drawText ("TRANSIENT", inner.removeFromTop (12), juce::Justification::centredLeft);

    constexpr float minDb = -48.0f;
    auto toScale = [] (float linearPeak)
    {
        if (linearPeak <= 1.0e-5f)
            return 0.0f;
        const float db = 20.0f * std::log10 (linearPeak);
        return juce::jlimit (0.0f, 1.0f, (db - minDb) / -minDb);
    };

    auto drawBar = [&g, &toScale] (juce::Rectangle<int> rowInt, const juce::String& label,
                                   float peak, juce::Colour fill)
    {
        auto row = rowInt.toFloat();
        auto labelArea = row.removeFromLeft (58.0f);
        g.setColour (mutedText);
        g.setFont (juce::Font (juce::FontOptions (10.5f)));
        g.drawText (label, labelArea, juce::Justification::centredLeft);

        g.setColour (border);
        g.fillRoundedRectangle (row, 3.0f);
        g.setColour (fill);
        g.fillRoundedRectangle (row.withWidth (row.getWidth() * toScale (peak)), 3.0f);
    };

    inner.removeFromTop (2);
    drawBar (inner.removeFromTop (10), "Kick", kickPeak, orange.withAlpha (0.75f));
    inner.removeFromTop (3);
    drawBar (inner.removeFromTop (10), "Kick+Bass", sumPeak, teal.withAlpha (0.85f));

    inner.removeFromTop (6);
    auto refRow = inner.removeFromTop (16);
    if (hasReference)
    {
        const float delta = punchDb - referenceDb;
        g.setColour (delta >= 0.0f ? green : red);
        g.setFont (juce::Font (juce::FontOptions (11.5f)).boldened());
        g.drawText ("Δ vs ref: " + juce::String (delta >= 0.0f ? "+" : "") + juce::String (delta, 1) + " dB",
                    refRow, juce::Justification::centred);
    }
    else
    {
        g.setColour (mutedText.withAlpha (0.7f));
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.drawText ("tap Set Ref to compare", refRow, juce::Justification::centred);
    }
}

KickLockAudioProcessorEditor::KickLockAudioProcessorEditor (KickLockAudioProcessor& p)
    : AudioProcessorEditor (&p),
      audioProcessor (p),
      oscilloscope (p.rawScopeFifo, p.getTriggeredHitCapture()),
      spectrumAnalyzer (p.spectrumFifo),
      correlationDisplay (p.liveMultiBandMatchPercent,
                          p.liveLowEndMatchPercent,
                          p.liveBroadbandMatchPercent,
                          p.liveBandMatchPercent,
                          p.liveMatchValid,
                          p.liveLowEndSubLossDb),
      splitter ([this] (int h) { bottomPanelHeight = juce::jlimit (100, getHeight() - 100, h); resized(); })
{
    setLookAndFeel (&lookAndFeel);
    sidechainStatusColour = mutedText;

    // --- Top bar controls --------------------------------------------------
    configureCombo (gridCombo, { "1/4", "1/2", "1", "4", "Bar", "ms" });
    configureCombo (viewCombo, { "Triggered", "Free-run", "Phase Delta", "Separate", "Spectrum" });
    gridCombo.setTooltip ("Sets the scope time grid.");
    viewCombo.setTooltip ("Scope view: Triggered, Free-run (raw live scope, no offset), "
                           "Phase Delta, Spectrum (aligned bass/kick comparison), or Separate lanes.");
    addAndMakeVisible (correctionModeSelector);



    freezeButton.setButtonText ("Freeze");
    freezeButton.setClickingTogglesState (true);
    freezeButton.setColour (juce::TextButton::buttonColourId, panel);
    freezeButton.setColour (juce::TextButton::textColourOffId, text);
    freezeButton.setColour (juce::TextButton::buttonOnColourId, teal);
    freezeButton.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    freezeButton.setTooltip ("Freezes the oscilloscope display so you can inspect the waveform.");
    freezeButton.onClick = [this] { oscilloscope.setFrozen (freezeButton.getToggleState()); oscilloscope.repaint(); };
    addAndMakeVisible (freezeButton);

    relockKickButton.setButtonText ("Re-lock");
    relockKickButton.setColour (juce::TextButton::buttonColourId, panel);
    relockKickButton.setColour (juce::TextButton::textColourOffId, text);
    relockKickButton.setTooltip ("Waits for the next kick transient and stores it as the triggered-scope reference.");
    relockKickButton.onClick = [this] { oscilloscope.relockKickReference(); oscilloscope.repaint(); };
    addAndMakeVisible (relockKickButton);

    analyzeButton.setButtonText ("Analyze");
    analyzeButton.setColour (juce::TextButton::buttonColourId, teal);
    analyzeButton.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
    analyzeButton.setTooltip ("Captures the current kick and bass loop and recommends a bass-path correction. "
                              "Use Apply Fix to apply it.");
    analyzeButton.setEnabled (false);
    analyzeButton.onClick = [this]
    {
        const bool dynamic = audioProcessor.apvts.getRawParameterValue ("correction_mode")->load() > 0.5f;
        const auto presentation = primaryWorkflowPresentation (dynamic, audioProcessor.getLearnState(),
                                                                canStartAnalyze,
                                                                analyzeStateIsBusy (audioProcessor.getAnalyzeState()));
        switch (presentation.action)
        {
            case PrimaryWorkflowAction::StartAnalyze:
                if (canStartAnalyze && audioProcessor.beginBackgroundAnalyze())
                {
                    latestResultAutoApplied = false;
                    haveResult = false;
                    latestResult = {};
                }
                break;
            case PrimaryWorkflowAction::StartLearn:
                audioProcessor.beginLearn();
                break;
            case PrimaryWorkflowAction::StopLearn:
                audioProcessor.stopLearn();
                break;
            case PrimaryWorkflowAction::LearnAgain:
                if (audioProcessor.discardLatestLearnResult())
                    audioProcessor.beginLearn();
                break;
            case PrimaryWorkflowAction::None:
                break;
        }
    };
    addAndMakeVisible (analyzeButton);

    applyFixButton.setButtonText ("Apply Fix");
    applyFixButton.setColour (juce::TextButton::buttonColourId, orange);
    applyFixButton.setColour (juce::TextButton::textColourOffId, juce::Colours::black);
    applyFixButton.setEnabled (false);
    applyFixButton.onClick = [this]
    {
        if (audioProcessor.getLearnState() == LearnState::ResultReady && audioProcessor.hasPendingLearnResult())
            audioProcessor.applyLatestLearnResult();
        else
            audioProcessor.applyLatestFix();
    };
    applyFixButton.setTooltip ("Applies the latest analyzer correction to the bass-path controls.");
    addAndMakeVisible (applyFixButton);

    discardButton.setButtonText ("Discard");
    discardButton.setColour (juce::TextButton::buttonColourId, panel);
    discardButton.setColour (juce::TextButton::textColourOffId, text);
    discardButton.setTooltip ("Discards the pending Learn result without changing the active map or parameters.");
    discardButton.setName ("Discard Learn result");
    discardButton.setDescription ("Discards the pending Learn result without applying it.");
    discardButton.onClick = [this] { audioProcessor.discardLatestLearnResult(); };
    addAndMakeVisible (discardButton);

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
    helpButton.setTooltip ("Shows user guide and sidechain routing instructions.");
    helpButton.onClick = [this] { helpOverlayVisible = ! helpOverlayVisible; repaint(); };
    addAndMakeVisible (helpButton);

    // --- Centre scope + live match ----------------------------------------
    addAndMakeVisible (oscilloscope);
    addAndMakeVisible (spectrumAnalyzer);
    const auto toggleCleanScopeMode = [this]
    {
        cleanScopeMode = ! cleanScopeMode;
        if (cleanScopeMode)
            helpOverlayVisible = false;
        resized();
        repaint();
    };
    oscilloscope.onToggleCleanMode = toggleCleanScopeMode;
    spectrumAnalyzer.onToggleCleanMode = toggleCleanScopeMode;

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

    configureOverlay (manualDelayOverlay, juce::Justification::centredRight, teal);
    configureOverlay (noSidechainOverlay, juce::Justification::centred, text);
    noSidechainOverlay.setText ("Route kick to sidechain to compare phase.",
                                juce::dontSendNotification);
    manualDelayOverlay.setText (manualDelayText, juce::dontSendNotification);

    // --- Manual alignment --------------------------------------------------
    configureSectionLabel (manualHeader, "MANUAL ALIGNMENT");

    // Value text for the parameter-attached rotaries comes from each
    // parameter's stringFromValue function (see createParameterLayout): the
    // SliderAttachment installs the parameter's text conversion as the
    // slider's textFromValueFunction, overwriting anything set here — which is
    // why custom slider-side formatters and setNumDecimalPlacesToDisplay never
    // took effect (textboxes showed raw "236.1548004" floats). Units live in
    // the parameter strings, so no setTextValueSuffix on these either (the
    // suffix is appended after textFromValueFunction and would double up).
    configureRotary (delaySlider);
    delaySlider.setTooltip ("Moves the bass earlier or later relative to the kick. "
                            "Negative values advance bass using plugin delay compensation.");
    addAndMakeVisible (delaySlider);

    polarityInvertButton.setButtonText ("Invert");
    polarityInvertButton.setTooltip ("Flips the bass polarity by 180 degrees. "
                                     "Useful when kick and bass cancel each other.");
    addAndMakeVisible (polarityInvertButton);

    phaseFilterButton.setButtonText ("Phase Filter");
    phaseFilterButton.setTooltip ("Rotates phase around the selected frequency "
                                  "without moving the whole signal in time.");
    addAndMakeVisible (phaseFilterButton);

    pitchTrackButton.setButtonText ("Follow Bass");
    pitchTrackButton.setTooltip ("Continuously tunes the Phase Filter to the bass's detected "
                                 "fundamental, so the phase correction stays on the note as the "
                                 "bassline moves. A static phase filter detunes the moment the "
                                 "bass changes notes.");
    addAndMakeVisible (pitchTrackButton);

    configureRotary (phaseFreqSlider);
    phaseFreqSlider.setTooltip ("Centre frequency of the phase filter (20 Hz - 500 Hz).");
    addAndMakeVisible (phaseFreqSlider);

    configureRotary (phaseQSlider);
    phaseQSlider.setTooltip ("Q (sharpness) of the phase filter around its centre frequency.");
    addAndMakeVisible (phaseQSlider);

    configureRotary (visualOffsetSlider);
    visualOffsetSlider.setTextValueSuffix (" smp");
    visualOffsetSlider.setNumDecimalPlacesToDisplay (0);
    visualOffsetSlider.setTooltip ("Display-only sample shift applied to the bass wave on the scope so you can align phases visually. Does not affect the sound.");
    addAndMakeVisible (visualOffsetSlider);

    dynamicStrengthSlider.setSliderStyle (juce::Slider::LinearBar);
    dynamicStrengthSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 42, 18);
    dynamicStrengthSlider.setColour (juce::Slider::trackColourId, teal);
    dynamicStrengthSlider.setColour (juce::Slider::textBoxTextColourId, text);
    dynamicStrengthSlider.setColour (juce::Slider::textBoxOutlineColourId, border);
    dynamicStrengthSlider.setTooltip (dynamicStrengthTooltip());
    dynamicStrengthSlider.setName ("Dynamic Strength");
    dynamicStrengthSlider.setDescription (dynamicStrengthTooltip());
    addAndMakeVisible (dynamicStrengthSlider);

    crossoverEnableButton.setButtonText ("Enable");
    crossoverEnableButton.setTooltip ("Toggles the crossover. When disabled, the entire signal passes through the delay and polarity invert.");
    addAndMakeVisible (crossoverEnableButton);

    configureRotary (crossoverSlider);
    addAndMakeVisible (crossoverSlider);

    configureControlLabel (delayLabel, "Delay");
    configureControlLabel (polarityLabel, "Polarity");
    configureControlLabel (phaseFilterLabel, "Phase");
    configureControlLabel (pitchTrackLabel, "Pitch");
    configureControlLabel (phaseFreqLabel, "Phase Freq");
    configureControlLabel (phaseQLabel, "Q");
    configureControlLabel (visualOffsetLabel, "Visual Offset");
    configureControlLabel (dynamicStrengthLabel, "Dynamic Strength");
    configureControlLabel (crossoverLabel, "Crossover Freq");
    configureControlLabel (crossoverEnableLabel, "Crossover");



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
                          "recommend a bass-path correction.", juce::dontSendNotification);
    addAndMakeVisible (analyzerBody);
    addAndMakeVisible (transientPunch);
    addAndMakeVisible (learnProgressDisplay);

    setRefButton.setButtonText ("Set Ref");
    setRefButton.setColour (juce::TextButton::buttonColourId, panel);
    setRefButton.setColour (juce::TextButton::textColourOffId, text);
    setRefButton.setTooltip ("Stores the current kick-punch reading as a reference, then shows the live delta against it.");
    setRefButton.onClick = [this]
    {
        if (showingLearnWorkflow)
        {
            audioProcessor.clearNoteMap();
            return;
        }
        if (audioProcessor.isTransientPunchReferenceSet())
            audioProcessor.clearTransientPunchReference();
        else
            audioProcessor.setTransientPunchReference();

        setRefButton.setButtonText (audioProcessor.isTransientPunchReferenceSet() ? "Clear Ref" : "Set Ref");
    };
    addAndMakeVisible (setRefButton);

    // --- Parameter attachments --------------------------------------------
    auto& apvts = audioProcessor.apvts;
    delayAttachment        = std::make_unique<SliderAttachment> (apvts, "delay_ms", delaySlider);
    polarityInvertAttachment = std::make_unique<ButtonAttachment> (apvts, "polarity_invert", polarityInvertButton);
    phaseFilterAttachment  = std::make_unique<ButtonAttachment> (apvts, "allpass_enable", phaseFilterButton);
    
    phaseFilterButton.onClick = [this]
    {
        const bool enabled = phaseFilterButton.getToggleState();
        phaseFreqSlider.setEnabled (enabled);
        phaseFreqLabel.setAlpha (enabled ? 1.0f : 0.5f);
        phaseQSlider.setEnabled (enabled);
        phaseQLabel.setAlpha (enabled ? 1.0f : 0.5f);
    };
    phaseFilterButton.onClick();

    pitchTrackAttachment   = std::make_unique<ButtonAttachment> (apvts, "pitch_track", pitchTrackButton);
    phaseFreqAttachment    = std::make_unique<SliderAttachment> (apvts, "allpass_freq", phaseFreqSlider);
    phaseQAttachment       = std::make_unique<SliderAttachment> (apvts, "rotatorQ", phaseQSlider);
    visualOffsetAttachment = std::make_unique<SliderAttachment> (apvts, "visualOffsetSamples", visualOffsetSlider);
    dynamicStrengthAttachment = std::make_unique<SliderAttachment> (apvts, "dynamic_strength", dynamicStrengthSlider);
    visualOffsetSlider.onValueChange = [this]
    {
        oscilloscope.setVisualOffsetSamples ((int) std::lround (visualOffsetSlider.getValue()));
    };
    crossoverEnableAttachment = std::make_unique<ButtonAttachment> (apvts, "crossover_enable", crossoverEnableButton);
    
    crossoverEnableButton.onClick = [this]
    {
        const bool enabled = crossoverEnableButton.getToggleState();
        crossoverSlider.setEnabled (enabled);
        crossoverLabel.setAlpha (enabled ? 1.0f : 0.5f);
    };
    crossoverEnableButton.onClick();
    
    crossoverAttachment    = std::make_unique<SliderAttachment> (apvts, "crossover_freq", crossoverSlider);

    gridAttachment         = std::make_unique<ComboAttachment> (apvts, "gridDivision", gridCombo);
    viewCombo.onChange = [this]
    {
        if (auto* parameter = audioProcessor.apvts.getParameter ("scopeViewMode"))
        {
            const auto mode = scopeViewModeFromUiIndex (viewCombo.getSelectedItemIndex());
            const float persistedIndex = (float) scopeViewModeToChoiceIndex (mode);
            parameter->beginChangeGesture();
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (persistedIndex));
            parameter->endChangeGesture();
        }

        pushScopeSettings();
    };
    delayInterpAttachment  = std::make_unique<ComboAttachment> (apvts, "delayInterp", delayInterpCombo);
    phaseStagesAttachment  = std::make_unique<ComboAttachment> (apvts, "rotatorStages", phaseStagesCombo);
    correctionModeAttachment = std::make_unique<ComboAttachment> (apvts, "correction_mode",
                                                                     correctionModeSelector.parameterCombo());

    oscilloscope.setTimebase (audioProcessor.getSampleRate(),
                              audioProcessor.getScopeDecimationFactor());
    spectrumAnalyzer.setSampleRate (audioProcessor.getSampleRate());
    oscilloscope.setDelayParameter (audioProcessor.apvts.getParameter ("delay_ms"));
    oscilloscope.setVisualOffsetParameter (audioProcessor.apvts.getParameter ("visualOffsetSamples"));
    pushScopeSettings();

    // Seed the analysis result from the processor: closing and reopening the
    // editor must not lose an already-computed fix (haveResult otherwise only
    // becomes true on an AnalyzeState transition this instance observes, so
    // Apply Fix would stay disabled until the user re-analyzed).
    latestResult = audioProcessor.getLatestFixResult();
    haveResult = latestResult.applyAllowed || latestResult.optionalApplyAllowed;

    resizeConstrainer.setSizeLimits (kMinEditorWidth, kMinEditorHeight,
                                     kMaxEditorWidth, kMaxEditorHeight);
    setConstrainer (&resizeConstrainer);
    setResizable (true, true);

    addAndMakeVisible (splitter);

    const int savedWidth = (int) audioProcessor.apvts.state.getProperty ("editorWidth", kDefaultEditorWidth);
    const int savedHeight = (int) audioProcessor.apvts.state.getProperty ("editorHeight", kDefaultEditorHeight);
    bottomPanelHeight = (int) audioProcessor.apvts.state.getProperty ("bottomPanelHeight", 252);

    setSize (juce::jlimit (kMinEditorWidth, kMaxEditorWidth, juce::jmax (savedWidth, kDefaultEditorWidth)),
             juce::jlimit (kMinEditorHeight, kMaxEditorHeight, juce::jmax (savedHeight, kDefaultEditorHeight)));
    startTimerHz (30);
}

KickLockAudioProcessorEditor::~KickLockAudioProcessorEditor()
{
    stopTimer();
    audioProcessor.setSpectrumCaptureEnabled (false);
    setLookAndFeel (nullptr);
}

void KickLockAudioProcessorEditor::configureRotary (juce::Slider& slider)
{
    slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 78, 18);
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

void KickLockAudioProcessorEditor::setChromeVisible (bool shouldBeVisible)
{
    const bool showScopeToolbar = shouldBeVisible || cleanScopeMode;
    gridCombo.setVisible (shouldBeVisible);
    viewCombo.setVisible (true);
    correctionModeSelector.setVisible (shouldBeVisible);
    freezeButton.setVisible (showScopeToolbar);
    relockKickButton.setVisible (showScopeToolbar);
    analyzeButton.setVisible (shouldBeVisible);
    applyFixButton.setVisible (shouldBeVisible);
    discardButton.setVisible (shouldBeVisible && showingLearnWorkflow);
    revertButton.setVisible (shouldBeVisible);
    compareAButton.setVisible (shouldBeVisible);
    compareBButton.setVisible (shouldBeVisible);
    compareCopyButton.setVisible (shouldBeVisible);
    helpButton.setVisible (shouldBeVisible);

    correlationDisplay.setVisible (shouldBeVisible);
    manualDelayOverlay.setVisible (shouldBeVisible);
    noSidechainOverlay.setVisible (shouldBeVisible);
    splitter.setVisible (shouldBeVisible);

    manualHeader.setVisible (shouldBeVisible);
    delaySlider.setVisible (shouldBeVisible);
    polarityInvertButton.setVisible (shouldBeVisible);
    phaseFilterButton.setVisible (shouldBeVisible);
    pitchTrackButton.setVisible (shouldBeVisible);
    phaseFreqSlider.setVisible (shouldBeVisible);
    phaseQSlider.setVisible (shouldBeVisible);
    visualOffsetSlider.setVisible (shouldBeVisible);
    dynamicStrengthSlider.setVisible (shouldBeVisible);
    crossoverEnableButton.setVisible (shouldBeVisible);
    crossoverSlider.setVisible (shouldBeVisible);

    delayLabel.setVisible (shouldBeVisible);
    polarityLabel.setVisible (shouldBeVisible);
    phaseFilterLabel.setVisible (shouldBeVisible);
    pitchTrackLabel.setVisible (shouldBeVisible);
    phaseFreqLabel.setVisible (shouldBeVisible);
    phaseQLabel.setVisible (shouldBeVisible);
    visualOffsetLabel.setVisible (shouldBeVisible);
    dynamicStrengthLabel.setVisible (shouldBeVisible);
    crossoverEnableLabel.setVisible (shouldBeVisible);
    crossoverLabel.setVisible (shouldBeVisible);

    advancedHeader.setVisible (shouldBeVisible);
    delayInterpCombo.setVisible (shouldBeVisible);
    phaseStagesCombo.setVisible (shouldBeVisible);
    delayInterpLabel.setVisible (shouldBeVisible);
    phaseStagesLabel.setVisible (shouldBeVisible);

    analyzerTitle.setVisible (shouldBeVisible);
    analyzerBody.setVisible (shouldBeVisible);
    transientPunch.setVisible (shouldBeVisible && ! showingLearnWorkflow);
    learnProgressDisplay.setVisible (shouldBeVisible && showingLearnWorkflow);
    setRefButton.setVisible (shouldBeVisible);
}

void KickLockAudioProcessorEditor::pushScopeSettings()
{
    int persistedViewIndex = 0;
    if (const auto* view = audioProcessor.apvts.getRawParameterValue ("scopeViewMode"))
        persistedViewIndex = (int) std::lround (view->load());

    const auto mode = scopeViewModeFromChoiceIndex (persistedViewIndex);
    const int uiIndex = uiIndexFromScopeViewMode (mode);
    if (viewCombo.getSelectedItemIndex() != uiIndex)
        viewCombo.setSelectedItemIndex (uiIndex, juce::dontSendNotification);

    oscilloscope.setViewMode (mode);
    updateVisualOffsetAvailability (mode);
    gridCombo.setVisible (! cleanScopeMode || scopeFullModeShowsGrid (mode));
    if (cleanScopeMode)
        relockKickButton.setVisible (mode == ScopeViewMode::Triggered);
    
    if (mode == ScopeViewMode::Spectrum)
    {
        oscilloscope.setVisible (false);
        spectrumAnalyzer.setVisible (true);
        audioProcessor.setSpectrumCaptureEnabled (true);
    }
    else
    {
        oscilloscope.setVisible (true);
        spectrumAnalyzer.setVisible (false);
        audioProcessor.setSpectrumCaptureEnabled (false);
    }
    oscilloscope.setGridDivision (gridDivisionFromChoiceIndex (gridCombo.getSelectedItemIndex()));

    if (const auto* offset = audioProcessor.apvts.getRawParameterValue ("visualOffsetSamples"))
        oscilloscope.setVisualOffsetSamples ((int) std::lround (offset->load()));
}

void KickLockAudioProcessorEditor::updateVisualOffsetAvailability (ScopeViewMode mode)
{
    const bool supported = scopeModeSupportsVisualOffset (mode);
    visualOffsetSlider.setEnabled (supported);
    visualOffsetSlider.setAlpha (supported ? 1.0f : 0.35f);
    visualOffsetLabel.setAlpha (supported ? 1.0f : 0.35f);
    visualOffsetSlider.setTooltip (supported
                                     ? "Display-only sample shift applied to the bass wave on the scope so you can align phases visually. Does not affect the sound."
                                     : "Visual Offset is available only in Separate and Phase Delta modes.");
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

    // Sustained strongly-negative low-end correlation almost always means the
    // bass is anti-phase: say so in words instead of leaving the user staring
    // at a low percentage. Hysteresis in the helper keeps it from flickering.
    polarityHintVisible = shouldShowPolarityHint (polarityHintVisible,
                                                  audioProcessor.liveLowEndMatchPercent.load(),
                                                  audioProcessor.liveMatchValid.load());

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
            const int beforePercent = (int) std::round (juce::jlimit (0.0f, 100.0f, latestResult.displayBeforeMatchPercent));
            const int afterPercent = (int) std::round (juce::jlimit (0.0f, 100.0f, latestResult.displayAfterMatchPercent));
            
            juce::String body;
            if (latestResult.quality == PhaseFixQuality::StrongImprovement
                || latestResult.quality == PhaseFixQuality::PartialImprovement)
            {
                body << (latestResult.quality == PhaseFixQuality::StrongImprovement
                             ? "Fix found: Predicted WTD "
                             : "Partial fix: Predicted WTD ")
                     << juce::String (beforePercent) << "% -> "
                     << juce::String (afterPercent) << "%"
                     << (latestResult.quality == PhaseFixQuality::PartialImprovement
                             ? ". Apply if it sounds better."
                             : ".");
            }
            else
            {
                body << latestResult.message;
            }

            if (resultCanApply)
            {
                body << "\n\nRecommended Delay: " << formatSignedDelayMs (latestResult.bassDelayMs) << "\n"
                     << "Polarity: " << (latestResult.bassPolarityInvert ? "Invert" : "Normal") << "\n";

                if (latestResult.phaseFilterEnabled)
                    body << "Phase Filter: " << juce::String ((int) std::round (latestResult.phaseFilterFreqHz))
                         << " Hz, Q " << juce::String (latestResult.phaseFilterQ, 2) << "\n";
                else
                    body << "Phase Filter: off\n";
                
                body << "Confidence: " << juce::String ((int) std::round (latestResult.confidence * 100.0f)) << "%";
            }
            else
            {
                if (latestResult.quality == PhaseFixQuality::AlreadyGood)
                    body << "\n\nNo applicable correction is needed.";
                else
                    body << "\n\nNo applicable bass-path change was found.";

                body << "\n\nCurrent Predicted WTD: " << juce::String (beforePercent) << "%";
            }

            if (resultCanApply && latestResult.largeTimingOffset)
                body << "\n\nWarning: Detected offset is " << juce::String (latestResult.detectedTimingOffsetMs, 1) << " ms, which exceeds the max delay of " << juce::String(PhaseFixEngine::defaultAutoFixMaxDelayMs, 1) << " ms.";
            else if (resultCanApply && latestResult.requiresTimelineMove)
                body << "\n\nWarning: This correction conceptually needs a DAW timeline move (" << juce::String(latestResult.suggestedKickMoveMs, 1) << " ms). Delay was clamped to 0 ms for a best-effort fix.";
            else if (resultCanApply && latestResult.unstableRecommendation)
                body << "\n\nWarning: Different hits need different corrections; result is a best-effort consensus.";

            analyzerBody.setText (body, juce::dontSendNotification);
        }
        else
        {
            analyzerBody.setText (latestResult.message.isNotEmpty()
                                      ? latestResult.message
                                      : "Analyze did not find enough usable kick and bass material.",
                                  juce::dontSendNotification);
        }

        audioProcessor.acknowledgeAnalyzeState();
    }

    lastAnalyzeState = state;

    const auto verification = audioProcessor.getLatestFixResult();
    if (verification.verifiedAfterMatchPercent >= 0.0f
        && std::abs (verification.verifiedAfterMatchPercent - latestResult.verifiedAfterMatchPercent) > 1.0e-4f)
    {
        latestResult = verification;
        analyzerBody.setText (analyzerBody.getText() + "\n\nVerified WTD: "
                                  + juce::String ((int) std::round (juce::jlimit (0.0f, 100.0f,
                                                                                  verification.verifiedAfterMatchPercent)))
                                  + "%",
                              juce::dontSendNotification);
    }

    const bool canApply = applyFixAvailable (hasSidechain, haveResult);
    applyFixButton.setEnabled (canApply);
    revertButton.setEnabled (audioProcessor.hasRevertSnapshot());
}

void KickLockAudioProcessorEditor::refreshDynamicWorkflow()
{
    const bool dynamic = audioProcessor.apvts.getRawParameterValue ("correction_mode")->load() > 0.5f;
    const auto learnState = audioProcessor.getLearnState();
    const bool showLearn = dynamic || learnState != LearnState::Idle;
    const bool wasShowingLearn = showingLearnWorkflow;
    showingLearnWorkflow = showLearn;
    dynamicModeSelected = dynamic;

    const bool learnBusy = learnStateIsBusy (learnState);
    correctionModeSelector.setEnabled (! learnBusy);
    dynamicStrengthSlider.setEnabled (dynamic);
    dynamicStrengthSlider.setAlpha (dynamic ? 1.0f : 0.45f);
    dynamicStrengthLabel.setAlpha (dynamic ? 1.0f : 0.45f);
    pitchTrackButton.setEnabled (! dynamic);
    pitchTrackButton.setAlpha (dynamic ? 0.45f : 1.0f);
    if (dynamic)
    {
        pitchTrackLabel.setText ("Pitch (ignored)", juce::dontSendNotification);
        pitchTrackButton.setTooltip ("Pitch Follow is ignored in Dynamic mode. Its saved value is unchanged.");
    }
    else
    {
        pitchTrackButton.setTooltip ("Continuously tunes the Phase Filter to the bass's detected "
                                     "fundamental, so the phase correction stays on the note as the "
                                     "bassline moves. A static phase filter detunes the moment the "
                                     "bass changes notes.");
    }

    if (! showLearn)
    {
        transientPunch.setVisible (! cleanScopeMode);
        learnProgressDisplay.setVisible (false);
        discardButton.setVisible (false);
        setRefButton.setButtonText (audioProcessor.isTransientPunchReferenceSet() ? "Clear Ref" : "Set Ref");
        setRefButton.setEnabled (true);
        setRefButton.setTooltip ("Stores the current kick-punch reading as a reference, then shows the live delta against it.");
        if (wasShowingLearn)
            resized();
        return;
    }

    latestLearnProgress = audioProcessor.getLearnProgress();
    const bool hasPending = audioProcessor.hasPendingLearnResult();
    const auto pendingResult = hasPending ? audioProcessor.getPendingLearnResult() : LearnFinalizeResult {};
    // Runtime status must describe the applied processor-owned map. A pending
    // Learn result is intentionally inert until Apply Learn succeeds.
    latestNoteMap = audioProcessor.getNoteMapSnapshot();
    const int activeMidi = audioProcessor.activeMidiNote.load (std::memory_order_acquire);
    learnProgressDisplay.setModel (latestLearnProgress, latestNoteMap, activeMidi);

    const auto primary = primaryWorkflowPresentation (dynamic, learnState, canStartAnalyze,
                                                      analyzeStateIsBusy (audioProcessor.getAnalyzeState()));
    analyzeButton.setButtonText (primaryWorkflowText (primary, latestLearnProgress.capturedHits));
    analyzeButton.setEnabled (primary.enabled);
    analyzeButton.setTooltip (primary.action == PrimaryWorkflowAction::StopLearn
                                  ? "Stops new Learn captures and finishes the current captured material."
                                  : "Starts a Dynamic Learn session. Nothing changes until Apply Learn.");

    const bool resultReady = learnState == LearnState::ResultReady && hasPending;
    const auto blockedReason = resultReady ? audioProcessor.getLearnApplyBlockedReason() : juce::String {};
    applyFixButton.setVisible (resultReady || ! dynamic);
    applyFixButton.setButtonText (resultReady ? "Apply Learn" : "Apply Fix");
    applyFixButton.setEnabled (resultReady && blockedReason.isEmpty());
    applyFixButton.setTooltip (blockedReason.isNotEmpty() ? blockedReason
                              : "Applies the pending Learn map and global correction. Nothing has been applied yet.");
    discardButton.setVisible (learnState == LearnState::ResultReady
                              || learnState == LearnState::NotEnoughMaterial || learnState == LearnState::Failed);
    discardButton.setEnabled (discardButton.isVisible());
    revertButton.setEnabled (audioProcessor.hasRevertSnapshot());

    transientPunch.setVisible (false);
    learnProgressDisplay.setVisible (! cleanScopeMode);
    setRefButton.setButtonText ("Clear Map");
    setRefButton.setEnabled (! learnBusy && audioProcessor.hasValidNoteMap());
    setRefButton.setTooltip ("Clears the applied note map. Revert restores the previous map when available.");

    juce::String body;
    if (learnState == LearnState::ResultReady)
    {
        int learnedNotes = 0;
        for (const auto& entry : pendingResult.map.notes)
            learnedNotes += NoteMap::isValidNoteEntry (entry) ? 1 : 0;
        body << "Learned " << learnedNotes << " notes from " << pendingResult.diagnostics.analyzedHits
             << " usable hits.\nGlobal base: " << formatSignedDelayMs (pendingResult.globalFix.bassDelayMs)
             << ", " << (pendingResult.globalFix.bassPolarityInvert ? "invert" : "normal")
             << " polarity, " << pendingResult.map.base.allpassStages << " stages.";
        if (pendingResult.diagnostics.rejectedPitchHits > 0 || pendingResult.diagnostics.unusableSignalHits > 0)
            body << "\nRejected " << pendingResult.diagnostics.rejectedPitchHits << " pitch / "
                 << pendingResult.diagnostics.unusableSignalHits << " unusable hits.";
        if (pendingResult.diagnostics.warning.isNotEmpty())
            body << "\nWarning: " << pendingResult.diagnostics.warning;
        else if (pendingResult.diagnostics.multipleTimingFamilies)
            body << "\nWarning: multiple timing families; using the dominant consensus.";
        body << "\n\nNothing has been applied yet.";
    }
    else if (learnState == LearnState::NotEnoughMaterial || learnState == LearnState::Failed)
    {
        body = pendingResult.message.isNotEmpty() ? pendingResult.message
                                                   : "Learn needs more usable kick and bass hits. Play the loop, then try again.";
    }
    else
    {
        body << "Captured " << latestLearnProgress.capturedHits << " hits, analyzed "
             << latestLearnProgress.drainedHits << ".\nQueue " << latestLearnProgress.pendingQueueHits
             << ", dropped " << latestLearnProgress.droppedQueueHits
             << ", overlaps " << latestLearnProgress.ignoredOverlappingTriggers << ".";
    }

    const auto runtime = dynamicRuntimeStatus (dynamic, audioProcessor.dynamicMapStale.load (std::memory_order_acquire),
                                               NoteMap::isValidNoteMap (latestNoteMap),
                                               phaseFilterButton.getToggleState(),
                                               audioProcessor.dynamicFallbackActive.load (std::memory_order_acquire));
    analyzerTitle.setText (dynamic ? "DYNAMIC - " + dynamicRuntimeStatusText (runtime, activeMidi) : "LEARN", juce::dontSendNotification);
    analyzerBody.setText (body, juce::dontSendNotification);
    if (wasShowingLearn != showingLearnWorkflow)
        resized();
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

void KickLockAudioProcessorEditor::timerCallback()
{
    oscilloscope.setTimebase (audioProcessor.getSampleRate(),
                              audioProcessor.getScopeDecimationFactor());
    spectrumAnalyzer.setSampleRate (audioProcessor.getSampleRate());
    oscilloscope.setTempoInfo (audioProcessor.getLatestBpm(),
                               audioProcessor.isTempoAvailable());
    pushScopeSettings();

    const bool relockPending = oscilloscope.getKickReferenceState() == KickReferenceState::RelockPending;
    relockKickButton.setButtonText (relockPending ? "Re-locking..." : "Re-lock");
    relockKickButton.setEnabled (! relockPending);
    relockKickButton.setTooltip (relockPending
                                   ? "Waiting for the next valid kick transient to replace the reference."
                                   : "Waits for the next kick transient and stores it as the triggered-scope reference.");

    const bool hasSidechainForPunch = audioProcessor.hasSidechainReference();
    const bool punchValid = hasSidechainForPunch && audioProcessor.isTransientPunchValid();
    transientPunch.setValues (audioProcessor.getTransientPunchDb(),
                              punchValid,
                              hasSidechainForPunch,
                              audioProcessor.isTransientPunchReferenceSet(),
                              audioProcessor.getTransientPunchReferenceDb(),
                              punchValid ? audioProcessor.getTransientKickPeak() : 0.0f,
                              punchValid ? audioProcessor.getTransientSumPeak() : 0.0f);
    setRefButton.setButtonText (audioProcessor.isTransientPunchReferenceSet() ? "Clear Ref" : "Set Ref");

    // Live pitch readout: while Follow Bass is on, the label shows the tracked
    // fundamental the phase filter is currently tuned to. Label::setText
    // no-ops (no repaint) when the text is unchanged.
    const float trackedHz = audioProcessor.trackedBassHz.load();
    pitchTrackLabel.setText (pitchTrackButton.getToggleState() && trackedHz > 0.0f
                                 ? "Pitch " + juce::String ((int) std::lround (trackedHz)) + " Hz"
                                 : "Pitch",
                             juce::dontSendNotification);

    const auto oldSidechainStatusText = sidechainStatusText;
    const auto oldSidechainStatusColour = sidechainStatusColour;
    const auto oldBpmText = bpmText;
    const auto oldPdcText = pdcText;
    const auto oldPolarityHintVisible = polarityHintVisible;

    refreshStatusStrings();
    refreshAnalyzeWorkflow();
    refreshDynamicWorkflow();
    refreshCompareButtons();

    // Only the top-bar status texts (sidechain state, BPM, PDC) are drawn
    // directly by this editor's paint() and change per tick; the scope,
    // correlation meter, overlays, analyzer body and kick-punch meter are all
    // child components that repaint themselves when their own state changes.
    // Repainting the whole window at 30 Hz forced the scope to redraw on an
    // irregular cadence and wasted CPU, so invalidate just the top bar here.
    if (sidechainStatusText != oldSidechainStatusText ||
        sidechainStatusColour != oldSidechainStatusColour ||
        bpmText != oldBpmText ||
        pdcText != oldPdcText ||
        polarityHintVisible != oldPolarityHintVisible)
    {
        repaint (0, 0, getWidth(), kTopBarHeight);
    }
}

void KickLockAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (background);

    if (cleanScopeMode)
    {
        auto toolbar = getLocalBounds().removeFromTop (42);
        g.setColour (panel);
        g.fillRect (toolbar);
        g.setColour (border);
        g.drawLine ((float) toolbar.getX(), (float) toolbar.getBottom(),
                    (float) toolbar.getRight(), (float) toolbar.getBottom(), 1.0f);

        auto info = toolbar.reduced (8, 5);
        g.setColour (text);
        g.setFont (juce::Font (juce::FontOptions (14.0f)).boldened());
        g.drawText ("KICKLOCK", info.removeFromLeft (94), juce::Justification::centredLeft);

        g.setColour (sidechainStatusColour);
        g.setFont (juce::Font (juce::FontOptions (11.0f)).boldened());
        g.drawText (sidechainStatusText, info.removeFromLeft (154), juce::Justification::centredLeft);

        g.setColour (mutedText);
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText (bpmText + "  |  " + pdcText, info.removeFromLeft (146),
                    juce::Justification::centredLeft);
        return;
    }

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
    infoRow.removeFromRight (118);

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

    if (polarityHintVisible)
    {
        g.setColour (amber);
        g.setFont (juce::Font (juce::FontOptions (11.5f)).boldened());
        g.drawText ("low end cancelling - try Invert", infoRow,
                    juce::Justification::centredRight);
    }

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
    if (cleanScopeMode)
        return;

    if (! helpOverlayVisible)
        return;

    auto bounds = getLocalBounds().reduced (40, 40);
    g.setColour (juce::Colours::black.withAlpha (0.75f));
    g.fillAll();

    g.setColour (panel);
    g.fillRoundedRectangle (bounds.toFloat(), 8.0f);
    g.setColour (orange.withAlpha (0.8f));
    g.drawRoundedRectangle (bounds.toFloat(), 8.0f, 1.4f);

    auto area = bounds.reduced (32, 28);
    
    g.setColour (text);
    g.setFont (juce::Font (juce::FontOptions (22.0f)).boldened());
    g.drawText ("KickLock - User Guide", area.removeFromTop (28), juce::Justification::centred);
    area.removeFromTop (12);

    auto drawBlock = [&](const juce::String& title, const juce::String& body, int bodyHeight) {
        g.setColour (orange);
        g.setFont (juce::Font (juce::FontOptions (15.0f)).boldened());
        g.drawText (title, area.removeFromTop (20), juce::Justification::centredLeft);
        
        g.setColour (mutedText);
        g.setFont (juce::Font (juce::FontOptions (13.5f)));
        g.drawFittedText (body, area.removeFromTop (bodyHeight), juce::Justification::topLeft, 10);
        area.removeFromTop (10);
    };

    drawBlock("What it does", 
              "KickLock fixes low-end phase cancellation between your kick and bass. "
              "By aligning their phase, it restores lost punch and guarantees a tight, powerful low end.", 
              38);

    drawBlock("How it works",
              "1. Place KickLock on Bass; route the Kick to its Sidechain input.\n"
              "2. Choose Static or Dynamic mode.\n"
              "3. Static: play and click Analyze for one fixed Delay, Polarity, Freq, and Q correction.\n"
              "4. Dynamic: click Learn and play the full bass part. Captured-note hit chips show counts; trusted notes get separate Freq/Q. Delay and Polarity stay global.\n"
              "5. Apply Fix (Static) or Apply Learn (Dynamic). Dynamic maps are saved with the project.",
              72);

    drawBlock("Key Features",
              "- Phase Filter (Allpass): Rotates phase smoothly without shifting the audio in time.\n"
              "- Delay & Polarity: Aligns transients perfectly with sub-millisecond precision.\n"
              "- Crossover: Ensures only the low frequencies (e.g., < 150 Hz) are affected, leaving mids/highs untouched.\n"
              "- A/B Compare: Easily switch between two configurations to verify improvements.\n"
              "- Dynamic Mode: learns a separate Freq/Q phase correction for each trusted bass note. Delay and Polarity remain global, while unknown or low-confidence notes fall back to the global correction.",
              72);

    drawBlock("Meters & Displays",
              "- Live Scope: Visualizes the Kick (sidechain) and Bass waveforms in real-time.\n"
              "- Correlation / Phase Match: Shows how well the frequencies are aligned (100% = perfect, 50% = neutral, 0% = cancelling).\n"
              "- Transient Punch: Measures the actual dB gain/loss at the exact moment the kick hits.",
              58);
              
    drawBlock("Sidechain Routing",
              "Ableton: Sidechain drop-down in plugin header.  |  FL Studio: Wrapper settings -> Processing. \n"
              "Logic: Sidechain menu in header.  |  Cubase: Activate sidechain icon, route kick send. \n"
              "Reaper: Route kick to plugin channels 3/4.",
              54);
              
    g.setColour (orange);
    g.setFont (juce::Font (juce::FontOptions (13.0f)).boldened());
    g.drawText ("Press ? to close", area.removeFromBottom (18), juce::Justification::centredRight);
}

void KickLockAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds();
    setChromeVisible (! cleanScopeMode);

    if (cleanScopeMode)
    {
        manualPanelBounds = {};
        analyzerPanelBounds = {};
        auto cleanToolbar = bounds.removeFromTop (42);
        auto controls = cleanToolbar.reduced (8, 5);
        controls.removeFromLeft (394);

        gridCombo.setBounds (controls.removeFromLeft (66).reduced (0, 1));
        controls.removeFromLeft (5);
        viewCombo.setBounds (controls.removeFromLeft (102).reduced (0, 1));
        controls.removeFromLeft (5);
        freezeButton.setBounds (controls.removeFromLeft (64).reduced (0, 1));
        controls.removeFromLeft (5);
        relockKickButton.setBounds (controls.removeFromLeft (64).reduced (0, 1));

        const auto scopeBounds = bounds.reduced (8);
        oscilloscope.setBounds (scopeBounds);
        spectrumAnalyzer.setBounds (scopeBounds);
        viewCombo.toFront (false);
        gridCombo.toFront (false);
        freezeButton.toFront (false);
        relockKickButton.toFront (false);
        pushScopeSettings();
        repaint();
        return;
    }

    // --- Top bar -----------------------------------------------------------
    auto topBar = bounds.removeFromTop (kTopBarHeight).reduced (14, 6);
    auto infoRow = topBar.removeFromTop (20);
    correctionModeSelector.setBounds (infoRow.removeFromRight (118).reduced (0, 1));
    auto controls = topBar.removeFromBottom (30);

    gridCombo.setBounds (controls.removeFromLeft (68).reduced (0, 3));
    controls.removeFromLeft (5);
    viewCombo.setBounds (controls.removeFromLeft (96).reduced (0, 3));
    controls.removeFromLeft (5);

    freezeButton.setBounds (controls.removeFromLeft (56).reduced (0, 2));
    controls.removeFromLeft (6);
    relockKickButton.setBounds (controls.removeFromLeft (56).reduced (0, 2));
    controls.removeFromLeft (8);
    compareAButton.setBounds (controls.removeFromLeft (30).reduced (0, 2));
    controls.removeFromLeft (4);
    compareBButton.setBounds (controls.removeFromLeft (30).reduced (0, 2));
    controls.removeFromLeft (4);
    compareCopyButton.setBounds (controls.removeFromLeft (48).reduced (0, 2));
    controls.removeFromLeft (10);
    analyzeButton.setBounds (controls.removeFromLeft (156).reduced (0, 2));
    controls.removeFromLeft (6);
    applyFixButton.setBounds (controls.removeFromLeft (82).reduced (0, 2));
    controls.removeFromLeft (6);
    discardButton.setBounds (controls.removeFromLeft (64).reduced (0, 2));
    controls.removeFromLeft (6);
    revertButton.setBounds (controls.removeFromLeft (64).reduced (0, 2));
    controls.removeFromLeft (6);
    helpButton.setBounds (controls.removeFromLeft (30).reduced (0, 2));

    bounds.reduce (14, 12);

    // --- Live hero + scope overlays ---------------------------------------
    splitter.setBottomHeightBase (bottomPanelHeight);
    
    constexpr int scopeLowerGap = 10;
    bottomPanelHeight = juce::jlimit (100, bounds.getHeight() - 150, bottomPanelHeight);
    const int scopeBlockHeight = bounds.getHeight() - bottomPanelHeight - scopeLowerGap;
    
    auto scopeBlock = bounds.removeFromTop (scopeBlockHeight);
    // +16 over the historical 108 for the sub-loss-dB headline row (see
    // CorrelationDisplay::paint) — taken from the scope block, not the bottom
    // panel, so the manual-alignment layout below is unaffected.
    correlationDisplay.setBounds (scopeBlock.removeFromTop (124));
    scopeBlock.removeFromTop (6);
    auto scopeArea = scopeBlock;
    oscilloscope.setBounds (scopeArea);
    spectrumAnalyzer.setBounds (scopeArea);

    noSidechainOverlay.setBounds (scopeArea.withSizeKeepingCentre (scopeArea.getWidth(), 24));

    auto markerRow = scopeArea.removeFromBottom (18);
    manualDelayOverlay.setBounds (markerRow.reduced (6, 0));

    bounds.removeFromTop (2);
    splitter.setBounds (bounds.removeFromTop (6));
    bounds.removeFromTop (2);

    // --- Lower area: manual controls (left) + analyzer/live (right) -------
    // The analyzer column scales gently with the window so it doesn't look
    // orphaned at very wide sizes.
    auto lower = bounds;
    auto right = lower.removeFromRight (juce::jlimit (280, 360, getWidth() / 4));
    lower.removeFromRight (12);
    auto manualArea = lower;

    manualPanelBounds = manualArea;
    analyzerPanelBounds = right;
    manualArea = manualArea.reduced (14, 12);
    right = right.reduced (12, 12);

    // Manual alignment panel.
    manualHeader.setBounds (manualArea.removeFromTop (20));
    manualArea.removeFromTop (2);

    // Row 1 (rotaries) grows to fill whatever height the resizable bottom panel
    // leaves after the toggle row, advanced row and their headers, so dragging
    // the splitter taller enlarges the knobs instead of just adding dead space
    // below them. The lower rows need a fixed ~94 px; the jlimit keeps the knobs
    // comfortably large without starving those rows at small sizes.
    constexpr int lowerRowsHeight = 96; // row2 + advanced header + advanced row + gaps
    const int rowH1 = juce::jlimit (96, 150, manualArea.getHeight() - lowerRowsHeight);
    auto row1 = manualArea.removeFromTop (rowH1);
    const int delayW = juce::jlimit (100, 160, (manualArea.getWidth() - 32) * 2 / 6);
    const int knobW = juce::jmax (70, (manualArea.getWidth() - delayW - 32) / 4);

    auto delayCell = row1.removeFromLeft (delayW);
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

    manualArea.removeFromTop (2);
    auto row2 = manualArea.removeFromTop (38);
    auto polCell = row2.removeFromLeft (knobW);
    polarityLabel.setBounds (polCell.removeFromTop (14));
    polarityInvertButton.setBounds (polCell.removeFromTop (24));

    row2.removeFromLeft (8);
    auto crossEnCell = row2.removeFromLeft (knobW);
    crossoverEnableLabel.setBounds (crossEnCell.removeFromTop (14));
    crossoverEnableButton.setBounds (crossEnCell.removeFromTop (24));

    row2.removeFromLeft (8);
    auto phCell = row2.removeFromLeft (knobW * 2);
    phaseFilterLabel.setBounds (phCell.removeFromTop (14));
    phaseFilterButton.setBounds (phCell.removeFromTop (24));

    row2.removeFromLeft (8);
    auto pitchCell = row2.removeFromLeft (knobW);
    pitchTrackLabel.setBounds (pitchCell.removeFromTop (14));
    pitchTrackButton.setBounds (pitchCell.removeFromTop (24));

    manualArea.removeFromTop (2);
    advancedHeader.setBounds (manualArea.removeFromTop (16));
    manualArea.removeFromTop (2);
    auto advRow = manualArea.removeFromTop (34);
    const int advancedCellWidth = juce::jmax (80, (advRow.getWidth() - 16) / 3);
    auto interpCell = advRow.removeFromLeft (advancedCellWidth);
    delayInterpLabel.setBounds (interpCell.removeFromTop (12));
    delayInterpCombo.setBounds (interpCell.removeFromTop (22).reduced (0, 1));
    advRow.removeFromLeft (8);
    auto stagesCell = advRow.removeFromLeft (advancedCellWidth);
    phaseStagesLabel.setBounds (stagesCell.removeFromTop (12));
    phaseStagesCombo.setBounds (stagesCell.removeFromTop (22).reduced (0, 1));
    advRow.removeFromLeft (8);
    dynamicStrengthLabel.setBounds (advRow.removeFromTop (12));
    dynamicStrengthSlider.setBounds (advRow.removeFromTop (22).reduced (0, 1));

    // Right column: kick-punch meter, its reference button, then analyzer.
    transientPunch.setBounds (right.removeFromTop (84));
    learnProgressDisplay.setBounds (transientPunch.getBounds());
    right.removeFromTop (4);
    setRefButton.setBounds (right.removeFromTop (22).reduced (0, 1));
    right.removeFromTop (4);
    analyzerTitle.setBounds (right.removeFromTop (20));
    analyzerBody.setBounds (right);

    audioProcessor.apvts.state.setProperty ("editorWidth", getWidth(), nullptr);
    audioProcessor.apvts.state.setProperty ("editorHeight", getHeight(), nullptr);
    audioProcessor.apvts.state.setProperty ("bottomPanelHeight", bottomPanelHeight, nullptr);
    
    pushScopeSettings();
    repaint();
}
