#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include <JuceHeader.h>

#include "DynamicPackageMorpher.h"
#include "LinkwitzRileyCrossover.h"

// =============================================================================
// Phase 5: shared multi-tap hot-branch engine.
//
// One Global crossover, one shared low-band history, one common high-band PDC
// path, and permanently hot Global / eight State / optional Service low-band
// branches. Phase 4 packages supply delay taps and allpass coefficients.
// Phase 5 does not select, Hold, fade, or connect to PluginProcessor.
// =============================================================================

namespace DynamicHotBranchContract
{
    inline constexpr int kMaxChannels = 2;
    inline constexpr int kMaxAllpassStages = 4;
    inline constexpr int kStateSlots = DynamicStateMapContract::kMaxPersistentStates;
    inline constexpr float kReportedLatencyMs = DynamicStateMapContract::kReportedLatencyMs;
    inline constexpr float kMinimumPhysicalTapMs = 13.0f; // 20 + (-4) + (-3)
    inline constexpr float kMaximumPhysicalTapMs = 40.0f; // 20 + 17 + 3
    inline constexpr double kServiceWarmupMs = 300.0;
    inline constexpr double kSharedHistoryMs = 340.0; // 300 warmup + 40 max tap
    inline constexpr int kInterpolationGuardSamples = 8;
    inline constexpr double kDenormalThreshold = 1.0e-20;
}

enum class DynamicHotBranchKind : uint8_t
{
    Global = 0,
    State,
    Service
};

struct DynamicHotBranchConfig
{
    bool active = false;
    uint64_t stableStateId = 0;
    double effectiveAbsoluteDelayMs = 0.0;
    DynamicAllpassCoefficients coefficients;
    bool polarityInvert = false;
    int allpassStages = 2;
    bool crossoverEnabled = true;
    double crossoverHz = 150.0;
    bool allpassEnabled = true;
    int delayInterpolationIndex = 0;
};

struct DynamicHotBranchInfo
{
    bool active = false;
    DynamicHotBranchKind kind = DynamicHotBranchKind::Global;
    uint64_t stableStateId = 0;
    double effectiveAbsoluteDelayMs = 0.0;
    double physicalTapSamples = 0.0;
    bool coefficientsValid = false;
    bool warm = false;
};

struct DynamicServicePrimeResult
{
    bool valid = false;
    int requestedSamples = 0;
    int availableSamples = 0;
    int primedSamples = 0;
    bool fullyPrimed = false;
};

struct DynamicHotBranchProcessResult
{
    bool valid = false;
    int samplesProcessed = 0;
    uint64_t nonFiniteInputCount = 0;
};

namespace DynamicHotBranchDetail
{
    inline bool isFinite (double value) noexcept
    {
        return std::isfinite (value);
    }

    inline float sanitizeSample (float value, uint64_t& nonFiniteCount) noexcept
    {
        if (std::isfinite (value))
            return value;
        ++nonFiniteCount;
        return 0.0f;
    }

    inline float flushDenormal (float value) noexcept
    {
        return std::abs (value) < (float) DynamicHotBranchContract::kDenormalThreshold ? 0.0f : value;
    }

    inline double flushDenormal (double value) noexcept
    {
        return std::abs (value) < DynamicHotBranchContract::kDenormalThreshold ? 0.0 : value;
    }

    inline bool configsEqual (const DynamicHotBranchConfig& a, const DynamicHotBranchConfig& b) noexcept
    {
        return a.active == b.active
            && a.stableStateId == b.stableStateId
            && a.effectiveAbsoluteDelayMs == b.effectiveAbsoluteDelayMs
            && a.coefficients.b0 == b.coefficients.b0
            && a.coefficients.b1 == b.coefficients.b1
            && a.coefficients.b2 == b.coefficients.b2
            && a.coefficients.a1 == b.coefficients.a1
            && a.coefficients.a2 == b.coefficients.a2
            && a.polarityInvert == b.polarityInvert
            && a.allpassStages == b.allpassStages
            && a.crossoverEnabled == b.crossoverEnabled
            && a.crossoverHz == b.crossoverHz
            && a.allpassEnabled == b.allpassEnabled
            && a.delayInterpolationIndex == b.delayInterpolationIndex;
    }

    inline bool globalFieldsMatch (const DynamicHotBranchConfig& global,
                                   const DynamicHotBranchConfig& other) noexcept
    {
        return global.polarityInvert == other.polarityInvert
            && global.allpassStages == other.allpassStages
            && global.crossoverEnabled == other.crossoverEnabled
            && global.crossoverHz == other.crossoverHz
            && global.allpassEnabled == other.allpassEnabled
            && global.delayInterpolationIndex == other.delayInterpolationIndex;
    }

    inline bool isValidBranchConfig (const DynamicHotBranchConfig& config,
                                     DynamicHotBranchKind kind,
                                     double sampleRate) noexcept
    {
        using namespace DynamicHotBranchContract;
        if (! config.active)
            return true;
        if (! isFinite (config.effectiveAbsoluteDelayMs)
            || ! isFinite (config.crossoverHz)
            || ! DynamicAllpassPoleDomain::isValidCoefficients (config.coefficients)
            || config.allpassStages < 2 || config.allpassStages > kMaxAllpassStages
            || (config.delayInterpolationIndex != 0 && config.delayInterpolationIndex != 1)
            || config.crossoverHz < (double) DynamicStateMapContract::kCrossoverMinHz
            || config.crossoverHz > (double) DynamicStateMapContract::kCrossoverMaxHz)
            return false;

        if (kind == DynamicHotBranchKind::State && config.stableStateId == 0)
            return false;
        if (kind != DynamicHotBranchKind::State && config.stableStateId != 0)
            return false;

        const double physicalTap = std::ceil (sampleRate * (double) kReportedLatencyMs / 1000.0)
            + config.effectiveAbsoluteDelayMs * sampleRate / 1000.0;
        const double minTap = sampleRate * (double) kMinimumPhysicalTapMs / 1000.0;
        const double maxTap = sampleRate * (double) kMaximumPhysicalTapMs / 1000.0;
        // Neighbor sample for fractional interpolation requires capacity > maxTap.
        return isFinite (physicalTap) && physicalTap >= 0.0
            && physicalTap <= maxTap + 1.0e-9
            && physicalTap + 1.0e-9 >= minTap;
    }

    struct FractionalInterpolator
    {
        float xPrev = 0.0f;
        float yPrev = 0.0f;

        void reset() noexcept
        {
            xPrev = 0.0f;
            yPrev = 0.0f;
        }

        float process (float x0, float x1, float frac, int interpolationIndex) noexcept
        {
            if (interpolationIndex == 0)
                return x0 + frac * (x1 - x0);

            if (frac <= 1.0e-4f)
            {
                xPrev = x0;
                yPrev = x0;
                return x0;
            }

            const float eta = (1.0f - frac) / (1.0f + frac);
            const float y = eta * x0 + xPrev - eta * yPrev;
            xPrev = x0;
            yPrev = flushDenormal (y);
            return yPrev;
        }
    };

    struct AllpassStageState
    {
        double z1 = 0.0;
        double z2 = 0.0;

        void reset() noexcept
        {
            z1 = 0.0;
            z2 = 0.0;
        }

        double process (double x, const DynamicAllpassCoefficients& c) noexcept
        {
            const double y = c.b0 * x + z1;
            z1 = flushDenormal (c.b1 * x - c.a1 * y + z2);
            z2 = flushDenormal (c.b2 * x - c.a2 * y);
            return flushDenormal (y);
        }
    };

    // Warm once enough live frames exist for the physical tap and its
    // fractional neighbor sample (integerDelay + 1).
    inline int warmFramesRequired (double physicalTapSamples) noexcept
    {
        if (! isFinite (physicalTapSamples) || physicalTapSamples < 0.0)
            return 0;
        const int integerDelay = (int) physicalTapSamples;
        return integerDelay + 2;
    }

    struct BranchDspState
    {
        DynamicHotBranchConfig config;
        double physicalTapSamples = 0.0;
        int processedFrames = 0;
        int warmFramesNeeded = 0;
        bool warm = false;
        std::array<FractionalInterpolator, DynamicHotBranchContract::kMaxChannels> interpolators {};
        std::array<std::array<AllpassStageState, DynamicHotBranchContract::kMaxAllpassStages>,
                   DynamicHotBranchContract::kMaxChannels> stages {};

        void resetRuntime() noexcept
        {
            processedFrames = 0;
            warm = false;
            for (auto& interpolator : interpolators)
                interpolator.reset();
            for (auto& channel : stages)
                for (auto& stage : channel)
                    stage.reset();
        }

        void clear() noexcept
        {
            config = {};
            physicalTapSamples = 0.0;
            warmFramesNeeded = 0;
            resetRuntime();
        }

        void noteLiveFrame() noexcept
        {
            if (processedFrames < std::numeric_limits<int>::max())
                ++processedFrames;
            warm = processedFrames >= warmFramesNeeded;
        }

        void applyPrimedFrames (int frames, bool fullyPrimed) noexcept
        {
            if (frames < 0)
                frames = 0;
            processedFrames = frames;
            warm = fullyPrimed && processedFrames >= warmFramesNeeded;
        }
    };
}

class DynamicHotBranchEngine
{
public:
    bool prepare (double newSampleRate, int maxBlockSize, int channels) noexcept
    {
        using namespace DynamicHotBranchContract;
        prepared = false;
        if (! DynamicAllpassPoleDomain::isValidSampleRate (newSampleRate)
            || maxBlockSize <= 0
            || channels < 1 || channels > kMaxChannels)
            return false;

        const double latencySamplesD = std::ceil (newSampleRate * (double) kReportedLatencyMs / 1000.0);
        // Integer-ceil of 340 ms plus guard and one neighbor sample for fractional taps.
        const double historySamplesD = std::ceil (newSampleRate * kSharedHistoryMs / 1000.0)
            + (double) kInterpolationGuardSamples + 1.0;
        if (! DynamicHotBranchDetail::isFinite (latencySamplesD)
            || ! DynamicHotBranchDetail::isFinite (historySamplesD)
            || latencySamplesD < 0.0 || historySamplesD < latencySamplesD
            || historySamplesD > (double) std::numeric_limits<int>::max() - 16.0)
            return false;

        sampleRate = newSampleRate;
        maxBlock = maxBlockSize;
        numChannels = channels;
        reportedLatencySamples = (int) latencySamplesD;
        lowHistoryCapacity = (int) historySamplesD;
        highHistoryCapacity = reportedLatencySamples + kInterpolationGuardSamples;

        lowHistory.assign ((size_t) numChannels * (size_t) lowHistoryCapacity, 0.0f);
        highHistory.assign ((size_t) numChannels * (size_t) highHistoryCapacity, 0.0f);
        writeIndex = 0;
        highWriteIndex = 0;
        validLowSamples = 0;
        validHighSamples = 0;
        nonFiniteInputCount = 0;

        inputScratch.setSize (numChannels, maxBlock, false, true, true);
        lowScratch.setSize (numChannels, maxBlock, false, true, true);
        highScratch.setSize (numChannels, maxBlock, false, true, true);
        globalLowOut.setSize (numChannels, maxBlock, false, true, true);
        serviceLowOut.setSize (numChannels, maxBlock, false, true, true);
        highOut.setSize (numChannels, maxBlock, false, true, true);
        for (auto& stateOut : stateLowOut)
            stateOut.setSize (numChannels, maxBlock, false, true, true);

        juce::dsp::ProcessSpec spec {
            sampleRate,
            (juce::uint32) maxBlock,
            (juce::uint32) numChannels
        };
        crossover.prepare (spec);

        globalBranch.clear();
        serviceBranch.clear();
        for (auto& state : stateBranches)
            state.clear();
        globalConfigured = false;
        serviceConfigured = false;

        prepared = true;
        return true;
    }

    void reset() noexcept
    {
        if (! prepared)
            return;

        std::fill (lowHistory.begin(), lowHistory.end(), 0.0f);
        std::fill (highHistory.begin(), highHistory.end(), 0.0f);
        writeIndex = 0;
        highWriteIndex = 0;
        validLowSamples = 0;
        validHighSamples = 0;
        nonFiniteInputCount = 0;
        crossover.reset();
        globalBranch.resetRuntime();
        serviceBranch.resetRuntime();
        for (auto& state : stateBranches)
            state.resetRuntime();
        inputScratch.clear();
        lowScratch.clear();
        highScratch.clear();
        globalLowOut.clear();
        serviceLowOut.clear();
        highOut.clear();
        for (auto& stateOut : stateLowOut)
            stateOut.clear();
    }

    bool isPrepared() const noexcept { return prepared; }
    double getSampleRate() const noexcept { return sampleRate; }
    int getMaxBlockSize() const noexcept { return maxBlock; }
    int getChannelCount() const noexcept { return numChannels; }
    int getReportedLatencySamples() const noexcept { return reportedLatencySamples; }
    int getLowHistoryCapacity() const noexcept { return lowHistoryCapacity; }
    int getValidLowHistorySamples() const noexcept { return validLowSamples; }
    uint64_t getNonFiniteInputCount() const noexcept { return nonFiniteInputCount; }

    bool configureGlobal (const DynamicHotBranchConfig& config) noexcept
    {
        if (! prepared || ! DynamicHotBranchDetail::isValidBranchConfig (
                config, DynamicHotBranchKind::Global, sampleRate))
            return false;
        if (config.active && config.stableStateId != 0)
            return false;
        if (globalConfigured && DynamicHotBranchDetail::configsEqual (globalBranch.config, config))
            return true;

        const bool wasActive = globalBranch.config.active;
        const int previousInterp = globalBranch.config.delayInterpolationIndex;
        const bool fieldsChanged = globalConfigured
            && ! DynamicHotBranchDetail::globalFieldsMatch (globalBranch.config, config);

        globalBranch.config = config;
        globalBranch.physicalTapSamples = physicalTapSamplesFor (config.effectiveAbsoluteDelayMs);
        globalBranch.warmFramesNeeded = DynamicHotBranchDetail::warmFramesRequired (
            globalBranch.physicalTapSamples);
        if (! wasActive || ! config.active)
            globalBranch.resetRuntime();
        else
        {
            if (previousInterp != config.delayInterpolationIndex)
                for (auto& interpolator : globalBranch.interpolators)
                    interpolator.reset();
            globalBranch.warm = globalBranch.processedFrames >= globalBranch.warmFramesNeeded;
        }

        // Global is the single source of truth for Global-only fields. Propagate
        // atomically to every active State and Service without clearing identity.
        if (fieldsChanged || config.active)
            propagateGlobalOnlyFields (config);

        globalConfigured = true;
        if (config.active)
            applyCrossoverSettings (config);
        return true;
    }

    bool configureStateSlot (int slot, const DynamicHotBranchConfig& config) noexcept
    {
        using namespace DynamicHotBranchContract;
        if (! prepared || slot < 0 || slot >= kStateSlots || ! globalConfigured
            || ! DynamicHotBranchDetail::isValidBranchConfig (config, DynamicHotBranchKind::State, sampleRate))
            return false;
        if (config.active)
        {
            for (int i = 0; i < kStateSlots; ++i)
                if (i != slot && stateBranches[(size_t) i].config.active
                    && stateBranches[(size_t) i].config.stableStateId == config.stableStateId)
                    return false;
        }

        // Normalize to live Global-only fields before equality / storage.
        DynamicHotBranchConfig normalized = config;
        copyGlobalOnlyFields (normalized, globalBranch.config);

        auto& branch = stateBranches[(size_t) slot];
        if (DynamicHotBranchDetail::configsEqual (branch.config, normalized))
            return true;

        const bool identityChanged = branch.config.stableStateId != normalized.stableStateId
            || branch.config.active != normalized.active;
        const int previousInterp = branch.config.delayInterpolationIndex;
        branch.config = normalized;
        branch.physicalTapSamples = normalized.active
            ? physicalTapSamplesFor (normalized.effectiveAbsoluteDelayMs) : 0.0;
        branch.warmFramesNeeded = normalized.active
            ? DynamicHotBranchDetail::warmFramesRequired (branch.physicalTapSamples) : 0;
        if (identityChanged || ! normalized.active)
            branch.resetRuntime();
        else
        {
            if (previousInterp != branch.config.delayInterpolationIndex)
                for (auto& interpolator : branch.interpolators)
                    interpolator.reset();
            branch.warm = branch.processedFrames >= branch.warmFramesNeeded;
        }
        return true;
    }

    void clearStateSlot (int slot) noexcept
    {
        if (! prepared || slot < 0 || slot >= DynamicHotBranchContract::kStateSlots)
            return;
        stateBranches[(size_t) slot].clear();
    }

    bool configureService (const DynamicHotBranchConfig& config) noexcept
    {
        if (! prepared || ! globalConfigured
            || ! DynamicHotBranchDetail::isValidBranchConfig (config, DynamicHotBranchKind::Service, sampleRate))
            return false;

        DynamicHotBranchConfig normalized = config;
        normalized.stableStateId = 0;
        copyGlobalOnlyFields (normalized, globalBranch.config);

        if (serviceConfigured && DynamicHotBranchDetail::configsEqual (serviceBranch.config, normalized))
            return true;

        const bool wasActive = serviceBranch.config.active;
        const int previousInterp = serviceBranch.config.delayInterpolationIndex;
        serviceBranch.config = normalized;
        serviceBranch.physicalTapSamples = normalized.active
            ? physicalTapSamplesFor (normalized.effectiveAbsoluteDelayMs) : 0.0;
        serviceBranch.warmFramesNeeded = normalized.active
            ? DynamicHotBranchDetail::warmFramesRequired (serviceBranch.physicalTapSamples) : 0;
        if (! wasActive || ! normalized.active)
            serviceBranch.resetRuntime();
        else
        {
            if (previousInterp != serviceBranch.config.delayInterpolationIndex)
                for (auto& interpolator : serviceBranch.interpolators)
                    interpolator.reset();
            serviceBranch.warm = serviceBranch.processedFrames >= serviceBranch.warmFramesNeeded;
        }
        serviceConfigured = true;
        return true;
    }

    void clearService() noexcept
    {
        serviceBranch.clear();
        serviceConfigured = false;
    }

    bool configureFromPackage (DynamicHotBranchKind kind,
                               int stateSlot,
                               const DynamicPackageResolution& package) noexcept
    {
        DynamicHotBranchConfig config;
        if (! package.valid)
            return false;

        config.polarityInvert = package.polarityInvert;
        config.allpassStages = package.allpassStages;
        config.crossoverEnabled = package.crossoverEnabled;
        config.crossoverHz = package.crossoverHz;
        config.allpassEnabled = package.allpassEnabled;
        config.delayInterpolationIndex = package.delayInterpolationIndex;
        config.effectiveAbsoluteDelayMs = package.effectiveAbsoluteDelayMs;
        config.coefficients = package.allpassCoefficients;

        if (kind == DynamicHotBranchKind::Global)
        {
            config.active = true;
            config.stableStateId = 0;
            return configureGlobal (config);
        }

        if (kind == DynamicHotBranchKind::Service)
        {
            config.active = true;
            config.stableStateId = 0;
            return configureService (config);
        }

        if (package.decision != DynamicPackageDecision::StateCorrection
            || package.selectedStableStateId == 0)
            return false;
        config.active = true;
        config.stableStateId = package.selectedStableStateId;
        return configureStateSlot (stateSlot, config);
    }

    DynamicHotBranchInfo getGlobalInfo() const noexcept
    {
        return makeInfo (globalBranch, DynamicHotBranchKind::Global);
    }

    DynamicHotBranchInfo getStateInfo (int slot) const noexcept
    {
        if (slot < 0 || slot >= DynamicHotBranchContract::kStateSlots)
            return {};
        return makeInfo (stateBranches[(size_t) slot], DynamicHotBranchKind::State);
    }

    DynamicHotBranchInfo getServiceInfo() const noexcept
    {
        return makeInfo (serviceBranch, DynamicHotBranchKind::Service);
    }

    DynamicServicePrimeResult primeService (int requestedSamples) noexcept
    {
        DynamicServicePrimeResult result;
        if (! prepared || ! serviceBranch.config.active || requestedSamples < 0)
            return result;

        const int maxPrime = (int) std::ceil (sampleRate
            * (double) DynamicHotBranchContract::kServiceWarmupMs / 1000.0);
        result.requestedSamples = requestedSamples > maxPrime ? maxPrime : requestedSamples;
        if (result.requestedSamples == 0)
        {
            result.valid = true;
            result.fullyPrimed = true;
            return result;
        }

        const int integerTap = (int) serviceBranch.physicalTapSamples;
        // Fractional read also needs the neighbor sample at integerTap + 1.
        result.availableSamples = juce::jmax (0, validLowSamples - integerTap - 1);
        result.primedSamples = juce::jmin (result.requestedSamples, result.availableSamples);
        if (result.primedSamples <= 0)
        {
            result.valid = true;
            result.fullyPrimed = false;
            return result;
        }

        // Replay the most recent primedSamples history frames chronologically
        // through Service delay + polarity + allpass without moving writeIndex.
        serviceBranch.resetRuntime();
        for (int n = 0; n < result.primedSamples; ++n)
        {
            const int currentBack = result.primedSamples - 1 - n;
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float delayed = readPrimedBranchTap (serviceBranch, ch, currentBack);
                processBranchSample (serviceBranch, ch, delayed);
            }
        }

        result.valid = true;
        result.fullyPrimed = result.primedSamples == result.requestedSamples;
        serviceBranch.applyPrimedFrames (result.primedSamples, result.fullyPrimed);
        return result;
    }

    // Processes up to prepared maxBlock samples. Larger requests must be split
    // by the caller; this never allocates or resizes.
    DynamicHotBranchProcessResult process (const juce::AudioBuffer<float>& input, int numSamples) noexcept
    {
        DynamicHotBranchProcessResult result;
        if (! prepared || ! globalConfigured || numSamples <= 0 || numSamples > maxBlock
            || input.getNumChannels() < numChannels || input.getNumSamples() < numSamples)
            return result;

        applyCrossoverSettings (globalBranch.config);

        // Sanitize raw input before the recursive crossover or any history write
        // so NaN/Inf cannot permanently poison filter state.
        for (int ch = 0; ch < numChannels; ++ch)
        {
            for (int i = 0; i < numSamples; ++i)
            {
                const float sanitized = DynamicHotBranchDetail::sanitizeSample (
                    input.getSample (ch, i), nonFiniteInputCount);
                inputScratch.setSample (ch, i, sanitized);
            }
        }

        if (globalBranch.config.crossoverEnabled)
        {
            crossover.split (inputScratch, lowScratch, highScratch, numSamples);
        }
        else
        {
            for (int ch = 0; ch < numChannels; ++ch)
            {
                lowScratch.copyFrom (ch, 0, inputScratch, ch, 0, numSamples);
                highScratch.clear (ch, 0, numSamples);
            }
        }

        for (int i = 0; i < numSamples; ++i)
        {
            // Write all channels first, then advance the shared pointers once so
            // every branch tap observes the same history state for this sample.
            for (int ch = 0; ch < numChannels; ++ch)
            {
                writeLowHistory (ch, lowScratch.getSample (ch, i));
                writeHighHistory (ch, highScratch.getSample (ch, i));
            }
            advanceWritePointers();

            for (int ch = 0; ch < numChannels; ++ch)
            {
                if (globalBranch.config.active)
                {
                    const float delayed = readBranchTap (globalBranch, ch);
                    globalLowOut.setSample (ch, i, processBranchSample (globalBranch, ch, delayed));
                }
                else
                {
                    globalLowOut.setSample (ch, i, 0.0f);
                }

                for (int slot = 0; slot < DynamicHotBranchContract::kStateSlots; ++slot)
                {
                    auto& branch = stateBranches[(size_t) slot];
                    if (branch.config.active)
                    {
                        const float delayed = readBranchTap (branch, ch);
                        stateLowOut[(size_t) slot].setSample (
                            ch, i, processBranchSample (branch, ch, delayed));
                    }
                    else
                    {
                        stateLowOut[(size_t) slot].setSample (ch, i, 0.0f);
                    }
                }

                if (serviceBranch.config.active)
                {
                    const float delayed = readBranchTap (serviceBranch, ch);
                    serviceLowOut.setSample (ch, i, processBranchSample (serviceBranch, ch, delayed));
                }
                else
                {
                    serviceLowOut.setSample (ch, i, 0.0f);
                }

                highOut.setSample (ch, i, readHighTap (ch));
            }

            // Count frames once per sample, not once per channel.
            if (globalBranch.config.active)
                globalBranch.noteLiveFrame();
            for (int slot = 0; slot < DynamicHotBranchContract::kStateSlots; ++slot)
                if (stateBranches[(size_t) slot].config.active)
                    stateBranches[(size_t) slot].noteLiveFrame();
            if (serviceBranch.config.active)
                serviceBranch.noteLiveFrame();
        }

        result.valid = true;
        result.samplesProcessed = numSamples;
        result.nonFiniteInputCount = nonFiniteInputCount;
        return result;
    }

    const juce::AudioBuffer<float>& getGlobalLowOutput() const noexcept { return globalLowOut; }
    const juce::AudioBuffer<float>& getStateLowOutput (int slot) const noexcept
    {
        static juce::AudioBuffer<float> empty;
        if (slot < 0 || slot >= DynamicHotBranchContract::kStateSlots)
            return empty;
        return stateLowOut[(size_t) slot];
    }
    const juce::AudioBuffer<float>& getServiceLowOutput() const noexcept { return serviceLowOut; }
    const juce::AudioBuffer<float>& getHighOutput() const noexcept { return highOut; }

    // Independent TDF-II reference for tests and external verification.
    // All four stages always advance; the returned sample is taken after the
    // active Global stage count (2, 3 or 4).
    static float processAllpassReference (float input,
                                          const DynamicAllpassCoefficients& coefficients,
                                          int stages,
                                          std::array<DynamicHotBranchDetail::AllpassStageState,
                                                     DynamicHotBranchContract::kMaxAllpassStages>& state) noexcept
    {
        double x = (double) input;
        double selected = x;
        const int active = juce::jlimit (2, DynamicHotBranchContract::kMaxAllpassStages, stages);
        for (int s = 0; s < DynamicHotBranchContract::kMaxAllpassStages; ++s)
        {
            x = state[(size_t) s].process (x, coefficients);
            if (s + 1 == active)
                selected = x;
        }
        return (float) selected;
    }

private:
    double physicalTapSamplesFor (double effectiveAbsoluteDelayMs) const noexcept
    {
        return (double) reportedLatencySamples
            + effectiveAbsoluteDelayMs * sampleRate / 1000.0;
    }

    static void copyGlobalOnlyFields (DynamicHotBranchConfig& destination,
                                      const DynamicHotBranchConfig& global) noexcept
    {
        destination.polarityInvert = global.polarityInvert;
        destination.allpassStages = global.allpassStages;
        destination.crossoverEnabled = global.crossoverEnabled;
        destination.crossoverHz = global.crossoverHz;
        destination.allpassEnabled = global.allpassEnabled;
        destination.delayInterpolationIndex = global.delayInterpolationIndex;
    }

    void propagateGlobalOnlyFields (const DynamicHotBranchConfig& global) noexcept
    {
        for (auto& branch : stateBranches)
        {
            if (! branch.config.active)
                continue;
            const int previousInterp = branch.config.delayInterpolationIndex;
            copyGlobalOnlyFields (branch.config, global);
            if (previousInterp != branch.config.delayInterpolationIndex)
                for (auto& interpolator : branch.interpolators)
                    interpolator.reset();
        }

        if (serviceBranch.config.active)
        {
            const int previousInterp = serviceBranch.config.delayInterpolationIndex;
            copyGlobalOnlyFields (serviceBranch.config, global);
            serviceBranch.config.stableStateId = 0;
            if (previousInterp != serviceBranch.config.delayInterpolationIndex)
                for (auto& interpolator : serviceBranch.interpolators)
                    interpolator.reset();
        }
    }

    DynamicHotBranchInfo makeInfo (const DynamicHotBranchDetail::BranchDspState& branch,
                                   DynamicHotBranchKind kind) const noexcept
    {
        DynamicHotBranchInfo info;
        info.active = branch.config.active;
        info.kind = kind;
        info.stableStateId = branch.config.stableStateId;
        info.effectiveAbsoluteDelayMs = branch.config.effectiveAbsoluteDelayMs;
        info.physicalTapSamples = branch.physicalTapSamples;
        info.coefficientsValid = DynamicAllpassPoleDomain::isValidCoefficients (branch.config.coefficients);
        info.warm = branch.warm;
        return info;
    }

    void applyCrossoverSettings (const DynamicHotBranchConfig& config) noexcept
    {
        crossover.setCrossoverFrequency ((float) config.crossoverHz);
    }

    float* lowHistoryChannel (int channel) noexcept
    {
        return lowHistory.data() + (size_t) channel * (size_t) lowHistoryCapacity;
    }

    const float* lowHistoryChannel (int channel) const noexcept
    {
        return lowHistory.data() + (size_t) channel * (size_t) lowHistoryCapacity;
    }

    float* highHistoryChannel (int channel) noexcept
    {
        return highHistory.data() + (size_t) channel * (size_t) highHistoryCapacity;
    }

    const float* highHistoryChannel (int channel) const noexcept
    {
        return highHistory.data() + (size_t) channel * (size_t) highHistoryCapacity;
    }

    void writeLowHistory (int channel, float sample) noexcept
    {
        lowHistoryChannel (channel)[writeIndex] = sample;
    }

    void writeHighHistory (int channel, float sample) noexcept
    {
        highHistoryChannel (channel)[highWriteIndex] = sample;
    }

    void advanceWritePointers() noexcept
    {
        writeIndex = (writeIndex + 1) % lowHistoryCapacity;
        highWriteIndex = (highWriteIndex + 1) % highHistoryCapacity;
        if (validLowSamples < lowHistoryCapacity)
            ++validLowSamples;
        if (validHighSamples < highHistoryCapacity)
            ++validHighSamples;
    }

    float readHistoryAtOffset (int channel, int samplesBack) const noexcept
    {
        if (samplesBack < 0 || samplesBack >= validLowSamples || lowHistoryCapacity <= 0)
            return 0.0f;
        int index = writeIndex - 1 - samplesBack;
        index %= lowHistoryCapacity;
        if (index < 0)
            index += lowHistoryCapacity;
        return lowHistoryChannel (channel)[index];
    }

    float readBranchTap (DynamicHotBranchDetail::BranchDspState& branch, int channel) noexcept
    {
        const double tap = branch.physicalTapSamples;
        const int integerDelay = (int) tap;
        const float frac = (float) (tap - (double) integerDelay);
        const float x0 = readHistoryAtOffset (channel, integerDelay);
        const float x1 = readHistoryAtOffset (channel, integerDelay + 1);
        return branch.interpolators[(size_t) channel].process (
            x0, x1, frac, branch.config.delayInterpolationIndex);
    }

    // Like readBranchTap, but the "current" sample is samplesBack frames in the
    // past so Service priming can replay history without advancing writeIndex.
    float readPrimedBranchTap (DynamicHotBranchDetail::BranchDspState& branch,
                               int channel,
                               int currentSamplesBack) noexcept
    {
        const double tap = branch.physicalTapSamples;
        const int integerDelay = (int) tap;
        const float frac = (float) (tap - (double) integerDelay);
        const float x0 = readHistoryAtOffset (channel, currentSamplesBack + integerDelay);
        const float x1 = readHistoryAtOffset (channel, currentSamplesBack + integerDelay + 1);
        return branch.interpolators[(size_t) channel].process (
            x0, x1, frac, branch.config.delayInterpolationIndex);
    }

    float readHighTap (int channel) const noexcept
    {
        if (reportedLatencySamples <= 0)
            return highHistoryChannel (channel)[(highWriteIndex + highHistoryCapacity - 1) % highHistoryCapacity];
        if (validHighSamples <= reportedLatencySamples)
            return 0.0f;
        int index = highWriteIndex - 1 - reportedLatencySamples;
        index %= highHistoryCapacity;
        if (index < 0)
            index += highHistoryCapacity;
        return highHistoryChannel (channel)[index];
    }

    float processBranchSample (DynamicHotBranchDetail::BranchDspState& branch,
                               int channel,
                               float delayedLow) noexcept
    {
        const float polarized = branch.config.polarityInvert ? -delayedLow : delayedLow;
        double x = (double) polarized;
        double selected = x;
        const int activeStages = juce::jlimit (2, DynamicHotBranchContract::kMaxAllpassStages,
                                               branch.config.allpassStages);
        auto& channelStages = branch.stages[(size_t) channel];

        // Always advance all four stages so a Global stage-count change is warm.
        for (int s = 0; s < DynamicHotBranchContract::kMaxAllpassStages; ++s)
        {
            x = channelStages[(size_t) s].process (x, branch.config.coefficients);
            if (s + 1 == activeStages)
                selected = x;
        }

        if (! branch.config.allpassEnabled)
            return polarized;
        return (float) selected;
    }

    bool prepared = false;
    bool globalConfigured = false;
    bool serviceConfigured = false;
    double sampleRate = 0.0;
    int maxBlock = 0;
    int numChannels = 0;
    int reportedLatencySamples = 0;
    int lowHistoryCapacity = 0;
    int highHistoryCapacity = 0;
    int writeIndex = 0;
    int highWriteIndex = 0;
    int validLowSamples = 0;
    int validHighSamples = 0;
    uint64_t nonFiniteInputCount = 0;

    std::vector<float> lowHistory;
    std::vector<float> highHistory;
    LinkwitzRileyCrossover crossover;
    juce::AudioBuffer<float> inputScratch;
    juce::AudioBuffer<float> lowScratch;
    juce::AudioBuffer<float> highScratch;
    juce::AudioBuffer<float> globalLowOut;
    juce::AudioBuffer<float> serviceLowOut;
    juce::AudioBuffer<float> highOut;
    std::array<juce::AudioBuffer<float>, DynamicHotBranchContract::kStateSlots> stateLowOut {};

    DynamicHotBranchDetail::BranchDspState globalBranch;
    DynamicHotBranchDetail::BranchDspState serviceBranch;
    std::array<DynamicHotBranchDetail::BranchDspState, DynamicHotBranchContract::kStateSlots> stateBranches {};
};
