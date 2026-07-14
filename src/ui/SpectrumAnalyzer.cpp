#include "SpectrumAnalyzer.h"
#include "../dsp/FftPlanCache.h"
#include "../util/SpectrumAnalysis.h"
#include "ScopeVisuals.h"

class SpectrumAnalyzer::SpectrumWorker final : public juce::Thread
{
public:
    explicit SpectrumWorker (SpectrumAnalyzer& analyzer)
        : juce::Thread ("KickLock Spectrum"), owner (analyzer) {}

    void run() override { owner.runSpectrumWorker(); }

private:
    SpectrumAnalyzer& owner;
};

SpectrumAnalyzer::SpectrumAnalyzer(SpectrumFifo& fifoToUse)
    : spectrumFifo (fifoToUse)
{
    setOpaque (false);
    startTimerHz (30);
    spectrumMain.fill (SpectrumAnalysis::minimumDb);
    spectrumSide.fill (SpectrumAnalysis::minimumDb);

    speedComboBox.addItem("Slow", 1);
    speedComboBox.addItem("Medium", 2);
    speedComboBox.addItem("Fast", 3);
    speedComboBox.setSelectedId(2, juce::dontSendNotification);
    
    speedComboBox.onChange = [this] {
        switch (speedComboBox.getSelectedId()) {
            case 1: smoothingFactor.store (0.15f, std::memory_order_release); break; // Slow
            case 2: smoothingFactor.store (0.4f, std::memory_order_release);  break; // Medium
            case 3: smoothingFactor.store (0.8f, std::memory_order_release);  break; // Fast
        }
    };
    
    // Style the combobox to match the dark UI
    speedComboBox.setColour(juce::ComboBox::backgroundColourId, juce::Colours::black.withAlpha(0.3f));
    speedComboBox.setColour(juce::ComboBox::outlineColourId, juce::Colours::transparentWhite);
    speedComboBox.setColour(juce::ComboBox::textColourId, juce::Colours::white.withAlpha(0.7f));
    speedComboBox.setJustificationType(juce::Justification::centred);
    
    addAndMakeVisible(speedComboBox);
    spectrumWorker = std::make_unique<SpectrumWorker> (*this);
    spectrumWorker->startThread();
}

SpectrumAnalyzer::~SpectrumAnalyzer()
{
    stopTimer();
    if (spectrumWorker != nullptr)
    {
        spectrumWorkerStopping.store (true, std::memory_order_release);
        spectrumWorker->signalThreadShouldExit();
        spectrumWorker->notify();
        const bool stopped = spectrumWorker->waitForThreadToExit (cooperativeTeardownBoundMs);
        if (! stopped)
            spectrumWorker->waitForThreadToExit (-1);
        spectrumWorker.reset();
    }
}

void SpectrumAnalyzer::setSampleRate (double newSampleRate)
{
    sampleRate = newSampleRate;
    rebuildBinCache();
    rebuildSpectrumPaths();
}

void SpectrumAnalyzer::setColours (juce::Colour mainCol, juce::Colour sideCol)
{
    bassColour = mainCol;
    kickColour = sideCol;
    repaint();
}

void SpectrumAnalyzer::timerCallback()
{
    const auto published = publishedSpectrumSequence.load (std::memory_order_acquire);
    if (published == consumedSpectrumSequence)
        return;

    {
        const std::lock_guard<std::mutex> lock (spectrumSnapshotMutex);
        const auto latest = publishedSpectrumSequence.load (std::memory_order_relaxed);
        if (latest == consumedSpectrumSequence)
            return;
        spectrumMain = publishedSpectrumMain;
        spectrumSide = publishedSpectrumSide;
        consumedSpectrumSequence = latest;
    }
    rebuildSpectrumPaths();
    repaint();
}

void SpectrumAnalyzer::runSpectrumWorker()
{
    std::array<float, 8192> mainLBuf {};
    std::array<float, 8192> mainRBuf {};
    std::array<float, 8192> sideLBuf {};
    std::array<float, 8192> sideRBuf {};
    
    uint64_t observedGeneration = spectrumFifo.getGeneration();
    while (! spectrumWorkerStopping.load (std::memory_order_acquire))
    {
        const auto generation = spectrumFifo.getGeneration();
        if (generation != observedGeneration)
        {
            observedGeneration = generation;
            mainLHistory.fill (0.0f);
            mainRHistory.fill (0.0f);
            sideLHistory.fill (0.0f);
            sideRHistory.fill (0.0f);
            writeIndex = 0;
        }

        const int mainChannels = juce::jlimit (0, 2, spectrumFifo.getMainChannels());
        const int sideChannels = juce::jlimit (0, 2, spectrumFifo.getSideChannels());
        const int numRead = spectrumFifo.readLatest (mainLBuf.data(), mainRBuf.data(),
                                                      sideLBuf.data(), sideRBuf.data(), 8192);
        if (numRead <= 0)
        {
            juce::Thread::sleep (5);
            continue;
        }

        for (int i = 0; i < numRead; ++i)
        {
            if ((i & 255) == 0 && spectrumWorkerStopping.load (std::memory_order_acquire))
                return;
            mainLHistory[(size_t) writeIndex] = mainLBuf[(size_t) i];
            mainRHistory[(size_t) writeIndex] = mainChannels > 1 ? mainRBuf[(size_t) i] : mainLBuf[(size_t) i];
            sideLHistory[(size_t) writeIndex] = sideLBuf[(size_t) i];
            sideRHistory[(size_t) writeIndex] = sideChannels > 1 ? sideRBuf[(size_t) i] : sideLBuf[(size_t) i];
            writeIndex = (writeIndex + 1) % historyLength;
        }
        if (! calculateSpectrum())
            return;
        publishSpectrumSnapshot();
    }
}

bool SpectrumAnalyzer::calculateSpectrum()
{
    for (int i = 0; i < historyLength; ++i)
    {
        if ((i & 255) == 0 && spectrumWorkerStopping.load (std::memory_order_acquire))
            return false;
        int readIdx = (writeIndex + i) % historyLength;
        fftScratchMainL[(size_t) i] = mainLHistory[(size_t) readIdx];
        fftScratchMainR[(size_t) i] = mainRHistory[(size_t) readIdx];
        fftScratchSideL[(size_t) i] = sideLHistory[(size_t) readIdx];
        fftScratchSideR[(size_t) i] = sideRHistory[(size_t) readIdx];
    }

    SpectrumAnalysis::calculatePowerSpectrumDb (fftScratchMainL.data(), fftScratchMainR.data(),
                                                fftWindow, workerSpectrumMain.data(), smoothingFactor.load (std::memory_order_acquire));
    if (spectrumWorkerStopping.load (std::memory_order_acquire))
        return false;
    SpectrumAnalysis::calculatePowerSpectrumDb (fftScratchSideL.data(), fftScratchSideR.data(),
                                                fftWindow, workerSpectrumSide.data(), smoothingFactor.load (std::memory_order_acquire));
    return ! spectrumWorkerStopping.load (std::memory_order_acquire);
}

void SpectrumAnalyzer::publishSpectrumSnapshot()
{
    const std::lock_guard<std::mutex> lock (spectrumSnapshotMutex);
    publishedSpectrumMain = workerSpectrumMain;
    publishedSpectrumSide = workerSpectrumSide;
    publishedSpectrumSequence.fetch_add (1, std::memory_order_release);
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
    auto titleArea = plotBounds.removeFromTop (16.0f).toNearestInt();
    
    // Label for the combobox
    juce::Rectangle<int> comboBounds = speedComboBox.getBounds();
    g.setColour (juce::Colours::white.withAlpha (0.5f));
    g.setFont (juce::Font (juce::FontOptions (10.0f)));
    g.drawText("Speed:", comboBounds.translated(-45, 0).withWidth(40), juce::Justification::centredRight);
    
    g.setColour (juce::Colours::white.withAlpha (0.9f));
    g.setFont (juce::Font (juce::FontOptions (12.0f)).boldened());
    g.drawText ("SPECTRUM ANALYZER",
                titleArea,
                juce::Justification::centredLeft);
                
    const float minFreq = 20.0f;
    const float maxFreq = spectrumDisplayMaximumFrequency ((float) sampleRate);
    const float minDb = -108.0f;
    const float maxDb = 0.0f;
    
    // Draw vertical/horizontal grid lines (UNDER the curves)
    g.setColour (juce::Colours::white.withAlpha (0.05f));
    
    // Frequency grid lines (logarithmic scale)
    const float freqs[] = { 20.0f, 50.0f, 100.0f, 200.0f, 500.0f, 1000.0f, 2000.0f, 5000.0f, 10000.0f, 20000.0f };
    for (float f : freqs)
    {
        if (f > maxFreq)
            continue;

        float x = plotBounds.getX() + plotBounds.getWidth() * (std::log (f / minFreq) / std::log (maxFreq / minFreq));
        g.drawVerticalLine ((int) std::round (x), plotBounds.getY(), plotBounds.getBottom());
    }
    
    // dB grid lines
    const float dbs[] = { -6.0f, -12.0f, -24.0f, -36.0f, -48.0f, -60.0f, -72.0f, -84.0f, -96.0f };
    for (float db : dbs)
    {
        float y = plotBounds.getBottom() - (db - minDb) / (maxDb - minDb) * plotBounds.getHeight();
        g.drawHorizontalLine ((int) std::round (y), plotBounds.getX(), plotBounds.getRight());
    }

    // Draw paths prepared when the latest worker snapshot arrived.
    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (plotBounds.toNearestInt());
        g.setColour (kickColour.withAlpha (0.15f));
        g.fillPath (spectrumSideFillPath);
        g.setColour (kickColour.withAlpha (0.85f));
        g.strokePath (spectrumSidePath, juce::PathStrokeType (1.5f, juce::PathStrokeType::mitered));
        g.setColour (bassColour.withAlpha (0.15f));
        g.fillPath (spectrumMainFillPath);
        g.setColour (bassColour.withAlpha (0.85f));
        g.strokePath (spectrumMainPath, juce::PathStrokeType (1.5f, juce::PathStrokeType::mitered));
    }
    
    // Draw Grid text labels (OVER the curves so they are readable)
    g.setFont (juce::Font (juce::FontOptions (10.0f)));
    
    // Frequency labels
    for (float f : freqs)
    {
        if (f > maxFreq)
            continue;

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
        g.drawText (juce::String (db, 0) + " dB", (int) plotBounds.getX() + 4, (int) std::round (y) - 6, 50, 12, juce::Justification::left, false);
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
    rebuildBinCache();
    rebuildSpectrumPaths();
    
    auto panelBounds = getLocalBounds().reduced (8, 8);
    auto plotBounds = panelBounds.reduced (12, 10);
    plotBounds.removeFromTop (12);
    auto titleArea = plotBounds.removeFromTop (16);
    
    titleArea.removeFromRight (110); // Leave space for wave legend
    juce::Rectangle<int> comboBounds = titleArea.removeFromRight(70).withSizeKeepingCentre(70, 16);
    speedComboBox.setBounds(comboBounds);
}

void SpectrumAnalyzer::rebuildBinCache()
{
    if (sampleRate <= 0.0) return;
    
    auto panelBounds = getLocalBounds().toFloat().reduced (8.0f, 8.0f);
    auto plotBounds = panelBounds.reduced (12.0f, 10.0f);
    plotBounds.removeFromTop (12.0f);
    plotBounds.removeFromBottom (20.0f);
    plotBounds.removeFromTop (16.0f);
    
    if (plotBounds.getWidth() <= 0.0f || plotBounds.getHeight() <= 0.0f) return;
    if (sampleRate == lastCacheSampleRate && plotBounds.getWidth() == lastCacheWidth && plotBounds.getX() == lastCacheX) return;
    
    binCache.clear();
    const float minFreq = 20.0f;
    const float maxFreq = spectrumDisplayMaximumFrequency ((float) sampleRate);
    
    for (float x = plotBounds.getX(); x <= plotBounds.getRight(); x += 1.0f)
    {
        float freqStart = minFreq * std::pow (maxFreq / minFreq, (x - 0.5f - plotBounds.getX()) / plotBounds.getWidth());
        float freqEnd   = minFreq * std::pow (maxFreq / minFreq, (x + 0.5f - plotBounds.getX()) / plotBounds.getWidth());
        
        const float maxBin = (float) (historyLength / 2);
        float binStart = juce::jlimit (0.0f, maxBin, freqStart * (float) historyLength / (float) sampleRate);
        float binEnd   = juce::jlimit (0.0f, maxBin, freqEnd * (float) historyLength / (float) sampleRate);
        
        binCache.push_back ({x, binStart, binEnd});
    }
    
    lastCacheSampleRate = sampleRate;
    lastCacheWidth = plotBounds.getWidth();
    lastCacheX = plotBounds.getX();
}

void SpectrumAnalyzer::rebuildSpectrumPaths()
{
    spectrumMainPath.clear();
    spectrumSidePath.clear();
    spectrumMainFillPath.clear();
    spectrumSideFillPath.clear();
    if (binCache.empty())
        return;

    auto panelBounds = getLocalBounds().toFloat().reduced (8.0f, 8.0f);
    auto plotBounds = panelBounds.reduced (12.0f, 10.0f);
    plotBounds.removeFromTop (12.0f);
    plotBounds.removeFromBottom (20.0f);
    plotBounds.removeFromTop (16.0f);
    if (plotBounds.getWidth() <= 0.0f || plotBounds.getHeight() <= 0.0f)
        return;

    auto build = [&] (const std::array<float, historyLength>& spectrumData,
                      juce::Path& line, juce::Path& fill)
    {
        line.preallocateSpace ((int) binCache.size() * 3);
        bool first = true;
        const int maxBin = historyLength / 2;
        for (const auto& cacheItem : binCache)
        {
            float db = SpectrumAnalysis::minimumDb;
            if (cacheItem.binEnd - cacheItem.binStart < 1.0f)
            {
                const float centre = (cacheItem.binStart + cacheItem.binEnd) * 0.5f;
                const int firstBin = juce::jlimit (0, maxBin, (int) std::floor (centre));
                const int secondBin = juce::jlimit (0, maxBin, firstBin + 1);
                const float mu = (1.0f - std::cos ((centre - (float) firstBin) * juce::MathConstants<float>::pi)) * 0.5f;
                db = spectrumData[(size_t) firstBin] * (1.0f - mu) + spectrumData[(size_t) secondBin] * mu;
            }
            else
            {
                const int firstBin = juce::jlimit (0, maxBin, (int) std::floor (cacheItem.binStart));
                const int lastBin = juce::jlimit (0, maxBin, (int) std::ceil (cacheItem.binEnd));
                for (int bin = firstBin; bin <= lastBin; ++bin)
                    db = std::max (db, spectrumData[(size_t) bin]);
            }

            const float y = plotBounds.getBottom()
                - (juce::jlimit (-108.0f, 0.0f, db) + 108.0f) / 108.0f * plotBounds.getHeight();
            if (first) { line.startNewSubPath (cacheItem.x, y); first = false; }
            else line.lineTo (cacheItem.x, y);
        }
        if (! first)
        {
            fill = line;
            fill.lineTo (plotBounds.getRight(), plotBounds.getBottom());
            fill.lineTo (plotBounds.getX(), plotBounds.getBottom());
            fill.closeSubPath();
        }
    };

    build (spectrumSide, spectrumSidePath, spectrumSideFillPath);
    build (spectrumMain, spectrumMainPath, spectrumMainFillPath);
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

void SpectrumAnalyzer::mouseDoubleClick (const juce::MouseEvent& e)
{
    juce::ignoreUnused (e);

    if (onToggleCleanMode != nullptr)
        onToggleCleanMode();
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
