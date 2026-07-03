#include "Oscilloscope.h"

Oscilloscope::Oscilloscope (ScopeFifo& fifoToRead)
    : fifo (fifoToRead),
      mainHistory ((size_t) historyLength, 0.0f),
      sidechainHistory ((size_t) historyLength, 0.0f)
{
    startTimerHz (30);
}

Oscilloscope::~Oscilloscope()
{
    stopTimer();
}

void Oscilloscope::timerCallback()
{
    // Stack scratch, no allocation per tick.
    std::array<float, scratchSize> mainScratch;
    std::array<float, scratchSize> sidechainScratch;

    const int count = fifo.readAvailable (mainScratch.data(), sidechainScratch.data(), scratchSize);

    // When frozen we still drain the fifo (so it doesn't back up and the audio
    // thread keeps dropping cleanly) but we don't advance the display.
    if (frozen)
        return;

    // Append each new sample at the circular write index, overwriting the
    // oldest slot. No physical shift, no reallocation.
    for (int i = 0; i < count; ++i)
    {
        mainHistory[(size_t) writeIndex]      = mainScratch[(size_t) i];
        sidechainHistory[(size_t) writeIndex] = sidechainScratch[(size_t) i];
        writeIndex = (writeIndex + 1) % historyLength;
    }

    if (count > 0)
        repaint();
}

void Oscilloscope::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    const juce::Colour bg     { 0xff141414 };
    const juce::Colour grid   { 0xff2a2a2a };
    const juce::Colour teal    { 0xff2dd4bf }; // main / bass
    const juce::Colour orange  { 0xfff97316 }; // sidechain / kick

    // Rounded panel background.
    g.setColour (bg);
    g.fillRoundedRectangle (bounds, 6.0f);

    const float w      = bounds.getWidth();
    const float h      = bounds.getHeight();
    const float midY   = bounds.getCentreY();
    const float halfH  = h * 0.5f;

    // --- Grid -------------------------------------------------------------
    g.setColour (grid);
    for (int i = 1; i < 4; ++i) // vertical divisions
    {
        const float x = bounds.getX() + w * (float) i / 4.0f;
        g.drawVerticalLine ((int) x, bounds.getY() + 4.0f, bounds.getBottom() - 4.0f);
    }
    for (int i = 1; i < 4; ++i) // horizontal divisions
    {
        const float y = bounds.getY() + h * (float) i / 4.0f;
        g.drawHorizontalLine ((int) y, bounds.getX() + 4.0f, bounds.getRight() - 4.0f);
    }

    // Brighter zero line.
    g.setColour (grid.brighter (0.3f));
    g.drawHorizontalLine ((int) midY, bounds.getX() + 4.0f, bounds.getRight() - 4.0f);

    // --- Auto-gain --------------------------------------------------------
    // Find the loudest sample across both traces and pick a gain that maps it
    // to ~85% of half-height, so quiet material still fills the view. Smoothed
    // so it doesn't jump around. Clamped to a sane range.
    float peak = 0.0f;
    for (int i = 0; i < historyLength; ++i)
    {
        peak = juce::jmax (peak, std::abs (mainHistory[(size_t) i]));
        peak = juce::jmax (peak, std::abs (sidechainHistory[(size_t) i]));
    }

    const float targetGain = peak > 1.0e-4f ? juce::jlimit (1.0f, 40.0f, 0.85f / peak) : 1.0f;
    displayGain += 0.2f * (targetGain - displayGain); // one-pole smoothing

    const float gain = displayGain * ampZoom; // amplitude zoom stacks on auto-gain

    // Time zoom: show only the most-recent slice of the history, stretched
    // across the full width. `visible` is how many samples we draw; the oldest
    // of them starts `visible` back from the newest sample in the ring.
    const int   visible = juce::jlimit (2, historyLength,
                                        (int) std::round ((float) historyLength / timeZoom));
    const int   first   = historyLength - visible; // offset into chronological order
    const float xStep   = w / (float) (visible - 1);

    auto buildPath = [&] (const std::vector<float>& history, juce::Path& stroke, juce::Path& fill)
    {
        for (int i = 0; i < visible; ++i)
        {
            // chronological position (first..historyLength) mapped into the ring
            const int   idx = (writeIndex + first + i) % historyLength;
            const float x   = bounds.getX() + (float) i * xStep;
            const float v   = juce::jlimit (-1.0f, 1.0f, history[(size_t) idx] * gain);
            const float y   = midY - v * halfH * 0.92f;

            if (i == 0)
            {
                stroke.startNewSubPath (x, y);
                fill.startNewSubPath (x, midY);
                fill.lineTo (x, y);
            }
            else
            {
                stroke.lineTo (x, y);
                fill.lineTo (x, y);
            }
        }
        fill.lineTo (bounds.getRight(), midY);
        fill.closeSubPath();
    };

    juce::Path mainStroke, mainFill, scStroke, scFill;
    buildPath (sidechainHistory, scStroke, scFill);
    buildPath (mainHistory,      mainStroke, mainFill);

    // Sidechain (kick) drawn first, underneath.
    g.setColour (orange.withAlpha (0.15f));
    g.fillPath (scFill);
    g.setColour (orange);
    g.strokePath (scStroke, juce::PathStrokeType (1.2f));

    // Main (bass) on top.
    g.setColour (teal.withAlpha (0.15f));
    g.fillPath (mainFill);
    g.setColour (teal);
    g.strokePath (mainStroke, juce::PathStrokeType (1.6f));

    // --- Legend -----------------------------------------------------------
    g.setFont (juce::Font (juce::FontOptions (12.0f)));
    const float ly = bounds.getY() + 8.0f;
    float lx = bounds.getX() + 10.0f;

    g.setColour (teal);
    g.fillRoundedRectangle (lx, ly, 14.0f, 4.0f, 2.0f);
    g.drawText ("Bass", (int) (lx + 18.0f), (int) (ly - 6.0f), 40, 16, juce::Justification::centredLeft);

    lx += 66.0f;
    g.setColour (orange);
    g.fillRoundedRectangle (lx, ly, 14.0f, 4.0f, 2.0f);
    g.drawText ("Kick", (int) (lx + 18.0f), (int) (ly - 6.0f), 40, 16, juce::Justification::centredLeft);

    // --- Zoom / window read-out ------------------------------------------
    // The full 2048-sample history spans ~1 second, so the visible window in
    // milliseconds scales inversely with the time zoom.
    {
        const int windowMs = (int) std::round (1000.0f / timeZoom);
        g.setColour (juce::Colour (0xff6b7280));
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.drawText (juce::String (windowMs) + " ms  ·  " + juce::String (ampZoom, 1) + "x amp",
                    (int) (bounds.getX() + 10.0f), (int) (bounds.getBottom() - 20.0f),
                    200, 16, juce::Justification::centredLeft);
    }

    // --- Frozen badge -----------------------------------------------------
    if (frozen)
    {
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.setFont (juce::Font (juce::FontOptions (12.0f)).boldened());
        g.drawText ("FROZEN", (int) (bounds.getRight() - 78.0f), (int) (bounds.getY() + 6.0f),
                    68, 16, juce::Justification::centredRight);
    }
}

void Oscilloscope::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    juce::ignoreUnused (e);

    const float step = 1.0f + juce::jlimit (-0.5f, 0.5f, wheel.deltaY) * 0.6f;

    if (e.mods.isShiftDown())
        setAmpZoom (ampZoom * step);
    else
        setTimeZoom (timeZoom * step);

    if (onZoomChanged != nullptr)
        onZoomChanged();
}

void Oscilloscope::resized()
{
    // paint() recomputes geometry from getLocalBounds() each call.
}
