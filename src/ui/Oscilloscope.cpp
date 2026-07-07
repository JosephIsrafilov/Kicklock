#include "Oscilloscope.h"
#include "UiFormatters.h"

namespace
{
    const auto scopeBackground = juce::Colour (0xff101418);
    const auto scopeBorder     = juce::Colour (0xff27313a);
    const auto gridMajor       = juce::Colour (0xff2d3943);
    const auto gridMinor       = juce::Colour (0xff1c252d);
    const auto labelColour     = juce::Colour (0xff8b99a6);
    const auto bassColour      = juce::Colour (0xff2dd4bf);
    const auto kickColour      = juce::Colour (0xfff97316);
    const auto constructive    = juce::Colour (0xff47c972);
    const auto destructive     = juce::Colour (0xffff5d5d);
    const auto traceColour     = juce::Colour (0xfff5f7fa);

    float chooseMajorStepMs (float visibleWindowMs) noexcept
    {
        constexpr float steps[] = { 0.1f, 0.2f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 20.0f,
                                    50.0f, 100.0f, 200.0f, 500.0f, 1000.0f };

        constexpr float targetDivisions = 6.0f;

        for (float step : steps)
            if (visibleWindowMs / step <= targetDivisions)
                return step;

        return steps[std::size (steps) - 1];
    }

    juce::String formatTimeLabel (float ms)
    {
        if (std::abs (ms) >= 100.0f)
            return juce::String ((int) std::round (ms)) + " ms";

        return juce::String (ms, std::abs (ms) >= 10.0f ? 1 : 2) + " ms";
    }

    void drawMarkerTriangle (juce::Graphics& g, float centreX, float topY, juce::Colour colour)
    {
        juce::Path triangle;
        triangle.startNewSubPath (centreX, topY);
        triangle.lineTo (centreX - 5.0f, topY - 8.0f);
        triangle.lineTo (centreX + 5.0f, topY - 8.0f);
        triangle.closeSubPath();

        g.setColour (colour);
        g.fillPath (triangle);
    }
}

Oscilloscope::Oscilloscope (ScopeFifo& fifoToRead, HitCaptureBuffer& hitCaptureToRead)
    : fifo (fifoToRead),
      hitCapture (hitCaptureToRead),
      mainHistory ((size_t) historyLength, 0.0f),
      sidechainHistory ((size_t) historyLength, 0.0f)
{
    startTimerHz (60);
}

Oscilloscope::~Oscilloscope()
{
    stopTimer();
}

void Oscilloscope::setTimebase (double newSampleRate, int newDecimationFactor) noexcept
{
    const double resolvedSampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
    const int resolvedDecimation = juce::jmax (1, newDecimationFactor);

    if (sampleRate != resolvedSampleRate || decimationFactor != resolvedDecimation)
    {
        sampleRate = resolvedSampleRate;
        decimationFactor = resolvedDecimation;
        ensureSweepBuffersSized();
        repaint();
    }
}

void Oscilloscope::setTempoInfo (double newBpm, bool available) noexcept
{
    const bool resolvedAvailable = available && newBpm > 0.0;
    const double resolvedBpm = resolvedAvailable ? newBpm : 0.0;

    if (tempoAvailable != resolvedAvailable || bpm != resolvedBpm)
    {
        tempoAvailable = resolvedAvailable;
        bpm = resolvedBpm;
        repaint();
    }
}

void Oscilloscope::timerCallback()
{
    std::array<float, scratchSize> mainScratch {};
    std::array<float, scratchSize> sidechainScratch {};

    bool anyRead = false;

    while (true)
    {
        const int count = fifo.readAvailable (mainScratch.data(), sidechainScratch.data(), scratchSize);
        if (count == 0)
            break;

        anyRead = true;

        // Always keep draining the FIFO so it never overflows, but only advance
        // the displayed history when the display isn't held (manual freeze or a
        // temporary mouse-hold inspection). Held => the on-screen snapshot stays
        // put so a drag/scrub has a stable reference underneath it.
        if (! isDisplayFrozen())
        {
            for (int i = 0; i < count; ++i)
            {
                mainHistory[(size_t) writeIndex]      = mainScratch[(size_t) i];
                sidechainHistory[(size_t) writeIndex] = sidechainScratch[(size_t) i];
                writeIndex = (writeIndex + 1) % historyLength;
            }
        }
    }

    // Track whether the input stream is alive; the triggered status line uses
    // this to say "bass live" vs "input idle — showing last capture" so stale
    // data can never be mistaken for a live view.
    if (anyRead)
        ticksSinceFifoRead = 0;
    else if (ticksSinceFifoRead < 1000)
    {
        // One repaint exactly when the stream is declared idle, so the status
        // line flips to "input idle" without a per-tick repaint while stopped.
        if (++ticksSinceFifoRead == idleAfterTicks && viewMode == ScopeViewMode::Triggered)
            repaint();
    }

    const bool relockPending = kickReferenceState == KickReferenceState::RelockPending;

    // Always drain the sweep stream so it can never overflow, but only advance
    // the on-screen sweep when the display isn't held. An armed re-lock keeps
    // consuming even while frozen so the new reference can still lock on the
    // next hit (matching the old snapshot behaviour).
    const bool consumeSweep = ! isDisplayFrozen() || relockPending;
    bool triggeredDirty = drainSweepStream (consumeSweep);

    if (viewMode == ScopeViewMode::Triggered && consumeSweep)
    {
        if (triggeredDirty)
        {
            freeRunTicks = 0;
        }
        else if (anyRead && kickReferenceState == KickReferenceState::NoReference
                 && sweepFill == 0
                 && ++freeRunTicks >= freeRunWatchdogTicks)
        {
            // The watchdog fallback only exists to show SOMETHING before the
            // first kick is detected. It must (a) require samples to actually
            // be flowing — a stopped transport is not "no trigger detected" —
            // and (b) never engage once a sweep has started or a reference is
            // locked, so a gap in the material can't destroy the display.
            buildWaitingFallback();
            triggeredDirty = true;
        }

        // The auto-gain glides toward its per-hit target every tick; repaint
        // while it actually moves so the trace settles smoothly between hits.
        if (glideTriggeredAutoGain())
            triggeredDirty = true;

        if (triggeredDirty)
            repaint();
    }

    if (viewMode != ScopeViewMode::Triggered && anyRead && ! isDisplayFrozen())
    {
        repaint();
    }

    // Glide the live zoom values toward the wheel targets (~80 ms settle at
    // 60 Hz) so zooming animates instead of stepping per wheel notch. While a
    // held/scrolled view is gliding, compensate the scroll so the time under
    // the cursor stays fixed; a live view keeps its right ("now") edge anchored.
    if (std::abs (targetTimeZoom - timeZoom) > 1.0e-3f || std::abs (targetAmpZoom - ampZoom) > 1.0e-3f)
    {
        const int oldVisible = calculateVisibleScopeSamples (historyLength, sampleRate, decimationFactor,
                                                             timeZoom, gridDivision, tempoAvailable, bpm);

        timeZoom += 0.35f * (targetTimeZoom - timeZoom);
        ampZoom  += 0.35f * (targetAmpZoom  - ampZoom);
        if (std::abs (targetTimeZoom - timeZoom) <= 1.0e-3f) timeZoom = targetTimeZoom;
        if (std::abs (targetAmpZoom  - ampZoom)  <= 1.0e-3f) ampZoom  = targetAmpZoom;

        if (viewMode != ScopeViewMode::Triggered)
        {
            if (isDisplayFrozen() || displayScrollMs > 0.01f)
            {
                const int newVisible = calculateVisibleScopeSamples (historyLength, sampleRate, decimationFactor,
                                                                     timeZoom, gridDivision, tempoAvailable, bpm);
                const float oldMs = (float) calculateVisibleWindowMs (oldVisible, sampleRate, decimationFactor);
                const float newMs = (float) calculateVisibleWindowMs (newVisible, sampleRate, decimationFactor);
                displayScrollMs = clampDisplayScrollMs (scopeAnchoredZoomScrollMs (displayScrollMs, oldMs, newMs,
                                                                                   zoomAnchorFraction));
            }
            else
            {
                displayScrollMs = clampDisplayScrollMs (displayScrollMs);
            }
        }

        repaint();
    }
}

void Oscilloscope::paint (juce::Graphics& g)
{
    const auto panelBounds = getLocalBounds().toFloat().reduced (1.0f);

    g.setColour (scopeBackground);
    g.fillRoundedRectangle (panelBounds, 12.0f);
    g.setColour (scopeBorder);
    g.drawRoundedRectangle (panelBounds, 12.0f, 1.2f);

    auto plotBounds = panelBounds.reduced (12.0f, 10.0f);
    plotBounds.removeFromTop (12.0f);
    // The footer gets its own strip BELOW the plot so it can never collide
    // with the grid's time labels drawn along the plot's bottom edge.
    const auto footerStrip = plotBounds.removeFromBottom (20.0f);

    const int visible = calculateVisibleScopeSamples (historyLength, sampleRate, decimationFactor,
                                                      timeZoom, gridDivision, tempoAvailable, bpm);

    if (viewMode == ScopeViewMode::Triggered)
    {
        drawTriggeredMode (g, plotBounds, displayGain * ampZoom);

        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.setFont (juce::Font (juce::FontOptions (12.0f)).boldened());
        g.drawText (scopeModeCaption (viewMode),
                    plotBounds.removeFromTop (16.0f).toNearestInt(),
                    juce::Justification::centredLeft);

        drawHoldIndicator (g, panelBounds);
        return;
    }

    // Free-run neither shades phase relation nor draws Δ markers, so skip the
    // envelope-smoothing passes there — two O(n) loops per frame at 60 Hz.
    rebuildVisibleBuffers (visible, scopeModeAppliesVisualOffset (viewMode),
                           viewMode != ScopeViewMode::FreeRun);

    float peak = 0.0f;
    for (int i = 0; i < visible; ++i)
    {
        peak = juce::jmax (peak, std::abs (visibleMainBuffer[(size_t) i]));
        peak = juce::jmax (peak, std::abs (visibleSideBuffer[(size_t) i]));
    }

    const float targetGain = peak > 1.0e-4f ? juce::jlimit (1.0f, 40.0f, 0.88f / peak) : 1.0f;
    displayGain += 0.18f * (targetGain - displayGain);
    const float gain = displayGain * ampZoom;

    drawGrid (g, plotBounds, plotBounds.getCentreY(), viewMode == ScopeViewMode::Separate, visible);

    switch (viewMode)
    {
        case ScopeViewMode::Triggered:  break;
        case ScopeViewMode::FreeRun:    drawFreeRunMode (g, plotBounds, visible, gain, plotBounds.getCentreY()); break;
        case ScopeViewMode::PhaseDelta: drawPhaseDeltaMode (g, plotBounds, visible, gain, plotBounds.getCentreY()); break;
        case ScopeViewMode::Overlay:    drawOverlayMode (g, plotBounds, visible, gain, plotBounds.getCentreY()); break;
        case ScopeViewMode::Separate:   drawSeparateMode (g, plotBounds, visible, gain); break;
    }

    // Transient markers read the aligned relationship, so they only make sense
    // in the alignment views — not in the raw Free-run scope.
    if (viewMode != ScopeViewMode::FreeRun)
        drawTransientMarkers (g, plotBounds, visible);

    drawScopeFooter (g, footerStrip, visible);

    g.setColour (juce::Colours::white.withAlpha (0.9f));
    g.setFont (juce::Font (juce::FontOptions (12.0f)).boldened());
    g.drawText (scopeModeCaption (viewMode),
                plotBounds.removeFromTop (16.0f).toNearestInt(),
                juce::Justification::centredLeft);

    drawHoldIndicator (g, panelBounds);
}

void Oscilloscope::drawHoldIndicator (juce::Graphics& g, juce::Rectangle<float> panelBounds) const
{
    // Manual freeze wins the badge; a temporary inspection hold shows HOLD so
    // the user can tell "I paused this by holding" from "I froze this".
    const char* text = frozen ? "FROZEN" : interactionHoldActive ? "HOLD" : nullptr;
    if (text == nullptr)
        return;

    g.setColour (juce::Colours::white.withAlpha (0.85f));
    g.setFont (juce::Font (juce::FontOptions (11.0f)).boldened());
    g.drawText (text,
                juce::Rectangle<int> ((int) (panelBounds.getRight() - 76.0f),
                                      (int) (panelBounds.getY() + 8.0f), 64, 14),
                juce::Justification::centredRight);
}

void Oscilloscope::drawTriggeredMode (juce::Graphics& g,
                                      juce::Rectangle<float> bounds,
                                      float gain)
{
    const float midY = bounds.getCentreY();
    const float halfHeight = bounds.getHeight() * 0.46f;

    g.setColour (gridMajor.brighter (0.25f));
    g.drawHorizontalLine ((int) std::round (midY), bounds.getX(), bounds.getRight());
    g.setColour (gridMinor.brighter (0.15f));
    g.drawHorizontalLine ((int) std::round (midY - halfHeight * 0.5f), bounds.getX(), bounds.getRight());
    g.drawHorizontalLine ((int) std::round (midY + halfHeight * 0.5f), bounds.getX(), bounds.getRight());

    // Pick the data source: the live full-rate sweep once hits are flowing,
    // or the decimated-ring fallback before the first hit so the view is
    // never blank. Both share the exact same rendering path below.
    bool anyGhost = false;
    for (int fill : ghostFill)
        anyGhost = anyGhost || fill > 1;

    const bool sweepHasContent = sweepFill > 1 || anyGhost || kickReferenceValid;
    const bool fallbackActive = ! sweepHasContent && (int) fallbackBass.size() > 1;

    const int n = fallbackActive ? (int) fallbackBass.size() : sweepWindowSamples;

    if (n <= 1 || (! sweepHasContent && ! fallbackActive))
    {
        g.setColour (labelColour);
        g.setFont (juce::Font (juce::FontOptions (12.0f)).boldened());
        g.drawText (triggeredScopeEmptyText (kickReferenceState),
                    bounds.withSizeKeepingCentre (220.0f, 18.0f).toNearestInt(),
                    juce::Justification::centred);
        return;
    }

    const float* bassData = fallbackActive ? fallbackBass.data() : sweepBass.data();
    const int bassFillCount = fallbackActive ? n : sweepFill;
    const float* kickData = fallbackActive ? fallbackKick.data()
                          : kickReferenceValid ? kickReference.data()
                                               : sweepKick.data();
    const int kickFillCount = fallbackActive ? n : (kickReferenceValid ? n : sweepFill);
    const int preRoll = fallbackActive ? fallbackPreRoll : sweepPreRollSamples;
    const double rate = fallbackActive ? fallbackRate : sampleRate;

    const int safePreRoll = juce::jlimit (0, n - 1, preRoll);

    // Time zoom: select the visible slice of the capture window, keeping the
    // trigger line at a fixed fractional x. The ms axis uses the source's own
    // sample rate (full-rate sweep vs the decimated fallback ring).
    const auto range = computeTriggeredVisibleRange (n, safePreRoll, timeZoom);
    const int first = range.first;
    const int visible = juce::jmax (2, range.visible);
    const int last = first + visible - 1;

    const float sampleXStep = bounds.getWidth() / (float) (visible - 1);
    const float triggerX = bounds.getX() + (float) (safePreRoll - first) * sampleXStep;

    const float preMs = samplesToMs (safePreRoll - first, rate);
    const float postMs = samplesToMs (last - safePreRoll, rate);

    // Zoom-adaptive gridline spacing: a fixed step clutters when zoomed out and
    // starves when zoomed deep; pick the step from the visible span instead.
    const float windowMs = juce::jmax (0.1f, preMs + postMs);
    const float gridStepMs = chooseMajorStepMs (windowMs);

    g.setColour (gridMinor);
    for (float ms = -std::ceil (preMs / gridStepMs) * gridStepMs;
         ms <= postMs + 0.001f;
         ms += gridStepMs)
    {
        const float x = triggerX + bounds.getWidth() * (ms / windowMs);
        if (x >= bounds.getX() && x <= bounds.getRight())
            g.drawVerticalLine ((int) std::round (x), bounds.getY(), bounds.getBottom());
    }

    g.setColour (juce::Colours::white.withAlpha (0.65f));
    g.drawVerticalLine ((int) std::round (triggerX), bounds.getY(), bounds.getBottom());

    // Faint decaying "ghost" strokes of the last few completed bass sweeps
    // (oldest first, so the most recent sits brightest just under the live
    // sweep) — phosphor persistence instead of an abrupt swap, so every hit
    // visibly redraws in place without blinking.
    if (! fallbackActive)
        for (int gi = ghostCount - 1; gi >= 0; --gi)
            drawTriggeredTrace (g, bounds, ghostBass[(size_t) gi].data(), ghostFill[(size_t) gi],
                                first, visible, sampleXStep, midY, halfHeight, gain,
                                bassColour.withAlpha (scopeSweepGhostAlpha (gi, ghostCount)),
                                1.0f, 0.0f, 0.0f);

    // Kick lane: the locked reference (or, before the first lock completes,
    // the kick assembling live alongside the sweep).
    drawTriggeredTrace (g, bounds, kickData, kickFillCount, first, visible, sampleXStep,
                        midY, halfHeight, gain,
                        kickColour.withAlpha (kickReferenceValid ? 0.95f : 0.80f),
                        1.8f, 0.30f, 0.0f);

    // Bass lane: the live sweep, redrawn left-to-right on every hit
    // (ReVision behaviour). Samples past the sweep head simply don't exist
    // yet, so the trace grows in real time and never jumps.
    drawTriggeredTrace (g, bounds, bassData, bassFillCount, first, visible, sampleXStep,
                        midY, halfHeight, gain,
                        bassColour.withAlpha (0.98f), 2.2f, 0.34f, 0.16f);

    // Sweep head: while the current hit's window is still filling, a bright
    // leading edge shows exactly where the live redraw has got to.
    if (! fallbackActive && sweepFill > 1 && sweepFill < n
        && ! isDisplayFrozen() && refreshingSweepIsLive())
    {
        const int headIndex = sweepFill - 1;
        if (headIndex >= first && headIndex <= last)
        {
            const float headX = bounds.getX() + (float) (headIndex - first) * sampleXStep;
            const float headY = midY - juce::jlimit (-1.0f, 1.0f,
                                                     bassData[(size_t) headIndex] * gain) * halfHeight;

            g.setColour (bassColour.brighter (0.6f).withAlpha (0.30f));
            g.drawVerticalLine ((int) std::round (headX), bounds.getY(), bounds.getBottom());
            g.setColour (bassColour.brighter (0.8f));
            g.fillEllipse (headX - 2.5f, headY - 2.5f, 5.0f, 5.0f);
        }
    }

    // Phase agreement strip: an always-visible cancellation read-out along the
    // bottom of the plot — green where the (envelope-smoothed) kick and bass
    // low ends push the same way, red where they fight. Covers only the part
    // of the window both lanes have actually reached, so it grows with the
    // sweep. Smoothed envelopes (same tested classifier Phase Delta uses) keep
    // it stable instead of flickering on broadband zero crossings; runs of
    // equal colour merge into single rects so it costs a handful of fills.
    {
        const int filled = juce::jmin (bassFillCount, kickFillCount);
        const int stripSpan = juce::jmin (visible, filled - first);
        if (stripSpan > 1)
        {
            // Sample the swept slice down to pixel-column resolution FIRST,
            // then envelope-smooth the columns: the capture window can exceed
            // the historyLength-sized scratch buffers at high sample rates,
            // and the strip only needs per-column values anyway — O(columns),
            // not O(window).
            const float stripPixelSpan = (float) (stripSpan - 1) * sampleXStep;
            const int columns = juce::jlimit (2, historyLength, (int) std::ceil (stripPixelSpan));
            for (int c = 0; c < columns; ++c)
            {
                const int k = (int) ((long long) c * (stripSpan - 1) / (columns - 1));
                columnMinScratch[(size_t) c] = bassData[(size_t) (first + k)];
                columnMaxScratch[(size_t) c] = kickData[(size_t) (first + k)];
            }

            const double columnRate = rate * (double) columns / (double) stripSpan;
            smoothScopeEnvelope (columnMinScratch.data(), smoothedMainBuffer.data(), columns, columnRate);
            smoothScopeEnvelope (columnMaxScratch.data(), smoothedSideBuffer.data(), columns, columnRate);

            const float stripY = bounds.getBottom() - 22.0f;
            const float stripH = 6.0f;

            auto colourFor = [] (PhaseRelation r) -> juce::Colour
            {
                if (r == PhaseRelation::Constructive) return constructive;
                if (r == PhaseRelation::Destructive)  return destructive;
                return {};
            };

            int runStart = 0;
            auto runRelation = classifyPhaseRelation (smoothedMainBuffer[0], smoothedSideBuffer[0]);

            auto flushRun = [&] (int endColumn)
            {
                const auto colour = colourFor (runRelation);
                if (! colour.isTransparent())
                {
                    const float x0 = bounds.getX() + stripPixelSpan * (float) runStart / (float) columns;
                    const float x1 = bounds.getX() + stripPixelSpan * (float) endColumn / (float) columns;
                    g.setColour (colour.withAlpha (0.55f));
                    g.fillRect (juce::Rectangle<float> (x0, stripY, juce::jmax (1.0f, x1 - x0), stripH));
                }
            };

            for (int c = 1; c < columns; ++c)
            {
                const auto relation = classifyPhaseRelation (smoothedMainBuffer[(size_t) c],
                                                             smoothedSideBuffer[(size_t) c]);
                if (relation != runRelation)
                {
                    flushRun (c);
                    runStart = c;
                    runRelation = relation;
                }
            }
            flushRun (columns);

            g.setColour (labelColour.withAlpha (0.8f));
            g.setFont (juce::Font (juce::FontOptions (9.0f)).boldened());
            g.drawText ("PHASE", juce::Rectangle<int> ((int) bounds.getX(), (int) (stripY - 11.0f), 40, 10),
                        juce::Justification::centredLeft);
            g.setColour (constructive.withAlpha (0.9f));
            g.drawText ("add", juce::Rectangle<int> ((int) bounds.getX() + 42, (int) (stripY - 11.0f), 26, 10),
                        juce::Justification::centredLeft);
            g.setColour (destructive.withAlpha (0.9f));
            g.drawText ("cancel", juce::Rectangle<int> ((int) bounds.getX() + 70, (int) (stripY - 11.0f), 40, 10),
                        juce::Justification::centredLeft);
        }
    }

    // ReVision-style transient markers: triangles at the kick/bass envelope
    // peaks plus a timing verdict in producer language ("bass late"). Computed
    // ONCE per completed hit (see finishSweep) over the full window, so the
    // read-out never jitters frame-to-frame and doesn't change with zoom;
    // here it is only mapped into the visible slice.
    if (! fallbackActive && sweepMarkers.valid)
    {
        auto drawPeakMarker = [&] (int index, juce::Colour colour, float triangleY, float lineAlpha)
        {
            if (index < first || index > last)
                return;

            const float x = bounds.getX() + (float) (index - first) * sampleXStep;
            g.setColour (colour.withAlpha (lineAlpha));
            g.drawLine (x, bounds.getY(), x, bounds.getBottom(), 1.0f);
            drawMarkerTriangle (g, x, bounds.getY() + triangleY, colour);
        };

        drawPeakMarker (sweepMarkers.kickPeakIndex, kickColour, 10.0f, 0.45f);
        drawPeakMarker (sweepMarkers.bassPeakIndex, bassColour, 22.0f, 0.7f);

        g.setColour (juce::Colours::white.withAlpha (0.92f));
        g.setFont (juce::Font (juce::FontOptions (11.0f)).boldened());
        const juce::String deltaLabel = juce::String::fromUTF8 ("\xCE\x94 ")
                                      + (sweepMarkers.deltaMs >= 0.0f ? "+" : "")
                                      + juce::String (sweepMarkers.deltaMs, 2)
                                      + juce::String::fromUTF8 (" ms \xC2\xB7 ")
                                      + timingVerdictText (sweepMarkers.deltaMs);
        g.drawText (deltaLabel,
                    juce::Rectangle<int> ((int) (bounds.getCentreX() - 95.0f),
                                          (int) (bounds.getBottom() - 36.0f),
                                          190, 14),
                    juce::Justification::centred);
    }

    // Always-on status line: the user must never have to guess what the
    // triggered view is showing. Covers every reference state plus whether
    // the input stream is currently alive.
    {
        const bool receiving = refreshingSweepIsLive();
        juce::String status;

        if (fallbackActive)
            status = juce::String::fromUTF8 ("waiting for kick \xE2\x80\x94 showing live input");
        else if (! kickReferenceValid && kickReferenceState == KickReferenceState::NoReference)
            status = "locking kick reference...";
        else if (kickReferenceState == KickReferenceState::RelockPending)
            status = juce::String::fromUTF8 ("re-lock armed \xE2\x80\x94 waiting for next kick");
        else if (frozen)
            status = juce::String::fromUTF8 ("kick locked \xC2\xB7 display frozen");
        else if (! receiving)
            status = juce::String::fromUTF8 ("kick locked \xC2\xB7 input idle \xE2\x80\x94 showing last capture");
        else
            status = juce::String::fromUTF8 ("kick locked \xC2\xB7 bass live");

        g.setColour (labelColour.withAlpha (0.9f));
        g.setFont (juce::Font (juce::FontOptions (11.0f)));
        g.drawText (status,
                    juce::Rectangle<int> ((int) bounds.getX(), (int) (bounds.getY() + 2.0f),
                                          (int) bounds.getWidth(), 14),
                    juce::Justification::centred);
    }

    drawWaveLegend (g, bounds);

    g.setColour (labelColour);
    g.setFont (juce::Font (juce::FontOptions (10.5f)));
    g.drawText ("-" + juce::String (preMs, 0) + " ms",
                juce::Rectangle<int> ((int) bounds.getX(), (int) (bounds.getBottom() - 14.0f), 64, 12),
                juce::Justification::centredLeft);
    g.drawText ("0",
                juce::Rectangle<int> ((int) triggerX - 18, (int) (bounds.getBottom() - 14.0f), 36, 12),
                juce::Justification::centred);
    g.drawText ("+" + juce::String (postMs, 0) + " ms",
                juce::Rectangle<int> ((int) bounds.getRight() - 64, (int) (bounds.getBottom() - 14.0f), 64, 12),
                juce::Justification::centredRight);
}

void Oscilloscope::drawTriggeredTrace (juce::Graphics& g, juce::Rectangle<float> bounds,
                                       const float* data, int fill, int first, int visible,
                                       float sampleXStep, float midY, float halfHeight, float gain,
                                       juce::Colour colour, float strokeWidth,
                                       float fillAlpha, float glowAlpha) const
{
    if (data == nullptr || colour.isTransparent())
        return;

    // Clip the visible slice to the filled prefix; a sweep that hasn't reached
    // this slice yet simply draws nothing (samples past "now" don't exist).
    const int drawCount = juce::jmin (visible, fill - first);
    if (drawCount <= 1)
        return;

    const int pixelSpan = juce::jmax (2, (int) std::ceil ((float) drawCount * sampleXStep));
    const int pointCount = calculateTriggeredRenderPointCount (drawCount, pixelSpan);

    juce::Path stroke;
    stroke.preallocateSpace (pointCount * 3 + 8);

    for (int point = 0; point < pointCount; ++point)
    {
        const int k = triggeredRenderSampleIndex (point, pointCount, drawCount);
        const float x = bounds.getX() + (float) k * sampleXStep;
        const float y = midY - juce::jlimit (-1.0f, 1.0f, data[(size_t) (first + k)] * gain) * halfHeight;

        if (point == 0)
            stroke.startNewSubPath (x, y);
        else
            stroke.lineTo (x, y);
    }

    if (fillAlpha > 0.0f)
    {
        // Vertical gradient fill — strongest at the waveform extremes, fading
        // to near-transparent at the midline — gives the trace depth without
        // washing out the grid the way a flat alpha fill does.
        juce::Path fillPath (stroke);
        fillPath.lineTo (bounds.getX() + (float) (drawCount - 1) * sampleXStep, midY);
        fillPath.lineTo (bounds.getX(), midY);
        fillPath.closeSubPath();

        juce::ColourGradient gradient (colour.withAlpha (fillAlpha), 0.0f, bounds.getY(),
                                       colour.withAlpha (fillAlpha), 0.0f, bounds.getBottom(),
                                       false);
        gradient.addColour (0.5, colour.withAlpha (fillAlpha * 0.1f));
        g.setGradientFill (gradient);
        g.fillPath (fillPath);
    }

    if (glowAlpha > 0.0f)
    {
        // Soft glow under the primary stroke lifts it off the background.
        g.setColour (colour.withMultipliedAlpha (glowAlpha));
        g.strokePath (stroke, juce::PathStrokeType (strokeWidth + 2.6f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
    }

    g.setColour (colour);
    g.strokePath (stroke, juce::PathStrokeType (strokeWidth, juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
}

void Oscilloscope::drawGrid (juce::Graphics& g,
                             juce::Rectangle<float> bounds,
                             float midY,
                             bool separateMode,
                             int visible) const
{
    const float visibleWindowMs = (float) calculateVisibleWindowMs (visible, sampleRate, decimationFactor);

    float majorStepMs = 0.0f;
    int minorDivisions = 2;

    if (tempoAvailable && gridDivision != GridDivision::Milliseconds)
    {
        if (gridDivision == GridDivision::Bar)
        {
            majorStepMs = (float) bpmToQuarterMs (bpm);
            minorDivisions = 0;
        }
        else
        {
            majorStepMs = (float) gridDivisionToMs (bpm, gridDivision);
            minorDivisions = 0;
        }
    }

    if (majorStepMs <= 0.0f)
    {
        majorStepMs = juce::jmax (0.1f, chooseMajorStepMs (visibleWindowMs));
        minorDivisions = 2;
    }

    if (visibleWindowMs > 0.0f)
    {
        // Anchor gridlines to the live ("now") edge so they sit on ROUND ages:
        // the old loop anchored at the buffer's left edge, producing labels
        // like "-2925 ms". A line's age = displayScrollMs + distance from the
        // right edge; draw lines at every multiple of the major step.
        const float scroll = displayScrollMs;
        const int firstMajor = (int) std::ceil (scroll / majorStepMs - 1.0e-3f);

        for (int m = firstMajor; ; ++m)
        {
            const float age = (float) m * majorStepMs;
            const float x = bounds.getRight() - bounds.getWidth() * ((age - scroll) / visibleWindowMs);
            if (x < bounds.getX() - 0.5f)
                break;

            g.setColour (gridMajor);
            g.drawVerticalLine ((int) std::round (x), bounds.getY(), bounds.getBottom());

            for (int minor = 1; minor <= minorDivisions; ++minor)
            {
                const float minorAge = age + majorStepMs * (float) minor / (float) (minorDivisions + 1);
                const float minorX = bounds.getRight() - bounds.getWidth() * ((minorAge - scroll) / visibleWindowMs);
                if (minorX < bounds.getX() - 0.5f || minorX > bounds.getRight() + 0.5f)
                    continue;

                g.setColour (gridMinor);
                g.drawVerticalLine ((int) std::round (minorX), bounds.getY(), bounds.getBottom());
            }

            // Right-align each label so it ends at its line and never spills
            // over the plot edge (or the footer strip below).
            g.setColour (labelColour);
            g.setFont (juce::Font (juce::FontOptions (10.0f)));
            const int labelRight = (int) std::round (x - 3.0f);
            if (labelRight - 56 >= (int) bounds.getX())
                g.drawText (formatTimeLabel (-age),
                            juce::Rectangle<int> (labelRight - 56,
                                                  (int) std::round (bounds.getBottom() - 16.0f),
                                                  56, 12),
                            juce::Justification::centredRight);
        }
    }

    auto drawHorizontalMarkers = [&] (float centreY, float halfHeight)
    {
        g.setColour (gridMajor.brighter (0.25f));
        g.drawHorizontalLine ((int) std::round (centreY), bounds.getX(), bounds.getRight());

        g.setColour (gridMinor.brighter (0.15f));
        g.drawHorizontalLine ((int) std::round (centreY - halfHeight * 0.5f), bounds.getX(), bounds.getRight());
        g.drawHorizontalLine ((int) std::round (centreY + halfHeight * 0.5f), bounds.getX(), bounds.getRight());
    };

    if (separateMode)
    {
        const float laneHalfHeight = bounds.getHeight() * 0.22f;
        drawHorizontalMarkers (bounds.getY() + bounds.getHeight() * 0.25f, laneHalfHeight);
        drawHorizontalMarkers (bounds.getY() + bounds.getHeight() * 0.75f, laneHalfHeight);

        g.setColour (gridMajor.withAlpha (0.65f));
        g.drawHorizontalLine ((int) std::round (bounds.getCentreY()), bounds.getX(), bounds.getRight());
    }
    else
    {
        drawHorizontalMarkers (midY, bounds.getHeight() * 0.46f);
    }
}

void Oscilloscope::drawPhaseDeltaMode (juce::Graphics& g,
                                       juce::Rectangle<float> bounds,
                                       int visible,
                                       float gain,
                                       float midY)
{
    juce::Path whiteTrace;

    // Decimate to at most ~2 points per pixel: iterating every sample issued
    // up to 8192 drawLine calls per frame at 60 Hz — a real CPU hog once the
    // visible window exceeded the width.
    const int pointCount = calculateTriggeredRenderPointCount (visible, (int) std::ceil (bounds.getWidth()));
    const float xStep = bounds.getWidth() / (float) juce::jmax (1, pointCount - 1);
    const float halfHeight = bounds.getHeight() * 0.46f;
    const float lineWidth = juce::jmax (1.0f, xStep + 0.35f);

    for (int point = 0; point < pointCount; ++point)
    {
        const int i = triggeredRenderSampleIndex (point, pointCount, visible);
        const float x = bounds.getX() + (float) point * xStep;
        const float bass = juce::jlimit (-1.0f, 1.0f, visibleMainBuffer[(size_t) i] * gain);
        const float kick = juce::jlimit (-1.0f, 1.0f, visibleSideBuffer[(size_t) i] * gain);
        const float combined = juce::jlimit (-1.0f, 1.0f,
                                             0.5f * (visibleMainBuffer[(size_t) i]
                                                     + visibleSideBuffer[(size_t) i]) * gain);
        const float overlap = juce::jmin (std::abs (bass), std::abs (kick));
        const float signedOverlap = (bass >= 0.0f ? 1.0f : -1.0f) * overlap;
        const float overlapY = midY - signedOverlap * halfHeight;
        const float combinedY = midY - combined * halfHeight;

        // Colour the constructive/destructive fill from the LOW-BAND envelope,
        // not the raw broadband sample sign, so the green/red regions track the
        // musical kick/bass body relationship instead of flickering on every
        // high-frequency zero crossing (P7).
        const auto relation = classifyPhaseRelation (smoothedMainBuffer[(size_t) i],
                                                     smoothedSideBuffer[(size_t) i]);
        if (relation == PhaseRelation::Constructive || relation == PhaseRelation::Destructive)
        {
            g.setColour ((relation == PhaseRelation::Constructive ? constructive : destructive)
                             .withAlpha (0.28f));
            g.drawLine (x, midY, x, overlapY, lineWidth);
        }

        if (point == 0)
            whiteTrace.startNewSubPath (x, combinedY);
        else
            whiteTrace.lineTo (x, combinedY);
    }

    g.setColour (traceColour.withAlpha (0.96f));
    g.strokePath (whiteTrace, juce::PathStrokeType (1.7f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));
}

void Oscilloscope::drawOverlayMode (juce::Graphics& g,
                                    juce::Rectangle<float> bounds,
                                    int visible,
                                    float gain,
                                    float midY)
{
    const float xStep = bounds.getWidth() / (float) (visible - 1);
    const float halfHeight = bounds.getHeight() * 0.46f;

    // Heavily zoomed out, render min/max envelope bands (the midline fills
    // would be visual mud at that density anyway); zoomed in, keep the filled
    // polyline aesthetic.
    if (scopeShouldRenderMinMaxBand (visible, (int) bounds.getWidth()))
    {
        strokeMinMaxBand (g, bounds, visibleSideBuffer.data(), visible, gain,
                          midY, halfHeight, kickColour.withAlpha (0.92f), 1.6f);
        strokeMinMaxBand (g, bounds, visibleMainBuffer.data(), visible, gain,
                          midY, halfHeight, bassColour.withAlpha (0.96f), 2.0f);
        drawWaveLegend (g, bounds);
        return;
    }

    juce::Path bassPath, kickPath, bassFill, kickFill;

    auto buildPaths = [&] (const std::array<float, historyLength>& source,
                           juce::Path& stroke, juce::Path& fill)
    {
        for (int i = 0; i < visible; ++i)
        {
            const float x = bounds.getX() + (float) i * xStep;
            const float v = juce::jlimit (-1.0f, 1.0f, source[(size_t) i] * gain);
            const float y = midY - v * halfHeight;

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

    buildPaths (visibleSideBuffer, kickPath, kickFill);
    buildPaths (visibleMainBuffer, bassPath, bassFill);

    g.setColour (kickColour.withAlpha (0.14f));
    g.fillPath (kickFill);
    g.setColour (kickColour.withAlpha (0.92f));
    g.strokePath (kickPath, juce::PathStrokeType (1.25f, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));

    g.setColour (bassColour.withAlpha (0.18f));
    g.fillPath (bassFill);
    g.setColour (bassColour.withAlpha (0.96f));
    g.strokePath (bassPath, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));

    drawWaveLegend (g, bounds);
}

void Oscilloscope::drawFreeRunMode (juce::Graphics& g,
                                    juce::Rectangle<float> bounds,
                                    int visible,
                                    float gain,
                                    float midY)
{
    // Raw live scope. Unlike Overlay this draws thin unfilled traces (no
    // alignment fills) and, crucially, the visible buffers were rebuilt with
    // NO visual/PDC offset applied — see scopeModeAppliesVisualOffset — so it
    // shows the untouched incoming signals, continuously scrolling with the
    // newest material at the right edge.
    const float xStep = bounds.getWidth() / (float) (visible - 1);
    const float halfHeight = bounds.getHeight() * 0.46f;

    // Zoomed out (several samples per pixel), point-sampled polylines alias
    // and "boil" as the history scrolls; render stable min/max envelope bands
    // instead. Zoomed in, a polyline is smoother-looking and cheaper.
    const bool useBand = scopeShouldRenderMinMaxBand (visible, (int) bounds.getWidth());

    auto strokeTrace = [&] (const std::array<float, historyLength>& source,
                            juce::Colour colour, float width)
    {
        if (useBand)
        {
            strokeMinMaxBand (g, bounds, source.data(), visible, gain, midY, halfHeight, colour, width);
            return;
        }

        juce::Path path;
        for (int i = 0; i < visible; ++i)
        {
            const float x = bounds.getX() + (float) i * xStep;
            const float v = juce::jlimit (-1.0f, 1.0f, source[(size_t) i] * gain);
            const float y = midY - v * halfHeight;

            if (i == 0)
                path.startNewSubPath (x, y);
            else
                path.lineTo (x, y);
        }

        g.setColour (colour);
        g.strokePath (path, juce::PathStrokeType (width, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
    };

    strokeTrace (visibleSideBuffer, kickColour.withAlpha (0.85f), 1.6f);

    strokeTrace (visibleMainBuffer, bassColour.withAlpha (0.92f), 2.0f);

    // Live-edge indicator: the right edge is "now". A faint playhead line plus a
    // small LIVE tag make the newest material obvious and reinforce that this is
    // a running scope, not a frozen alignment snapshot.
    const float edgeX = bounds.getRight();
    g.setColour (juce::Colours::white.withAlpha (0.45f));
    g.drawVerticalLine ((int) std::round (edgeX), bounds.getY(), bounds.getBottom());

    g.setColour (juce::Colours::white.withAlpha (0.55f));
    g.setFont (juce::Font (juce::FontOptions (9.0f)).boldened());
    g.drawText ("LIVE",
                juce::Rectangle<int> ((int) (edgeX - 36.0f), (int) (bounds.getY() + 2.0f), 34, 10),
                juce::Justification::centredRight);

    drawWaveLegend (g, bounds);
}

void Oscilloscope::drawSeparateMode (juce::Graphics& g,
                                     juce::Rectangle<float> bounds,
                                     int visible,
                                     float gain)
{
    const float xStep = bounds.getWidth() / (float) (visible - 1);
    const float laneHalfHeight = bounds.getHeight() * 0.22f;
    const float bassCentreY = bounds.getY() + bounds.getHeight() * 0.25f;
    const float kickCentreY = bounds.getY() + bounds.getHeight() * 0.75f;
    const bool useBand = scopeShouldRenderMinMaxBand (visible, (int) bounds.getWidth());

    auto drawLane = [&] (const std::array<float, historyLength>& source,
                         float centreY,
                         juce::Colour colour,
                         const juce::String& label)
    {
        if (useBand)
        {
            strokeMinMaxBand (g, bounds, source.data(), visible, gain,
                              centreY, laneHalfHeight, colour.withAlpha (0.95f), 1.35f);

            g.setColour (colour.withAlpha (0.8f));
            g.setFont (juce::Font (juce::FontOptions (11.0f)).boldened());
            g.drawText (label,
                        juce::Rectangle<int> ((int) bounds.getX(),
                                              (int) (centreY - laneHalfHeight - 14.0f),
                                              48, 12),
                        juce::Justification::centredLeft);
            return;
        }

        juce::Path stroke;
        juce::Path fill;

        for (int i = 0; i < visible; ++i)
        {
            const float x = bounds.getX() + (float) i * xStep;
            const float v = juce::jlimit (-1.0f, 1.0f, source[(size_t) i] * gain);
            const float y = centreY - v * laneHalfHeight;

            if (i == 0)
            {
                stroke.startNewSubPath (x, y);
                fill.startNewSubPath (x, centreY);
                fill.lineTo (x, y);
            }
            else
            {
                stroke.lineTo (x, y);
                fill.lineTo (x, y);
            }
        }

        fill.lineTo (bounds.getRight(), centreY);
        fill.closeSubPath();

        g.setColour (colour.withAlpha (0.14f));
        g.fillPath (fill);
        g.setColour (colour.withAlpha (0.95f));
        g.strokePath (stroke, juce::PathStrokeType (1.35f, juce::PathStrokeType::curved,
                                                    juce::PathStrokeType::rounded));

        g.setColour (colour.withAlpha (0.8f));
        g.setFont (juce::Font (juce::FontOptions (11.0f)).boldened());
        g.drawText (label,
                    juce::Rectangle<int> ((int) bounds.getX(),
                                          (int) (centreY - laneHalfHeight - 14.0f),
                                          48, 12),
                    juce::Justification::centredLeft);
    };

    drawLane (visibleMainBuffer, bassCentreY, bassColour, "BASS");
    drawLane (visibleSideBuffer, kickCentreY, kickColour, "KICK");
}

void Oscilloscope::drawWaveLegend (juce::Graphics& g, juce::Rectangle<float> bounds) const
{
    auto legend = juce::Rectangle<float> (bounds.getRight() - 112.0f,
                                          bounds.getY() + 18.0f,
                                          104.0f,
                                          14.0f);

    auto drawItem = [&] (juce::Rectangle<float> item, juce::Colour colour, const char* label)
    {
        const float y = item.getCentreY();
        g.setColour (colour.withAlpha (0.92f));
        g.drawLine (item.getX(), y, item.getX() + 14.0f, y, 2.0f);
        g.setFont (juce::Font (juce::FontOptions (10.5f)).boldened());
        g.drawText (label,
                    item.withTrimmedLeft (18.0f).toNearestInt(),
                    juce::Justification::centredLeft);
    };

    drawItem (legend.removeFromLeft (52.0f), bassColour, "BASS");
    drawItem (legend, kickColour, "KICK");
}

void Oscilloscope::strokeMinMaxBand (juce::Graphics& g, juce::Rectangle<float> bounds,
                                     const float* source, int visible, float gain,
                                     float centreY, float halfHeight,
                                     juce::Colour colour, float strokeWidth)
{
    const int numColumns = juce::jlimit (2, historyLength, (int) std::ceil (bounds.getWidth()));
    buildMinMaxColumns (source, 0, visible, numColumns,
                        columnMinScratch.data(), columnMaxScratch.data());

    const float columnW = bounds.getWidth() / (float) numColumns;

    juce::Path band;
    band.preallocateSpace (numColumns * 6 + 8);

    // Forward pass along the column maxima, back along the minima, closed —
    // the filled band IS the trace body, with a stroked outline for crispness.
    // A minimum band thickness keeps a silent/flat signal visible as a line.
    for (int c = 0; c < numColumns; ++c)
    {
        const float x = bounds.getX() + ((float) c + 0.5f) * columnW;
        float yTop = centreY - juce::jlimit (-1.0f, 1.0f, columnMaxScratch[(size_t) c] * gain) * halfHeight;
        float yBot = centreY - juce::jlimit (-1.0f, 1.0f, columnMinScratch[(size_t) c] * gain) * halfHeight;

        if (yBot - yTop < 1.2f)
        {
            const float mid = 0.5f * (yTop + yBot);
            yTop = mid - 0.6f;
            yBot = mid + 0.6f;
        }

        columnMinScratch[(size_t) c] = yBot;   // reuse as screen-space storage
        if (c == 0)
            band.startNewSubPath (x, yTop);
        else
            band.lineTo (x, yTop);
    }

    for (int c = numColumns - 1; c >= 0; --c)
    {
        const float x = bounds.getX() + ((float) c + 0.5f) * columnW;
        band.lineTo (x, columnMinScratch[(size_t) c]);
    }

    band.closeSubPath();

    g.setColour (colour.withMultipliedAlpha (0.55f));
    g.fillPath (band);
    g.setColour (colour);
    g.strokePath (band, juce::PathStrokeType (strokeWidth * 0.6f, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::rounded));
}

void Oscilloscope::drawTransientMarkers (juce::Graphics& g,
                                         juce::Rectangle<float> bounds,
                                         int visible) const
{
    // P7: markers sit on the amplitude ENVELOPE peak (rectify + centered smooth)
    // rather than the raw broadband peak sample, so the Δ-ms read-out reflects
    // the musical kick/bass body offset and stays stable frame to frame. Δ is
    // computed at the scope's effective (decimated) sample rate.
    const double scopeRate = sampleRate / (double) juce::jmax (1, decimationFactor);
    const auto markers = findEnvelopePeakMarkers (visibleMainBuffer.data(),
                                                 visibleSideBuffer.data(),
                                                 visible, scopeRate);
    if (! markers.valid || visible <= 1)
        return;

    const float xStep = bounds.getWidth() / (float) (visible - 1);
    const float bassX = bounds.getX() + (float) markers.bassPeakIndex * xStep;
    const float kickX = bounds.getX() + (float) markers.kickPeakIndex * xStep;

    g.setColour (kickColour.withAlpha (0.45f));
    g.drawLine (kickX, bounds.getY(), kickX, bounds.getBottom(), 1.0f);
    drawMarkerTriangle (g, kickX, bounds.getY() + 10.0f, kickColour);

    g.setColour (bassColour.withAlpha (0.7f));
    g.drawLine (bassX, bounds.getY(), bassX, bounds.getBottom(), 1.0f);
    drawMarkerTriangle (g, bassX, bounds.getY() + 22.0f, bassColour);

    g.setColour (juce::Colours::white.withAlpha (0.92f));
    g.setFont (juce::Font (juce::FontOptions (11.0f)).boldened());
    const juce::String deltaLabel = juce::String::fromUTF8 ("\xCE\x94 ")
                                  + (markers.deltaMs >= 0.0f ? "+" : "")
                                  + juce::String (markers.deltaMs, 2) + " ms · "
                                  + timingVerdictText (markers.deltaMs);
    g.drawText (deltaLabel,
                juce::Rectangle<int> ((int) (bounds.getRight() - 172.0f),
                                      (int) (bounds.getY() + 4.0f),
                                      164, 14),
                juce::Justification::centredRight);
}

void Oscilloscope::drawScopeFooter (juce::Graphics& g,
                                    juce::Rectangle<float> footerStrip,
                                    int visible) const
{
    // Drawn in the dedicated strip BELOW the plot (see paint()), so it never
    // collides with the grid's time labels inside the plot.
    const float visibleWindowMs = samplesToMs (visible * decimationFactor, sampleRate);

    g.setColour (labelColour);
    g.setFont (juce::Font (juce::FontOptions (10.5f)));
    juce::String footer = "Window "
                        + juce::String ((float) visibleWindowMs, visibleWindowMs >= 10.0 ? 1 : 2) + " ms"
                + "  |  Zoom " + juce::String (timeZoom, 1) + "x"
                + "  |  Amp " + juce::String (ampZoom, 1) + "x"
                + "  |  PDC "
                + (scopeModeAppliesVisualOffset (viewMode) ? juce::String (visualOffsetSamples) + " smp"
                                                           : juce::String ("off (raw)"));

    if (tempoAvailable && gridDivision != GridDivision::Milliseconds)
        footer << "  |  " << juce::String ((float) bpm, 1) << " BPM";

    g.drawText (footer,
                footerStrip.toNearestInt().withTrimmedTop (4),
                juce::Justification::centredLeft);
}

void Oscilloscope::rebuildVisibleBuffers (int visible, bool applyVisualOffset,
                                          bool computeSmoothedEnvelope)
{
    // Free-run passes applyVisualOffset=false so it shows the raw incoming
    // signal; every other mode applies the display-only visual/PDC offset so
    // bass and kick line up for comparison.
    const int effectiveOffset = applyVisualOffset ? visualOffsetSamples : 0;
    const int undecimatedScrollSamples = msToSamples(displayScrollMs, sampleRate);
    const int decimatedScrollSamples = undecimatedScrollSamples / std::max(1, decimationFactor);
    const int firstVisible = historyLength - visible - decimatedScrollSamples;

    for (int i = 0; i < visible; ++i)
    {
        const auto indices = resolveRelativeDisplayHistoryIndices (writeIndex, historyLength,
                                                                   firstVisible, i,
                                                                   effectiveOffset,
                                                                   decimationFactor);
        visibleMainBuffer[(size_t) i] = mainHistory[(size_t) indices.bassIndex];
        visibleSideBuffer[(size_t) i] = sidechainHistory[(size_t) indices.kickIndex];
    }

    // P7: low-band envelope-smoothed copies for a musically-readable phase
    // relationship. Raw scope samples are broadband and jitter sample-to-sample,
    // so the green/red constructive-destructive shading and the peak markers
    // read from these smoothed traces instead. The smoothing window is derived
    // from the scope's effective sample rate so it tracks ~a low-frequency
    // period rather than a fixed sample count. Callers whose mode uses neither
    // (Free-run) skip the two O(n) passes.
    if (computeSmoothedEnvelope)
    {
        const double scopeRate = sampleRate / (double) juce::jmax (1, decimationFactor);
        smoothScopeEnvelope (visibleMainBuffer.data(), smoothedMainBuffer.data(), visible, scopeRate);
        smoothScopeEnvelope (visibleSideBuffer.data(), smoothedSideBuffer.data(), visible, scopeRate);
    }
}

void Oscilloscope::ensureSweepBuffersSized()
{
    const int window = hitCapture.getWindowSamples();
    if (window <= 0 || window == sweepWindowSamples)
        return;

    // Window geometry changed (sample rate / prepare). Every full-rate buffer
    // is sized here once, on the message thread, so the drain loop never
    // reallocates. Previously captured content is meaningless at the new
    // rate, so the whole triggered state starts over.
    sweepWindowSamples = window;
    sweepPreRollSamples = hitCapture.getPreRollSamples();

    sweepBass.assign ((size_t) window, 0.0f);
    sweepKick.assign ((size_t) window, 0.0f);
    pendingRelockBass.assign ((size_t) window, 0.0f);
    pendingRelockKick.assign ((size_t) window, 0.0f);
    kickReference.assign ((size_t) window, 0.0f);
    for (auto& buffer : ghostBass)
        buffer.assign ((size_t) window, 0.0f);
    ghostFill.fill (0);

    sweepFill = 0;
    sweepPeak = 0.0f;
    sweepDiscarding = true;
    pendingRelockFill = 0;
    pendingRelockPeak = 0.0f;
    pendingRelockDiscarding = true;
    kickReferenceValid = false;
    kickReferencePeak = 0.0f;
    sweepMarkers = {};
    kickReferenceState = KickReferenceState::NoReference;
    fallbackBass.clear();
    fallbackKick.clear();
}

bool Oscilloscope::drainSweepStream (bool consume)
{
    std::array<float, scratchSize> bassScratch {};
    std::array<float, scratchSize> kickScratch {};
    std::array<unsigned char, scratchSize> flagScratch {};

    bool dirty = false;

    while (true)
    {
        const int count = hitCapture.readSweepStream (bassScratch.data(), kickScratch.data(),
                                                      flagScratch.data(), scratchSize);
        if (count == 0)
            break;

        ensureSweepBuffersSized();

        if (! consume)
        {
            // Display held: keep draining so the stream can't overflow, but
            // leave the on-screen sweep untouched. Discarding until the next
            // start marker means releasing the hold can never resume
            // mid-window with a hidden gap in the trace.
            sweepDiscarding = true;
            continue;
        }

        if (sweepWindowSamples <= 0)
            continue;

        for (int i = 0; i < count; ++i)
        {
            const bool captureRelockSilently = kickReferenceState == KickReferenceState::RelockPending
                                            && kickReferenceValid;

            if ((flagScratch[(size_t) i] & HitCaptureBuffer::sweepStartFlag) != 0)
            {
                if (captureRelockSilently)
                    beginPendingRelockSweep();
                else
                {
                    beginNewSweep();
                    dirty = true;
                }
            }

            if (captureRelockSilently)
            {
                if (pendingRelockDiscarding || pendingRelockFill >= sweepWindowSamples)
                    continue;

                const float bassValue = bassScratch[(size_t) i];
                const float kickValue = kickScratch[(size_t) i];
                pendingRelockBass[(size_t) pendingRelockFill] = bassValue;
                pendingRelockKick[(size_t) pendingRelockFill] = kickValue;
                pendingRelockPeak = juce::jmax (pendingRelockPeak,
                                                std::abs (bassValue),
                                                std::abs (kickValue));
                ++pendingRelockFill;

                if (pendingRelockFill == sweepWindowSamples)
                {
                    finishPendingRelockSweep();
                    dirty = true;
                }

                continue;
            }

            if (sweepDiscarding || sweepFill >= sweepWindowSamples)
                continue;

            const float bassValue = bassScratch[(size_t) i];
            const float kickValue = kickScratch[(size_t) i];
            sweepBass[(size_t) sweepFill] = bassValue;
            sweepKick[(size_t) sweepFill] = kickValue;
            sweepPeak = juce::jmax (sweepPeak, std::abs (bassValue), std::abs (kickValue));
            ++sweepFill;
            dirty = true;

            if (sweepFill == sweepWindowSamples)
                finishSweep();
        }
    }

    return dirty;
}

void Oscilloscope::beginNewSweep()
{
    promoteCurrentSweepToGhost();

    sweepFill = 0;
    sweepPeak = 0.0f;
    sweepDiscarding = false;

    // A real hit is on screen now — the pre-first-kick fallback is done for
    // good (it must never replace a captured display again).
    fallbackBass.clear();
    fallbackKick.clear();
}

void Oscilloscope::beginPendingRelockSweep()
{
    pendingRelockFill = 0;
    pendingRelockPeak = 0.0f;
    pendingRelockDiscarding = false;
}

void Oscilloscope::promoteCurrentSweepToGhost()
{
    // The outgoing sweep becomes the newest ghost only if the full previous
    // window completed. Partial/torn prefixes are not trustworthy history.
    if (! scopeSweepWorthKeepingAsGhost (sweepFill, sweepWindowSamples))
        return;

    for (int i = ghostCount - 1; i > 0; --i)
    {
        ghostBass[(size_t) i].swap (ghostBass[(size_t) (i - 1)]);
        ghostFill[(size_t) i] = ghostFill[(size_t) (i - 1)];
    }

    ghostBass[0].swap (sweepBass);
    ghostFill[0] = sweepFill;
}

void Oscilloscope::finishSweep()
{
    if (kickReferenceShouldReplaceOnCapture (kickReferenceState))
    {
        float peak = 0.0f;
        for (float value : sweepKick)
            peak = juce::jmax (peak, std::abs (value));

        if (scopeKickReferenceCaptureIsValid (peak))
        {
            std::copy (sweepKick.begin(), sweepKick.end(), kickReference.begin());
            kickReferenceValid = true;
            kickReferencePeak = peak;
            kickReferenceState = kickReferenceStateAfterCapture (kickReferenceState);
        }
    }

    // Δ markers are computed ONCE per completed hit over the full window (not
    // the zoomed slice), so the read-out can't jitter frame-to-frame and
    // doesn't change with zoom. Bass from this sweep, kick from the reference.
    sweepMarkers = findEnvelopePeakMarkers (sweepBass.data(),
                                            kickReferenceValid ? kickReference.data()
                                                               : sweepKick.data(),
                                            sweepWindowSamples, sampleRate);

    // Auto-gain retargets per completed hit — with hysteresis, so near-equal
    // hits don't make the whole display breathe.
    const float candidate = scopeAutoGainTargetFromPeak (juce::jmax (sweepPeak, kickReferencePeak));
    if (scopeAutoGainShouldRetarget (targetDisplayGain, candidate))
        targetDisplayGain = candidate;
}

void Oscilloscope::finishPendingRelockSweep()
{
    float kickPeak = 0.0f;
    for (float value : pendingRelockKick)
        kickPeak = juce::jmax (kickPeak, std::abs (value));

    if (! scopeKickReferenceCaptureIsValid (kickPeak))
    {
        pendingRelockFill = 0;
        pendingRelockPeak = 0.0f;
        pendingRelockDiscarding = true;
        return;
    }

    promoteCurrentSweepToGhost();

    sweepBass.swap (pendingRelockBass);
    sweepKick.swap (pendingRelockKick);
    sweepFill = sweepWindowSamples;
    sweepPeak = pendingRelockPeak;
    sweepDiscarding = true;

    std::copy (sweepKick.begin(), sweepKick.end(), kickReference.begin());
    kickReferenceValid = true;
    kickReferencePeak = kickPeak;
    kickReferenceState = kickReferenceStateAfterCapture (kickReferenceState);

    sweepMarkers = findEnvelopePeakMarkers (sweepBass.data(),
                                            kickReference.data(),
                                            sweepWindowSamples, sampleRate);

    const float candidate = scopeAutoGainTargetFromPeak (juce::jmax (sweepPeak, kickReferencePeak));
    if (scopeAutoGainShouldRetarget (targetDisplayGain, candidate))
        targetDisplayGain = candidate;

    pendingRelockFill = 0;
    pendingRelockPeak = 0.0f;
    pendingRelockDiscarding = true;
}

bool Oscilloscope::glideTriggeredAutoGain() noexcept
{
    const float step = 0.18f * (targetDisplayGain - displayGain);
    displayGain += step;

    // Report whether the gain moved enough to be worth a repaint, so the timer
    // can settle the trace between hits without repainting on every idle tick.
    return std::abs (step) > 1.0e-3f;
}

void Oscilloscope::relockKickReference() noexcept
{
    kickReferenceState = kickReferenceStateAfterRelock();

    // Deliberately keeps the current traces on screen while the re-lock waits
    // for the next hit — the reference swaps in place when the next sweep
    // completes, so re-locking never blanks or flickers the view.
    repaint();
}

void Oscilloscope::buildWaitingFallback()
{
    const int desiredSamples = juce::jlimit (2, historyLength,
                                            msToSamples (170.0f, sampleRate) / juce::jmax (1, decimationFactor));
    fallbackBass.resize ((size_t) desiredSamples);
    fallbackKick.resize ((size_t) desiredSamples);

    const int start = writeIndex - desiredSamples;
    float peak = 0.0f;
    for (int i = 0; i < desiredSamples; ++i)
    {
        const int idx = wrapHistoryIndex (start + i, historyLength);
        fallbackBass[(size_t) i] = mainHistory[(size_t) idx];
        fallbackKick[(size_t) i] = sidechainHistory[(size_t) idx];
        peak = juce::jmax (peak, std::abs (fallbackBass[(size_t) i]),
                           std::abs (fallbackKick[(size_t) i]));
    }

    fallbackPreRoll = juce::jlimit (0, desiredSamples - 1,
                                    msToSamples (20.0f, sampleRate) / juce::jmax (1, decimationFactor));
    // The fallback fills from the DECIMATED scope ring, so its ms axis must
    // use the decimated rate or the labels/grid come out ~decimation× wrong.
    fallbackRate = sampleRate / (double) juce::jmax (1, decimationFactor);
    freeRunTicks = freeRunWatchdogTicks;

    // No completed hit exists yet to retarget from, so aim the auto-gain at
    // the live ring content directly (still glided by the timer).
    targetDisplayGain = scopeAutoGainTargetFromPeak (peak);
}

void Oscilloscope::setDelayFromDrag (const juce::MouseEvent& e)
{
    if (delayParameter == nullptr)
        return;

    // Shift is now the gesture that starts a delay-drag at all (it must be held
    // for mouseDown to have entered this gesture in the first place), so it can
    // no longer double as a separate "fine steps" modifier — use coarse
    // sensitivity throughout the drag.
    const float deltaMs = scopeDragPixelsToDelayDeltaMs (e.position.x - dragStartX, false);
    const float nextValue = juce::jlimit (-20.0f, 20.0f, dragStartDelayMs + deltaMs);
    delayParameter->setValueNotifyingHost (delayParameter->convertTo0to1 (nextValue));
}

float Oscilloscope::clampDisplayScrollMs (float value) const noexcept
{
    const int visible = calculateVisibleScopeSamples (historyLength, sampleRate, decimationFactor,
                                                      timeZoom, gridDivision, tempoAvailable, bpm);
    return clampScopeScrollMs (value, historyLength, visible, sampleRate, decimationFactor);
}

void Oscilloscope::beginInspectionHold (float mouseX) noexcept
{
    // Freeze the displayed snapshot WITHOUT touching the manual Freeze state,
    // and anchor the pan to this mouse position + the current scroll so the
    // whole drag is computed deterministically from here.
    interactionHoldActive = true;
    panGestureActive = true;
    dragStartX = mouseX;
    panStartScrollMs = displayScrollMs;
    repaint();
}

void Oscilloscope::updateInspectionPan (float mouseX) noexcept
{
    if (! panGestureActive)
        return;

    const float pixelsMoved = mouseX - dragStartX;
    const int componentWidth = juce::jmax (1, getWidth());
    const int visible = calculateVisibleScopeSamples (historyLength, sampleRate, decimationFactor,
                                                      timeZoom, gridDivision, tempoAvailable, bpm);
    const float msPerPixel = (float) calculateVisibleWindowMs (visible, sampleRate, decimationFactor)
                           / (float) componentWidth;

    displayScrollMs = clampDisplayScrollMs (scopeDragToScrollMs (panStartScrollMs, pixelsMoved, msPerPixel));
    repaint();
}

void Oscilloscope::endInspectionHold() noexcept
{
    // Resume live movement. Deliberately does NOT clear `frozen`, so a manual
    // freeze set before the click stays active after releasing the mouse.
    interactionHoldActive = false;
    panGestureActive = false;
    repaint();
}

void Oscilloscope::mouseDown (const juce::MouseEvent& e)
{
    // Delay-drag is an explicit modifier gesture (Shift + drag) in Triggered
    // mode only. A plain click/hold/drag ALWAYS pauses the display and scrubs
    // the waveform instead, in every mode including Triggered, so inspecting
    // the scope can never silently move Delay.
    if (scopeWantsDelayDragGesture (viewMode, e.mods.isShiftDown()) && delayParameter != nullptr
        && ! e.mods.isRightButtonDown() && ! e.mods.isCommandDown())
    {
        dragStartX = e.position.x;
        dragStartDelayMs = delayParameter->convertFrom0to1 (delayParameter->getValue());
        delayGestureActive = true;
        delayParameter->beginChangeGesture();
    }
    else
    {
        beginInspectionHold (e.position.x);
    }
}

void Oscilloscope::mouseDrag (const juce::MouseEvent& e)
{
    if (delayGestureActive)
        setDelayFromDrag (e);
    else if (panGestureActive)
        updateInspectionPan (e.position.x);
}

void Oscilloscope::mouseUp (const juce::MouseEvent&)
{
    cancelActiveGestures();
}

void Oscilloscope::mouseExit (const juce::MouseEvent&)
{
    // The mouse leaving mid-drag must not leave delayGestureActive latched —
    // otherwise a later mouseDrag delivered without a fresh mouseDown (e.g. the
    // pointer re-entering while still held from outside) could keep nudging
    // Delay from what looks like ordinary movement.
    cancelActiveGestures();
}

void Oscilloscope::cancelActiveGestures() noexcept
{
    if (delayGestureActive && delayParameter != nullptr)
    {
        delayParameter->endChangeGesture();
        delayGestureActive = false;
    }

    if (interactionHoldActive || panGestureActive)
        endInspectionHold();
}

void Oscilloscope::mouseDoubleClick (const juce::MouseEvent& e)
{
    juce::ignoreUnused (e);

    // Universal scope convention: double-click returns to the default view.
    // The zoom glides back via the timer animation rather than snapping.
    displayScrollMs = 0.0f;
    targetTimeZoom = 1.0f;
    targetAmpZoom = 1.0f;

    if (onZoomChanged != nullptr)
        onZoomChanged();

    repaint();
}

void Oscilloscope::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    // Some Windows drivers translate Shift+scroll into a horizontal wheel
    // event (deltaX only), which would make Shift+wheel amplitude zoom go
    // dead — fall back to deltaX as the zoom delta in that case.
    const bool shiftWheelAsHorizontal = e.mods.isShiftDown() && wheel.deltaY == 0.0f;
    const float zoomDelta = wheel.deltaY != 0.0f ? wheel.deltaY
                          : (shiftWheelAsHorizontal ? -wheel.deltaX : 0.0f);

    if (std::abs (zoomDelta) > 0.0f)
    {
        // Adjust the TARGET zoom; the timer glides the live value toward it.
        // Remember where the cursor sat so a held/scrolled view zooms around
        // that point instead of the right edge.
        const float step = 1.0f + juce::jlimit (-0.5f, 0.5f, zoomDelta) * 0.6f;
        zoomAnchorFraction = juce::jlimit (0.0f, 1.0f,
                                           e.position.x / (float) juce::jmax (1, getWidth()));

        if (e.mods.isShiftDown())
            targetAmpZoom = juce::jlimit (1.0f, 8.0f, targetAmpZoom * step);
        else
            targetTimeZoom = juce::jlimit (1.0f, 16.0f, targetTimeZoom * step);

        if (onZoomChanged != nullptr)
            onZoomChanged();
    }

    if (std::abs (wheel.deltaX) > 0.0f && ! shiftWheelAsHorizontal)
    {
        if (viewMode == ScopeViewMode::Triggered)
            return;

        const int componentWidth = juce::jmax (1, getWidth());
        const int visible = calculateVisibleScopeSamples (historyLength, sampleRate, decimationFactor, timeZoom, gridDivision, tempoAvailable, bpm);
        const float msPerPixel = (float) calculateVisibleWindowMs (visible, sampleRate, decimationFactor)
                               / (float) componentWidth;

        displayScrollMs = clampDisplayScrollMs (displayScrollMs - wheel.deltaX * 100.0f * msPerPixel);
        repaint();
    }
}

void Oscilloscope::resized()
{
}
