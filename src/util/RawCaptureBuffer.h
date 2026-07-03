#pragma once

#include <array>
#include <vector>
#include <atomic>
#include <algorithm>
#include <juce_audio_basics/juce_audio_basics.h>

// Rolling capture of raw (pre-processing) mono bass/kick samples for offline
// analysis by the Analyze button. The audio thread writes one pair per sample
// via push(); a message/background thread calls snapshot() on demand.
//
// Realtime-safe by construction: the audio thread only ever does O(1) writes
// into a preallocated ring (buffers sized once in prepare()). There is NO
// per-block full-buffer copy and no lock. snapshot() reads the live ring
// directly from the reader thread. A handful of torn samples at the wrap
// boundary are acceptable for a correlation estimate over tens of thousands of
// samples, so no double buffering is used (that would just move an expensive
// copy back onto the audio thread, which is exactly what we are removing here).
class RawCaptureBuffer
{
public:
    void prepare (int capacitySamples)
    {
        capacity = juce::jmax (1, capacitySamples);
        captureBass.assign ((size_t) capacity, 0.0f);
        captureKick.assign ((size_t) capacity, 0.0f);
        writePos.store (0, std::memory_order_relaxed);
        filledSamples.store (0, std::memory_order_release);
    }

    void reset()
    {
        std::fill (captureBass.begin(), captureBass.end(), 0.0f);
        std::fill (captureKick.begin(), captureKick.end(), 0.0f);
        writePos.store (0, std::memory_order_relaxed);
        filledSamples.store (0, std::memory_order_release);
    }

    // Audio thread. O(1), never allocates, never locks.
    void push (float bassValue, float kickValue) noexcept
    {
        const int cap = capacity;
        if (cap <= 0)
            return;

        int w = writePos.load (std::memory_order_relaxed);
        captureBass[(size_t) w] = bassValue;
        captureKick[(size_t) w] = kickValue;
        w = (w + 1) % cap;

        // Publish the new write position and fill count with release semantics
        // so a reader that acquires them sees the sample stores above.
        writePos.store (w, std::memory_order_release);

        const int filled = filledSamples.load (std::memory_order_relaxed);
        if (filled < cap)
            filledSamples.store (filled + 1, std::memory_order_release);
    }

    int getCapacity() const noexcept { return capacity; }

    // Number of valid samples captured so far (saturates at capacity). Used by
    // the "enough material captured" status check. Reader thread.
    int getFilledSamples() const noexcept
    {
        return filledSamples.load (std::memory_order_acquire);
    }

    // UI/message/background thread. Copies the live ring into the provided
    // vectors in chronological order (oldest first). Returns the sample count.
    // Only the samples actually written so far are returned, so an early
    // snapshot (before the ring is full) never reports capacity-length zero
    // padding as if it were captured material.
    int snapshot (std::vector<float>& bassOut, std::vector<float>& kickOut) const
    {
        const int cap = capacity;
        if (cap <= 0)
            return 0;

        const int samples = filledSamples.load (std::memory_order_acquire);
        if (samples <= 0)
            return 0;

        const int w = writePos.load (std::memory_order_acquire);

        bassOut.resize ((size_t) samples);
        kickOut.resize ((size_t) samples);

        // Oldest sample sits `samples` positions behind the write head once the
        // ring is full; before that it starts at index 0.
        const int start = samples < cap ? 0 : w;
        for (int i = 0; i < samples; ++i)
        {
            const int idx = (start + i) % cap;
            bassOut[(size_t) i] = captureBass[(size_t) idx];
            kickOut[(size_t) i] = captureKick[(size_t) idx];
        }

        return samples;
    }

private:
    int capacity = 0;
    std::vector<float> captureBass, captureKick;
    std::atomic<int> writePos { 0 };
    std::atomic<int> filledSamples { 0 };
};
