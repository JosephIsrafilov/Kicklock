#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <vector>
#include <juce_audio_basics/juce_audio_basics.h>

class HitCaptureBuffer
{
public:
    // Flag carried by the sweep stream on the FIRST sample of each capture
    // window (i.e. the first pre-roll sample of a fresh hit), so the UI can
    // restart its left-to-right sweep exactly at hit boundaries.
    static constexpr unsigned char sweepStartFlag = 1;

    void prepare (double sampleRate, float preRollMs, float postRollMs)
    {
        preRollSamples = juce::jmax (1, (int) std::round (sampleRate * preRollMs / 1000.0));
        postRollSamples = juce::jmax (1, (int) std::round (sampleRate * postRollMs / 1000.0));
        windowSamples = preRollSamples + postRollSamples;

        preBass.assign ((size_t) preRollSamples, 0.0f);
        preKick.assign ((size_t) preRollSamples, 0.0f);
        captureBass.assign ((size_t) windowSamples, 0.0f);
        captureKick.assign ((size_t) windowSamples, 0.0f);
        for (auto& buffer : publishedBass)
            buffer.assign ((size_t) windowSamples, 0.0f);
        for (auto& buffer : publishedKick)
            buffer.assign ((size_t) windowSamples, 0.0f);

        // Sweep stream: two windows of slack so one UI hiccup never drops a
        // whole hit. Sized here (never on the audio thread).
        const int streamCapacity = juce::jmax (64, windowSamples * 2);
        streamFifo.setTotalSize (streamCapacity);
        streamBass.assign ((size_t) streamCapacity, 0.0f);
        streamKick.assign ((size_t) streamCapacity, 0.0f);
        streamFlags.assign ((size_t) streamCapacity, 0);

        reset();
    }

    void reset() noexcept
    {
        std::fill (preBass.begin(), preBass.end(), 0.0f);
        std::fill (preKick.begin(), preKick.end(), 0.0f);
        std::fill (captureBass.begin(), captureBass.end(), 0.0f);
        std::fill (captureKick.begin(), captureKick.end(), 0.0f);
        for (auto& buffer : publishedBass)
            std::fill (buffer.begin(), buffer.end(), 0.0f);
        for (auto& buffer : publishedKick)
            std::fill (buffer.begin(), buffer.end(), 0.0f);
        publishedSamples = {};

        preWriteIndex = 0;
        captureIndex = 0;
        capturing = false;
        streamWindowDropped = false;
        streamFifo.reset();
        sequence.store (0, std::memory_order_relaxed);
        publishedSlot.store (0, std::memory_order_release);
    }

    void pushSample (float bassValue, float kickValue, bool transientDetected) noexcept
    {
        if (windowSamples <= 0 || preRollSamples <= 0)
            return;

        if (transientDetected && ! capturing)
            startCapture();

        if (capturing)
        {
            captureBass[(size_t) captureIndex] = bassValue;
            captureKick[(size_t) captureIndex] = kickValue;
            ++captureIndex;

            pushStreamSample (bassValue, kickValue, 0);

            if (captureIndex >= windowSamples)
                finishCapture();
        }

        preBass[(size_t) preWriteIndex] = bassValue;
        preKick[(size_t) preWriteIndex] = kickValue;
        preWriteIndex = (preWriteIndex + 1) % preRollSamples;
    }

    int getSequence() const noexcept
    {
        return sequence.load (std::memory_order_acquire);
    }

    int getWindowSamples() const noexcept { return windowSamples; }
    int getPreRollSamples() const noexcept { return preRollSamples; }

    // Message/UI thread. Drains the progressive sweep stream: the SAME hit
    // windows the snapshot API publishes, but delivered sample-by-sample while
    // the window is still being captured, so the UI can draw a ReVision-style
    // left-to-right sweep in real time. Each window begins with sweepStartFlag
    // on its first (pre-roll) sample. If the stream ever overflows, the rest of
    // that window is dropped wholesale and the next hit starts a fresh window,
    // so the consumer only ever sees contiguous runs — never a torn middle.
    // Returns the number of sample triples actually read.
    int readSweepStream (float* bassOut, float* kickOut, unsigned char* flagsOut, int maxSamples) noexcept
    {
        int start1, size1, start2, size2;
        streamFifo.prepareToRead (maxSamples, start1, size1, start2, size2);

        int count = 0;

        auto copyRange = [&] (int start, int size)
        {
            for (int i = 0; i < size; ++i)
            {
                bassOut[count]  = streamBass[(size_t) (start + i)];
                kickOut[count]  = streamKick[(size_t) (start + i)];
                flagsOut[count] = streamFlags[(size_t) (start + i)];
                ++count;
            }
        };

        copyRange (start1, size1);
        copyRange (start2, size2);

        streamFifo.finishedRead (size1 + size2);
        return count;
    }

    int snapshotLatest (std::vector<float>& bassOut, std::vector<float>& kickOut, int* copiedSequence = nullptr) const
    {
        for (int attempt = 0; attempt < 3; ++attempt)
        {
            const int sequenceBefore = sequence.load (std::memory_order_acquire);
            const int slot = publishedSlot.load (std::memory_order_acquire);
            const int samples = publishedSamples[(size_t) slot];
            if (samples <= 0)
                return 0;

            bassOut.resize ((size_t) samples);
            kickOut.resize ((size_t) samples);

            for (int i = 0; i < samples; ++i)
            {
                bassOut[(size_t) i] = publishedBass[(size_t) slot][(size_t) i];
                kickOut[(size_t) i] = publishedKick[(size_t) slot][(size_t) i];
            }

            if (sequenceBefore == sequence.load (std::memory_order_acquire))
            {
                if (copiedSequence != nullptr)
                    *copiedSequence = sequenceBefore;
                return samples;
            }
        }

        return 0;
    }

private:
    void startCapture() noexcept
    {
        for (int i = 0; i < preRollSamples; ++i)
        {
            const int idx = (preWriteIndex + i) % preRollSamples;
            captureBass[(size_t) i] = preBass[(size_t) idx];
            captureKick[(size_t) i] = preKick[(size_t) idx];
        }

        captureIndex = preRollSamples;
        capturing = true;

        // A fresh hit always gets a fresh chance at the stream, even if the
        // previous window overflowed. The pre-roll rides along up front so the
        // UI sweep covers the full window from its very first drain.
        streamWindowDropped = false;
        for (int i = 0; i < preRollSamples; ++i)
            pushStreamSample (captureBass[(size_t) i], captureKick[(size_t) i],
                              i == 0 ? sweepStartFlag : 0);
    }

    // Audio thread. Drops the REST of the current window on overflow (instead
    // of dropping arbitrary middle samples) so the UI never assembles a sweep
    // with a hidden gap in it; the next hit resets and streams normally.
    void pushStreamSample (float bassValue, float kickValue, unsigned char flags) noexcept
    {
        if (streamWindowDropped)
            return;

        int start1, size1, start2, size2;
        streamFifo.prepareToWrite (1, start1, size1, start2, size2);

        if (size1 + size2 == 0)
        {
            streamWindowDropped = true;
            return;
        }

        const int index = size1 > 0 ? start1 : start2;
        streamBass[(size_t) index] = bassValue;
        streamKick[(size_t) index] = kickValue;
        streamFlags[(size_t) index] = flags;

        streamFifo.finishedWrite (size1 + size2);
    }

    void finishCapture() noexcept
    {
        const int slot = 1 - publishedSlot.load (std::memory_order_acquire);

        for (int i = 0; i < windowSamples; ++i)
        {
            publishedBass[(size_t) slot][(size_t) i] = captureBass[(size_t) i];
            publishedKick[(size_t) slot][(size_t) i] = captureKick[(size_t) i];
        }

        publishedSamples[(size_t) slot] = windowSamples;
        publishedSlot.store (slot, std::memory_order_release);
        sequence.fetch_add (1, std::memory_order_acq_rel);

        captureIndex = 0;
        capturing = false;
    }

    int preRollSamples = 0;
    int postRollSamples = 0;
    int windowSamples = 0;
    int preWriteIndex = 0;
    int captureIndex = 0;
    bool capturing = false;
    bool streamWindowDropped = false;   // audio-thread-only overflow latch

    std::vector<float> preBass;
    std::vector<float> preKick;
    std::vector<float> captureBass;
    std::vector<float> captureKick;
    std::array<std::vector<float>, 2> publishedBass;
    std::array<std::vector<float>, 2> publishedKick;
    std::array<int, 2> publishedSamples {};
    std::atomic<int> sequence { 0 };
    std::atomic<int> publishedSlot { 0 };

    // Progressive sweep stream (single audio-thread producer, single UI-thread
    // consumer). Sized in prepare(); no allocation on the audio thread.
    juce::AbstractFifo streamFifo { 1 };
    std::vector<float> streamBass;
    std::vector<float> streamKick;
    std::vector<unsigned char> streamFlags;
};
