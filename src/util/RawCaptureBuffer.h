#pragma once

#include <vector>
#include <atomic>
#include <algorithm>
#include <juce_audio_basics/juce_audio_basics.h>

// Rolling capture of raw (pre-processing) mono bass/kick samples for offline
// analysis by the Analyze button. The audio thread writes one pair per sample
// via push(); the UI/message thread calls snapshot() on demand.
//
// Lock-free and allocation-free on the audio thread (buffers sized once in
// prepare()). A few torn samples at the wrap boundary are harmless for a
// correlation estimate over tens of thousands of samples, so no double
// buffering is used.
class RawCaptureBuffer
{
public:
    void prepare (int capacitySamples)
    {
        capacity = juce::jmax (1, capacitySamples);
        bass.assign ((size_t) capacity, 0.0f);
        kick.assign ((size_t) capacity, 0.0f);
        writePos.store (0, std::memory_order_relaxed);
    }

    void reset()
    {
        std::fill (bass.begin(), bass.end(), 0.0f);
        std::fill (kick.begin(), kick.end(), 0.0f);
        writePos.store (0, std::memory_order_relaxed);
    }

    // Audio thread. Never allocates or blocks.
    void push (float bassValue, float kickValue) noexcept
    {
        const int cap = capacity;
        if (cap <= 0)
            return;

        int w = writePos.load (std::memory_order_relaxed);
        bass[(size_t) w] = bassValue;
        kick[(size_t) w] = kickValue;
        w = (w + 1) % cap;
        writePos.store (w, std::memory_order_relaxed);
    }

    int getCapacity() const noexcept { return capacity; }

    // UI/message thread. Copies the whole ring into the provided vectors in
    // chronological order (oldest first). Returns the sample count.
    int snapshot (std::vector<float>& bassOut, std::vector<float>& kickOut) const
    {
        const int cap = capacity;
        if (cap <= 0)
            return 0;

        bassOut.resize ((size_t) cap);
        kickOut.resize ((size_t) cap);

        // writePos holds the slot about to be overwritten == the oldest sample,
        // so reading cap samples from there yields chronological order.
        const int w = writePos.load (std::memory_order_relaxed);
        for (int i = 0; i < cap; ++i)
        {
            const int idx = (w + i) % cap;
            bassOut[(size_t) i] = bass[(size_t) idx];
            kickOut[(size_t) i] = kick[(size_t) idx];
        }
        return cap;
    }

private:
    int capacity = 0;
    std::vector<float> bass, kick;
    std::atomic<int> writePos { 0 };
};
