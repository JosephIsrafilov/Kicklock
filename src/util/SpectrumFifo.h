#pragma once

#include <atomic>
#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>

class SpectrumFifo
{
public:
    void prepare (int capacitySamples)
    {
        const int capacity = juce::jmax (1, capacitySamples);
        fifo.setTotalSize (capacity);
        mainL.assign ((size_t) capacity, 0.0f);
        mainR.assign ((size_t) capacity, 0.0f);
        sideL.assign ((size_t) capacity, 0.0f);
        sideR.assign ((size_t) capacity, 0.0f);
        mainChannels.store (0, std::memory_order_release);
        sideChannels.store (0, std::memory_order_release);
        reset();
    }

    void reset()
    {
        fifo.reset();
    }

    void pushSample (float mL, float mR, float sL, float sR, int mainCh, int sideCh)
    {
        int start1, size1, start2, size2;
        fifo.prepareToWrite (1, start1, size1, start2, size2);

        if (size1 + size2 == 0)
            return;

        const int index = size1 > 0 ? start1 : start2;
        mainL[(size_t) index] = mL;
        mainR[(size_t) index] = mR;
        sideL[(size_t) index] = sL;
        sideR[(size_t) index] = sR;

        mainChannels.store (juce::jlimit (0, 2, mainCh), std::memory_order_release);
        sideChannels.store (juce::jlimit (0, 2, sideCh), std::memory_order_release);

        fifo.finishedWrite (size1 + size2);
    }

    int readAvailable (float* mL_out, float* mR_out, float* sL_out, float* sR_out, int maxSamples)
    {
        int start1, size1, start2, size2;
        fifo.prepareToRead (maxSamples, start1, size1, start2, size2);

        int count = 0;

        auto copyBlock = [&](int start, int size) {
            for (int i = 0; i < size; ++i)
            {
                mL_out[count] = mainL[(size_t) (start + i)];
                mR_out[count] = mainR[(size_t) (start + i)];
                sL_out[count] = sideL[(size_t) (start + i)];
                sR_out[count] = sideR[(size_t) (start + i)];
                ++count;
            }
        };

        copyBlock(start1, size1);
        copyBlock(start2, size2);

        fifo.finishedRead (size1 + size2);

        return count;
    }

    int getMainChannels() const { return mainChannels.load (std::memory_order_acquire); }
    int getSideChannels() const { return sideChannels.load (std::memory_order_acquire); }

private:
    juce::AbstractFifo fifo { 1 };
    std::vector<float> mainL;
    std::vector<float> mainR;
    std::vector<float> sideL;
    std::vector<float> sideR;
    std::atomic<int> mainChannels { 2 };
    std::atomic<int> sideChannels { 2 };
};
