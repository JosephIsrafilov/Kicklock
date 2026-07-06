#include "Oscilloscope.h"

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

    if (viewMode == ScopeViewMode::Triggered)
    {
        if (! isDisplayFrozen())
        {
            if (refreshTriggeredSnapshot())
                freeRunTicks = 0;
            else if (++freeRunTicks >= freeRunWatchdogTicks)
                buildFreeRunTriggeredSnapshot();

            updateTriggeredAutoGain();
        }
    }
    else if (anyRead && ! isDisplayFrozen())
    {
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
    plotBounds.removeFromBottom (20.0f);

    const int visible = calculateVisibleScopeSamples (historyLength, sampleRate, decimationFactor,
                                                      timeZoom, gridDivision, tempoAvailable, bpm);

    if (viewMode == ScopeViewMode::Triggered)
    {
        float peak = 0.0f;
        for (size_t i = 0; i < triggeredBass.size() && i < triggeredKick.size(); ++i)
        {
            peak = juce::jmax (peak, std::abs (triggeredBass[i]));
            peak = juce::jmax (peak, std::abs (triggeredKick[i]));
        }

        drawTriggeredMode (g, plotBounds, displayGain * ampZoom);

        g.setColour (juce::Colours::white.withAlpha (0.9f));
        g.setFont (juce::Font (juce::FontOptions (12.0f)).boldened());
        g.drawText (scopeModeCaption (viewMode),
                    plotBounds.removeFromTop (16.0f).toNearestInt(),
                    juce::Justification::centredLeft);

        drawHoldIndicator (g, panelBounds);
        return;
    }

    rebuildVisibleBuffers (visible, scopeModeAppliesVisualOffset (viewMode));

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

    drawScopeFooter (g, plotBounds, visible);

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
        g.drawText ("WAITING FOR KICK",
                    bounds.withSizeKeepingCentre (180.0f, 18.0f).toNearestInt(),
                    juce::Justification::centred);
        return;
    }

    const int safePreRoll = juce::jlimit (0, n - 1, triggeredPreRollSamples);
    const float triggerX = bounds.getX() + bounds.getWidth() * (float) safePreRoll / (float) (n - 1);

    const float totalMs = samplesToMs (n, sampleRate);
    const float preMs = samplesToMs (safePreRoll, sampleRate);
    const float postMs = totalMs - preMs;

    g.setColour (gridMinor);
    for (float ms = -20.0f; ms <= 150.0f; ms += 10.0f)
    {
        const float x = triggerX + bounds.getWidth() * (ms / juce::jmax (1.0f, preMs + postMs));
        if (x >= bounds.getX() && x <= bounds.getRight())
            g.drawVerticalLine ((int) std::round (x), bounds.getY(), bounds.getBottom());
    }

    g.setColour (juce::Colours::white.withAlpha (0.65f));
    g.drawVerticalLine ((int) std::round (triggerX), bounds.getY(), bounds.getBottom());

    auto drawPair = [&] (const std::vector<float>& bass,
                         const std::vector<float>& kick,
                         float alpha)
    {
        const int samples = juce::jmin ((int) bass.size(), (int) kick.size());
        if (samples <= 1)
            return;

        juce::Path bassPath, kickPath;
        const int pointCount = calculateTriggeredRenderPointCount (samples, (int) std::ceil (bounds.getWidth()));
        const float sampleXStep = bounds.getWidth() / (float) (samples - 1);
        bassPath.preallocateSpace (pointCount * 3);
        kickPath.preallocateSpace (pointCount * 3);

        for (int point = 0; point < pointCount; ++point)
        {
            const int i = triggeredRenderSampleIndex (point, pointCount, samples);
            const float x = bounds.getX() + (float) i * sampleXStep;
            const float bassY = midY - juce::jlimit (-1.0f, 1.0f, bass[(size_t) i] * gain) * halfHeight;
            const float kickY = midY - juce::jlimit (-1.0f, 1.0f, kick[(size_t) i] * gain) * halfHeight;

            if (point == 0)
            {
                bassPath.startNewSubPath (x, bassY);
                kickPath.startNewSubPath (x, kickY);
            }
            else
            {
                bassPath.lineTo (x, bassY);
                kickPath.lineTo (x, kickY);
            }
        }

        g.setColour (kickColour.withAlpha (alpha));
        g.strokePath (kickPath, juce::PathStrokeType (1.25f, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
        g.setColour (bassColour.withAlpha (juce::jmin (1.0f, alpha + 0.04f)));
        g.strokePath (bassPath, juce::PathStrokeType (1.45f, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::rounded));
    };

    drawPair (triggeredBass, triggeredKick, 0.95f);
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
        for (float t = 0.0f; t <= visibleWindowMs + 0.0001f; t += majorStepMs)
        {
            const float x = bounds.getX() + bounds.getWidth() * (t / visibleWindowMs);

            g.setColour (gridMajor);
            g.drawVerticalLine ((int) std::round (x), bounds.getY(), bounds.getBottom());

            if (t + majorStepMs <= visibleWindowMs + 0.0001f)
            {
                for (int minor = 1; minor <= minorDivisions; ++minor)
                {
                    const float minorT = t + majorStepMs * (float) minor / (float) (minorDivisions + 1);
                    if (minorT >= visibleWindowMs)
                        continue;

                    const float minorX = bounds.getX() + bounds.getWidth() * (minorT / visibleWindowMs);
                    g.setColour (gridMinor);
                    g.drawVerticalLine ((int) std::round (minorX), bounds.getY(), bounds.getBottom());
                }
            }

            g.setColour (labelColour);
            g.setFont (juce::Font (juce::FontOptions (10.0f)));
            const float relativeMs = t - visibleWindowMs;
            g.drawText (formatTimeLabel (relativeMs),
                        juce::Rectangle<int> ((int) std::round (x + 3.0f),
                                              (int) std::round (bounds.getBottom() - 16.0f),
                                              56, 12),
                        juce::Justification::centredLeft);
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

    const float xStep = bounds.getWidth() / (float) (visible - 1);
    const float halfHeight = bounds.getHeight() * 0.46f;
    const float lineWidth = juce::jmax (1.0f, xStep + 0.35f);

    for (int i = 0; i < visible; ++i)
    {
        const float x = bounds.getX() + (float) i * xStep;
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

        if (i == 0)
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
    juce::Path bassPath, kickPath, bassFill, kickFill;

    const float xStep = bounds.getWidth() / (float) (visible - 1);
    const float halfHeight = bounds.getHeight() * 0.46f;

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

    auto strokeTrace = [&] (const std::array<float, historyLength>& source,
                            juce::Colour colour, float width)
    {
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

    strokeTrace (visibleSideBuffer, kickColour.withAlpha (0.85f), 1.1f);
    strokeTrace (visibleMainBuffer, bassColour.withAlpha (0.92f), 1.35f);

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

    auto drawLane = [&] (const std::array<float, historyLength>& source,
                         float centreY,
                         juce::Colour colour,
                         const juce::String& label)
    {
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
                                  + juce::String (markers.deltaMs, 2) + " ms";
    g.drawText (deltaLabel,
                juce::Rectangle<int> ((int) (bounds.getRight() - 92.0f),
                                      (int) (bounds.getY() + 4.0f),
                                      84, 14),
                juce::Justification::centredRight);
}

void Oscilloscope::drawScopeFooter (juce::Graphics& g,
                                    juce::Rectangle<float> bounds,
                                    int visible) const
{
    const float visibleWindowMs = samplesToMs (visible * decimationFactor, sampleRate);

    g.setColour (labelColour);
    g.setFont (juce::Font (juce::FontOptions (10.5f)));
    juce::String footer = "Window "
                        + juce::String ((float) visibleWindowMs, visibleWindowMs >= 10.0 ? 1 : 2) + " ms"
                + "  |  Amp " + juce::String (ampZoom, 1) + "x"
                + "  |  PDC "
                + (scopeModeAppliesVisualOffset (viewMode) ? juce::String (visualOffsetSamples) + " smp"
                                                           : juce::String ("off (raw)"));

    if (tempoAvailable && gridDivision != GridDivision::Milliseconds)
        footer << "  |  " << juce::String ((float) bpm, 1) << " BPM";

    g.drawText (footer,
                juce::Rectangle<int> ((int) bounds.getX(), (int) (bounds.getBottom() - 14.0f),
                                      320, 12),
                juce::Justification::centredLeft);
}

void Oscilloscope::rebuildVisibleBuffers (int visible, bool applyVisualOffset)
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
    // period rather than a fixed sample count.
    const double scopeRate = sampleRate / (double) juce::jmax (1, decimationFactor);
    smoothScopeEnvelope (visibleMainBuffer.data(), smoothedMainBuffer.data(), visible, scopeRate);
    smoothScopeEnvelope (visibleSideBuffer.data(), smoothedSideBuffer.data(), visible, scopeRate);
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

    // Window length changed size (timebase/decimation changed) — the locked
    // kick reference no longer matches, so let the next hit re-establish it.
    kickTraceLocked = false;
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

    if (! triggeredBass.empty())
    {
        for (int i = ghostCount - 1; i > 0; --i)
        {
            ghostBass[(size_t) i].swap (ghostBass[(size_t) (i - 1)]);
            ghostKick[(size_t) i].swap (ghostKick[(size_t) (i - 1)]);
        }

        ghostBass[0].swap (triggeredBass);

        if (! kickTraceLocked)
            ghostKick[0].swap (triggeredKick);
    }

    triggeredBass.swap (triggeredScratchBass);

    // Kick shape is stationary hit-to-hit (same one-shot at a fixed offset
    // from the trigger point) — lock it to the first captured window so it
    // holds still on screen and only the bass trace visibly moves each
    // retrigger. Without this, both traces got replaced on every hit, which
    // reads as the whole scope "running" at the kick's tempo instead of
    // showing bass settling against a fixed reference.
    if (! kickTraceLocked)
    {
        triggeredKick.swap (triggeredScratchKick);
        kickTraceLocked = true;
    }

    triggeredPreRollSamples = hitCapture.getPreRollSamples();
    latestTriggeredSequence = copiedSequence;
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
    freeRunTicks = freeRunWatchdogTicks;

    // Free-run is a raw scrolling fallback, not a real trigger-aligned
    // capture — once real triggering resumes, treat the next hit as a fresh
    // reference rather than keeping whatever was on screen during free-run.
    kickTraceLocked = false;
    repaint();
}

void Oscilloscope::updateTriggeredAutoGain() noexcept
{
    float peak = 0.0f;
    for (size_t i = 0; i < triggeredBass.size() && i < triggeredKick.size(); ++i)
    {
        peak = juce::jmax (peak, std::abs (triggeredBass[i]));
        peak = juce::jmax (peak, std::abs (triggeredKick[i]));
    }

    const float targetGain = peak > 1.0e-4f ? juce::jlimit (1.0f, 40.0f, 0.88f / peak) : 1.0f;
    displayGain += 0.18f * (targetGain - displayGain);
}

void Oscilloscope::setDelayFromDrag (const juce::MouseEvent& e)
{
    if (delayParameter == nullptr)
        return;

    const float deltaMs = scopeDragPixelsToDelayDeltaMs (e.position.x - dragStartX, e.mods.isShiftDown());
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
    if (scopeModeUsesDelayDrag (viewMode) && delayParameter != nullptr
        && !e.mods.isRightButtonDown() && !e.mods.isCommandDown())
    {
        dragStartX = e.position.x;
        dragStartDelayMs = delayParameter->convertFrom0to1 (delayParameter->getValue());
        delayGestureActive = true;
        delayParameter->beginChangeGesture();
    }
    else
    {
        // Non-triggered modes: click-hold pauses the display and starts panning.
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
    displayScrollMs = 0.0f;
    repaint();

    if (scopeModeUsesDelayDrag (viewMode) && delayParameter != nullptr
        && !e.mods.isRightButtonDown() && !e.mods.isCommandDown())
    {
        delayParameter->beginChangeGesture();
        delayParameter->setValueNotifyingHost (delayParameter->convertTo0to1 (0.0f));
        delayParameter->endChangeGesture();
    }
}

void Oscilloscope::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    juce::ignoreUnused (e);

    const float step = 1.0f + juce::jlimit (-0.5f, 0.5f, wheel.deltaY) * 0.6f;

    if (std::abs (wheel.deltaY) > 0.0f)
    {
        if (e.mods.isShiftDown())
            setAmpZoom (ampZoom * step);
        else
            setTimeZoom (timeZoom * step);
            
        if (onZoomChanged != nullptr)
            onZoomChanged();
    }

    if (std::abs (wheel.deltaX) > 0.0f)
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
