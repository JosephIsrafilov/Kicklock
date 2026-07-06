#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>

#include "../dsp/PhaseBands.h"

// Timer-polled live-match read-out. Phase 6 makes this the single hero number:
// the low-end-weighted live match is large and centered, while weighted/low-end/
// broadband and per-band values sit in a compact collapsible Details row.
class CorrelationDisplay : public juce::Component,
                           public juce::SettableTooltipClient,
                           private juce::Timer
{
public:
    CorrelationDisplay (std::atomic<float>& weightedPercentToRead,
                        std::atomic<float>& lowEndPercentToRead,
                        std::atomic<float>& broadbandPercentToRead,
                        std::array<std::atomic<float>, PhaseBands::numBands>& bandPercentsToRead,
                        std::atomic<float>& appliedBeforePercentToRead,
                        std::atomic<bool>& matchValidToRead)
        : weightedPercentRef (weightedPercentToRead),
          lowEndPercentRef (lowEndPercentToRead),
          broadbandPercentRef (broadbandPercentToRead),
          appliedBeforePercentRef (appliedBeforePercentToRead),
          matchValidRef (matchValidToRead)
    {
        for (int i = 0; i < PhaseBands::numBands; ++i)
            bandRefs[(size_t) i] = &bandPercentsToRead[(size_t) i];

        startTimerHz (30);
    }

    ~CorrelationDisplay() override
    {
        stopTimer();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat().reduced (1.0f);
        const float pct = juce::jlimit (0.0f, 100.0f, displayWeightedPercent);
        const auto accent = colourForMatch (pct);

        g.setColour (juce::Colour (0xff171d23));
        g.fillRoundedRectangle (bounds, 7.0f);
        g.setColour (accent.withAlpha (0.58f));
        g.drawRoundedRectangle (bounds, 7.0f, 1.1f);

        auto area = bounds.reduced (10.0f, 6.0f);
        const bool drawDetails = detailsVisible && area.getHeight() >= 86.0f;

        auto top = area.removeFromTop (drawDetails ? 56.0f : area.getHeight());
        g.setColour (juce::Colour (0xff97a5b2));
        g.setFont (juce::Font (juce::FontOptions (10.5f)).boldened());
        g.drawText ("LIVE MATCH", top.removeFromTop (12.0f).toNearestInt(), juce::Justification::centred);

        // While nothing meaningful is playing, every internal reading is a
        // neutral 50% — printing that reads as "half aligned", which is a lie.
        // Show an explicit no-signal state instead.
        if (! displayValid)
        {
            g.setColour (juce::Colour (0xff97a5b2));
            g.setFont (juce::Font (juce::FontOptions (drawDetails ? 42.0f : 48.0f)).boldened());
            g.drawText ("--", top.removeFromTop (drawDetails ? 30.0f : 48.0f).toNearestInt(),
                        juce::Justification::centred);
            g.setFont (juce::Font (juce::FontOptions (11.0f)));
            g.drawText ("no signal — play kick + bass",
                        top.toNearestInt(), juce::Justification::centred);
            detailsToggleBounds = {};   // nothing to expand while silent
            return;
        }

        g.setColour (accent);
        g.setFont (juce::Font (juce::FontOptions (drawDetails ? 42.0f : 48.0f)).boldened());
        g.drawText (juce::String ((int) std::round (pct)) + "%",
                    top.toNearestInt(),
                    juce::Justification::centred);

        if (displayAppliedBeforePercent >= 0.0f)
        {
            g.setColour (juce::Colour (0xff97a5b2).withAlpha (0.82f));
            g.setFont (juce::Font (juce::FontOptions (11.0f)).boldened());
            g.drawText ("before: " + juce::String ((int) std::round (displayAppliedBeforePercent)) + "%",
                        juce::Rectangle<int> ((int) bounds.getRight() - 116,
                                              (int) bounds.getY() + 12,
                                              96,
                                              16),
                        juce::Justification::centredRight);
        }

        detailsToggleBounds = juce::Rectangle<int> ((int) bounds.getX() + 10,
                                                    (int) bounds.getY() + 10,
                                                    74,
                                                    18);
        g.setColour (juce::Colour (0xff97a5b2).withAlpha (0.82f));
        g.setFont (juce::Font (juce::FontOptions (10.5f)).boldened());
        g.drawText (detailsVisible ? "DETAILS -" : "DETAILS +",
                    detailsToggleBounds,
                    juce::Justification::centredLeft);

        if (! drawDetails)
            return;

        area.removeFromTop (2.0f);
        auto summary = area.removeFromTop (14.0f);
        drawSummaryCell (g, summary.removeFromLeft (summary.getWidth() / 3.0f), "WTD", displayWeightedPercent);
        drawSummaryCell (g, summary.removeFromLeft (summary.getWidth() / 2.0f), "LOW", displayLowEndPercent);
        drawSummaryCell (g, summary, "BROAD", displayBroadbandPercent);

        area.removeFromTop (3.0f);
        auto bands = area.removeFromTop (20.0f);
        const float gap = 6.0f;
        const float cellW = (bands.getWidth() - gap * 3.0f) / 4.0f;
        for (int i = 0; i < PhaseBands::numBands; ++i)
        {
            auto cell = bands.removeFromLeft (cellW);
            drawBandMeter (g, cell, PhaseBands::table[(size_t) i].name, displayBandPercent[(size_t) i]);
            if (i != PhaseBands::numBands - 1)
                bands.removeFromLeft (gap);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (detailsToggleBounds.contains (e.getPosition()))
        {
            detailsVisible = ! detailsVisible;
            repaint();
        }
    }

private:
    static juce::Colour colourForMatch (float pct) noexcept
    {
        if (pct < 55.0f)
            return juce::Colour (0xffef4444);
        if (pct < 75.0f)
            return juce::Colour (0xfff59e0b);
        return juce::Colour (0xff22c55e);
    }

    static void drawSummaryCell (juce::Graphics& g,
                                 juce::Rectangle<float> bounds,
                                 const char* label,
                                 float value)
    {
        bounds = bounds.reduced (2.0f, 0.0f);
        g.setColour (juce::Colour (0xff97a5b2));
        g.setFont (juce::Font (juce::FontOptions (10.0f)).boldened());
        g.drawText (label, bounds.removeFromLeft (42.0f).toNearestInt(), juce::Justification::centredLeft);
        g.setColour (colourForMatch (value));
        g.drawText (juce::String ((int) std::round (value)) + "%",
                    bounds.toNearestInt(),
                    juce::Justification::centredLeft);
    }

    static void drawBandMeter (juce::Graphics& g,
                               juce::Rectangle<float> bounds,
                               const char* label,
                               float value)
    {
        const float pct = juce::jlimit (0.0f, 100.0f, value);
        g.setColour (juce::Colour (0xff2b3540));
        g.fillRoundedRectangle (bounds, 4.0f);

        auto fill = bounds.withWidth (bounds.getWidth() * pct / 100.0f);
        g.setColour (colourForMatch (pct).withAlpha (0.78f));
        g.fillRoundedRectangle (fill, 4.0f);

        g.setColour (juce::Colour (0xffedf2f7));
        g.setFont (juce::Font (juce::FontOptions (10.0f)).boldened());
        g.drawText (juce::String (label) + " " + juce::String ((int) std::round (pct)) + "%",
                    bounds.reduced (5.0f, 0.0f).toNearestInt(),
                    juce::Justification::centredLeft);
    }

    void timerCallback() override
    {
        // Short hold on the valid flag so the display doesn't flicker between
        // "no signal" and a number in the gaps between kick hits (the meter's
        // energy gate is EMA-smoothed but can dip on very sparse material).
        if (matchValidRef.load())
            validHoldTicks = 15;    // ~0.5 s at 30 Hz
        else if (validHoldTicks > 0)
            --validHoldTicks;
        displayValid = validHoldTicks > 0;

        const float target = weightedPercentRef.load();
        const float lowTarget = lowEndPercentRef.load();
        const float broadbandTarget = broadbandPercentRef.load();
        const float beforeTarget = appliedBeforePercentRef.load();

        displayWeightedPercent += 0.25f * (target - displayWeightedPercent);
        displayLowEndPercent += 0.25f * (lowTarget - displayLowEndPercent);
        displayBroadbandPercent += 0.25f * (broadbandTarget - displayBroadbandPercent);
        displayAppliedBeforePercent = beforeTarget;

        for (int i = 0; i < PhaseBands::numBands; ++i)
        {
            const float bandTarget = bandRefs[(size_t) i] != nullptr ? bandRefs[(size_t) i]->load() : 50.0f;
            displayBandPercent[(size_t) i] += 0.25f * (bandTarget - displayBandPercent[(size_t) i]);
        }

        repaint();
    }

    std::atomic<float>& weightedPercentRef;
    std::atomic<float>& lowEndPercentRef;
    std::atomic<float>& broadbandPercentRef;
    std::atomic<float>& appliedBeforePercentRef;
    std::atomic<bool>& matchValidRef;
    bool displayValid = false;
    int validHoldTicks = 0;
    std::array<std::atomic<float>*, PhaseBands::numBands> bandRefs {};

    float displayWeightedPercent = 50.0f;
    float displayLowEndPercent = 50.0f;
    float displayBroadbandPercent = 50.0f;
    float displayAppliedBeforePercent = -1.0f;
    std::array<float, PhaseBands::numBands> displayBandPercent { 50.0f, 50.0f, 50.0f, 50.0f };
    bool detailsVisible = true;
    juce::Rectangle<int> detailsToggleBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CorrelationDisplay)
};
