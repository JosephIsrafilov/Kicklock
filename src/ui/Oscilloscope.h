#pragma once

#include <JuceHeader.h>
#include <array>
#include <vector>

#include "../util/ScopeFifo.h"
#include "../util/HitCaptureBuffer.h"
#include "ScopeVisuals.h"

// Timer-polled scope. Reads paired main/sidechain samples from a ScopeFifo
// into two fixed-size ring buffers and draws them as two filled traces with a
// grid and legend. Auto-gain scales quiet signals up so they fill the view,
// a freeze toggle stops the scroll so the waveform can be inspected, and
// time/amplitude zoom (sliders or mouse wheel) let you inspect alignment
// closely.
class Oscilloscope : public juce::Component,
                     public juce::SettableTooltipClient,
                     private juce::Timer
{
public:
    Oscilloscope (ScopeFifo& fifoToRead, const HitCaptureBuffer& hitCaptureToRead);
    ~Oscilloscope() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    void setFrozen (bool shouldFreeze) noexcept { frozen = shouldFreeze; }
    bool isFrozen() const noexcept              { return frozen; }

    void setViewMode (ScopeViewMode mode) noexcept
    {
        if (viewMode != mode)
        {
            viewMode = mode;
            repaint();
        }
    }

    void setGridDivision (GridDivision division) noexcept
    {
        if (gridDivision != division)
        {
            gridDivision = division;
            repaint();
        }
    }

    void setTempoInfo (double bpm, bool available) noexcept;

    void setVisualOffsetSamples (int offset) noexcept
    {
        if (visualOffsetSamples != offset)
        {
            visualOffsetSamples = offset;
            repaint();
        }
    }
    void setTimebase (double newSampleRate, int newDecimationFactor) noexcept;
    void setDelayParameter (juce::RangedAudioParameter* parameter) noexcept { delayParameter = parameter; }

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
    void drawGrid (juce::Graphics&, juce::Rectangle<float>, float, bool, int) const;
    void drawPhaseDeltaMode (juce::Graphics&, juce::Rectangle<float>, int, float, float);
    void drawTriggeredMode (juce::Graphics&, juce::Rectangle<float>, float);
    void drawOverlayMode (juce::Graphics&, juce::Rectangle<float>, int, float, float);
    void drawSeparateMode (juce::Graphics&, juce::Rectangle<float>, int, float);
    void drawTransientMarkers (juce::Graphics&, juce::Rectangle<float>, int) const;
    void drawScopeFooter (juce::Graphics&, juce::Rectangle<float>, int) const;
    void rebuildVisibleBuffers (int visible);
    void refreshTriggeredSnapshot();
    void setDelayFromDrag (const juce::MouseEvent&);

    static constexpr int historyLength = 8192;
    static constexpr int scratchSize   = 256;

    ScopeFifo& fifo;
    const HitCaptureBuffer& hitCapture;

    // Ring buffers sized once in the ctor, never reallocated.
    std::vector<float> mainHistory;
    std::vector<float> sidechainHistory;
    int writeIndex = 0;   // next slot to overwrite (oldest sample)

    bool  frozen      = false;
    float displayGain = 1.0f;   // smoothed auto-gain applied to both traces
    float timeZoom    = 1.0f;   // horizontal zoom (1..16)
    float ampZoom     = 1.0f;   // vertical zoom (1..8)
    ScopeViewMode viewMode = ScopeViewMode::Triggered;
    GridDivision gridDivision = GridDivision::Milliseconds;
    int visualOffsetSamples = 0;
    double sampleRate = 44100.0;
    int decimationFactor = 1;
    double bpm = 0.0;
    bool tempoAvailable = false;
    std::array<float, historyLength> visibleMainBuffer {};
    std::array<float, historyLength> visibleSideBuffer {};

    // P7: low-band envelope-smoothed copies of the visible buffers. The raw
    // buffers still drive the waveform traces (so the scope stays a scope), but
    // the phase-delta colouring and the Δ-ms transient markers read from these
    // so the green/red regions and the peak markers track the musical low/body
    // content instead of broadband sample jitter.
    std::array<float, historyLength> smoothedMainBuffer {};
    std::array<float, historyLength> smoothedSideBuffer {};

    static constexpr int ghostCount = 3;
    std::vector<float> triggeredBass;
    std::vector<float> triggeredKick;
    std::array<std::vector<float>, ghostCount> ghostBass;
    std::array<std::vector<float>, ghostCount> ghostKick;
    int latestTriggeredSequence = 0;
    int triggeredPreRollSamples = 0;

    juce::RangedAudioParameter* delayParameter = nullptr;
    bool delayGestureActive = false;
    float dragStartX = 0.0f;
    float dragStartDelayMs = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Oscilloscope)
};
