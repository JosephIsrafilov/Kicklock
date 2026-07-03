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

Oscilloscope::Oscilloscope (ScopeFifo& fifoToRead)
    : fifo (fifoToRead),
      mainHistory ((size_t) historyLength, 0.0f),
      sidechainHistory ((size_t) historyLength, 0.0f)
{
    startTimerHz (30);
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

    const int count = fifo.readAvailable (mainScratch.data(), sidechainScratch.data(), scratchSize);

    if (frozen)
        return;

    for (int i = 0; i < count; ++i)
    {
        mainHistory[(size_t) writeIndex]      = mainScratch[(size_t) i];
        sidechainHistory[(size_t) writeIndex] = sidechainScratch[(size_t) i];
        writeIndex = (writeIndex + 1) % historyLength;
    }

    if (count > 0)
        repaint();
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
    rebuildVisibleBuffers (visible);

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
        case ScopeViewMode::PhaseDelta: drawPhaseDeltaMode (g, plotBounds, visible, gain, plotBounds.getCentreY()); break;
        case ScopeViewMode::Overlay:    drawOverlayMode (g, plotBounds, visible, gain, plotBounds.getCentreY()); break;
        case ScopeViewMode::Separate:   drawSeparateMode (g, plotBounds, visible, gain); break;
    }

    drawTransientMarkers (g, plotBounds, visible);
    drawScopeFooter (g, plotBounds, visible);

    g.setColour (juce::Colours::white.withAlpha (0.9f));
    g.setFont (juce::Font (juce::FontOptions (12.0f)).boldened());
    g.drawText (viewMode == ScopeViewMode::PhaseDelta ? "PHASE DELTA"
               : viewMode == ScopeViewMode::Overlay  ? "OVERLAY"
                                                     : "SEPARATE",
                plotBounds.removeFromTop (16.0f).toNearestInt(),
                juce::Justification::centredLeft);

    if (frozen)
    {
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.setFont (juce::Font (juce::FontOptions (11.0f)).boldened());
        g.drawText ("FROZEN",
                    juce::Rectangle<int> ((int) (panelBounds.getRight() - 76.0f),
                                          (int) (panelBounds.getY() + 8.0f), 64, 14),
                    juce::Justification::centredRight);
    }
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

        const auto relation = classifyPhaseRelation (visibleMainBuffer[(size_t) i],
                                                     visibleSideBuffer[(size_t) i]);
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

void Oscilloscope::drawTransientMarkers (juce::Graphics& g,
                                         juce::Rectangle<float> bounds,
                                         int visible) const
{
    const auto markers = findScopePeakMarkers (visibleMainBuffer.data(),
                                               visibleSideBuffer.data(),
                                               visible, sampleRate * (double) decimationFactor);
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
                + "  |  PDC " + juce::String (visualOffsetSamples) + " smp";

    if (tempoAvailable && gridDivision != GridDivision::Milliseconds)
        footer << "  |  " << juce::String ((float) bpm, 1) << " BPM";

    g.drawText (footer,
                juce::Rectangle<int> ((int) bounds.getX(), (int) (bounds.getBottom() - 14.0f),
                                      320, 12),
                juce::Justification::centredLeft);
}

void Oscilloscope::rebuildVisibleBuffers (int visible)
{
    const int firstVisible = historyLength - visible;

    for (int i = 0; i < visible; ++i)
    {
        const auto indices = resolveRelativeDisplayHistoryIndices (writeIndex, historyLength,
                                                                   firstVisible, i,
                                                                   visualOffsetSamples,
                                                                   decimationFactor);
        visibleMainBuffer[(size_t) i] = mainHistory[(size_t) indices.bassIndex];
        visibleSideBuffer[(size_t) i] = sidechainHistory[(size_t) indices.kickIndex];
    }
}

void Oscilloscope::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    juce::ignoreUnused (e);

    const float step = 1.0f + juce::jlimit (-0.5f, 0.5f, wheel.deltaY) * 0.6f;

    if (e.mods.isShiftDown())
        setAmpZoom (ampZoom * step);
    else
        setTimeZoom (timeZoom * step);

    if (onZoomChanged != nullptr)
        onZoomChanged();
}

void Oscilloscope::resized()
{
}
