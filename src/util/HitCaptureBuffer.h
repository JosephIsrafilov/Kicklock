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

    int snapshotLatest (std::vector<float>& bassOut, std::vector<float>& kickOut) const
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
                return samples;
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

    std::vector<float> preBass;
    std::vector<float> preKick;
    std::vector<float> captureBass;
    std::vector<float> captureKick;
    std::array<std::vector<float>, 2> publishedBass;
    std::array<std::vector<float>, 2> publishedKick;
    std::array<int, 2> publishedSamples {};
    std::atomic<int> sequence { 0 };
    std::atomic<int> publishedSlot { 0 };
};
