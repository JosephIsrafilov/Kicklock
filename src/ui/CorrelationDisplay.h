#pragma once

#include <JuceHeader.h>
#include <atomic>

// Timer-polled phase-match read-out. Shows a titled panel with the correlation
// percent as large text, a status word (ALIGNED / PARTIAL / FIGHTING), and a
// horizontal level bar. Colour lerps red (0%, out of phase) -> green (100%,
// perfectly aligned). The value is smoothed on the UI side so the number and
// bar glide rather than jitter.
class CorrelationDisplay : public juce::Component, private juce::Timer
{
public:
    explicit CorrelationDisplay (std::atomic<float>& percentToRead)
        : percentRef (percentToRead)
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
        g.drawText ("PHASE MATCH", area.removeFromTop (16.0f).toNearestInt(),
                    juce::Justification::centred);

        g.setColour (juce::Colour (0xff6f7d89));
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText ("Multi-band 20 Hz-2 kHz (low-end weighted)",
                    area.removeFromTop (14.0f).toNearestInt(),
                    juce::Justification::centred);

        // Big percent.
        g.setColour (accent);
        g.setFont (juce::Font (juce::FontOptions (44.0f)).boldened());
        g.drawText (juce::String ((int) std::round (pct)) + "%",
                    area.removeFromTop (48.0f).toNearestInt(),
                    juce::Justification::centred);

        // Status word.
        const juce::String status = pct >= 75.0f ? "% ALIGNED"
                                   : pct >= 50.0f ? "MIXED"
                                                  : "CANCELING";
        g.setColour (accent);
        g.setFont (juce::Font (juce::FontOptions (13.0f)).boldened());
        g.drawText (status, area.removeFromTop (18.0f).toNearestInt(),
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
        const float target = percentRef.load();
        displayPercent += 0.25f * (target - displayPercent); // UI-side smoothing
        repaint();
    }

    std::atomic<float>& percentRef;
    float displayPercent = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (CorrelationDisplay)
};
