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
        if (prepared)
        {
            // The UI worker may be reading while a host re-prepares. Keep this
            // preallocated SPSC storage stable and let the consumer discard the
            // old generation instead of racing a vector reallocation/reset.
            generation.fetch_add (1, std::memory_order_release);
            return;
        }

        fifo.setTotalSize (capacity);
        mainL.assign ((size_t) capacity, 0.0f);
        mainR.assign ((size_t) capacity, 0.0f);
        sideL.assign ((size_t) capacity, 0.0f);
        sideR.assign ((size_t) capacity, 0.0f);
        mainChannels.store (0, std::memory_order_release);
        sideChannels.store (0, std::memory_order_release);
        reset();
        prepared = true;
        generation.fetch_add (1, std::memory_order_release);
    }

    void reset()
    {
        fifo.reset();
    }

    void pushSample (float mL, float mR, float sL, float sR, int mainCh, int sideCh) noexcept
    {
        int start1, size1, start2, size2;
        fifo.prepareToWrite (1, start1, size1, start2, size2);

        if (size1 + size2 == 0)
        {
            droppedSamples.fetch_add (1, std::memory_order_relaxed);
            return;
        }

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

    // Consumer-only. Drop the oldest queued samples and retain one bounded
    // newest window so FFT work never turns into a growing display backlog.
    int readLatest (float* mL_out, float* mR_out, float* sL_out, float* sR_out, int maxSamples)
    {
        const int available = fifo.getNumReady();
        const int keep = juce::jmin (juce::jmax (0, maxSamples), available);
        const int discard = available - keep;
        if (discard > 0)
        {
            int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
            fifo.prepareToRead (discard, start1, size1, start2, size2);
            const int consumed = size1 + size2;
            fifo.finishedRead (consumed);
            droppedSamples.fetch_add (consumed, std::memory_order_relaxed);
        }
        return readAvailable (mL_out, mR_out, sL_out, sR_out, keep);
    }

    int getMainChannels() const { return mainChannels.load (std::memory_order_acquire); }
    int getSideChannels() const { return sideChannels.load (std::memory_order_acquire); }
    int getDroppedSampleCount() const noexcept { return droppedSamples.load (std::memory_order_relaxed); }
    uint64_t getGeneration() const noexcept { return generation.load (std::memory_order_acquire); }

private:
    juce::AbstractFifo fifo { 1 };
    std::vector<float> mainL;
    std::vector<float> mainR;
    std::vector<float> sideL;
    std::vector<float> sideR;
    std::atomic<int> mainChannels { 2 };
    std::atomic<int> sideChannels { 2 };
    std::atomic<int> droppedSamples { 0 };
    std::atomic<uint64_t> generation { 0 };
    bool prepared = false;
};
