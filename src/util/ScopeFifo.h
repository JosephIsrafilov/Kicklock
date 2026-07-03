#pragma once

#include <vector>
#include <juce_audio_basics/juce_audio_basics.h>

// Lock-free single-producer/single-consumer FIFO carrying paired
// main/sidechain sample values from the audio thread to the UI thread
// for scope display. Backed by juce::AbstractFifo; capacity is fixed at
// prepare() time so no allocation ever happens on the audio thread.
class ScopeFifo
{
public:
    void prepare (int capacitySamples)
    {
        fifo.setTotalSize (capacitySamples);
        mainBuffer.assign ((size_t) capacitySamples, 0.0f);
        sidechainBuffer.assign ((size_t) capacitySamples, 0.0f);
        reset();
    }

    void reset()
    {
        fifo.reset();
    }

    // Audio thread. Drops the sample (no blocking, no growing) if the
    // fifo is full.
    void pushSample (float mainValue, float sidechainValue)
    {
        int start1, size1, start2, size2;
        fifo.prepareToWrite (1, start1, size1, start2, size2);

        if (size1 + size2 == 0)
            return; // fifo full: drop sample

        const int index = size1 > 0 ? start1 : start2;
        mainBuffer[(size_t) index] = mainValue;
        sidechainBuffer[(size_t) index] = sidechainValue;

        fifo.finishedWrite (size1 + size2);
    }

    // Message/UI thread. Returns the number of sample pairs actually read.
    int readAvailable (float* mainOut, float* sidechainOut, int maxSamples)
    {
        int start1, size1, start2, size2;
        fifo.prepareToRead (maxSamples, start1, size1, start2, size2);

        int count = 0;

        for (int i = 0; i < size1; ++i)
        {
            mainOut[count] = mainBuffer[(size_t) (start1 + i)];
            sidechainOut[count] = sidechainBuffer[(size_t) (start1 + i)];
            ++count;
        }

        for (int i = 0; i < size2; ++i)
        {
            mainOut[count] = mainBuffer[(size_t) (start2 + i)];
            sidechainOut[count] = sidechainBuffer[(size_t) (start2 + i)];
            ++count;
        }

        fifo.finishedRead (size1 + size2);

        return count;
    }

private:
    juce::AbstractFifo fifo { 1 };
    std::vector<float> mainBuffer;
    std::vector<float> sidechainBuffer;
};
