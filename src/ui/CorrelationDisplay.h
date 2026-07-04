#pragma once

#include <JuceHeader.h>
#include <atomic>

// Timer-polled live-match read-out. Shows a large multi-band percent, a short
// status word, supporting low-end / broadband figures, and a horizontal level
// bar. Colour lerps red (0%, out of phase) -> green (100%, perfectly
// aligned). Values are smoothed on the UI side so the display glides rather
// than jittering.
class CorrelationDisplay : public juce::Component, private juce::Timer
{
public:
    CorrelationDisplay (std::atomic<float>& multiBandPercentToRead,
                        std::atomic<float>& lowEndPercentToRead,
                        std::atomic<float>& broadbandPercentToRead)
        : multiBandPercentRef (multiBandPercentToRead),
          lowEndPercentRef (lowEndPercentToRead),
          broadbandPercentRef (broadbandPercentToRead)
    {
        startTimerHz (30);
    }

    ~CorrelationDisplay() override
    {
        stopTimer();
    }

    void paint (juce::Graphics& g) override
    {
        auto bounds = getLocalBounds().toFloat().reduced (1.0f);
        const float pct = juce::jlimit (0.0f, 100.0f, displayPercent);
        const float lowPct = juce::jlimit (0.0f, 100.0f, displayLowEndPercent);
        const float broadbandPct = juce::jlimit (0.0f, 100.0f, displayBroadbandPercent);
        const auto  accent = juce::Colours::red.interpolatedWith (juce::Colours::green, pct / 100.0f);

        // Panel background + subtle border in the accent colour.
        g.setColour (juce::Colour (0xff1f1f1f));
        g.fillRoundedRectangle (bounds, 6.0f);
        g.setColour (accent.withAlpha (0.5f));
        g.drawRoundedRectangle (bounds, 6.0f, 1.2f);

        auto area = bounds.reduced (10.0f, 8.0f);

        // Title.
        g.setColour (juce::Colour (0xff9a9a9a));
        g.setFont (juce::Font (juce::FontOptions (12.0f)).boldened());
        g.drawText ("LIVE MATCH", area.removeFromTop (16.0f).toNearestInt(),
                    juce::Justification::centred);

        g.setColour (juce::Colour (0xff6f7d89));
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText ("Multi-band 20 Hz-500 Hz while audio is playing",
                    area.removeFromTop (14.0f).toNearestInt(),
                    juce::Justification::centred);

        // Big percent.
        g.setColour (accent);
        g.setFont (juce::Font (juce::FontOptions (52.0f)).boldened());
        g.drawText (juce::String ((int) std::round (pct)) + "%",
                    area.removeFromTop (56.0f).toNearestInt(),
                    juce::Justification::centred);

        // Status word.
        const juce::String status = pct >= 85.0f ? "LOCKED"
                                   : pct >= 65.0f ? "CLOSING"
                                                  : "FIGHTING";
        g.setColour (accent);
        g.setFont (juce::Font (juce::FontOptions (13.0f)).boldened());
        g.drawText (status, area.removeFromTop (18.0f).toNearestInt(),
                    juce::Justification::centred);

        g.setColour (juce::Colour (0xffc7d2db));
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.drawText ("Low-End " + juce::String ((int) std::round (lowPct))
                        + "%   |   20-500 " + juce::String ((int) std::round (broadbandPct)) + "%",
                    area.removeFromTop (16.0f).toNearestInt(),
                    juce::Justification::centred);

        // Level bar.
        area.removeFromTop (6.0f);
        auto bar = area.removeFromTop (10.0f);
        g.setColour (juce::Colour (0xff2a2a2a));
        g.fillRoundedRectangle (bar, 5.0f);
        auto fill = bar.withWidth (bar.getWidth() * pct / 100.0f);
        g.setColour (accent);
        g.fillRoundedRectangle (fill, 5.0f);
    }

private:
    void timerCallback() override
    {
        const float target = multiBandPercentRef.load();
        const float lowTarget = lowEndPercentRef.load();
        const float broadbandTarget = broadbandPercentRef.load();
        displayPercent += 0.25f * (target - displayPercent);
        displayLowEndPercent += 0.25f * (lowTarget - displayLowEndPercent);
        displayBroadbandPercent += 0.25f * (broadbandTarget - displayBroadbandPercent);
        repaint();
    }

    std::atomic<float>& multiBandPercentRef;
    std::atomic<float>& lowEndPercentRef;
    std::atomic<float>& broadbandPercentRef;
    float displayPercent = 50.0f;
    float displayLowEndPercent = 50.0f;
    float displayBroadbandPercent = 50.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CorrelationDisplay)
};
