#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <cmath>

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
                        std::atomic<bool>& matchValidToRead,
                        std::atomic<float>& lowEndSubLossDbToRead)
        : weightedPercentRef (weightedPercentToRead),
          lowEndPercentRef (lowEndPercentToRead),
          broadbandPercentRef (broadbandPercentToRead),
          matchValidRef (matchValidToRead),
          lowEndSubLossDbRef (lowEndSubLossDbToRead)
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

        // +16 over the historical 56/area.getHeight() budget, reserved below as
        // a fixed-height row for the sub-loss-dB headline so the percent
        // number's own box is unchanged from before that line existed.
        auto top = area.removeFromTop (drawDetails ? 72.0f : area.getHeight());
        g.setColour (juce::Colour (0xff97a5b2));
        g.setFont (juce::Font (juce::FontOptions (10.5f)).boldened());
        g.drawText ("LIVE MATCH", top.removeFromTop (12.0f).toNearestInt(), juce::Justification::centred);

        auto subLossRow = top.removeFromBottom (16.0f);

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
            g.drawText ("no signal - play kick + bass",
                        top.toNearestInt(), juce::Justification::centred);
            detailsToggleBounds = {};   // nothing to expand while silent
            return;
        }

        g.setColour (accent);
        g.setFont (juce::Font (juce::FontOptions (drawDetails ? 42.0f : 48.0f)).boldened());
        g.drawText (juce::String ((int) std::round (pct)) + "%",
                    top.toNearestInt(),
                    juce::Justification::centred);

        // The flagship read-out: the SAME live low-end relationship as the
        // percent above, reframed as an honest dB cost instead of an abstract
        // match score — "72% match" doesn't tell a producer how much level
        // they're losing; "-4.2 dB sub loss" does.
        g.setColour (colourForLoss (displaySubLossDb));
        g.setFont (juce::Font (juce::FontOptions (12.5f)).boldened());
        g.drawText (formatSubLossDb (displaySubLossDb),
                    subLossRow.toNearestInt(),
                    juce::Justification::centred);

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

    // Thresholds are looser than colourForMatch's 55/75% because a few dB of
    // sub loss is routine on real material; only flag it once it is musically
    // obvious (a producer would actually hear a hollow low end).
    static juce::Colour colourForLoss (float lossDb) noexcept
    {
        if (lossDb <= -6.0f)
            return juce::Colour (0xffef4444);
        if (lossDb <= -2.0f)
            return juce::Colour (0xfff59e0b);
        return juce::Colour (0xff22c55e);
    }

    // subLossDb() never reports a gain over the best case, so the value is
    // always <= 0; round-to-zero avoids ever printing a stray "-0.0".
    static juce::String formatSubLossDb (float lossDb) noexcept
    {
        const float rounded = std::round (lossDb * 10.0f) / 10.0f;
        const float clean = rounded == 0.0f ? 0.0f : rounded;
        return juce::String (clean, 1) + " dB sub loss";
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
        g.drawText (juce::String ((int) std::round (juce::jlimit (0.0f, 100.0f, value))) + "%",
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
        bool needsRepaint = false;
        const bool prevDisplayValid = displayValid;

        if (matchValidRef.load())
            validHoldTicks = 15;    // ~0.5 s at 30 Hz
        else if (validHoldTicks > 0)
            --validHoldTicks;
        displayValid = validHoldTicks > 0;
        
        if (displayValid != prevDisplayValid)
            needsRepaint = true;

        const float target = weightedPercentRef.load();
        const float lowTarget = lowEndPercentRef.load();
        const float broadbandTarget = broadbandPercentRef.load();
        const float subLossTarget = lowEndSubLossDbRef.load();

        auto updateSmoothed = [&needsRepaint](float& current, float targetValue) {
            int oldRounded = (int) std::round (current);
            current += 0.25f * (targetValue - current);
            if ((int) std::round (current) != oldRounded)
                needsRepaint = true;
        };

        updateSmoothed (displayWeightedPercent, target);
        updateSmoothed (displayLowEndPercent, lowTarget);
        updateSmoothed (displayBroadbandPercent, broadbandTarget);
        
        int oldSubLoss = (int) std::round (displaySubLossDb * 10.0f);
        displaySubLossDb += 0.25f * (subLossTarget - displaySubLossDb);
        if ((int) std::round (displaySubLossDb * 10.0f) != oldSubLoss)
            needsRepaint = true;

        for (int i = 0; i < PhaseBands::numBands; ++i)
        {
            const float bandTarget = bandRefs[(size_t) i] != nullptr ? bandRefs[(size_t) i]->load() : 50.0f;
            updateSmoothed (displayBandPercent[(size_t) i], bandTarget);
        }

        if (needsRepaint)
            repaint();
    }

    std::atomic<float>& weightedPercentRef;
    std::atomic<float>& lowEndPercentRef;
    std::atomic<float>& broadbandPercentRef;
    std::atomic<bool>& matchValidRef;
    std::atomic<float>& lowEndSubLossDbRef;
    bool displayValid = false;
    int validHoldTicks = 0;
    std::array<std::atomic<float>*, PhaseBands::numBands> bandRefs {};

    float displayWeightedPercent = 50.0f;
    float displayLowEndPercent = 50.0f;
    float displayBroadbandPercent = 50.0f;
    float displaySubLossDb = 0.0f;
    std::array<float, PhaseBands::numBands> displayBandPercent { 50.0f, 50.0f, 50.0f, 50.0f };
    bool detailsVisible = true;
    juce::Rectangle<int> detailsToggleBounds;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CorrelationDisplay)
};
