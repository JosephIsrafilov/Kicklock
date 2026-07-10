#include "Oscilloscope.h"
#include "UiFormatters.h"
#include "../dsp/FftPlanCache.h"

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

    void drawYAxisGrid (juce::Graphics& g, juce::Rectangle<float> bounds, float centreY, float halfHeight, float gain,
                        juce::Colour majorColor, juce::Colour minorColor)
    {
        const float laneTop = centreY - halfHeight;
        const float laneBottom = centreY + halfHeight;
        g.setColour (majorColor.brighter (0.25f));
        g.drawHorizontalLine ((int) std::round (centreY), bounds.getX(), bounds.getRight());

        const float dbLevels[] = { 24.0f, 18.0f, 12.0f, 6.0f, 3.0f, 0.0f, -3.0f, -6.0f, -12.0f, -18.0f };

        for (float db : dbLevels)
        {
            const float linear = std::pow (10.0f, db / 20.0f);
            const float scaled = linear * gain;
            
            if (scaled > 1.08f) continue;

            const float yTop = centreY - scaled * halfHeight;
            const float yBot = centreY + scaled * halfHeight;
            
            juce::Colour lineCol = minorColor.brighter (0.15f);

            if (db > 0.0f)
            {
                lineCol = destructive.withAlpha(0.2f);
            }
            else if (db == 0.0f)
            {
                lineCol = minorColor.brighter (0.4f);
            }

            if (scaled > 0.05f)
            {
                if (yTop >= laneTop)
                {
                    g.setColour (lineCol);
                    g.drawHorizontalLine ((int) std::round (yTop), bounds.getX(), bounds.getRight());
                }

                if (yBot <= laneBottom)
                {
                    g.setColour (lineCol);
                    g.drawHorizontalLine ((int) std::round (yBot), bounds.getX(), bounds.getRight());
                }
            }
        }
    }

    void drawYAxisLabels (juce::Graphics& g, juce::Rectangle<float> bounds, float centreY, float halfHeight, float gain,
                          juce::Colour textColor)
    {
        const float laneTop = centreY - halfHeight;
        const float laneBottom = centreY + halfHeight;
        const float dbLevels[] = { 24.0f, 18.0f, 12.0f, 6.0f, 3.0f, 0.0f, -3.0f, -6.0f, -12.0f, -18.0f };
        g.setFont (juce::Font (juce::FontOptions (10.0f)).boldened());

        for (float db : dbLevels)
        {
            const float linear = std::pow (10.0f, db / 20.0f);
            const float scaled = linear * gain;
            
            if (scaled > 1.08f) continue;

            const float yTop = centreY - scaled * halfHeight;
            const float yBot = centreY + scaled * halfHeight;

            juce::String label = (db > 0.0f ? "+" : "") + (db == 0.0f ? "0" : juce::String (db, 0)) + " dB";
            
            juce::Colour textCol = textColor.withAlpha(0.85f);

            if (db > 0.0f)
                textCol = destructive.withAlpha(0.95f);

            auto drawLabel = [&] (float yPos) {
                juce::Rectangle<int> textRect ((int) bounds.getRight() - 34, 
                                               (int) std::round (yPos) - 6, 
                                               30, 12);
                
                // Optional: Draw a faint dark pill behind the text for maximum readability over waveforms
                g.setColour (juce::Colours::black.withAlpha(0.5f));
                g.fillRoundedRectangle (textRect.toFloat().expanded(2.0f, 1.0f), 3.0f);

                g.setColour (textCol);
                g.drawText (label, textRect, juce::Justification::centredRight);
            };

            if (scaled > 0.05f)
            {
                if (yTop >= laneTop)
                    drawLabel (yTop);

                if (db != 0.0f && yBot <= laneBottom)
                    drawLabel (yBot);
            }
        }
    }
}

Oscilloscope::Oscilloscope (ScopeFifo& fifoToRead, HitCaptureBuffer& hitCaptureToRead)
    : fifo (fifoToRead),
      hitCapture (hitCaptureToRead),
      mainHistory ((size_t) historyLength, 0.0f),
      sidechainHistory ((size_t) historyLength, 0.0f),
      snapshotMainHistory ((size_t) historyLength, 0.0f),
      snapshotSidechainHistory ((size_t) historyLength, 0.0f)
{
    vblankAttachment = std::make_unique<juce::VBlankAttachment> (this, [this] { vblankCallback(); });
    startTimerHz (15);
}

Oscilloscope::~Oscilloscope()
{
    stopTimer();
}

void Oscilloscope::updateSnapshotOwnership()
{
    const bool shouldBeActive = isDisplayFrozen();
    if (snapshotActive != shouldBeActive)
    {
        snapshotActive = shouldBeActive;
        if (snapshotActive)
        {
            snapshotMainHistory = mainHistory;
            snapshotSidechainHistory = sidechainHistory;
            snapshotWriteIndex = writeIndex;
        }
        visibleBuffersDirty = true;
    }
}

void Oscilloscope::evaluateAutoRelockEdge()
{
    const bool sidechainAvailable = fifo.getSideChannels() > 0;
    const bool triggeredMode = viewMode == ScopeViewMode::Triggered;

    const bool sidechainDisconnected = wasSidechainAvailable && ! sidechainAvailable;

    const bool enteredTriggeredWithSidechain = shouldAutoRelockKickReference (
        kickReferenceState, wasSidechainAvailable, sidechainAvailable,
        wasTriggeredMode, triggeredMode);

    // Disconnects clear stale Triggered state immediately.  A reconnect does
    // not re-arm a second fence if that transition is already pending; the
    // pending reset marker fences the first post-reconnect hit.
    if ((enteredTriggeredWithSidechain && ! relockFencePending)
        || (sidechainDisconnected && triggeredMode))
        beginTriggeredRelockTransition();

    wasSidechainAvailable = sidechainAvailable;
    wasTriggeredMode = triggeredMode;
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
    if (isShowing())
        return;

    std::array<float, scratchSize> mainScratch {};
    std::array<float, scratchSize> sidechainScratch {};
    while (fifo.readAvailable (mainScratch.data(), sidechainScratch.data(), scratchSize) > 0) {}

    drainSweepStream (true);
}

void Oscilloscope::vblankCallback()
{
    if (! isShowing())
        return;

    evaluateAutoRelockEdge();

    std::array<float, scratchSize> mainScratch {};
    std::array<float, scratchSize> sidechainScratch {};

    bool anyRead = false;

    while (true)
    {
        const int count = fifo.readAvailable (mainScratch.data(), sidechainScratch.data(), scratchSize);
        if (count == 0)
            break;

        anyRead = true;

        for (int i = 0; i < count; ++i)
        {
            mainHistory[(size_t) writeIndex]      = mainScratch[(size_t) i];
            sidechainHistory[(size_t) writeIndex] = sidechainScratch[(size_t) i];
            writeIndex = (writeIndex + 1) % historyLength;
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

    if (viewMode == ScopeViewMode::Triggered)
    {
        // The auto-gain glides toward its per-hit target every tick; repaint
        // while it actually moves so the trace settles smoothly between hits.
        if (glideTriggeredAutoGain())
            triggeredDirty = true;

        if (triggeredDirty)
            repaint();
    }

    if (viewMode != ScopeViewMode::Triggered)
    {
        bool viewChanged = false;

        const int visible = calculateVisibleScopeSamples (historyLength, sampleRate, decimationFactor,
                                                          timeZoom, gridDivision, tempoAvailable, bpm);

        const bool isGliding = std::abs (targetTimeZoom - timeZoom) > 1.0e-3f || std::abs (targetAmpZoom - ampZoom) > 1.0e-3f;
        const bool scrollChanged = displayScrollMs != lastDisplayScrollMs;

        if ((! isDisplayFrozen() && anyRead) || panGestureActive || isGliding || scrollChanged || visibleBuffersDirty)
        {
            rebuildVisibleBuffers (visible, scopeModeSupportsVisualOffset (viewMode),
                                   viewMode == ScopeViewMode::PhaseDelta);
            visibleBuffersDirty = false;

            if (! isDisplayFrozen())
            {
                // Non-triggered modes behave like a normal oscilloscope: no auto-gain.
                targetDisplayGain = 1.0f;

                if (anyRead)
                {
                    viewChanged = true;
                }
            }
            else
            {
                viewChanged = true;
            }
        }

        lastDisplayScrollMs = displayScrollMs;

        const float oldGain = displayGain;
        displayGain = scopeGlideAutoGain (displayGain, targetDisplayGain > 0.0f ? targetDisplayGain : 1.0f);
        if (std::abs (oldGain - displayGain) > 1.0e-4f)
            viewChanged = true;

        if (viewChanged)
            repaint();
    }
    if (std::abs (targetTimeZoom - timeZoom) > 1.0e-3f || std::abs (targetAmpZoom - ampZoom) > 1.0e-3f)
    {
        const int oldVisible = calculateVisibleScopeSamples (historyLength, sampleRate, decimationFactor,
                                                             timeZoom, gridDivision, tempoAvailable, bpm);
        const int triggeredSamples = sweepWindowSamples > 1 ? sweepWindowSamples : hitCapture.getWindowSamples();
        const int triggeredIndex = juce::jlimit (0, juce::jmax (0, triggeredSamples - 1), sweepTriggerSample);
        const auto oldTriggeredRange = computeTriggeredVisibleRange (triggeredSamples, triggeredIndex, timeZoom);

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
        else
        {
            const auto range = computeTriggeredVisibleRange (triggeredSamples, triggeredIndex, timeZoom);
            if (isDisplayFrozen() || displayScrollMs > 0.01f)
            {
                const float oldMs = samplesToMs (oldTriggeredRange.visible, sampleRate);
                const float newMs = samplesToMs (range.visible, sampleRate);
                displayScrollMs = scopeAnchoredZoomScrollMs (displayScrollMs, oldMs, newMs, zoomAnchorFraction);
            }

            displayScrollMs = clampTriggeredPanScrollMs (displayScrollMs, triggeredSamples,
                                                          range.visible, range.first, sampleRate);
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
        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.setFont (juce::Font (juce::FontOptions (12.0f)).boldened());
        g.drawText (scopeModeCaption (viewMode),
                    plotBounds.removeFromTop (16.0f).toNearestInt(),
                    juce::Justification::centredLeft);

        {
            juce::Graphics::ScopedSaveState state (g);
            g.reduceClipRegion (plotBounds.toNearestInt());
            drawTriggeredMode (g, plotBounds, displayGain * ampZoom);
            drawHoverCrosshair (g, plotBounds, displayGain * ampZoom);
        }

        drawYAxisLabels (g, plotBounds, plotBounds.getCentreY(), plotBounds.getHeight() * 0.46f, displayGain * ampZoom, labelColour);

        drawHoldIndicator (g, panelBounds);
        return;
    }

    // Buffers and auto-gain are now managed exclusively by vblankCallback.
    const float gain = displayGain * ampZoom;

    drawGrid (g, plotBounds, plotBounds.getCentreY(),
              viewMode == ScopeViewMode::Separate,
              visible, gain);

    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (plotBounds.toNearestInt());

        switch (viewMode)
        {
            case ScopeViewMode::Triggered:  break;
            case ScopeViewMode::FreeRun:    drawFreeRunMode (g, plotBounds, visible, gain, plotBounds.getCentreY()); break;
            case ScopeViewMode::PhaseDelta: drawPhaseDeltaMode (g, plotBounds, visible, gain, plotBounds.getCentreY()); break;
            case ScopeViewMode::Separate:   drawSeparateMode (g, plotBounds, visible, gain); break;
        }

        drawHoverCrosshair (g, plotBounds, gain);
    }

    if (viewMode == ScopeViewMode::Separate)
    {
        const auto geom = calculateSeparateModeGeometry (plotBounds.getHeight());
        drawYAxisLabels (g, plotBounds, plotBounds.getY() + geom.bassCenterY,
                         geom.bassHeight * 0.5f, gain, labelColour);
        drawYAxisLabels (g, plotBounds, plotBounds.getY() + geom.kickCenterY,
                         geom.kickHeight * 0.5f, gain, labelColour);
    }
    else
    {
        drawYAxisLabels (g, plotBounds, plotBounds.getCentreY(), plotBounds.getHeight() * 0.46f, gain, labelColour);
    }

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
void Oscilloscope::drawHoverCrosshair (juce::Graphics& g, juce::Rectangle<float> bounds, float gain)
{
    if (! isMouseOverScope || lastMousePos.y < bounds.getY() || lastMousePos.y > bounds.getBottom() ||
        lastMousePos.x < bounds.getX() || lastMousePos.x > bounds.getRight())
        return;

    juce::String text;
    juce::Colour crosshairCol = juce::Colours::lightgreen.withAlpha (0.75f);
    

        const bool separateMode = viewMode == ScopeViewMode::Separate;
        const auto geom = calculateSeparateModeGeometry (bounds.getHeight());
        const int lane = separateMode ? getSeparateLaneForY (lastMousePos.y - bounds.getY(), geom) : 0;
        const float midY = separateMode
                             ? bounds.getY() + (lane == 0 ? geom.bassCenterY : geom.kickCenterY)
                             : bounds.getCentreY();
        const float halfHeight = separateMode
                                   ? 0.5f * (lane == 0 ? geom.bassHeight : geom.kickHeight)
                                   : bounds.getHeight() * 0.46f;
        
        const float laneTop = separateMode ? midY - halfHeight : bounds.getY();
        const float laneBottom = separateMode ? midY + halfHeight : bounds.getBottom();
        const float crosshairY = juce::jlimit (laneTop, laneBottom, lastMousePos.y);
        const float amplitude = std::abs (crosshairY - midY) / (halfHeight * gain);
        float db = juce::Decibels::gainToDecibels (amplitude, -144.0f);
        
        text = juce::String (formatScopeHoverDbBadge (viewMode, lane, db));
            
        g.setColour (crosshairCol);
        g.drawHorizontalLine ((int) std::round (crosshairY), bounds.getX(), bounds.getRight());

    juce::Font font (juce::FontOptions(10.0f).withStyle("bold"));
    g.setFont (font);
    
    const float textWidth = juce::GlyphArrangement::getStringWidth (font, text) + 12.0f;
    const float textHeight = 16.0f;
    
    juce::Rectangle<float> badge (lastMousePos.x + 8.0f, 
                                  lastMousePos.y - textHeight - 4.0f, 
                                  textWidth, textHeight);
                                  
    if (badge.getRight() > bounds.getRight())
        badge.setX (lastMousePos.x - textWidth - 8.0f);
                                  
    if (badge.getY() < bounds.getY())
        badge.setY (lastMousePos.y + 4.0f);
        
    g.setColour (crosshairCol.withAlpha(0.2f));
    g.fillRoundedRectangle (badge, 4.0f);
    
    g.setColour (crosshairCol);
    g.drawRoundedRectangle (badge, 4.0f, 1.0f);
    
    g.setColour (juce::Colours::lightgreen.brighter());
    g.drawText (text, badge, juce::Justification::centred, false);
}

void Oscilloscope::drawTriggeredMode (juce::Graphics& g,
                                      juce::Rectangle<float> bounds,
                                      float gain)
{
    const float midY = bounds.getCentreY();
    const float halfHeight = bounds.getHeight() * 0.46f;

    drawYAxisGrid (g, bounds, midY, halfHeight, gain, gridMajor, gridMinor);

    // Triggered mode has one source of truth: the full-rate HitCaptureBuffer
    // sweep assembled below. In particular, never substitute live history
    // for Bass while Kick is coming from a captured frame.
    bool anyGhost = false;
    for (int fill : ghostFill)
        anyGhost = anyGhost || fill > 1;

    const bool sweepHasContent = sweepFill > 1 || anyGhost || kickReferenceValid;
    const int n = sweepWindowSamples;

    const auto triggeredSource = triggeredSweepSourceFor (sweepHasContent);
    if (n <= 1 || triggeredSource == TriggeredSweepSource::None)
    {
        g.setColour (labelColour);
        g.setFont (juce::Font (juce::FontOptions (12.0f)).boldened());
        g.drawText (fifo.getSideChannels() > 0 ? triggeredScopeEmptyText (kickReferenceState)
                                               : "WAITING FOR SIDECHAIN",
                    bounds.withSizeKeepingCentre (220.0f, 18.0f).toNearestInt(),
                    juce::Justification::centred);
        return;
    }

    const float* bassData = sweepBass.data();
    const int bassFillCount = sweepFill;
    const auto kickSource = selectTriggeredKickSource (kickReferenceValid,
                                                       kickReference.data(), kickReferenceHitId,
                                                       sweepKick.data(), sweepFill,
                                                       sweepWindowSamples, sweepHitId);
    const float* kickData = kickSource.data;
    const int kickFillCount = kickSource.fillCount;
    const int preRoll = sweepTriggerSample;
    const double rate = sampleRate;

    const int triggerSample = juce::jlimit (0, n - 1, preRoll);
    const bool referenceCompatible = kickReferenceValid
        && triggeredReferenceSharesFrameGeometry (kickReferenceTriggerSample,
                                                  triggerSample,
                                                  n);

    // Time zoom: select the visible slice of the capture window, keeping the
    // visual 0 ms line at a fixed fractional x. The ms axis uses the source's own
    // sample rate of the full-rate capture.
    const auto range = computeTriggeredVisibleRange (n, triggerSample, timeZoom);
    const int first = computeTriggeredPannedFirst (n, range.visible, range.first, displayScrollMs, rate);
    const int visible = range.visible;
    if (visible <= 1)
        return;

    const int last = first + visible - 1;

    const float sampleXStep = bounds.getWidth() / (float) (visible - 1);
    const float triggerX = bounds.getX() + (float) (triggerSample - first) * sampleXStep;

    const float preMs = samplesToMs (triggerSample - first, rate);
    const float postMs = samplesToMs (last - triggerSample, rate);

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
    {
        GhostsCacheKey currentGhostsKey;
        currentGhostsKey.first = first;
        currentGhostsKey.visible = visible;
        currentGhostsKey.triggerSample = triggerSample;
        currentGhostsKey.gain = gain;
        currentGhostsKey.timeZoom = timeZoom;
        currentGhostsKey.boundsW = (int) bounds.getWidth();
        currentGhostsKey.boundsH = (int) bounds.getHeight();
        currentGhostsKey.newestGhostId = ghostsVersion;

        if (ghostsKey != currentGhostsKey || ghostsCache.isNull())
        {
            ghostsKey = currentGhostsKey;
            ghostsCache = juce::Image (juce::Image::ARGB, 
                                       juce::jmax(1, currentGhostsKey.boundsW),
                                       juce::jmax(1, currentGhostsKey.boundsH), true);
            juce::Graphics gCache (ghostsCache);
            gCache.setOrigin ((int)-bounds.getX(), (int)-bounds.getY());

            for (int gi = ghostCount - 1; gi >= 0; --gi)
            {
                const float* ghostData = ghostBass[(size_t) gi].data();
                
                drawTriggeredTrace (gCache, bounds, ghostData, ghostFill[(size_t) gi],
                                    first, visible, sampleXStep, midY, halfHeight, gain,
                                    bassColour.withAlpha (scopeSweepGhostAlpha (gi, ghostCount)),
                                    1.0f, 0.0f, 0.0f);
            }
        }
        g.drawImageAt (ghostsCache, (int) bounds.getX(), (int) bounds.getY());
    }

    // Kick and Bass are deliberately read from the same captured sweep. The
    // locked reference state controls capture policy, not a second render
    // source; using kickReference here would pair an older hit with the live
    // Bass sweep and reintroduce the synchronization bug.
    if (kickReferenceValid)
    {
        KickRefCacheKey currentKickKey;
        currentKickKey.fill = kickFillCount;
        currentKickKey.first = first;
        currentKickKey.visible = visible;
        currentKickKey.triggerSample = triggerSample;
        currentKickKey.gain = gain;
        currentKickKey.timeZoom = timeZoom;
        currentKickKey.boundsW = (int) bounds.getWidth();
        currentKickKey.boundsH = (int) bounds.getHeight();
        currentKickKey.hitId = kickSource.hitId;

        if (kickRefKey != currentKickKey || kickRefCache.isNull())
        {
            kickRefKey = currentKickKey;
            kickRefCache = juce::Image (juce::Image::ARGB, 
                                        juce::jmax(1, currentKickKey.boundsW),
                                        juce::jmax(1, currentKickKey.boundsH), true);
            juce::Graphics gCache (kickRefCache);
            gCache.setOrigin ((int)-bounds.getX(), (int)-bounds.getY());

            drawTriggeredTrace (gCache, bounds, kickData, kickFillCount, first, visible, sampleXStep,
                                midY, halfHeight, gain,
                                kickColour.withAlpha (0.95f),
                                1.8f, 0.30f, 0.0f);
        }
        g.drawImageAt (kickRefCache, (int) bounds.getX(), (int) bounds.getY());
    }
    else
    {
        drawTriggeredTrace (g, bounds, kickData, kickFillCount, first, visible, sampleXStep,
                            midY, halfHeight, gain,
                            kickColour.withAlpha (kickReferenceValid ? 0.95f : 0.80f),
                            1.8f, 0.30f, 0.0f);
    }

    // Bass lane: the live sweep, redrawn left-to-right on every hit
    // (ReVision behaviour). Samples past the sweep head simply don't exist
    // yet, so the trace grows in real time and never jumps.
    const float* finalBassData = bassData;
    drawTriggeredTrace (g, bounds, finalBassData, bassFillCount, first, visible, sampleXStep,
                        midY, halfHeight, gain,
                        bassColour.withAlpha (0.98f), 2.2f, 0.34f, 0.16f);

    // Sweep head: while the current hit's window is still filling, a bright
    // leading edge shows exactly where the live redraw has got to.
    if (sweepFill > 1 && sweepFill < n
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
    if (referenceCompatible
        && sweepMarkers.valid
        && triggeredMarkersBelongToFrame (sweepMarkersHitId, sweepHitId,
                                          sweepFill, sweepWindowSamples))
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

        if (! kickReferenceValid && kickReferenceState == KickReferenceState::NoReference)
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
                                       float fillAlpha, float glowAlpha)
{
    if (data == nullptr || colour.isTransparent())
        return;

    const int drawCount = juce::jmin (visible, fill - first);
    if (drawCount <= 1)
        return;

    const int pixelSpan = juce::jmax (2, (int) std::ceil ((float) drawCount * sampleXStep));
    juce::Path stroke;

    if (drawCount > pixelSpan * 3 && pixelSpan < historyLength)
    {
        const int numColumns = pixelSpan;
        buildMinMaxColumns (data + first, 0, drawCount, numColumns,
                            const_cast<float*>(columnMinScratch.data()), 
                            const_cast<float*>(columnMaxScratch.data()));

        juce::Path envelope;
        stroke.preallocateSpace (numColumns * 3 + 8);
        envelope.preallocateSpace (numColumns * 6 + 8);
        const float columnW = (float) drawCount * sampleXStep / (float) numColumns;

        for (int c = 0; c < numColumns; ++c)
        {
            const float x = bounds.getX() + ((float) c + 0.5f) * columnW;
            float yTop = midY - juce::jlimit (-1.0f, 1.0f, columnMaxScratch[(size_t) c] * gain) * halfHeight;
            float yBot = midY - juce::jlimit (-1.0f, 1.0f, columnMinScratch[(size_t) c] * gain) * halfHeight;

            if (yBot - yTop < 1.2f)
            {
                const float mid = 0.5f * (yTop + yBot);
                yTop = mid - 0.6f;
                yBot = mid + 0.6f;
            }

            const_cast<float*>(columnMinScratch.data())[(size_t) c] = yBot;
            const float yMid = 0.5f * (yTop + yBot);

            if (c == 0)
            {
                envelope.startNewSubPath (x, yTop);
                stroke.startNewSubPath (x, yMid);
            }
            else
            {
                envelope.lineTo (x, yTop);
                stroke.lineTo (x, yMid);
            }
        }

        for (int c = numColumns - 1; c >= 0; --c)
        {
            const float x = bounds.getX() + ((float) c + 0.5f) * columnW;
            envelope.lineTo (x, columnMinScratch[(size_t) c]);
        }
        envelope.closeSubPath();

        if (fillAlpha > 0.0f)
        {
            g.setColour (colour.withAlpha (fillAlpha));
            g.fillPath (envelope);
        }
    }
    else
    {
        const int pointCount = calculateTriggeredRenderPointCount (drawCount, pixelSpan);
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
    }

    if (glowAlpha > 0.0f)
    {
        g.setColour (colour.withMultipliedAlpha (glowAlpha));
        g.strokePath (stroke, juce::PathStrokeType (strokeWidth + 2.6f, juce::PathStrokeType::mitered,
                                                    juce::PathStrokeType::rounded));
    }

    g.setColour (colour);
    g.strokePath (stroke, juce::PathStrokeType (strokeWidth, juce::PathStrokeType::mitered,
                                                juce::PathStrokeType::rounded));
}

void Oscilloscope::drawGrid (juce::Graphics& g,
                             juce::Rectangle<float> bounds,
                             float midY,
                             bool separateMode,
                             int visible,
                             float gain)
{
    const float visibleWindowMs = (float) calculateVisibleWindowMs (visible, sampleRate, decimationFactor);

    GridCacheKey currentKey;
    currentKey.visibleWindowMs = visibleWindowMs;
    currentKey.scrollMs = displayScrollMs;
    currentKey.bpm = (float) bpm;
    currentKey.boundsW = (int) bounds.getWidth();
    currentKey.boundsH = (int) bounds.getHeight();
    currentKey.tempoAvailable = tempoAvailable;
    currentKey.separateMode = separateMode;

    currentKey.division = gridDivision;

    if (gridKey != currentKey || gridCache.isNull())
    {
        gridKey = currentKey;
        gridCache = juce::Image (juce::Image::ARGB, 
                                 juce::jmax (1, currentKey.boundsW), 
                                 juce::jmax (1, currentKey.boundsH), 
                                 true);
        juce::Graphics gCache (gridCache);
        gCache.setOrigin ((int)-bounds.getX(), (int)-bounds.getY());

        float majorStepMs = 0.0f;
        int minorDivisions = 2;

        if (tempoAvailable && gridDivision != GridDivision::Milliseconds)
        {
            if (gridDivision == GridDivision::Bar || gridDivision == GridDivision::Whole)
            {
                // Draw grid lines on every beat for 1 Bar views
                majorStepMs = (float) bpmToQuarterMs (bpm);
                minorDivisions = 0;
            }
            else if (gridDivision == GridDivision::FourBars)
            {
                // Draw grid lines on every bar for multi-bar views
                majorStepMs = (float) bpmToQuarterMs (bpm) * 4.0f;
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
                const float scroll = displayScrollMs;

                for (int m = 0; ; ++m)
                {
                    const float screenAge = (float) m * majorStepMs;
                    const float x = bounds.getRight() - bounds.getWidth() * (screenAge / visibleWindowMs);
                    if (x < bounds.getX() - 0.5f)
                        break;

                    gCache.setColour (gridMajor);
                    gCache.drawVerticalLine ((int) std::round (x), bounds.getY(), bounds.getBottom());

                    for (int minor = 1; minor <= minorDivisions; ++minor)
                    {
                        const float minorAge = screenAge + majorStepMs * (float) minor / (float) (minorDivisions + 1);
                        const float minorX = bounds.getRight() - bounds.getWidth() * (minorAge / visibleWindowMs);
                        if (minorX < bounds.getX() - 0.5f || minorX > bounds.getRight() + 0.5f)
                            continue;

                        gCache.setColour (gridMinor);
                        gCache.drawVerticalLine ((int) std::round (minorX), bounds.getY(), bounds.getBottom());
                    }

                    gCache.setColour (labelColour);
                    gCache.setFont (juce::Font (juce::FontOptions (10.0f)));
                    const int labelRight = (int) std::round (x - 3.0f);
                    if (labelRight - 56 >= (int) bounds.getX())
                    {
                        const float absoluteAge = screenAge + scroll;
                        gCache.drawText (formatTimeLabel (-absoluteAge),
                                         juce::Rectangle<int> (labelRight - 56,
                                                               (int) std::round (bounds.getBottom() - 16.0f),
                                                               56, 12),
                                         juce::Justification::centredRight);
                    }
                }
            }

            auto drawHorizontalMarkers = [&] (float centreY, float halfHeight)
            {
                drawYAxisGrid (gCache, bounds, centreY, halfHeight, gain, gridMajor, gridMinor);
            };

            if (separateMode)
            {
                const auto geom = calculateSeparateModeGeometry (bounds.getHeight());
                drawHorizontalMarkers (bounds.getY() + geom.bassCenterY, geom.bassHeight * 0.5f);
                drawHorizontalMarkers (bounds.getY() + geom.kickCenterY, geom.kickHeight * 0.5f);

                gCache.setColour (gridMajor.withAlpha (0.65f));
                gCache.drawHorizontalLine ((int) std::round (bounds.getY() + geom.dividerY), bounds.getX(), bounds.getRight());
            }
            else
            {
                drawHorizontalMarkers (midY, bounds.getHeight() * 0.46f);
            }
        }

    g.drawImageAt (gridCache, (int) bounds.getX(), (int) bounds.getY());
}

void Oscilloscope::drawPhaseDeltaMode (juce::Graphics& g,
                                       juce::Rectangle<float> bounds,
                                       int visible,
                                       float gain,
                                       float midY)
{
    if (visible < 2)
        return;

    const float halfHeight = bounds.getHeight() * 0.46f;

    // Number of points to draw. If zoomed in closely, don't draw more points than samples.
    const int columns = juce::jmax (1, (int) std::ceil (bounds.getWidth()));
    const int numPoints = juce::jmin (visible, columns);
    const float xStep = bounds.getWidth() / (float) numPoints;

    juce::Path whiteTrace;
    
    // Create premium gradients for the fill from the zero axis to the peaks
    juce::ColourGradient constrGrad (constructive.withAlpha (0.0f), bounds.getCentreX(), midY,
                                     constructive.withAlpha (0.8f), bounds.getCentreX(), bounds.getY(), false);
    juce::ColourGradient destrGrad (destructive.withAlpha (0.0f), bounds.getCentreX(), midY,
                                    destructive.withAlpha (0.8f), bounds.getCentreX(), bounds.getY(), false);

    for (int p = 0; p < numPoints; ++p)
    {
        const int sampleStart = (int) ((long long) p * visible / numPoints);
        const int sampleEnd   = juce::jmin (visible, (int) ((long long) (p + 1) * visible / numPoints));
        const int span        = juce::jmax (1, sampleEnd - sampleStart);
        
        // Sub-sample step for very large zooms to cap CPU, while keeping enough
        // resolution to find the true peaks.
        const int step = juce::jmax (1, span / 128);

        float maxOverlap    = 0.0f;
        float minCombined   = 1.0f;
        float maxCombined   = -1.0f;
        
        float energySum   = 0.0f;
        float envelopeSum = 0.0f;
        int count = 0;

        for (int s = sampleStart; s < sampleEnd; s += step)
        {
            const float rawBass = visibleMainBuffer[(size_t) s];
            const float rawKick = visibleSideBuffer[(size_t) s];
            const float sBass   = smoothedMainBuffer[(size_t) s];
            const float sKick   = smoothedSideBuffer[(size_t) s];

            const float bassGained = juce::jlimit (-1.0f, 1.0f, rawBass * gain);
            const float kickGained = juce::jlimit (-1.0f, 1.0f, rawKick * gain);

            // Envelope and Energy for coloring/opacity
            envelopeSum += sBass * sKick;
            energySum   += std::abs (bassGained) + std::abs (kickGained);
            ++count;

            // Envelope Min/Max for drawing shapes (prevents HF cancelling to 0)
            const float combined = 0.5f * (bassGained + kickGained);
            if (combined < minCombined) minCombined = combined;
            if (combined > maxCombined) maxCombined = combined;

            const float ov = juce::jmin (std::abs (bassGained), std::abs (kickGained));
            if (ov > maxOverlap) maxOverlap = ov;
        }

        if (count == 0)
            continue;

        if (minCombined > maxCombined) minCombined = maxCombined = 0.0f;

        const float invCount  = 1.0f / (float) count;
        const float avgEnergy = energySum * invCount;

        // ----- DYNAMIC ALPHA -----
        const float rawAlpha  = juce::jlimit (0.06f, 1.0f, avgEnergy);
        const float glowAlpha = 0.06f + 0.25f * rawAlpha;

        const float x0 = bounds.getX() + (float) p * xStep;
        const float w  = xStep + 0.5f;

        // Draw symmetric overlap area around zero-axis
        const float h = juce::jmax (1.0f, maxOverlap * halfHeight);
        const float y = midY - h;
        const float hFull = h * 2.0f;

        if (envelopeSum > 1.0e-6f)
        {
            g.setGradientFill (constrGrad);
            g.fillRect (x0, y, w, hFull);
            g.setColour (constructive.withAlpha (glowAlpha));
            g.fillRect (x0, y - 1.0f, w, hFull + 2.0f);
        }
        else if (envelopeSum < -1.0e-6f)
        {
            g.setGradientFill (destrGrad);
            g.fillRect (x0, y, w, hFull);
            g.setColour (destructive.withAlpha (glowAlpha));
            g.fillRect (x0, y - 1.0f, w, hFull + 2.0f);
        }

        // Min/Max envelope for the combined white trace
        const float yTop = midY - maxCombined * halfHeight;
        const float yBot = midY - minCombined * halfHeight;

        if (p == 0)
        {
            whiteTrace.startNewSubPath (x0, yTop);
            whiteTrace.lineTo (x0, yBot);
        }
        else
        {
            whiteTrace.lineTo (x0, yTop);
            whiteTrace.lineTo (x0, yBot);
        }
    }

    g.setColour (traceColour.withAlpha (0.96f));
    g.strokePath (whiteTrace, juce::PathStrokeType (1.7f, juce::PathStrokeType::mitered,
                                                    juce::PathStrokeType::rounded));
}

void Oscilloscope::drawFreeRunMode (juce::Graphics& g,
                                    juce::Rectangle<float> bounds,
                                    int visible,
                                    float gain,
                                    float midY)
{
    // Raw live scope. Unlike Overlay this draws thin unfilled traces (no
    // alignment fills) and, crucially, the visible buffers were rebuilt with
    // NO visual/PDC offset applied — see scopeModeSupportsVisualOffset — so it
    // shows the untouched incoming signals, continuously scrolling with the
    // newest material at the right edge.
    const float xStep = bounds.getWidth() / (float) (visible - 1);
    const float halfHeight = bounds.getHeight() * 0.46f;

    drawTriggeredTrace (g, bounds, visibleSideBuffer.data(), visible, 0, visible, xStep,
                        midY, halfHeight, gain, kickColour.withAlpha (0.90f), 1.6f, 0.15f, 0.05f);

    drawTriggeredTrace (g, bounds, visibleMainBuffer.data(), visible, 0, visible, xStep,
                        midY, halfHeight, gain, bassColour.withAlpha (0.96f), 2.0f, 0.20f, 0.10f);

    // Live-edge indicator: the right edge is "now". A faint playhead line plus a
    // small LIVE tag make the newest material obvious and reinforce that this is
    // a running scope, not a frozen alignment snapshot.
    const float edgeX = bounds.getRight();
    g.setColour (juce::Colours::white.withAlpha (0.75f));
    g.drawVerticalLine ((int) std::round (edgeX), bounds.getY(), bounds.getBottom());

    g.setColour (juce::Colours::white.withAlpha (0.85f));
    g.setFont (juce::Font (juce::FontOptions (10.0f)).boldened());
    g.drawText ("LIVE",
                juce::Rectangle<int> ((int) (edgeX - 36.0f), (int) (bounds.getY() + 2.0f), 34, 12),
                juce::Justification::centredRight);

    drawWaveLegend (g, bounds);
}

void Oscilloscope::drawSeparateMode (juce::Graphics& g,
                                     juce::Rectangle<float> bounds,
                                     int visible,
                                     float gain)
{
    const float xStep = bounds.getWidth() / (float) (visible - 1);
    const auto geom = calculateSeparateModeGeometry (bounds.getHeight());

    // Draw bass in top lane
    drawTriggeredTrace (g, bounds, visibleMainBuffer.data(), visible, 0, visible, xStep,
                        bounds.getY() + geom.bassCenterY, geom.bassHeight * 0.5f, gain,
                        bassColour.withAlpha(0.96f), 1.6f, 0.15f, 0.08f);

    g.setColour (bassColour.withAlpha (0.9f));
    g.setFont (juce::Font (juce::FontOptions (11.0f)).boldened());
    g.drawText ("BASS",
                juce::Rectangle<int> ((int) bounds.getX() + 4,
                                      (int) (bounds.getY() + geom.bassCenterY - geom.bassHeight * 0.5f - 16.0f),
                                      48, 12),
                juce::Justification::centredLeft);

    // Draw kick in bottom lane
    drawTriggeredTrace (g, bounds, visibleSideBuffer.data(), visible, 0, visible, xStep,
                        bounds.getY() + geom.kickCenterY, geom.kickHeight * 0.5f, gain,
                        kickColour.withAlpha(0.96f), 1.6f, 0.15f, 0.08f);

    g.setColour (kickColour.withAlpha (0.9f));
    g.setFont (juce::Font (juce::FontOptions (11.0f)).boldened());
    g.drawText ("KICK",
                juce::Rectangle<int> ((int) bounds.getX() + 4,
                                      (int) (bounds.getY() + geom.kickCenterY - geom.kickHeight * 0.5f - 16.0f),
                                      48, 12),
                juce::Justification::centredLeft);
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
    g.strokePath (band, juce::PathStrokeType (strokeWidth * 0.6f, juce::PathStrokeType::mitered,
                                              juce::PathStrokeType::rounded));
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
                + (scopeModeSupportsVisualOffset (viewMode) ? juce::String (visualOffsetSamples) + " smp"
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
    const int effectiveOffset = applyVisualOffset
                                  ? effectiveVisualOffsetSamples (viewMode, visualOffsetSamples)
                                  : 0;
    const int undecimatedScrollSamples = msToSamples(displayScrollMs, sampleRate);
    const int decimatedScrollSamples = undecimatedScrollSamples / std::max(1, decimationFactor);
    const int firstVisible = historyLength - visible - decimatedScrollSamples;

    const auto& currentMain = snapshotActive ? snapshotMainHistory : mainHistory;
    const auto& currentSide = snapshotActive ? snapshotSidechainHistory : sidechainHistory;
    const int currentWriteIndex = snapshotActive ? snapshotWriteIndex : writeIndex;

    for (int i = 0; i < visible; ++i)
    {
        const auto indices = resolveRelativeDisplayHistoryIndices (currentWriteIndex, historyLength,
                                                                   firstVisible, i,
                                                                   effectiveOffset,
                                                                   decimationFactor);
        visibleMainBuffer[(size_t) i] = currentMain[(size_t) indices.bassIndex];
        visibleSideBuffer[(size_t) i] = currentSide[(size_t) indices.kickIndex];
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
    sweepTriggerSample = juce::jlimit (0, window - 1, sweepPreRollSamples);
    kickReferenceTriggerSample = sweepTriggerSample;

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
    targetDisplayGain = 0.0f;
    sweepMarkers = {};
    nextSweepHitId = 1;
    sweepHitId = 0;
    pendingRelockHitId = 0;
    kickReferenceHitId = 0;
    sweepMarkersHitId = 0;
    kickReferenceState = KickReferenceState::RelockPending;
    relockFencePending = false;
    triggeredPanUserSet = false;
    invalidateTriggeredRenderCaches();
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
            if (discardingSweepUntilRelock)
            {
                if ((flagScratch[(size_t) i] & HitCaptureBuffer::sweepResetFlag) == 0)
                    continue;

                discardingSweepUntilRelock = false;
                relockFencePending = false;
            }

            if ((flagScratch[(size_t) i] & HitCaptureBuffer::sweepStartFlag) != 0)
            {
                if (kickReferenceState == KickReferenceState::RelockPending && kickReferenceValid)
                {
                    if (! pendingRelockDiscarding
                        && pendingRelockFill >= sweepWindowSamples)
                    {
                        finishPendingRelockSweep();
                        dirty = true;
                    }

                    if (kickReferenceState == KickReferenceState::RelockPending)
                        beginPendingRelockSweep();
                }
                else if (! kickReferenceValid
                         && kickReferenceShouldReplaceOnCapture (kickReferenceState)
                         && ! sweepDiscarding)
                {
                    if (sweepFill >= sweepWindowSamples)
                    {
                        finishSweep();
                        dirty = true;
                    }
                }

                beginNewSweep();
                dirty = true;
            }

            if (kickReferenceState == KickReferenceState::RelockPending && kickReferenceValid
                && ! pendingRelockDiscarding && pendingRelockFill < sweepWindowSamples)
            {
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

    std::fill (sweepBass.begin(), sweepBass.end(), 0.0f);
    std::fill (sweepKick.begin(), sweepKick.end(), 0.0f);
    sweepFill = 0;
    sweepPeak = 0.0f;
    sweepDiscarding = false;
    sweepHitId = nextSweepHitId++;
    sweepTriggerSample = kickReferenceValid ? kickReferenceTriggerSample
                                            : juce::jlimit (0, juce::jmax (0, sweepWindowSamples - 1),
                                                            sweepPreRollSamples);
    sweepMarkers = {};
    sweepMarkersHitId = 0;

}

void Oscilloscope::beginPendingRelockSweep()
{
    std::fill (pendingRelockBass.begin(), pendingRelockBass.end(), 0.0f);
    std::fill (pendingRelockKick.begin(), pendingRelockKick.end(), 0.0f);
    pendingRelockFill = 0;
    pendingRelockPeak = 0.0f;
    pendingRelockDiscarding = false;
    pendingRelockHitId = nextSweepHitId++;
}

void Oscilloscope::promoteCurrentSweepToGhost()
{
    ++ghostsVersion;
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
    if (! scopeCompletedCaptureCanLock (sweepFill, sweepWindowSamples, sweepPeak))
        return;

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
            kickReferenceHitId = sweepHitId;
            kickReferenceState = kickReferenceStateAfterCapture (kickReferenceState);
            commitTriggeredReferenceFrame();
        }
    }

    // Δ markers are computed ONCE per completed hit over the full window (not
    // the zoomed slice), so the read-out can't jitter frame-to-frame and
    // doesn't change with zoom. Bass from this sweep, kick from the reference.
    if (kickReferenceValid && kickReferenceHitId != sweepHitId)
        sweepTriggerSample = kickReferenceTriggerSample;
    else if (! kickReferenceValid)
        sweepTriggerSample = findTriggeredKickOnsetIndex (sweepKick.data(),
                                                          sweepWindowSamples,
                                                          sweepPreRollSamples);

    sweepMarkers = findEnvelopePeakMarkers (sweepBass.data(),
                                            kickReferenceValid ? kickReference.data()
                                                               : sweepKick.data(),
                                            sweepWindowSamples, sampleRate);
    sweepMarkersHitId = sweepHitId;

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

    if (! scopeCompletedCaptureCanLock (pendingRelockFill, sweepWindowSamples, kickPeak))
    {
        pendingRelockFill = 0;
        pendingRelockPeak = 0.0f;
        pendingRelockDiscarding = true;
        pendingRelockHitId = 0;
        return;
    }

    promoteCurrentSweepToGhost();

    sweepBass.swap (pendingRelockBass);
    sweepKick.swap (pendingRelockKick);
    sweepFill = juce::jlimit (0, sweepWindowSamples, pendingRelockFill);
    sweepPeak = pendingRelockPeak;
    sweepDiscarding = true;
    sweepHitId = pendingRelockHitId;

    std::copy (sweepKick.begin(), sweepKick.end(), kickReference.begin());
    kickReferenceValid = true;
    kickReferencePeak = kickPeak;
    kickReferenceHitId = sweepHitId;
    kickReferenceState = kickReferenceStateAfterCapture (kickReferenceState);
    commitTriggeredReferenceFrame();

    const float candidate = scopeAutoGainTargetFromPeak (juce::jmax (sweepPeak, kickReferencePeak));
    if (scopeAutoGainShouldRetarget (targetDisplayGain, candidate))
        targetDisplayGain = candidate;

    pendingRelockFill = 0;
    pendingRelockPeak = 0.0f;
    pendingRelockDiscarding = true;
    pendingRelockHitId = 0;
}

bool Oscilloscope::glideTriggeredAutoGain() noexcept
{
    const float before = displayGain;
    displayGain = scopeGlideAutoGain (displayGain, targetDisplayGain > 0.0f ? targetDisplayGain : 1.0f);

    // Report whether the gain moved enough to be worth a repaint, so the timer
    // can settle the trace between hits without repainting on every idle tick.
    return std::abs (displayGain - before) > 1.0e-3f;
}

void Oscilloscope::commitTriggeredReferenceFrame()
{
    if (! kickReferenceValid || sweepWindowSamples <= 1 || sweepFill < sweepWindowSamples)
        return;

    // The 20 ms pre-roll remains internal capture context.  Once the complete
    // frame exists, replace that provisional index with the detected onset and
    // commit one geometry value to both lanes.
    const int detectedTrigger = findTriggeredKickOnsetIndex (kickReference.data(),
                                                              sweepWindowSamples,
                                                              sweepPreRollSamples);
    sweepTriggerSample = detectedTrigger;
    kickReferenceTriggerSample = detectedTrigger;
    sweepMarkers = findEnvelopePeakMarkers (sweepBass.data(), kickReference.data(),
                                            sweepWindowSamples, sampleRate);
    sweepMarkersHitId = sweepHitId;
    updateTriggeredDisplayRange();
    invalidateTriggeredRenderCaches();
}

void Oscilloscope::updateTriggeredDisplayRange() noexcept
{
    const int n = sweepWindowSamples;
    if (n <= 1)
        return;

    const int trigger = juce::jlimit (0, n - 1, sweepTriggerSample);
    const auto range = computeTriggeredVisibleRange (n, trigger, timeZoom);
    if (! triggeredPanUserSet)
        displayScrollMs = 0.0f;
    displayScrollMs = clampTriggeredPanScrollMs (displayScrollMs, n, range.visible,
                                                  range.first, sampleRate);
}

void Oscilloscope::invalidateTriggeredRenderCaches()
{
    kickRefCache = {};
    ghostsCache = {};
    gridCache = {};
    kickRefKey = {};
    ghostsKey = {};
    gridKey = {};
    visibleBuffersDirty = true;
    lastDisplayScrollMs = -1.0f;
}

void Oscilloscope::beginTriggeredRelockTransition() noexcept
{
    hitCapture.requestReset();
    relockFencePending = true;
    kickReferenceState = kickReferenceStateAfterRelock();
    kickReferenceValid = false;
    kickReferencePeak = 0.0f;
    kickReferenceTriggerSample = 0;
    kickReferenceHitId = 0;
    sweepFill = 0;
    sweepPeak = 0.0f;
    sweepDiscarding = true;
    sweepTriggerSample = sweepPreRollSamples;
    sweepMarkers = {};
    sweepMarkersHitId = 0;
    sweepHitId = 0;
    pendingRelockFill = 0;
    pendingRelockPeak = 0.0f;
    pendingRelockDiscarding = true;
    pendingRelockHitId = 0;
    for (auto& fill : ghostFill)
        fill = 0;
    std::fill (sweepBass.begin(), sweepBass.end(), 0.0f);
    std::fill (sweepKick.begin(), sweepKick.end(), 0.0f);
    std::fill (pendingRelockBass.begin(), pendingRelockBass.end(), 0.0f);
    std::fill (pendingRelockKick.begin(), pendingRelockKick.end(), 0.0f);
    for (auto& buffer : ghostBass)
        std::fill (buffer.begin(), buffer.end(), 0.0f);
    targetDisplayGain = 0.0f;
    discardingSweepUntilRelock = true;
    if (! triggeredPanUserSet)
        displayScrollMs = 0.0f;
    invalidateTriggeredRenderCaches();
    ++ghostsVersion;
    repaint();
}

void Oscilloscope::relockKickReference() noexcept
{
    beginTriggeredRelockTransition();
}

void Oscilloscope::setDelayFromDrag (const juce::MouseEvent& e)
{
    if (delayParameter == nullptr)
        return;

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
    updateSnapshotOwnership();
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

    if (viewMode == ScopeViewMode::Triggered)
    {
        const int n = sweepWindowSamples > 1 ? sweepWindowSamples : hitCapture.getWindowSamples();
        const int trigger = juce::jlimit (0, juce::jmax (0, n - 1), sweepTriggerSample);
        const auto range = computeTriggeredVisibleRange (n, trigger, timeZoom);
        const float msPerPixel = samplesToMs (range.visible, sampleRate) / (float) componentWidth;
        
        const float next = scopeDragToScrollMs (panStartScrollMs, pixelsMoved, msPerPixel);
        displayScrollMs = clampTriggeredPanScrollMs (next, n, range.visible, range.first, sampleRate);
        triggeredPanUserSet = true;
        
        repaint();
        return;
    }

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
    updateSnapshotOwnership();
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

void Oscilloscope::mouseMove (const juce::MouseEvent& e)
{
    if (lastMousePos != e.position)
    {
        lastMousePos = e.position;
        repaint();
    }
}

void Oscilloscope::mouseEnter (const juce::MouseEvent& e)
{
    isMouseOverScope = true;
    lastMousePos = e.position;
    repaint();
}

void Oscilloscope::mouseExit (const juce::MouseEvent&)
{
    isMouseOverScope = false;
    lastMousePos = { -1.0f, -1.0f };
    repaint();

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

    if (onToggleCleanMode != nullptr)
        onToggleCleanMode();

    repaint();
}

void Oscilloscope::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (! scopeModeAcceptsZoom (viewMode))
        return;

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
        zoomAnchorFraction = juce::jlimit (0.0f, 1.0f,
                                           e.position.x / (float) juce::jmax (1, getWidth()));

        if (e.mods.isShiftDown())
            targetAmpZoom = scopeZoomTargetFromWheelDelta (targetAmpZoom, zoomDelta, 1.0f, 8.0f);
        else
            targetTimeZoom = scopeZoomTargetFromWheelDelta (targetTimeZoom, zoomDelta, 1.0f, 16.0f);

        visibleBuffersDirty = true;

        if (onZoomChanged != nullptr)
            onZoomChanged();
    }

    if (std::abs (wheel.deltaX) > 0.0f && ! shiftWheelAsHorizontal)
    {
        const int componentWidth = juce::jmax (1, getWidth());

        if (viewMode == ScopeViewMode::Triggered)
        {
            const int n = sweepWindowSamples > 1 ? sweepWindowSamples : hitCapture.getWindowSamples();
            const int trigger = juce::jlimit (0, juce::jmax (0, n - 1), sweepTriggerSample);
            const auto range = computeTriggeredVisibleRange (n, trigger, timeZoom);
            const float msPerPixel = samplesToMs (range.visible, sampleRate) / (float) componentWidth;
            const float next = displayScrollMs - wheel.deltaX * 100.0f * msPerPixel;
            displayScrollMs = clampTriggeredPanScrollMs (next, n, range.visible, range.first, sampleRate);
            triggeredPanUserSet = true;
            repaint();
            return;
        }

        const int visible = calculateVisibleScopeSamples (historyLength, sampleRate, decimationFactor, timeZoom, gridDivision, tempoAvailable, bpm);
        const float msPerPixel = (float) calculateVisibleWindowMs (visible, sampleRate, decimationFactor)
                               / (float) componentWidth;

        displayScrollMs = clampDisplayScrollMs (displayScrollMs - wheel.deltaX * 100.0f * msPerPixel);
        repaint();
    }
}

void Oscilloscope::mouseMagnify (const juce::MouseEvent& e, float scaleFactor)
{
    if (! scopeModeAcceptsZoom (viewMode))
        return;

    if (e.mods.isShiftDown())
    {
        targetAmpZoom = scopeZoomTargetFromMagnify (targetAmpZoom, scaleFactor, 1.0f, 8.0f);
    }
    else
    {
        targetTimeZoom = scopeZoomTargetFromMagnify (targetTimeZoom, scaleFactor, 1.0f, 16.0f);
        zoomAnchorFraction = juce::jlimit (0.0f, 1.0f,
                                           e.position.x / (float) juce::jmax (1, getWidth()));
    }
    visibleBuffersDirty = true;
    if (onZoomChanged != nullptr)
        onZoomChanged();
    repaint();
}

void Oscilloscope::resized()
{
}
