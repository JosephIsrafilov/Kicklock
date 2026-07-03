#include "PluginProcessor.h"
#include "PluginEditor.h"

KickLockAudioProcessorEditor::KickLockAudioProcessorEditor (KickLockAudioProcessor& p)
    : AudioProcessorEditor (&p),
      audioProcessor (p),
      oscilloscope (p.scopeFifo),
      correlationDisplay (p.correlationPercent)
{
    setLookAndFeel (&lookAndFeel);

    addAndMakeVisible (oscilloscope);
    addAndMakeVisible (correlationDisplay);

    // Freeze toggles the scope's scrolling so the waveform can be inspected.
    freezeButton.setButtonText ("Freeze");
    freezeButton.setClickingTogglesState (true);
    freezeButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff2dd4bf));
    freezeButton.onClick = [this]
    {
        oscilloscope.setFrozen (freezeButton.getToggleState());
    };
    addAndMakeVisible (freezeButton);

    // Analyze inspects the raw kick + bass and reports the safest correction.
    // Default mode is recommendation-only: the sidechain is a reference, not
    // audible kick audio this plugin can move.
    analyzeButton.setButtonText ("Analyze");
    analyzeButton.setColour (juce::TextButton::buttonColourId, juce::Colour (0xfff97316));
    analyzeButton.setTooltip ("Measure kick vs bass and recommend bass delay, polarity, or timeline edits.");
    analyzeButton.onClick = [this]
    {
        const auto hit = audioProcessor.analyzeLatestHit();

        if (hit.valid)
        {
            analyzeResultLabel.setText ("Latest hit: " + hit.instruction.message,
                                        juce::dontSendNotification);
            return;
        }

        const auto instruction = audioProcessor.analyzeAndApply();
        analyzeResultLabel.setText ("Capture: " + instruction.message, juce::dontSendNotification);
    };
    addAndMakeVisible (analyzeButton);

    analyzeResultLabel.setText ("Press Analyze for latest-hit kick/bass alignment.",
                                juce::dontSendNotification);
    analyzeResultLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    analyzeResultLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9ca3af));
    addAndMakeVisible (analyzeResultLabel);

    // Zoom controls mirror the scope's time/amplitude zoom; the scope's mouse
    // wheel writes back here through onZoomChanged so the two stay in sync.
    timeZoomSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    timeZoomSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 46, 20);
    timeZoomSlider.setRange (1.0, 16.0, 0.1);
    timeZoomSlider.setValue (1.0, juce::dontSendNotification);
    timeZoomSlider.setTooltip ("Horizontal zoom. Scroll over the scope to zoom too.");
    timeZoomSlider.onValueChange = [this]
    {
        oscilloscope.setTimeZoom ((float) timeZoomSlider.getValue());
    };
    addAndMakeVisible (timeZoomSlider);

    timeZoomLabel.setText ("H Zoom", juce::dontSendNotification);
    addAndMakeVisible (timeZoomLabel);

    ampZoomSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    ampZoomSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 46, 20);
    ampZoomSlider.setRange (1.0, 8.0, 0.1);
    ampZoomSlider.setValue (1.0, juce::dontSendNotification);
    ampZoomSlider.setTooltip ("Vertical zoom. Shift+scroll over the scope to zoom too.");
    ampZoomSlider.onValueChange = [this]
    {
        oscilloscope.setAmpZoom ((float) ampZoomSlider.getValue());
    };
    addAndMakeVisible (ampZoomSlider);

    ampZoomLabel.setText ("V Zoom", juce::dontSendNotification);
    addAndMakeVisible (ampZoomLabel);

    // Wheel-zoom on the scope pushes values back into the sliders.
    oscilloscope.onZoomChanged = [this]
    {
        timeZoomSlider.setValue (oscilloscope.getTimeZoom(), juce::dontSendNotification);
        ampZoomSlider.setValue  (oscilloscope.getAmpZoom(),  juce::dontSendNotification);
    };

    // Section headers.
    auto configureHeader = [this] (juce::Label& header, const juce::String& text)
    {
        header.setText (text, juce::dontSendNotification);
        header.setFont (juce::Font (juce::FontOptions (13.0f)).boldened());
        header.setColour (juce::Label::textColourId, juce::Colour (0xff9ca3af));
        addAndMakeVisible (header);
    };
    configureHeader (alignmentHeader, "ALIGNMENT");
    configureHeader (rotatorHeader,   "PHASE ROTATOR");

    // Delay (ms): horizontal linear slider, value box to the right.
    delayMsSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    delayMsSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 22);
    delayMsSlider.setTextValueSuffix (" ms");
    delayMsSlider.setTooltip ("Audio delay on the bass/main path only. Kick timing changes are shown as recommendations.");
    addAndMakeVisible (delayMsSlider);

    delayMsLabel.setText ("Bass Delay", juce::dontSendNotification);
    addAndMakeVisible (delayMsLabel);

    // Interpolation: ComboBox item IDs are 1-based (param index 0 -> ID 1).
    delayInterpBox.addItem ("Linear", 1);
    delayInterpBox.addItem ("Allpass", 2);
    addAndMakeVisible (delayInterpBox);

    delayInterpLabel.setText ("Interpolation", juce::dontSendNotification);
    addAndMakeVisible (delayInterpLabel);

    polarityInvertButton.setButtonText ("Invert Bass Polarity");
    polarityInvertButton.setTooltip ("Flip the bass polarity 180 degrees.");
    addAndMakeVisible (polarityInvertButton);

    rotatorFreqSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    rotatorFreqSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 22);
    rotatorFreqSlider.setTextValueSuffix (" Hz");
    rotatorFreqSlider.setNumDecimalPlacesToDisplay (0);
    addAndMakeVisible (rotatorFreqSlider);

    rotatorFreqLabel.setText ("Frequency", juce::dontSendNotification);
    addAndMakeVisible (rotatorFreqLabel);

    rotatorQSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    rotatorQSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 70, 22);
    rotatorQSlider.setNumDecimalPlacesToDisplay (2);
    addAndMakeVisible (rotatorQSlider);

    rotatorQLabel.setText ("Resonance (Q)", juce::dontSendNotification);
    addAndMakeVisible (rotatorQLabel);

    rotatorStagesBox.addItem ("2", 1);
    rotatorStagesBox.addItem ("3", 2);
    rotatorStagesBox.addItem ("4", 3);
    addAndMakeVisible (rotatorStagesBox);

    rotatorStagesLabel.setText ("Stages", juce::dontSendNotification);
    addAndMakeVisible (rotatorStagesLabel);

    // Attachments: bind each control to its APVTS parameter.
    delayMsAttachment        = std::make_unique<SliderAttachment>   (audioProcessor.apvts, "delayMs", delayMsSlider);
    delayInterpAttachment    = std::make_unique<ComboBoxAttachment> (audioProcessor.apvts, "delayInterp", delayInterpBox);
    polarityInvertAttachment = std::make_unique<ButtonAttachment>   (audioProcessor.apvts, "polarityInvert", polarityInvertButton);
    rotatorFreqAttachment    = std::make_unique<SliderAttachment>   (audioProcessor.apvts, "rotatorFreq", rotatorFreqSlider);
    rotatorQAttachment       = std::make_unique<SliderAttachment>   (audioProcessor.apvts, "rotatorQ", rotatorQSlider);
    rotatorStagesAttachment  = std::make_unique<ComboBoxAttachment> (audioProcessor.apvts, "rotatorStages", rotatorStagesBox);

    setSize (720, 600);
}

KickLockAudioProcessorEditor::~KickLockAudioProcessorEditor()
{
    setLookAndFeel (nullptr);
}

void KickLockAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1a1a1a));

    // Title bar.
    g.setColour (juce::Colour (0xff2dd4bf));
    g.setFont (juce::Font (juce::FontOptions (20.0f)).boldened());
    g.drawText ("KICKLOCK", 16, 10, 300, 28, juce::Justification::centredLeft);

    g.setColour (juce::Colour (0xff6b7280));
    g.setFont (juce::Font (juce::FontOptions (12.0f)));
    g.drawText ("phase alignment", 150, 14, 300, 22, juce::Justification::centredLeft);
}

void KickLockAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds().reduced (16);

    bounds.removeFromTop (34); // title bar

    // --- Scope + correlation panel side by side --------------------------
    auto topArea = bounds.removeFromTop (200);

    // Correlation panel gets a fixed column on the right.
    auto corrArea = topArea.removeFromRight (150);
    topArea.removeFromRight (10); // gap
    oscilloscope.setBounds (topArea);
    correlationDisplay.setBounds (corrArea);

    bounds.removeFromTop (6);

    // --- Toolbar row: zoom sliders (left) + Analyze/Freeze (right) --------
    auto toolbar = bounds.removeFromTop (26);

    freezeButton.setBounds  (toolbar.removeFromRight (84).reduced (0, 2));
    toolbar.removeFromRight (8);
    analyzeButton.setBounds (toolbar.removeFromRight (96).reduced (0, 2));
    toolbar.removeFromRight (16);

    // Remaining toolbar width holds the two zoom sliders.
    auto zoomArea = toolbar;
    auto timeZoomArea = zoomArea.removeFromLeft (zoomArea.getWidth() / 2 - 6);
    zoomArea.removeFromLeft (12);
    auto ampZoomArea = zoomArea;

    timeZoomLabel.setBounds (timeZoomArea.removeFromLeft (52));
    timeZoomSlider.setBounds (timeZoomArea);
    ampZoomLabel.setBounds (ampZoomArea.removeFromLeft (52));
    ampZoomSlider.setBounds (ampZoomArea);

    bounds.removeFromTop (4);

    // --- Analyze result line ---------------------------------------------
    analyzeResultLabel.setBounds (bounds.removeFromTop (20));

    bounds.removeFromTop (6);

    // --- Two control columns ---------------------------------------------
    auto controlsArea = bounds;
    auto leftCol  = controlsArea.removeFromLeft (controlsArea.getWidth() / 2 - 8);
    controlsArea.removeFromLeft (16); // gutter
    auto rightCol = controlsArea;

    alignmentHeader.setBounds (leftCol.removeFromTop (22));
    rotatorHeader.setBounds  (rightCol.removeFromTop (22));

    leftCol.removeFromTop (4);
    rightCol.removeFromTop (4);

    const int rowH = 30;

    auto layoutRow = [] (juce::Rectangle<int>& col, juce::Label& label, juce::Component& control, int h)
    {
        auto row = col.removeFromTop (h);
        label.setBounds (row.removeFromLeft (96));
        control.setBounds (row);
        col.removeFromTop (8); // row spacing
    };

    // Left: alignment controls.
    layoutRow (leftCol, delayMsLabel,     delayMsSlider,  rowH);
    layoutRow (leftCol, delayInterpLabel, delayInterpBox, rowH);
    polarityInvertButton.setBounds (leftCol.removeFromTop (rowH));

    // Right: rotator controls.
    layoutRow (rightCol, rotatorFreqLabel,   rotatorFreqSlider, rowH);
    layoutRow (rightCol, rotatorQLabel,      rotatorQSlider,    rowH);
    layoutRow (rightCol, rotatorStagesLabel, rotatorStagesBox,  rowH);
}
