#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <array>

#include "../util/ScopeFifo.h"

class SpectrumAnalyzer : public juce::Component,
                         public juce::Timer
{
public:
    SpectrumAnalyzer(ScopeFifo& fifoToUse);
    ~SpectrumAnalyzer() override;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void timerCallback() override;

    void setSampleRate (double newSampleRate);
    void setColours (juce::Colour mainCol, juce::Colour sideCol);

    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;

private:
    void calculateSpectrum();
    void rebuildBinCache();
    void drawWaveLegend(juce::Graphics& g, juce::Rectangle<float> bounds) const;

    ScopeFifo& spectrumFifo;

    double sampleRate = 44100.0;
    
    juce::Colour bassColour = juce::Colours::cyan;
    juce::Colour kickColour = juce::Colours::orange;

    bool isMouseOverScope = false;
    juce::Point<float> lastMousePos;

    static constexpr int historyLength = 8192;
    std::array<float, historyLength> mainHistory {};
    std::array<float, historyLength> sidechainHistory {};
    int writeIndex = 0;

    std::array<float, historyLength * 2> fftScratchMain {};
    std::array<float, historyLength * 2> fftScratchSide {};
    juce::dsp::WindowingFunction<float> fftWindow { historyLength, juce::dsp::WindowingFunction<float>::hann, false };

    std::array<float, historyLength / 2> spectrumMain {};
    std::array<float, historyLength / 2> spectrumSide {};

    struct PixelBinCache {
        float x;
        float binStart;
        float binEnd;
    };
    std::vector<PixelBinCache> binCache;
    double lastCacheSampleRate = 0.0;
    float lastCacheWidth = 0.0f;
    float lastCacheX = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumAnalyzer)
};
