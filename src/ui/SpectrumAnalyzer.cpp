#include "SpectrumAnalyzer.h"
#include "../dsp/FftPlanCache.h"

SpectrumAnalyzer::SpectrumAnalyzer(ScopeFifo& fifoToUse)
    : spectrumFifo (fifoToUse)
{
    setOpaque (false);
    startTimerHz (30);
    spectrumMain.fill (0.0f);
    spectrumSide.fill (0.0f);
}

SpectrumAnalyzer::~SpectrumAnalyzer()
{
    stopTimer();
}

void SpectrumAnalyzer::setSampleRate (double newSampleRate)
{
    sampleRate = newSampleRate;
}

void SpectrumAnalyzer::setColours (juce::Colour mainCol, juce::Colour sideCol)
{
    bassColour = mainCol;
    kickColour = sideCol;
    repaint();
}

void SpectrumAnalyzer::timerCallback()
{
    std::array<float, 8192> mainBuf {};
    std::array<float, 8192> sideBuf {};
    
    int numRead = spectrumFifo.readAvailable (mainBuf.data(), sideBuf.data(), 8192);
    if (numRead > 0)
    {
        for (int i = 0; i < numRead; ++i)
        {
            mainHistory[(size_t)writeIndex] = mainBuf[(size_t)i];
            sidechainHistory[(size_t)writeIndex] = sideBuf[(size_t)i];
            writeIndex = (writeIndex + 1) % historyLength;
        }
        calculateSpectrum();
        repaint();
    }
}

void SpectrumAnalyzer::calculateSpectrum()
{
    for (int i = 0; i < historyLength; ++i)
    {
        int readIdx = (writeIndex + i) % historyLength;
        fftScratchMain[(size_t)i] = mainHistory[(size_t)readIdx];
        fftScratchSide[(size_t)i] = sidechainHistory[(size_t)readIdx];
    }
    
    fftWindow.multiplyWithWindowingTable (fftScratchMain.data(), historyLength);
    fftWindow.multiplyWithWindowingTable (fftScratchSide.data(), historyLength);
    
    std::fill (fftScratchMain.begin() + historyLength, fftScratchMain.end(), 0.0f);
    std::fill (fftScratchSide.begin() + historyLength, fftScratchSide.end(), 0.0f);

    auto& fft = FftPlanCache::get(13); // 2^13 = 8192
    
    fft.performFrequencyOnlyForwardTransform (fftScratchMain.data());
    fft.performFrequencyOnlyForwardTransform (fftScratchSide.data());
    
    const float minDb = -144.0f;
    for (int i = 0; i < historyLength / 2; ++i)
    {
        float magM = fftScratchMain[(size_t)i] / (float)historyLength;
        float dbM = juce::Decibels::gainToDecibels (magM, minDb);
        spectrumMain[(size_t)i] += (dbM - spectrumMain[(size_t)i]) * 0.4f;
        
        float magS = fftScratchSide[(size_t)i] / (float)historyLength;
        float dbS = juce::Decibels::gainToDecibels (magS, minDb);
        spectrumSide[(size_t)i] += (dbS - spectrumSide[(size_t)i]) * 0.4f;
    }
}

void SpectrumAnalyzer::paint (juce::Graphics& g)
{
    auto panelBounds = getLocalBounds().toFloat().reduced (8.0f, 8.0f);
    
    // Draw background panel to match Oscilloscope style
    g.setColour (findColour (juce::ResizableWindow::backgroundColourId).brighter (0.02f));
    g.fillRoundedRectangle (panelBounds, 12.0f);
    g.setColour (juce::Colours::white.withAlpha (0.08f));
    g.drawRoundedRectangle (panelBounds, 12.0f, 1.2f);
    
    auto plotBounds = panelBounds.reduced (12.0f, 10.0f);
    plotBounds.removeFromTop (12.0f);
    const auto footerStrip = plotBounds.removeFromBottom (20.0f);
    
    // Draw Title
    g.setColour (juce::Colours::white.withAlpha (0.9f));
    g.setFont (juce::Font (juce::FontOptions (12.0f)).boldened());
    g.drawText ("SPECTRUM ANALYZER",
                plotBounds.removeFromTop (16.0f).toNearestInt(),
                juce::Justification::centredLeft);
                
    const float minFreq = 20.0f;
    const float maxFreq = 20000.0f;
    const float minDb = -60.0f;
    const float maxDb = 0.0f;
    
    // Draw vertical/horizontal grid lines (UNDER the curves)
    g.setColour (juce::Colours::white.withAlpha (0.05f));
    
    // Frequency grid lines (logarithmic scale)
    const float freqs[] = { 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f };
    for (float f : freqs)
    {
        float x = plotBounds.getX() + plotBounds.getWidth() * (std::log (f / minFreq) / std::log (maxFreq / minFreq));
        g.drawVerticalLine ((int) std::round (x), plotBounds.getY(), plotBounds.getBottom());
    }
    
    // dB grid lines
    const float dbs[] = { -12.0f, -24.0f, -36.0f, -48.0f };
    for (float db : dbs)
    {
        float y = plotBounds.getBottom() - (db - minDb) / (maxDb - minDb) * plotBounds.getHeight();
        g.drawHorizontalLine ((int) std::round (y), plotBounds.getX(), plotBounds.getRight());
    }

    // Draw the spectrum curves
    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (plotBounds.toNearestInt());
        
        auto drawSpectrumCurve = [&](const std::array<float, historyLength / 2>& spectrumData, juce::Colour colour) {
            juce::Path path;
            bool first = true;
            
            for (float x = plotBounds.getX(); x <= plotBounds.getRight(); x += 1.0f)
            {
                float freq = minFreq * std::pow (maxFreq / minFreq, (x - plotBounds.getX()) / plotBounds.getWidth());
                float binIdx = freq * (float)historyLength / (float)sampleRate;
                int idx = juce::jlimit (0, historyLength / 2 - 1, (int) std::round (binIdx));
                float db = spectrumData[(size_t)idx];
                float y = plotBounds.getBottom() - (juce::jlimit (minDb, maxDb, db) - minDb) / (maxDb - minDb) * plotBounds.getHeight();
                
                if (first)
                {
                    path.startNewSubPath (x, y);
                    first = false;
                }
                else
                {
                    path.lineTo (x, y);
                }
            }
            
            juce::Path filledPath = path;
            filledPath.lineTo (plotBounds.getRight(), plotBounds.getBottom());
            filledPath.lineTo (plotBounds.getX(), plotBounds.getBottom());
            filledPath.closeSubPath();
            
            g.setColour (colour.withAlpha (0.15f)); // Soft translucency
            g.fillPath (filledPath);
            
            g.setColour (colour.withAlpha (0.85f));
            g.strokePath (path, juce::PathStrokeType (1.5f, juce::PathStrokeType::mitered));
        };

        drawSpectrumCurve (spectrumSide, kickColour);
        drawSpectrumCurve (spectrumMain, bassColour);
    }
    
    // Draw Grid text labels (OVER the curves so they are readable)
    g.setFont (juce::Font (juce::FontOptions (10.0f)));
    
    // Frequency labels
    for (float f : freqs)
    {
        float x = plotBounds.getX() + plotBounds.getWidth() * (std::log (f / minFreq) / std::log (maxFreq / minFreq));
        juce::String text = f >= 1000.0f ? juce::String (f / 1000.0f, 0) + "k" : juce::String (f, 0);
        
        g.setColour (juce::Colours::white.withAlpha (0.35f));
        g.drawText (text, (int) std::round (x) - 20, (int) plotBounds.getBottom() - 15, 40, 15, juce::Justification::centred, false);
    }
    
    // dB labels
    for (float db : dbs)
    {
        float y = plotBounds.getBottom() - (db - minDb) / (maxDb - minDb) * plotBounds.getHeight();
        
        g.setColour (juce::Colours::white.withAlpha (0.35f));
        g.drawText (juce::String (db, 0) + " dB", (int) plotBounds.getX() + 4, (int) std::round (y) - 12, 50, 12, juce::Justification::left, false);
    }
    
    // Tooltip/Crosshair (updated to use plotBounds instead of full bounds)
    if (isMouseOverScope && lastMousePos.y >= plotBounds.getY() && lastMousePos.y <= plotBounds.getBottom() &&
        lastMousePos.x >= plotBounds.getX() && lastMousePos.x <= plotBounds.getRight())
    {
        float freq = minFreq * std::pow (maxFreq / minFreq, (lastMousePos.x - plotBounds.getX()) / plotBounds.getWidth());
        float normalizedY = (plotBounds.getBottom() - lastMousePos.y) / plotBounds.getHeight();
        float db = minDb + normalizedY * (maxDb - minDb);
        
        juce::String freqStr = freq >= 1000.0f ? juce::String (freq / 1000.0f, 1) + " kHz" : juce::String (freq, 0) + " Hz";
        juce::String text = freqStr + ", " + juce::String (db, 1) + " dB";
        
        g.setColour (juce::Colours::lightgreen.withAlpha (0.4f));
        g.drawHorizontalLine ((int) std::round (lastMousePos.y), plotBounds.getX(), plotBounds.getRight());
        g.drawVerticalLine ((int) std::round (lastMousePos.x), plotBounds.getY(), plotBounds.getBottom());
        
        juce::Font font (juce::FontOptions(10.0f).withStyle("bold"));
        g.setFont (font);
        
        const float textWidth = juce::GlyphArrangement::getStringWidth (font, text) + 12.0f;
        const float textHeight = 16.0f;
        
        juce::Rectangle<float> badge (lastMousePos.x + 8.0f, 
                                      lastMousePos.y - textHeight - 4.0f, 
                                      textWidth, textHeight);
                                      
        if (badge.getRight() > plotBounds.getRight())
            badge.setX (lastMousePos.x - textWidth - 8.0f);
        if (badge.getY() < plotBounds.getY())
            badge.setY (lastMousePos.y + 4.0f);
            
        g.setColour (juce::Colours::black.withAlpha (0.8f));
        g.fillRoundedRectangle (badge, 3.0f);
        
        g.setColour (juce::Colours::white);
        g.drawText (text, badge, juce::Justification::centred, false);
    }

    drawWaveLegend (g, panelBounds);
}

void SpectrumAnalyzer::resized()
{
}

void SpectrumAnalyzer::mouseMove (const juce::MouseEvent& e)
{
    lastMousePos = e.position;
    isMouseOverScope = true;
    repaint();
}

void SpectrumAnalyzer::mouseExit (const juce::MouseEvent&)
{
    isMouseOverScope = false;
    repaint();
}

void SpectrumAnalyzer::drawWaveLegend(juce::Graphics& g, juce::Rectangle<float> bounds) const
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
