#pragma once

#include <JuceHeader.h>
#include <array>
#include <vector>

#include "../util/ScopeFifo.h"
#include "../util/HitCaptureBuffer.h"
#include "ScopeVisuals.h"

// Timer-polled scope. Reads paired main/sidechain samples from a ScopeFifo
// into two fixed-size ring buffers for the scrolling views, and assembles the
// HitCaptureBuffer's progressive sweep stream into a ReVision-style triggered
// view: the kick reference is locked once and held static while every new hit
// redraws the live bass left-to-right over it, with the previous hits fading
// out underneath like phosphor. Auto-gain scales quiet signals up so they fill
// the view, a freeze toggle stops the display so the waveform can be
// inspected, and time/amplitude zoom (sliders or mouse wheel) let you inspect
// alignment closely.
class Oscilloscope : public juce::Component,
                     public juce::SettableTooltipClient,
                     private juce::Timer
{
public:
    Oscilloscope (ScopeFifo& fifoToRead, HitCaptureBuffer& hitCaptureToRead);
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
    // inspection. Both the timer (whether it advances the history/sweep) and
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

    // The kick's captured window is locked to the first completed hit after
    // each (re)lock point and held static, since its shape doesn't
    // meaningfully change hit-to-hit — only the bass sweep keeps redrawing on
    // retrigger. Call this to force re-capturing a fresh kick reference on the
    // next hit (e.g. if the kick sample/pattern changed and the old reference
    // is stale). The current display stays on screen while the re-lock waits,
    // so re-locking never blanks the view.
    void relockKickReference() noexcept;
    KickReferenceState getKickReferenceState() const noexcept { return kickReferenceState; }

    // Time zoom: 1x shows the whole history, higher values show only the most
    // recent slice stretched across the width. Amplitude zoom multiplies the
    // auto-gain. Both are clamped to sane ranges. Wheel input adjusts the
    // TARGET values and the timer glides the live values toward them, so
    // zooming feels animated rather than stepped; these direct setters snap
    // both so programmatic calls take effect immediately.
    void setTimeZoom (float z) noexcept { timeZoom = targetTimeZoom = juce::jlimit (1.0f, 16.0f, z); repaint(); }
    void setAmpZoom  (float z) noexcept { ampZoom  = targetAmpZoom  = juce::jlimit (1.0f, 8.0f,  z); repaint(); }
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

    // Per-pixel-column min/max envelope band for a zoomed-out trace: fills the
    // band between the column minima and maxima and strokes its outline, so
    // heavily-decimated views render stable peak envelopes instead of the
    // shimmering point-sampled polylines. Uses the column scratch arrays.
    void strokeMinMaxBand (juce::Graphics&, juce::Rectangle<float> bounds,
                           const float* source, int visible, float gain,
                           float centreY, float halfHeight,
                           juce::Colour colour, float strokeWidth);
    void drawTransientMarkers (juce::Graphics&, juce::Rectangle<float>, int) const;
    void drawScopeFooter (juce::Graphics&, juce::Rectangle<float>, int) const;
    void drawHoldIndicator (juce::Graphics&, juce::Rectangle<float>) const;
    void rebuildVisibleBuffers (int visible, bool applyVisualOffset = true,
                                bool computeSmoothedEnvelope = true);

    // --- ReVision-style triggered sweep engine ------------------------------
    // Drains the HitCaptureBuffer's progressive full-rate sweep stream into
    // sweepBass/sweepKick. Each hit begins with a start marker: the previous
    // sweep rotates into the ghost stack and the new one grows left-to-right,
    // sample-accurately, so the trace never jumps or shimmers — the ONLY
    // change on screen is the sweep head advancing. When `consume` is false
    // (display held) the stream is still drained so it can't overflow, but the
    // on-screen sweep stays untouched and resyncs at the next hit.
    // Returns true when the displayed state changed (a repaint is due).
    bool drainSweepStream (bool consume);
    void beginNewSweep();
    void finishSweep();
    void beginPendingRelockSweep();
    void finishPendingRelockSweep();
    void promoteCurrentSweepToGhost();
    void ensureSweepBuffersSized();
    void buildWaitingFallback();
    bool glideTriggeredAutoGain() noexcept;

    // One waveform lane of the triggered window: the visible slice
    // [first, first + visible) clipped to the actually-filled prefix
    // [0, fill). Strokes the trace and optionally lays a soft glow under it
    // and a vertical-gradient fill beneath it toward the midline.
    void drawTriggeredTrace (juce::Graphics&, juce::Rectangle<float> bounds,
                             const float* data, int fill, int first, int visible,
                             float sampleXStep, float midY, float halfHeight, float gain,
                             juce::Colour colour, float strokeWidth,
                             float fillAlpha, float glowAlpha) const;

    bool refreshingSweepIsLive() const noexcept { return ticksSinceFifoRead < idleAfterTicks; }
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
    HitCaptureBuffer& hitCapture;

    // Ring buffers sized once in the ctor, never reallocated.
    std::vector<float> mainHistory;
    std::vector<float> sidechainHistory;
    int writeIndex = 0;   // next slot to overwrite (oldest sample)

    bool  frozen               = false;   // manual Freeze button
    bool  interactionHoldActive = false;  // temporary mouse-hold inspection
    float displayGain = 1.0f;   // smoothed auto-gain applied to both traces
    float timeZoom    = 1.0f;   // horizontal zoom (1..16), glides toward target
    float ampZoom     = 1.0f;   // vertical zoom (1..8), glides toward target
    float targetTimeZoom = 1.0f;
    float targetAmpZoom  = 1.0f;

    // Horizontal fraction (0 = left, 1 = right) of the last wheel event, used
    // to keep the time under the cursor fixed while zooming a held/scrolled
    // view. A live view stays anchored to the right ("now") edge instead.
    float zoomAnchorFraction = 1.0f;
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

    // Scratch for the per-pixel-column min/max envelope rendering (one entry
    // per pixel column; historyLength safely exceeds any plausible width).
    std::array<float, historyLength> columnMinScratch {};
    std::array<float, historyLength> columnMaxScratch {};

    // --- Triggered sweep state (all full-rate, windowSamples long) ----------
    static constexpr int ghostCount = 4;
    std::vector<float> kickReference;                    // locked kick window
    std::vector<float> sweepBass;                        // current/last bass sweep
    std::vector<float> sweepKick;                        // kick riding with the sweep (pending reference)
    std::vector<float> pendingRelockBass;                // hidden candidate while old ref remains visible
    std::vector<float> pendingRelockKick;
    std::array<std::vector<float>, ghostCount> ghostBass; // past sweeps, newest first
    std::array<int, ghostCount> ghostFill {};
    int sweepFill = 0;               // samples of the current sweep assembled so far
    bool sweepDiscarding = true;     // waiting for the next hit's start marker
    int pendingRelockFill = 0;
    bool pendingRelockDiscarding = true;
    float pendingRelockPeak = 0.0f;
    bool kickReferenceValid = false;
    float sweepPeak = 0.0f;          // running |peak| of the current sweep (both lanes)
    float kickReferencePeak = 0.0f;
    float targetDisplayGain = 0.0f;  // sticky auto-gain target; 0 = unseeded
    ScopePeakMarkers sweepMarkers;   // cached at sweep completion (full-window indices)
    int sweepWindowSamples = 0;
    int sweepPreRollSamples = 0;
    KickReferenceState kickReferenceState = KickReferenceState::NoReference;

    // Pre-first-kick fallback: the decimated ring shown live so the triggered
    // view is never blank while waiting for the first hit. Cleared for good
    // once a real sweep starts. Its ms axis runs at the DECIMATED ring rate,
    // not the full rate, hence the separate rate field.
    std::vector<float> fallbackBass;
    std::vector<float> fallbackKick;
    int fallbackPreRoll = 0;
    double fallbackRate = 44100.0;

    int freeRunTicks = 0;
    static constexpr int freeRunWatchdogTicks = 120;

    // Ticks since the FIFO last delivered samples. Drives the triggered view's
    // status line ("bass live" vs "input idle — showing last capture") so a
    // stale display can never be mistaken for a live one.
    int ticksSinceFifoRead = 1000;
    static constexpr int idleAfterTicks = 30;   // ~0.5 s at 60 Hz

    juce::RangedAudioParameter* delayParameter = nullptr;

    bool delayGestureActive = false;
    float dragStartX = 0.0f;
    float dragStartDelayMs = 0.0f;

    float displayScrollMs = 0.0f;
    bool panGestureActive = false;
    float panStartScrollMs = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Oscilloscope)
};
