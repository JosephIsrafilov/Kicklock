#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include <JuceHeader.h>

#include "DynamicSelectorScheduler.h"

// =============================================================================
// Phase 6: standalone continuity mixer.
//
// Consumes the ten-position gain vector produced by DynamicSelectorScheduler
// and the read-only Phase-5 branch outputs, and produces one full-band
// low+high mix per sample. It owns no DSP state of its own beyond fixed
// diagnostics counters: no allocation, no locking, no resizing after prepare.
// =============================================================================

// Fixed, read-only bundle of one block's Phase-5 output buffers. All eight
// State pointers must be non-null (Phase 5 always allocates all eight
// per-slot output buffers, even when a slot is inactive and reads zero).
struct DynamicContinuityMixerInputs
{
    const juce::AudioBuffer<float>* globalLow = nullptr;
    std::array<const juce::AudioBuffer<float>*, DynamicSelectorContract::kStateSlotCount> stateLow {};
    const juce::AudioBuffer<float>* serviceLow = nullptr;
    const juce::AudioBuffer<float>* commonHigh = nullptr;
};

class DynamicContinuityMixer
{
public:
    bool prepare (int channels) noexcept
    {
        prepared = false;
        if (channels < 1 || channels > DynamicHotBranchContract::kMaxChannels)
            return false;
        numChannels = channels;
        nonFiniteBranchSampleCount = 0;
        nonFiniteHighSampleCount = 0;
        nonFiniteOutputSampleCount = 0;
        prepared = true;
        return true;
    }

    void reset() noexcept
    {
        nonFiniteBranchSampleCount = 0;
        nonFiniteHighSampleCount = 0;
        nonFiniteOutputSampleCount = 0;
    }

    bool isPrepared() const noexcept { return prepared; }
    int getChannelCount() const noexcept { return numChannels; }
    uint64_t getNonFiniteBranchSampleCount() const noexcept { return nonFiniteBranchSampleCount; }
    uint64_t getNonFiniteHighSampleCount() const noexcept { return nonFiniteHighSampleCount; }
    uint64_t getNonFiniteOutputSampleCount() const noexcept { return nonFiniteOutputSampleCount; }

    // Renders exactly `numSamples` samples starting at the scheduler's expected
    // next absolute sample. Advances the scheduler one sample at a time so
    // event application, mid-fade retargeting, gain calculation, low-band
    // mixing and the single common-high addition all happen in the frozen
    // per-sample order (see DYNAMIC_DESIGN_FREEZE.md). Never resizes buffers.
    bool renderBlock (DynamicSelectorScheduler& scheduler,
                      const DynamicSelectorBranchRoster& roster,
                      const DynamicContinuityMixerInputs& inputs,
                      int numSamples,
                      juce::AudioBuffer<float>& output) noexcept
    {
        using namespace DynamicSelectorContract;

        if (! prepared || ! scheduler.isPrepared() || numSamples <= 0)
            return false;
        if (inputs.globalLow == nullptr || inputs.serviceLow == nullptr || inputs.commonHigh == nullptr)
            return false;
        for (const auto* statePtr : inputs.stateLow)
            if (statePtr == nullptr)
                return false;

        if (! hasBufferCapacity (*inputs.globalLow, numSamples)
            || ! hasBufferCapacity (*inputs.serviceLow, numSamples)
            || ! hasBufferCapacity (*inputs.commonHigh, numSamples)
            || ! hasBufferCapacity (output, numSamples))
            return false;
        for (const auto* statePtr : inputs.stateLow)
            if (! hasBufferCapacity (*statePtr, numSamples))
                return false;

        const int64_t blockStartSample = scheduler.getExpectedNextSample();

        for (int i = 0; i < numSamples; ++i)
        {
            if (! scheduler.advanceSample (blockStartSample + i, roster))
                return false;

            const auto& gains = scheduler.getCurrentGains();

            for (int ch = 0; ch < numChannels; ++ch)
            {
                float lowSum = 0.0f;
                lowSum += gains[(size_t) kGlobalBranchIndex]
                    * sanitizeBranchSample (inputs.globalLow->getSample (ch, i));

                for (int slot = 0; slot < kStateSlotCount; ++slot)
                {
                    const float gain = gains[(size_t) (kFirstStateBranchIndex + slot)];
                    if (gain <= 0.0f)
                        continue;
                    lowSum += gain * sanitizeBranchSample (inputs.stateLow[(size_t) slot]->getSample (ch, i));
                }

                lowSum += gains[(size_t) kServiceBranchIndex]
                    * sanitizeBranchSample (inputs.serviceLow->getSample (ch, i));

                const float highSample = sanitizeHighSample (inputs.commonHigh->getSample (ch, i));
                float full = lowSum + highSample;
                if (! std::isfinite (full))
                {
                    full = 0.0f;
                    ++nonFiniteOutputSampleCount;
                }
                output.setSample (ch, i, full);
            }
        }

        return true;
    }

private:
    bool hasBufferCapacity (const juce::AudioBuffer<float>& buffer, int numSamples) const noexcept
    {
        return buffer.getNumChannels() >= numChannels && buffer.getNumSamples() >= numSamples;
    }

    float sanitizeBranchSample (float value) noexcept
    {
        if (std::isfinite (value))
            return value;
        ++nonFiniteBranchSampleCount;
        return 0.0f;
    }

    float sanitizeHighSample (float value) noexcept
    {
        if (std::isfinite (value))
            return value;
        ++nonFiniteHighSampleCount;
        return 0.0f;
    }

    bool prepared = false;
    int numChannels = 0;
    uint64_t nonFiniteBranchSampleCount = 0;
    uint64_t nonFiniteHighSampleCount = 0;
    uint64_t nonFiniteOutputSampleCount = 0;
};
