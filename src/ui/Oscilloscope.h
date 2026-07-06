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
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;

    void setFrozen (bool shouldFreeze) noexcept { frozen = shouldFreeze; }
    bool isFrozen() const noexcept              { return frozen; }

    // Effective display freeze: manual Freeze button OR a temporary mouse-hold
    // inspection. Both the timer (whether it advances the history/snapshot) and
    // paint gate on this, but they are stored separately so ending an
    // inspection hold never clears a manual freeze the user set on purpose.
    bool isDisplayFrozen() const noexcept { return scopeDisplayHeld (frozen, interactionHoldActive); }

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

    // The kick's captured window is locked to the first hit after each
    // (re)lock point and held static, since its shape doesn't meaningfully
    // change hit-to-hit — only the bass trace keeps refreshing on retrigger.
    // Call this to force re-capturing a fresh kick reference on the next hit
    // (e.g. if the kick sample/pattern changed and the old reference is stale).
    void relockKickReference() noexcept;
    KickReferenceState getKickReferenceState() const noexcept { return kickReferenceState; }

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
    void drawFreeRunMode (juce::Graphics&, juce::Rectangle<float>, int, float, float);
    void drawSeparateMode (juce::Graphics&, juce::Rectangle<float>, int, float);
    void drawWaveLegend (juce::Graphics&, juce::Rectangle<float>) const;
    void drawTransientMarkers (juce::Graphics&, juce::Rectangle<float>, int) const;
    void drawScopeFooter (juce::Graphics&, juce::Rectangle<float>, int) const;
    void drawHoldIndicator (juce::Graphics&, juce::Rectangle<float>) const;
    void rebuildVisibleBuffers (int visible, bool applyVisualOffset = true);
    bool refreshTriggeredSnapshot();
    void reserveTriggeredBuffers();
    void buildFreeRunTriggeredSnapshot();
    bool updateTriggeredAutoGain() noexcept;
    void setDelayFromDrag (const juce::MouseEvent&);

    // Temporary mouse-hold inspection. beginInspectionHold pauses the display
    // (without touching the manual Freeze state) and anchors the pan; update
    // InspectionPan recomputes displayScrollMs deterministically from that
    // anchor; endInspectionHold resumes live movement.
    void beginInspectionHold (float mouseX) noexcept;
    void updateInspectionPan (float mouseX) noexcept;
    void endInspectionHold() noexcept;
    float clampDisplayScrollMs (float value) const noexcept;

    // Shared release path for mouseUp/mouseExit so a delay-drag or inspection
    // hold can never survive past the gesture that started it (e.g. the mouse
    // leaving the component mid-drag).
    void cancelActiveGestures() noexcept;

    static constexpr int historyLength = 8192;
    static constexpr int scratchSize   = 256;

    ScopeFifo& fifo;
    const HitCaptureBuffer& hitCapture;

    // Ring buffers sized once in the ctor, never reallocated.
    std::vector<float> mainHistory;
    std::vector<float> sidechainHistory;
    int writeIndex = 0;   // next slot to overwrite (oldest sample)

    bool  frozen               = false;   // manual Freeze button
    bool  interactionHoldActive = false;  // temporary mouse-hold inspection
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
    std::vector<float> triggeredScratchBass;
    std::vector<float> triggeredScratchKick;
    std::array<std::vector<float>, ghostCount> ghostBass;
    std::array<std::vector<float>, ghostCount> ghostKick;
    int latestTriggeredSequence = 0;
    int triggeredPreRollSamples = 0;
    KickReferenceState kickReferenceState = KickReferenceState::NoReference;
    int reservedTriggeredSamples = 0;

    // Effective sample rate of whatever currently fills the triggered buffers.
    // The real per-hit snapshot comes from the full-rate HitCaptureBuffer, but
    // the watchdog fallback fills from the DECIMATED scope ring, so the ms axis
    // has to know which source produced the data or the labels/grid come out
    // ~decimation× wrong. Updated whenever the snapshot is (re)built.
    double triggeredSnapshotRate = 44100.0;

    int freeRunTicks = 0;
    static constexpr int freeRunWatchdogTicks = 120;

    juce::RangedAudioParameter* delayParameter = nullptr;
    bool delayGestureActive = false;
    float dragStartX = 0.0f;
    float dragStartDelayMs = 0.0f;

    float displayScrollMs = 0.0f;
    bool panGestureActive = false;
    float panStartScrollMs = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Oscilloscope)
};
