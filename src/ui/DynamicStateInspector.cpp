#include "DynamicStateInspector.h"

namespace
{
    const auto kPanel = juce::Colour (0xff171d23);
    const auto kBorder = juce::Colour (0xff2b3540);
    const auto kText = juce::Colour (0xffedf2f7);
    const auto kMuted = juce::Colour (0xff97a5b2);
    const auto kTeal = juce::Colour (0xff2dd4bf);
    const auto kAmber = juce::Colour (0xfff59e0b);

    constexpr int kThrottleIntervalMs = 30;
}

DynamicStateInspector::DynamicStateInspector()
{
    setName ("Dynamic State Inspector");
    setDescription ("Inspects and edits the selected Dynamic State.");

    for (auto* label : { &identityLabel, &recognitionLabel, &correctionLabel, &measurementLabel,
                        &delayLabel, &freqLabel, &qLabel })
    {
        label->setColour (juce::Label::textColourId, kText);
        addAndMakeVisible (*label);
    }
    identityLabel.setFont (juce::Font (juce::FontOptions (13.0f)).boldened());
    correctionLabel.setColour (juce::Label::textColourId, kAmber);

    delaySlider.setRange (DynamicStateMapContract::kManualDelayTrimMinMs,
                         DynamicStateMapContract::kManualDelayTrimMaxMs, 0.01);
    delaySlider.setTextValueSuffix (" ms");
    delaySlider.setName ("Delay trim");
    delaySlider.setDescription ("Additive Delay trim in milliseconds, applied on top of the learned or manual base package.");

    freqSlider.setRange (DynamicStateMapContract::kAllpassFrequencyMinHz,
                        DynamicStateMapContract::kAllpassFrequencyMaxHz, 0.1);
    freqSlider.setSkewFactorFromMidPoint (90.0);
    freqSlider.setTextValueSuffix (" Hz");
    freqSlider.setName ("Effective allpass frequency");
    freqSlider.setDescription ("Effective allpass frequency in Hz for the selected State.");

    qSlider.setRange (DynamicStateMapContract::kAllpassQMin, DynamicStateMapContract::kAllpassQMax, 0.001);
    qSlider.setName ("Effective allpass Q");
    qSlider.setDescription ("Effective allpass Q for the selected State.");

    for (auto* slider : { &delaySlider, &freqSlider, &qSlider })
    {
        slider->setWantsKeyboardFocus (true);
        addAndMakeVisible (*slider);
    }

    delaySlider.onValueChange = [this]
    {
        if (updatingSlidersProgrammatically) return;
        trimThrottle.setDraft (currentTrimFromSliders());
    };
    freqSlider.onValueChange = [this]
    {
        if (updatingSlidersProgrammatically) return;
        trimThrottle.setDraft (currentTrimFromSliders());
    };
    qSlider.onValueChange = [this]
    {
        if (updatingSlidersProgrammatically) return;
        trimThrottle.setDraft (currentTrimFromSliders());
    };
    delaySlider.onDragEnd = [this] { commitTrimNow(); };
    freqSlider.onDragEnd = [this] { commitTrimNow(); };
    qSlider.onDragEnd = [this] { commitTrimNow(); };

    enableButton.setName ("Enable or disable the selected State");
    enableButton.onClick = [this]
    {
        if (! model.hasSelectedState || ! onEdit) return;
        onEdit ({ DynamicStateInspectorAction::SetEnabled, model.selectedStableStateId,
                 {}, ! model.selectedState.enabled });
    };
    addAndMakeVisible (enableButton);

    bypassButton.setName ("Bypass or unbypass the selected State");
    bypassButton.onClick = [this]
    {
        if (! model.hasSelectedState || ! onEdit) return;
        onEdit ({ DynamicStateInspectorAction::SetBypassed, model.selectedStableStateId,
                 {}, ! model.selectedState.bypassed });
    };
    addAndMakeVisible (bypassButton);

    resetTrimButton.setName ("Reset manual trim");
    resetTrimButton.setDescription ("Zeroes only the manual trim; preserves the learned or manual base package.");
    resetTrimButton.onClick = [this]
    {
        if (! model.hasSelectedState || ! onEdit) return;
        trimThrottle.cancel();
        onEdit ({ DynamicStateInspectorAction::ResetManualTrim, model.selectedStableStateId, {}, false });
    };
    addAndMakeVisible (resetTrimButton);

    resetLearnedButton.setName ("Reset to learned package");
    resetLearnedButton.setDescription ("Auto States only: restores exactly what Learn found.");
    resetLearnedButton.onClick = [this]
    {
        if (! model.hasSelectedState || ! onEdit) return;
        trimThrottle.cancel();
        onEdit ({ DynamicStateInspectorAction::ResetToLearned, model.selectedStableStateId, {}, false });
    };
    addAndMakeVisible (resetLearnedButton);

    resetGlobalButton.setName ("Reset to Global");
    resetGlobalButton.setDescription ("Auto States only: keeps the State recognized, but stops correcting it.");
    resetGlobalButton.onClick = [this]
    {
        if (! model.hasSelectedState || ! onEdit) return;
        trimThrottle.cancel();
        onEdit ({ DynamicStateInspectorAction::ResetToGlobal, model.selectedStableStateId, {}, false });
    };
    addAndMakeVisible (resetGlobalButton);

    startTimer (kThrottleIntervalMs);
}

DynamicStateInspector::~DynamicStateInspector()
{
    stopTimer();
}

void DynamicStateInspector::timerCallback()
{
    publishTrimIfPending();
}

void DynamicStateInspector::publishTrimIfPending()
{
    if (! trimThrottle.hasPendingEdit() || ! onEdit || ! model.hasSelectedState)
        return;
    const auto trim = trimThrottle.takePending();
    onEdit ({ DynamicStateInspectorAction::SetManualTrim, model.selectedStableStateId, trim, false });
}

void DynamicStateInspector::commitTrimNow()
{
    // Final exact commit on drag end: bypass the throttle entirely so the
    // committed value is always exactly what the slider displays, never a
    // stale throttled draft.
    trimThrottle.cancel();
    if (! model.hasSelectedState || ! onEdit)
        return;
    onEdit ({ DynamicStateInspectorAction::SetManualTrim, model.selectedStableStateId,
             currentTrimFromSliders(), false });
}

DynamicManualTrim DynamicStateInspector::currentTrimFromSliders() const noexcept
{
    DynamicManualTrim trim = model.selectedState.manualTrim;
    trim.delayTrimMs = (float) delaySlider.getValue();

    const DynamicZonePackage* source = nullptr;
    const auto& state = model.selectedState;
    if (state.origin == DynamicStateOrigin::Auto && state.hasLearnedPackage)
        source = &state.learnedPackage;
    else if (state.origin == DynamicStateOrigin::Manual && state.hasManualBasePackage)
        source = &state.manualBasePackage;

    if (source != nullptr)
    {
        float semitones = trim.frequencyTrimSemitones;
        float damping = trim.logPoleDampingTrim;
        if (DynamicManualTrimConversion::manualTrimForEffectiveFrequencyQ (
                *source, freqSlider.getValue(), qSlider.getValue(), model.sampleRate, semitones, damping))
        {
            trim.frequencyTrimSemitones = semitones;
            trim.logPoleDampingTrim = damping;
        }
    }
    return trim;
}

void DynamicStateInspector::setModel (const DynamicWorkspaceViewModel& newModel)
{
    model = newModel;
    effective = dynamicEffectivePackageDisplay (model.selectedState, model.sampleRate);
    eligibility = dynamicStateControlEligibility (model);
    updateSlidersFromModel();
    updateControlEnablement();
    repaint();
}

void DynamicStateInspector::updateSlidersFromModel()
{
    // Never fight an in-progress drag: only reflect the model's committed
    // values into the sliders when the user isn't actively holding one down.
    const bool anyDragging = delaySlider.isMouseButtonDown() || freqSlider.isMouseButtonDown()
        || qSlider.isMouseButtonDown();
    if (anyDragging)
        return;

    updatingSlidersProgrammatically = true;
    if (model.hasSelectedState)
    {
        delaySlider.setValue (model.selectedState.manualTrim.delayTrimMs, juce::dontSendNotification);
        if (effective.valid)
        {
            freqSlider.setValue (effective.frequencyHz, juce::dontSendNotification);
            qSlider.setValue (effective.q, juce::dontSendNotification);
        }
    }
    updatingSlidersProgrammatically = false;
}

void DynamicStateInspector::updateControlEnablement()
{
    const bool hasPackage = model.hasSelectedState
        && ((model.selectedState.origin == DynamicStateOrigin::Auto && model.selectedState.hasLearnedPackage)
            || (model.selectedState.origin == DynamicStateOrigin::Manual && model.selectedState.hasManualBasePackage));
    const bool slidersEnabled = eligibility.controlsEnabled && hasPackage;
    delaySlider.setEnabled (slidersEnabled);
    freqSlider.setEnabled (slidersEnabled);
    qSlider.setEnabled (slidersEnabled);

    const auto sliderTooltip = eligibility.controlsEnabled
        ? (hasPackage ? juce::String() : juce::String ("This State has no editable package yet."))
        : eligibility.disabledReason;
    delaySlider.setTooltip (sliderTooltip);
    freqSlider.setTooltip (sliderTooltip);
    qSlider.setTooltip (sliderTooltip);

    enableButton.setEnabled (eligibility.controlsEnabled);
    bypassButton.setEnabled (eligibility.controlsEnabled && model.hasSelectedState && model.selectedState.enabled);
    resetTrimButton.setEnabled (slidersEnabled);
    resetLearnedButton.setEnabled (eligibility.controlsEnabled && model.hasSelectedState
        && model.selectedState.origin == DynamicStateOrigin::Auto);
    resetGlobalButton.setEnabled (eligibility.controlsEnabled && model.hasSelectedState
        && model.selectedState.origin == DynamicStateOrigin::Auto);

    if (model.hasSelectedState)
    {
        enableButton.setButtonText (model.selectedState.enabled ? "Disable" : "Enable");
        bypassButton.setButtonText (model.selectedState.bypassed ? "Unbypass" : "Bypass");
    }
}

void DynamicStateInspector::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour (kPanel);
    g.fillRoundedRectangle (bounds, 7.0f);
    g.setColour (kBorder);
    g.drawRoundedRectangle (bounds, 7.0f, 1.0f);

    if (! model.hasSelectedState)
    {
        g.setColour (kMuted);
        g.setFont (juce::Font (juce::FontOptions (12.0f)));
        g.drawText ("Select a State to inspect it.", getLocalBounds(), juce::Justification::centred);
        return;
    }

    if (model.lastEditFailed && model.lastEditFailureReason.isNotEmpty())
    {
        auto reasonArea = getLocalBounds().reduced (10, 4).removeFromBottom (16);
        g.setColour (juce::Colour (0xffef4444));
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText (model.lastEditFailureReason, reasonArea, juce::Justification::centredLeft, true);
    }
}

void DynamicStateInspector::resized()
{
    auto area = getLocalBounds().reduced (10, 8);
    if (! model.hasSelectedState)
        return;

    const auto& state = model.selectedState;
    {
        juce::String identity = "State " + shortDynamicStateId (state.stableStateId);
        if (state.hasLikelyMidi)
            identity << "  " << juce::MidiMessage::getMidiNoteName (state.likelyMidi, true, true, 4);
        else if (state.hasLikelyPitchHz)
            identity << "  " << juce::String ((int) std::lround (state.likelyPitchHz)) << " Hz";
        identity << "   ID " << fullDynamicStateId (state.stableStateId);
        identityLabel.setText (identity, juce::dontSendNotification);
    }
    identityLabel.setBounds (area.removeFromTop (18));

    {
        juce::String recognition = juce::String (dynamicStateOriginLabel (state.origin))
            + "  " + dynamicStateEvidenceLabel (state.evidence)
            + "   Hits " + juce::String (state.hitCount)
            + "   Repeat " + juce::String ((int) std::round (state.repeatability * 100.0f)) + "%"
            + "   Amb " + juce::String ((int) std::round (state.ambiguity * 100.0f)) + "%";
        recognitionLabel.setText (recognition, juce::dontSendNotification);
    }
    recognitionLabel.setBounds (area.removeFromTop (16));

    correctionLabel.setText (dynamicStateCorrectionStatusLine (state), juce::dontSendNotification);
    correctionLabel.setBounds (area.removeFromTop (18));

    area.removeFromTop (4);
    auto sliderRow = [&] (juce::Label& label, juce::Slider& slider)
    {
        auto row = area.removeFromTop (24);
        label.setBounds (row.removeFromLeft (110));
        slider.setBounds (row);
        area.removeFromTop (2);
    };
    sliderRow (delayLabel, delaySlider);
    sliderRow (freqLabel, freqSlider);
    sliderRow (qLabel, qSlider);

    area.removeFromTop (6);
    {
        auto measurementArea = area.removeFromTop (28);
        // Predicted/Verified summaries live on the card (DynamicStateCard),
        // not raw DynamicState; the workspace already renders them per-card.
        // The inspector's own line focuses on the effective package values.
        juce::String text = "Effective: Delay " + juce::String (effective.valid ? effective.delayDeltaMs : 0.0f, 2) + " ms"
             + "  Freq " + juce::String (effective.valid ? effective.frequencyHz : 0.0, 1) + " Hz"
             + "  Q " + juce::String (effective.valid ? effective.q : 0.0, 2);
        measurementLabel.setText (text, juce::dontSendNotification);
        measurementLabel.setBounds (measurementArea);
    }

    area.removeFromTop (4);
    auto buttonRow = area.removeFromTop (26);
    const int gap = 6;
    const int buttonWidth = (buttonRow.getWidth() - gap * 4) / 5;
    enableButton.setBounds (buttonRow.removeFromLeft (buttonWidth));
    buttonRow.removeFromLeft (gap);
    bypassButton.setBounds (buttonRow.removeFromLeft (buttonWidth));
    buttonRow.removeFromLeft (gap);
    resetTrimButton.setBounds (buttonRow.removeFromLeft (buttonWidth));
    buttonRow.removeFromLeft (gap);
    resetLearnedButton.setBounds (buttonRow.removeFromLeft (buttonWidth));
    buttonRow.removeFromLeft (gap);
    resetGlobalButton.setBounds (buttonRow);
}
