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

Oscilloscope::Oscilloscope (ScopeFifo& fifoToRead, const HitCaptureBuffer& hitCaptureToRead)
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
        reserveTriggeredBuffers();
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

    if (viewMode == ScopeViewMode::Triggered || relockPending)
    {
        const bool allowTriggeredRefresh = ! isDisplayFrozen() || relockPending;

        if (allowTriggeredRefresh)
        {
            // The watchdog fallback only exists to show SOMETHING before the
            // first kick is detected. It must (a) require samples to actually
            // be flowing — a stopped transport is not "no trigger detected",
            // and rebuilding a static image at 60 Hz just burns CPU — and
            // (b) never engage once a kick reference is locked: falling back
            // used to DESTROY the frozen reference whenever no hit arrived
            // for 2 s (stop, breakdown), which is exactly the "kick doesn't
            // stay fixed" bug.
            if (refreshTriggeredSnapshot())
                freeRunTicks = 0;
            else if (viewMode == ScopeViewMode::Triggered && anyRead
                     && kickReferenceState == KickReferenceState::NoReference
                     && ++freeRunTicks >= freeRunWatchdogTicks)
                buildFreeRunTriggeredSnapshot();

            // The auto-gain smooths toward its target every tick; repaint when
            // it actually moves so the trace settles smoothly between hits
            // (the timer otherwise only repaints on a new snapshot).
            if (updateTriggeredAutoGain() && viewMode == ScopeViewMode::Triggered)
                repaint();
        }

        // Live triggered sweep: while samples flow and a kick reference is
        // locked, the bass overlay tracks the ring in real time, so keep
        // repainting like the scrolling modes do.
        if (viewMode == ScopeViewMode::Triggered && anyRead && ! isDisplayFrozen()
            && kickReferenceState == KickReferenceState::Locked
            && liveSamplesSinceTrigger != nullptr)
        {
            repaint();
        }
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
    const int n = juce::jmin ((int) triggeredBass.size(), (int) triggeredKick.size());
    const float midY = bounds.getCentreY();
    const float halfHeight = bounds.getHeight() * 0.46f;

    g.setColour (gridMajor.brighter (0.25f));
    g.drawHorizontalLine ((int) std::round (midY), bounds.getX(), bounds.getRight());
    g.setColour (gridMinor.brighter (0.15f));
    g.drawHorizontalLine ((int) std::round (midY - halfHeight * 0.5f), bounds.getX(), bounds.getRight());
    g.drawHorizontalLine ((int) std::round (midY + halfHeight * 0.5f), bounds.getX(), bounds.getRight());

    if (n <= 1)
    {
        g.setColour (labelColour);
        g.setFont (juce::Font (juce::FontOptions (12.0f)).boldened());
        g.drawText (triggeredScopeEmptyText (kickReferenceState),
                    bounds.withSizeKeepingCentre (220.0f, 18.0f).toNearestInt(),
                    juce::Justification::centred);
        return;
    }

    const int safePreRoll = juce::jlimit (0, n - 1, triggeredPreRollSamples);

    // Time zoom: select the visible slice of the captured window, keeping the
    // trigger line at a fixed fractional x. The ms axis uses the snapshot's own
    // sample rate (full-rate per-hit capture vs the decimated watchdog ring).
    const auto range = computeTriggeredVisibleRange (n, safePreRoll, timeZoom);
    const int first = range.first;
    const int visible = juce::jmax (2, range.visible);
    const int last = first + visible - 1;
    const double rate = triggeredSnapshotRate;

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

    // Faint decaying "ghost" strokes of the last few bass hits (oldest first,
    // so the most recent sits brightest just under the live pair). The kick is
    // frozen, so only past bass positions are worth persisting.
    auto drawGhostBass = [&] (const std::vector<float>& bass, float alpha)
    {
        const int available = (int) bass.size();
        const int ghostFirst = juce::jlimit (0, juce::jmax (0, available - 1), first);
        const int ghostVisible = juce::jmin (visible, available - ghostFirst);
        if (ghostVisible <= 1)
            return;

        juce::Path path;
        const int pointCount = calculateTriggeredRenderPointCount (ghostVisible, (int) std::ceil (bounds.getWidth()));
        path.preallocateSpace (pointCount * 3);

        for (int point = 0; point < pointCount; ++point)
        {
            const int k = triggeredRenderSampleIndex (point, pointCount, ghostVisible);
            const float x = bounds.getX() + (float) k * sampleXStep;
            const float y = midY - juce::jlimit (-1.0f, 1.0f, bass[(size_t) (ghostFirst + k)] * gain) * halfHeight;
            if (point == 0)
                path.startNewSubPath (x, y);
            else
                path.lineTo (x, y);
        }

        g.setColour (bassColour.withAlpha (alpha));
        g.strokePath (path, juce::PathStrokeType (1.0f, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
    };

    for (int gi = ghostCount - 1; gi >= 0; --gi)
        drawGhostBass (ghostBass[(size_t) gi], 0.09f * (float) (ghostCount - gi));

    auto drawPair = [&] (const std::vector<float>& bass,
                         const std::vector<float>& kick,
                         float alpha)
    {
        const int available = juce::jmin ((int) bass.size(), (int) kick.size());
        const int pairFirst = juce::jlimit (0, juce::jmax (0, available - 1), first);
        const int pairVisible = juce::jmin (visible, available - pairFirst);
        if (pairVisible <= 1)
            return;

        juce::Path bassPath, kickPath, bassFill, kickFill;
        const int pointCount = calculateTriggeredRenderPointCount (pairVisible, (int) std::ceil (bounds.getWidth()));
        bassPath.preallocateSpace (pointCount * 3);
        kickPath.preallocateSpace (pointCount * 3);
        bassFill.preallocateSpace (pointCount * 3);
        kickFill.preallocateSpace (pointCount * 3);

        for (int point = 0; point < pointCount; ++point)
        {
            const int k = triggeredRenderSampleIndex (point, pointCount, pairVisible);
            const int idx = pairFirst + k;
            const float x = bounds.getX() + (float) k * sampleXStep;
            const float bassY = midY - juce::jlimit (-1.0f, 1.0f, bass[(size_t) idx] * gain) * halfHeight;
            const float kickY = midY - juce::jlimit (-1.0f, 1.0f, kick[(size_t) idx] * gain) * halfHeight;

            if (point == 0)
            {
                bassPath.startNewSubPath (x, bassY);
                kickPath.startNewSubPath (x, kickY);
                bassFill.startNewSubPath (x, midY);
                bassFill.lineTo (x, bassY);
                kickFill.startNewSubPath (x, midY);
                kickFill.lineTo (x, kickY);
            }
            else
            {
                bassPath.lineTo (x, bassY);
                kickPath.lineTo (x, kickY);
                bassFill.lineTo (x, bassY);
                kickFill.lineTo (x, kickY);
            }
        }

        const float lastX = bounds.getX() + (float) (pairVisible - 1) * sampleXStep;
        bassFill.lineTo (lastX, midY);
        bassFill.closeSubPath();
        kickFill.lineTo (lastX, midY);
        kickFill.closeSubPath();

        // Vertical gradient fills — strongest at the waveform extremes, fading
        // to near-transparent at the midline — give the traces depth without
        // washing out the grid the way flat alpha fills do. Three stops keep
        // the lobes below the midline as visible as those above it.
        {
            juce::ColourGradient kickGrad (kickColour.withAlpha (alpha * 0.30f), 0.0f, bounds.getY(),
                                           kickColour.withAlpha (alpha * 0.30f), 0.0f, bounds.getBottom(),
                                           false);
            kickGrad.addColour (0.5, kickColour.withAlpha (alpha * 0.03f));
            g.setGradientFill (kickGrad);
            g.fillPath (kickFill);
        }

        g.setColour (kickColour.withAlpha (alpha));
        g.strokePath (kickPath, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));

        const float bassAlpha = juce::jmin (1.0f, alpha + 0.04f);
        {
            juce::ColourGradient bassGrad (bassColour.withAlpha (bassAlpha * 0.34f), 0.0f, bounds.getY(),
                                           bassColour.withAlpha (bassAlpha * 0.34f), 0.0f, bounds.getBottom(),
                                           false);
            bassGrad.addColour (0.5, bassColour.withAlpha (bassAlpha * 0.04f));
            g.setGradientFill (bassGrad);
            g.fillPath (bassFill);
        }

        // Soft glow under the primary bass stroke lifts it off the background.
        g.setColour (bassColour.withAlpha (bassAlpha * 0.16f));
        g.strokePath (bassPath, juce::PathStrokeType (4.5f, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
        g.setColour (bassColour.withAlpha (bassAlpha));
        g.strokePath (bassPath, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
    };

    drawPair (triggeredBass, triggeredKick, 0.95f);

    // Live triggered sweep (ReVision behaviour): the kick reference stays
    // frozen, and the CURRENT bass is drawn over it in real time, aligned to
    // the most recent detected kick via the processor's scope-stream trigger
    // markers. The sweep grows left-to-right as samples arrive after the hit
    // (samples past "now" simply don't exist yet), so every hit visibly
    // redraws the bass in place — per-hit checking without waiting for the
    // capture window to complete.
    if (liveSamplesSinceTrigger != nullptr
        && kickReferenceState == KickReferenceState::Locked)
    {
        const int sinceTrigger = liveSamplesSinceTrigger->load (std::memory_order_acquire);
        const double ringRate = sampleRate / (double) juce::jmax (1, decimationFactor);

        if (sinceTrigger >= 0 && sinceTrigger < historyLength - 2 && ringRate > 0.0)
        {
            const int triggerRingIndex = writeIndex - 1 - sinceTrigger;
            const int pointCount = calculateTriggeredRenderPointCount (visible, (int) std::ceil (bounds.getWidth()));

            juce::Path live;
            live.preallocateSpace (pointCount * 3);
            bool started = false;

            for (int point = 0; point < pointCount; ++point)
            {
                const int k = triggeredRenderSampleIndex (point, pointCount, visible);
                const double tRelSeconds = (double) ((first + k) - safePreRoll) / rate;
                const int ringOffset = (int) std::llround (tRelSeconds * ringRate);

                // Stop at "now" — the sweep head. Also never reach further back
                // than the ring actually holds.
                if (ringOffset > sinceTrigger)
                    break;
                if (sinceTrigger - ringOffset >= historyLength - 2)
                    continue;

                const int idx = wrapHistoryIndex (triggerRingIndex + ringOffset, historyLength);
                const float x = bounds.getX() + (float) k * sampleXStep;
                const float y = midY - juce::jlimit (-1.0f, 1.0f,
                                                     mainHistory[(size_t) idx] * gain) * halfHeight;

                if (! started)
                {
                    live.startNewSubPath (x, y);
                    started = true;
                }
                else
                {
                    live.lineTo (x, y);
                }
            }

            if (started)
            {
                g.setColour (bassColour.brighter (0.35f).withAlpha (0.6f));
                g.strokePath (live, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved,
                                                          juce::PathStrokeType::rounded));
            }
        }
    }

    // Phase agreement strip: an always-visible cancellation read-out along the
    // bottom of the plot — green where the (envelope-smoothed) kick and bass
    // low ends push the same way, red where they fight. This is the "can I see
    // the cancellation?" answer in the HERO view, not just the Phase Delta
    // mode. Smoothed envelopes (same tested classifier Phase Delta uses) keep
    // it stable instead of flickering on broadband zero crossings; runs of
    // equal colour merge into single rects so it costs a handful of fills.
    {
        const int stripSpan = juce::jmin (visible, n - first);
        if (stripSpan > 1)
        {
            // Sample the visible slice down to pixel-column resolution FIRST,
            // then envelope-smooth the columns: the triggered window can exceed
            // the historyLength-sized scratch buffers at high sample rates
            // (e.g. 170 ms @ 96 kHz = 16,320 samples), and the strip only needs
            // per-column values anyway — O(columns), not O(window).
            const int columns = juce::jlimit (2, historyLength, (int) std::ceil (bounds.getWidth()));
            for (int c = 0; c < columns; ++c)
            {
                const int k = (int) ((long long) c * (stripSpan - 1) / (columns - 1));
                columnMinScratch[(size_t) c] = triggeredBass[(size_t) (first + k)];
                columnMaxScratch[(size_t) c] = triggeredKick[(size_t) (first + k)];
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
                    const float x0 = bounds.getX() + bounds.getWidth() * (float) runStart / (float) columns;
                    const float x1 = bounds.getX() + bounds.getWidth() * (float) endColumn / (float) columns;
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
    // peaks plus a timing verdict in producer language ("bass late"), computed
    // over the visible slice of the triggered snapshot at its own sample rate.
    {
        const int markerSpan = juce::jmin (visible, n - first);
        const auto markers = findEnvelopePeakMarkers (triggeredBass.data() + first,
                                                      triggeredKick.data() + first,
                                                      markerSpan, rate);
        if (markers.valid && markerSpan > 1)
        {
            const float kickX = bounds.getX() + (float) markers.kickPeakIndex * sampleXStep;
            const float bassX = bounds.getX() + (float) markers.bassPeakIndex * sampleXStep;

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
                        juce::Rectangle<int> ((int) (bounds.getCentreX() - 95.0f),
                                              (int) (bounds.getBottom() - 36.0f),
                                              190, 14),
                        juce::Justification::centred);
        }
    }

    // Always-on status line: the user must never have to guess what the
    // triggered view is showing. Covers every reference state plus whether
    // the input stream is currently alive.
    {
        const bool receiving = ticksSinceFifoRead < idleAfterTicks;
        juce::String status;

        if (kickReferenceState == KickReferenceState::NoReference)
            status = "waiting for kick — showing live input";
        else if (kickReferenceState == KickReferenceState::RelockPending)
            status = "re-lock armed — waiting for next kick";
        else if (frozen)
            status = "kick locked · display frozen";
        else if (! receiving)
            status = "kick locked · input idle — showing last capture";
        else
            status = juce::String::fromUTF8 ("kick locked · bass live");

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

void Oscilloscope::reserveTriggeredBuffers()
{
    const int required = hitCapture.getWindowSamples();
    if (required <= 0 || required == reservedTriggeredSamples)
        return;

    auto reserveOne = [required] (std::vector<float>& buffer)
    {
        if ((int) buffer.capacity() < required)
            buffer.reserve ((size_t) required);
    };

    reserveOne (triggeredBass);
    reserveOne (triggeredKick);
    reserveOne (triggeredScratchBass);
    reserveOne (triggeredScratchKick);

    for (auto& buffer : ghostBass)
        reserveOne (buffer);
    for (auto& buffer : ghostKick)
        reserveOne (buffer);

    reservedTriggeredSamples = required;

    kickReferenceState = KickReferenceState::NoReference;
}

void Oscilloscope::relockKickReference() noexcept
{
    kickReferenceState = kickReferenceStateAfterRelock();
    latestTriggeredSequence = hitCapture.getSequence();

    triggeredBass.clear();
    triggeredKick.clear();
    for (auto& buffer : ghostBass)
        buffer.clear();
    for (auto& buffer : ghostKick)
        buffer.clear();

    repaint();
}

bool Oscilloscope::refreshTriggeredSnapshot()
{
    const int sequence = hitCapture.getSequence();
    if (sequence == latestTriggeredSequence)
        return false;

    reserveTriggeredBuffers();

    int copiedSequence = 0;
    const int samples = hitCapture.snapshotLatest (triggeredScratchBass, triggeredScratchKick, &copiedSequence);
    if (samples <= 0)
        return false;

    if (copiedSequence == latestTriggeredSequence)
        return false;

    const bool replaceKickReference = kickReferenceShouldReplaceOnCapture (kickReferenceState);

    if (! triggeredBass.empty())
    {
        for (int i = ghostCount - 1; i > 0; --i)
        {
            ghostBass[(size_t) i].swap (ghostBass[(size_t) (i - 1)]);
            ghostKick[(size_t) i].swap (ghostKick[(size_t) (i - 1)]);
        }

        ghostBass[0].swap (triggeredBass);

        if (replaceKickReference)
            ghostKick[0].swap (triggeredKick);
    }

    triggeredBass.swap (triggeredScratchBass);

    if (replaceKickReference)
    {
        triggeredKick.swap (triggeredScratchKick);
        kickReferenceState = kickReferenceStateAfterCapture (kickReferenceState);
    }

    triggeredPreRollSamples = hitCapture.getPreRollSamples();
    latestTriggeredSequence = copiedSequence;
    // The per-hit window comes from the full-rate HitCaptureBuffer.
    triggeredSnapshotRate = sampleRate;
    repaint();
    return true;
}

void Oscilloscope::buildFreeRunTriggeredSnapshot()
{
    reserveTriggeredBuffers();

    const int desiredSamples = juce::jlimit (2, historyLength,
                                            msToSamples (170.0f, sampleRate) / juce::jmax (1, decimationFactor));
    triggeredScratchBass.resize ((size_t) desiredSamples);
    triggeredScratchKick.resize ((size_t) desiredSamples);

    const int start = writeIndex - desiredSamples;
    for (int i = 0; i < desiredSamples; ++i)
    {
        const int idx = wrapHistoryIndex (start + i, historyLength);
        triggeredScratchBass[(size_t) i] = mainHistory[(size_t) idx];
        triggeredScratchKick[(size_t) i] = sidechainHistory[(size_t) idx];
    }

    triggeredBass.swap (triggeredScratchBass);
    triggeredKick.swap (triggeredScratchKick);
    triggeredPreRollSamples = juce::jlimit (0, desiredSamples - 1,
                                            msToSamples (20.0f, sampleRate) / juce::jmax (1, decimationFactor));
    // The fallback fills from the DECIMATED scope ring, so the ms axis must use
    // the decimated rate or the labels/grid come out ~decimation× too dense.
    triggeredSnapshotRate = sampleRate / (double) juce::jmax (1, decimationFactor);
    freeRunTicks = freeRunWatchdogTicks;

    // Note: the caller only enters this fallback while no kick reference is
    // locked (kickReferenceState == NoReference), so the next real hit still
    // captures a fresh reference — and a reference the user already has is
    // never destroyed by a gap in the material.
    repaint();
}

bool Oscilloscope::updateTriggeredAutoGain() noexcept
{
    float peak = 0.0f;
    for (size_t i = 0; i < triggeredBass.size() && i < triggeredKick.size(); ++i)
    {
        peak = juce::jmax (peak, std::abs (triggeredBass[i]));
        peak = juce::jmax (peak, std::abs (triggeredKick[i]));
    }

    const float targetGain = peak > 1.0e-4f ? juce::jlimit (1.0f, 40.0f, 0.88f / peak) : 1.0f;
    const float step = 0.18f * (targetGain - displayGain);
    displayGain += step;

    // Report whether the gain moved enough to be worth a repaint, so the timer
    // can settle the trace between hits without repainting on every idle tick.
    return std::abs (step) > 1.0e-3f;
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
