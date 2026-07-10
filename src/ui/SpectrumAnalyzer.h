#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <functional>

#include "../util/SpectrumFifo.h"

class SpectrumAnalyzer : public juce::Component,
                         public juce::Timer
{
public:
    SpectrumAnalyzer(SpectrumFifo& fifoToUse);
    ~SpectrumAnalyzer() override;

    void paint (juce::Graphics& g) override;
    void resized() override;
    void timerCallback() override;

    void setSampleRate (double newSampleRate);
    void setColours (juce::Colour mainCol, juce::Colour sideCol);

    void mouseMove (const juce::MouseEvent& e) override;
    void mouseExit (const juce::MouseEvent& e) override;
    void mouseDoubleClick (const juce::MouseEvent& e) override;

    std::function<void()> onToggleCleanMode;

private:
    void calculateSpectrum();
    void rebuildBinCache();
    void drawWaveLegend(juce::Graphics& g, juce::Rectangle<float> bounds) const;

    SpectrumFifo& spectrumFifo;

    double sampleRate = 44100.0;
    
    juce::Colour bassColour = juce::Colours::cyan;
    juce::Colour kickColour = juce::Colours::orange;

    bool isMouseOverScope = false;
    juce::Point<float> lastMousePos;

    static constexpr int historyLength = 8192;
    std::array<float, historyLength> mainLHistory {};
    std::array<float, historyLength> mainRHistory {};
    std::array<float, historyLength> sideLHistory {};
    std::array<float, historyLength> sideRHistory {};
    int writeIndex = 0;

    std::array<float, historyLength * 2> fftScratchMainL {};
    std::array<float, historyLength * 2> fftScratchMainR {};
    std::array<float, historyLength * 2> fftScratchSideL {};
    std::array<float, historyLength * 2> fftScratchSideR {};
    juce::dsp::WindowingFunction<float> fftWindow { historyLength, juce::dsp::WindowingFunction<float>::hann, false };

    std::array<float, historyLength> spectrumMain {};
    std::array<float, historyLength> spectrumSide {};

    struct PixelBinCache {
        float x;
        float binStart;
        float binEnd;
    };
    std::vector<PixelBinCache> binCache;
    double lastCacheSampleRate = 0.0;
    float lastCacheWidth = 0.0f;
    float lastCacheX = 0.0f;

    juce::ComboBox speedComboBox;
    float smoothingFactor = 0.4f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpectrumAnalyzer)
};
