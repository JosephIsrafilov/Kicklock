#pragma once

#include <JuceHeader.h>
#include <cmath>
#include <vector>

class DynamicTransientEQ
{
public:
    DynamicTransientEQ() {}

    void prepare (double sr, int maxBlock, int numChannels)
    {
        sampleRate = sr > 0.0 ? sr : 44100.0;
        channels = numChannels;

        juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) maxBlock, (juce::uint32) numChannels };
        
        peakFilters.clear();
        for (int i = 0; i < numChannels; ++i)
        {
            peakFilters.emplace_back();
            peakFilters.back().prepare(spec);
        }

        // Fast attack (1-2ms), moderate release (15-25ms)
        const float attackMs = 2.0f;
        const float releaseMs = 20.0f;
        attackCoeff = (float) std::exp (-1.0 / (attackMs * 0.001 * sampleRate));
        releaseCoeff = (float) std::exp (-1.0 / (releaseMs * 0.001 * sampleRate));

        envelope = 0.0f;
        
        // Initial neutral coefficients
        updateCoefficients(0.0f);
    }

    void reset()
    {
        for (auto& filter : peakFilters)
            filter.reset();
        envelope = 0.0f;
    }

    // Process a block of high-band bass, triggered by the raw kick sidechain
    void process (juce::AudioBuffer<float>& highBandBass, const juce::AudioBuffer<float>& rawKickSidechain, int numSamples)
    {
        if (highBandBass.getNumChannels() == 0 || rawKickSidechain.getNumChannels() == 0 || peakFilters.empty())
            return;

        const int numCh = std::min((int)peakFilters.size(), highBandBass.getNumChannels());
        const int kickCh = rawKickSidechain.getNumChannels();

        for (int i = 0; i < numSamples; ++i)
        {
            // Get kick peak for this sample
            float kickPeak = 0.0f;
            for (int ch = 0; ch < kickCh; ++ch)
                kickPeak = std::max(kickPeak, std::abs(rawKickSidechain.getSample(ch, i)));

            // Envelope follower
            const float coeff = kickPeak > envelope ? attackCoeff : releaseCoeff;
            envelope = coeff * envelope + (1.0f - coeff) * kickPeak;

            // Map envelope to EQ gain. 
            // When envelope is high (kick hits), gain goes up.
            // Let's say +10 dB boost when envelope is 1.0.
            const float maxBoostDb = 10.0f;
            const float currentBoostDb = juce::jlimit(0.0f, maxBoostDb, envelope * maxBoostDb);

            // Optimization: Only update filter coeffs if the gain changed significantly
            if (std::abs(currentBoostDb - lastBoostDb) > 0.1f)
            {
                updateCoefficients(currentBoostDb);
                lastBoostDb = currentBoostDb;
            }

            // Apply EQ
            for (int ch = 0; ch < numCh; ++ch)
            {
                const float s = highBandBass.getSample(ch, i);
                highBandBass.setSample(ch, i, peakFilters[(size_t)ch].processSample(s));
            }
        }
    }

private:
    void updateCoefficients(float gainDb)
    {
        // 3.5 kHz peak, high Q
        const float freq = 3500.0f;
        const float q = 2.0f;
        auto coeffs = juce::dsp::IIR::ArrayCoefficients<float>::makePeakFilter(sampleRate, freq, q, juce::Decibels::decibelsToGain(gainDb));
        for (auto& filter : peakFilters)
            *filter.coefficients = coeffs;
    }

    double sampleRate = 44100.0;
    int channels = 1;
    
    float attackCoeff = 0.0f;
    float releaseCoeff = 0.0f;
    float envelope = 0.0f;
    float lastBoostDb = -1.0f;

    std::vector<juce::dsp::IIR::Filter<float>> peakFilters;
};
