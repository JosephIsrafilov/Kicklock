#pragma once

#include <array>
#include <vector>
#include <atomic>
#include <algorithm>
#include <juce_audio_basics/juce_audio_basics.h>

// Rolling capture of raw (pre-processing) mono bass/kick samples for offline
// analysis by the Analyze button. The audio thread writes one pair per sample
// via push(); the UI/message thread calls snapshot() on demand.
//
// Lock-free and allocation-free on the audio thread (buffers sized once in
// prepare()). The audio thread publishes a copied snapshot into one of two
// buffers; the UI reads only the currently published buffer, never the live
// capture ring.
class RawCaptureBuffer
{
public:
    void prepare (int capacitySamples)
    {
        capacity = juce::jmax (1, capacitySamples);
        captureBass.assign ((size_t) capacity, 0.0f);
        captureKick.assign ((size_t) capacity, 0.0f);
        for (auto& buffer : publishedBass)
            buffer.assign ((size_t) capacity, 0.0f);
        for (auto& buffer : publishedKick)
            buffer.assign ((size_t) capacity, 0.0f);
        publishedWritePos = {};
        publishedFilledSamples = {};
        filledSamples = 0;
        writePos.store (0, std::memory_order_relaxed);
        publishedSlot.store (0, std::memory_order_release);
    }

    void reset()
    {
        std::fill (captureBass.begin(), captureBass.end(), 0.0f);
        std::fill (captureKick.begin(), captureKick.end(), 0.0f);
        for (auto& buffer : publishedBass)
            std::fill (buffer.begin(), buffer.end(), 0.0f);
        for (auto& buffer : publishedKick)
            std::fill (buffer.begin(), buffer.end(), 0.0f);
        publishedWritePos = {};
        publishedFilledSamples = {};
        filledSamples = 0;
        writePos.store (0, std::memory_order_relaxed);
        publishedSlot.store (0, std::memory_order_release);
    }

    // Audio thread. Never allocates or blocks.
    void push (float bassValue, float kickValue) noexcept
    {
        const int cap = capacity;
        if (cap <= 0)
            return;

        int w = writePos.load (std::memory_order_relaxed);
        captureBass[(size_t) w] = bassValue;
        captureKick[(size_t) w] = kickValue;
        w = (w + 1) % cap;
        writePos.store (w, std::memory_order_relaxed);
        filledSamples = std::min (filledSamples + 1, cap);
    }

    // Audio thread. Copies the current capture ring into the inactive
    // published slot once per processed block.
    void publishSnapshot() noexcept
    {
        const int cap = capacity;
        if (cap <= 0)
            return;

        const int slot = 1 - publishedSlot.load (std::memory_order_acquire);
        std::copy (captureBass.begin(), captureBass.end(), publishedBass[(size_t) slot].begin());
        std::copy (captureKick.begin(), captureKick.end(), publishedKick[(size_t) slot].begin());
        publishedWritePos[(size_t) slot] = writePos.load (std::memory_order_relaxed);
        publishedFilledSamples[(size_t) slot] = filledSamples;
        publishedSlot.store (slot, std::memory_order_release);
    }

    int getCapacity() const noexcept { return capacity; }

    // UI/message thread. Copies the latest published ring into the provided
    // vectors in chronological order (oldest first). Returns the sample count.
    int snapshot (std::vector<float>& bassOut, std::vector<float>& kickOut) const
    {
        const int slot = publishedSlot.load (std::memory_order_acquire);
        const int samples = publishedFilledSamples[(size_t) slot];
        if (samples <= 0)
            return 0;

        bassOut.resize ((size_t) samples);
        kickOut.resize ((size_t) samples);

        const int cap = capacity;
        const int start = samples < cap ? 0 : publishedWritePos[(size_t) slot];
        for (int i = 0; i < samples; ++i)
        {
            const int idx = (start + i) % cap;
            bassOut[(size_t) i] = publishedBass[(size_t) slot][(size_t) idx];
            kickOut[(size_t) i] = publishedKick[(size_t) slot][(size_t) idx];
        }
        return samples;
    }

private:
    int capacity = 0;
    int filledSamples = 0;
    std::vector<float> captureBass, captureKick;
    std::array<std::vector<float>, 2> publishedBass;
    std::array<std::vector<float>, 2> publishedKick;
    std::array<int, 2> publishedWritePos {};
    std::array<int, 2> publishedFilledSamples {};
    std::atomic<int> writePos { 0 };
    std::atomic<int> publishedSlot { 0 };
};
