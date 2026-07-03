#pragma once

#include <JuceHeader.h>
#include <array>
#include <vector>

#include "../util/ScopeFifo.h"

// Timer-polled scope. Reads paired main/sidechain samples from a ScopeFifo
// into two fixed-size ring buffers and draws them as two filled traces with a
// grid and legend. Auto-gain scales quiet signals up so they fill the view,
// a freeze toggle stops the scroll so the waveform can be inspected, and
// time/amplitude zoom (sliders or mouse wheel) let you inspect alignment
// closely.
class Oscilloscope : public juce::Component, private juce::Timer
{
public:
    explicit Oscilloscope (ScopeFifo& fifoToRead);
    ~Oscilloscope() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    void setFrozen (bool shouldFreeze) noexcept { frozen = shouldFreeze; }
    bool isFrozen() const noexcept              { return frozen; }

    // Time zoom: 1x shows the whole history, higher values show only the most
    // recent slice stretched across the width. Amplitude zoom multiplies the
    // auto-gain. Both are clamped to sane ranges.
    void setTimeZoom (float z) noexcept { timeZoom = juce::jlimit (1.0f, 16.0f, z); repaint(); }
    void setAmpZoom  (float z) noexcept { ampZoom  = juce::jlimit (1.0f, 8.0f,  z); repaint(); }
    float getTimeZoom() const noexcept  { return timeZoom; }
    float getAmpZoom()  const noexcept  { return ampZoom; }

    // Wheel over the scope nudges time zoom; Shift+wheel nudges amplitude zoom.
    // Fires onZoomChanged so the editor can keep its sliders in sync.
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    std::function<void()> onZoomChanged;

private:
    void timerCallback() override;

    static constexpr int historyLength = 2048;
    static constexpr int scratchSize   = 256;

    ScopeFifo& fifo;

    // Ring buffers sized once in the ctor, never reallocated.
    std::vector<float> mainHistory;
    std::vector<float> sidechainHistory;
    int writeIndex = 0;   // next slot to overwrite (oldest sample)

    bool  frozen      = false;
    float displayGain = 1.0f;   // smoothed auto-gain applied to both traces
    float timeZoom    = 1.0f;   // horizontal zoom (1..16)
    float ampZoom     = 1.0f;   // vertical zoom (1..8)

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Oscilloscope)
};
