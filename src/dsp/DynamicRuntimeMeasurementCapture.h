#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "DynamicStateMeasurements.h"
#include "DynamicSelectorTypes.h"
#include "DynamicFingerprintExtractor.h"

// =============================================================================
// Phase 9: fixed runtime measurement capture contract.
//
// Captures the canonical pre-correction bass/kick pair and the actual
// audible processed-bass / PDC-aligned-kick pair for the SAME physical kick
// hit, for up to kMaxConcurrentCaptures in-flight captures at once.
//
// Storage note: the before/after windows are std::vector<float>, sized ONCE
// in prepare() to the live sample rate's actual window length (never the
// worst-case compile-time bound). This mirrors every other bounded audio
// buffer in this codebase (DynamicHotBranchEngine's history, LearnHitQueue's
// slots, RawCaptureBuffer): a fixed std::array sized for the worst-case
//192 kHz rate would make this a many-hundred-kilobyte VALUE TYPE embedded
// kMaxConcurrentCaptures*2 times inside DynamicRuntimeMeasurementCapture,
// which is itself a stack-allocatable value member of
// DynamicProductionRuntime/PluginProcessor - several existing tests declare
// KickLockAudioProcessor/DynamicProductionRuntime as plain locals, and a
// large fixed-array version of this type reliably produced a genuine
// STATUS_STACK_OVERFLOW (0xC00000FD) there. All growth happens during
// prepare()/resize() only; the audio-thread producer methods
// (pushRawSample/pushOutputSample/beginCapture/serviceCompletions) never
// resize a vector, only write through already-sized indices.
//
// Alignment: DynamicHotBranchEngine renders branch output causally, so an
// input sample at absolute position T is rendered into the branch's output
// stream at absolute position T + tapSamples (tapSamples = the branch's
// physicalTapSamples at capture start, rounded to the nearest sample). The
// "after" window is therefore the same [-preRoll, +postRoll) span as the
// "before" window, shifted by that one fixed integer offset - never a
// second, independently-estimated alignment.
// =============================================================================

namespace DynamicRuntimeMeasurementCaptureContract
{
    inline constexpr int kMaxConcurrentCaptures = 4;
    inline constexpr float kPreRollMs = 20.0f;
    inline constexpr float kPostRollMs = 150.0f;
    inline constexpr double kMaxSupportedSampleRateForCapture = 192000.0;
    // Upper bound used only to validate/clamp a requested window; storage
    // itself is a runtime-sized std::vector, not a compile-time array.
    inline constexpr int kMaxWindowSamples = 32640; // ceil(0.17 * 192000)

    inline int windowSamplesFor (double sampleRate) noexcept
    {
        if (! std::isfinite (sampleRate) || sampleRate <= 0.0)
            return 0;
        const double total = (double) (kPreRollMs + kPostRollMs) / 1000.0 * sampleRate;
        const int samples = (int) std::ceil (total);
        return std::clamp (samples, 1, kMaxWindowSamples);
    }

    inline int preRollSamplesFor (double sampleRate) noexcept
    {
        if (! std::isfinite (sampleRate) || sampleRate <= 0.0)
            return 0;
        return std::clamp ((int) std::ceil ((double) kPreRollMs / 1000.0 * sampleRate), 1, kMaxWindowSamples);
    }
}

enum class DynamicCaptureRejectCategory : uint8_t
{
    None = 0,
    QueueExhausted,
    TransportInvalidated,
    SidechainLost,
    BypassActive,
    NonFiniteSample,
    StaleMapGeneration
};

// One completed, self-contained capture ready to be scored off the audio
// thread. `windowSamples` says how much of each vector is valid. All four
// vectors are sized identically and never resized outside of prepare()/
// resizeWindows(); copyFrom() does an element-wise std::copy into an
// already-sized destination so it never allocates, matching the SPSC
// queue's "no allocation after prepare" contract even though the payload
// is no longer a compile-time POD.
struct DynamicRuntimeMeasurementCaptureResult
{
    bool valid = false;
    uint64_t mapGeneration = 0;
    uint64_t stableStateId = 0;
    DynamicSelectorBranchKind branchKind = DynamicSelectorBranchKind::Global;
    int64_t triggerSample = -1;
    int64_t audibleTriggerSample = -1;
    int windowSamples = 0;
    double sampleRate = 0.0;
    std::vector<float> beforeBass;
    std::vector<float> beforeKick;
    std::vector<float> afterBass;
    std::vector<float> afterKick;

    // Allocates. Call only during prepare()/off the audio thread.
    void resizeWindows (int capacitySamples)
    {
        const size_t n = (size_t) std::max (0, capacitySamples);
        beforeBass.assign (n, 0.0f);
        beforeKick.assign (n, 0.0f);
        afterBass.assign (n, 0.0f);
        afterKick.assign (n, 0.0f);
    }

    // Resets scalar fields and zeroes the (already-sized) window vectors
    // without ever changing their capacity - safe on the audio thread.
    void clearForReuse() noexcept
    {
        valid = false;
        mapGeneration = 0;
        stableStateId = 0;
        branchKind = DynamicSelectorBranchKind::Global;
        triggerSample = -1;
        audibleTriggerSample = -1;
        std::fill (beforeBass.begin(), beforeBass.end(), 0.0f);
        std::fill (beforeKick.begin(), beforeKick.end(), 0.0f);
        std::fill (afterBass.begin(), afterBass.end(), 0.0f);
        std::fill (afterKick.begin(), afterKick.end(), 0.0f);
    }

    // Element-wise copy into `*this`, which must already be sized (via
    // resizeWindows()) to at least `other.windowSamples`. Never allocates
    // when that precondition holds - the audio-thread-safe alternative to
    // operator=, which could reallocate if capacities ever differed.
    void copyFrom (const DynamicRuntimeMeasurementCaptureResult& other) noexcept
    {
        valid = other.valid;
        mapGeneration = other.mapGeneration;
        stableStateId = other.stableStateId;
        branchKind = other.branchKind;
        triggerSample = other.triggerSample;
        audibleTriggerSample = other.audibleTriggerSample;
        windowSamples = other.windowSamples;
        sampleRate = other.sampleRate;
        const size_t n = std::min ({ beforeBass.size(), other.beforeBass.size(),
                                     beforeKick.size(), other.beforeKick.size(),
                                     afterBass.size(), other.afterBass.size(),
                                     afterKick.size(), other.afterKick.size() });
        std::copy_n (other.beforeBass.begin(), n, beforeBass.begin());
        std::copy_n (other.beforeKick.begin(), n, beforeKick.begin());
        std::copy_n (other.afterBass.begin(), n, afterBass.begin());
        std::copy_n (other.afterKick.begin(), n, afterKick.begin());
    }
};

// Fixed-slot capture engine. Lives on the audio thread inside
// DynamicProductionRuntime. Two independent small ring histories (raw
// bass/kick, and the branch's own recent processed output) let a capture
// that starts "now" still recover the small negative pre-roll a short
// physical tap requires without retaining unbounded history.
class DynamicRuntimeMeasurementCapture
{
public:
    bool prepare (double sampleRate) noexcept
    {
        prepared = false;
        if (! std::isfinite (sampleRate) || sampleRate <= 0.0)
            return false;

        windowSamples = DynamicRuntimeMeasurementCaptureContract::windowSamplesFor (sampleRate);
        preRollSamples = DynamicRuntimeMeasurementCaptureContract::preRollSamplesFor (sampleRate);
        if (windowSamples <= 0 || preRollSamples <= 0)
            return false;

        // Production only calls beginCapture() once the 4 ms Dynamic
        // fingerprint observation covering this trigger has COMPLETED (see
        // DynamicProductionRuntime::processChunk(): pushRawSample() and the
        // capture-bank's own pushSample() advance in lockstep, so by the time
        // takeCompleted() fires and beginCapture() runs, rawStreamPos is
        // already triggerSample + fingerprintWindowSamples, not
        // triggerSample). backfillBefore() therefore needs the raw ring to
        // still hold samples from up to (preRollSamples + fingerprintWindow-
        // Samples) ago, not just preRollSamples ago - otherwise the earliest
        // ~4 ms of the requested pre-roll is silently left at the
        // clearForReuse() zero fill instead of real audio. The output ring
        // has its own, separately-scoped staleness budget and is
        // deliberately left unchanged here.
        const int fingerprintWindowSamples = DynamicFingerprintWindow::forSampleRate (sampleRate).windowSamples;
        rawHistoryCapacity = preRollSamples + fingerprintWindowSamples + 8;
        rawBassHistory.assign ((size_t) rawHistoryCapacity, 0.0f);
        rawKickHistory.assign ((size_t) rawHistoryCapacity, 0.0f);
        outputHistoryCapacity = preRollSamples + 2;
        outputBassHistory.assign ((size_t) outputHistoryCapacity, 0.0f);
        outputKickHistory.assign ((size_t) outputHistoryCapacity, 0.0f);

        for (auto& slot : slots)
        {
            slot.active = false;
            slot.beforeDone = false;
            slot.afterDone = false;
            slot.beforeStartSample = 0;
            slot.afterStartSample = 0;
            slot.result.resizeWindows (windowSamples);
            slot.result.clearForReuse();
        }
        for (auto& completedResult : completed)
        {
            completedResult.resizeWindows (windowSamples);
            completedResult.clearForReuse();
        }

        reset();
        prepared = true;
        return true;
    }

    void reset() noexcept
    {
        rawWritePos = 0;
        rawFilled = 0;
        outputWritePos = 0;
        outputFilled = 0;
        rawStreamPos = 0;
        outputStreamPos = 0;
        std::fill (rawBassHistory.begin(), rawBassHistory.end(), 0.0f);
        std::fill (rawKickHistory.begin(), rawKickHistory.end(), 0.0f);
        std::fill (outputBassHistory.begin(), outputBassHistory.end(), 0.0f);
        std::fill (outputKickHistory.begin(), outputKickHistory.end(), 0.0f);
        for (auto& slot : slots)
            slot.active = false;
        exhaustedRequests = 0;
        droppedForTransport = 0;
    }

    bool isPrepared() const noexcept { return prepared; }
    int getWindowSamples() const noexcept { return windowSamples; }
    uint64_t getExhaustedRequestCount() const noexcept { return exhaustedRequests; }
    uint64_t getDroppedForTransportCount() const noexcept { return droppedForTransport; }
    int activeCaptureCount() const noexcept
    {
        int count = 0;
        for (const auto& slot : slots) count += slot.active ? 1 : 0;
        return count;
    }

    // Called once per raw input sample (mirrors DynamicFingerprintCaptureBank's
    // pushSample cadence). Feeds every active slot's before-window and the
    // small raw pre-roll ring.
    void pushRawSample (float rawBass, float rawKick) noexcept
    {
        if (! prepared)
            return;
        const float b = std::isfinite (rawBass) ? rawBass : 0.0f;
        const float k = std::isfinite (rawKick) ? rawKick : 0.0f;

        rawBassHistory[(size_t) rawWritePos] = b;
        rawKickHistory[(size_t) rawWritePos] = k;
        rawWritePos = (rawWritePos + 1) % rawHistoryCapacity;
        if (rawFilled < rawHistoryCapacity)
            ++rawFilled;

        for (auto& slot : slots)
        {
            if (! slot.active || slot.beforeDone)
                continue;
            if (rawStreamPos < slot.beforeStartSample || rawStreamPos >= slot.beforeStartSample + windowSamples)
                continue;
            const int index = (int) (rawStreamPos - slot.beforeStartSample);
            slot.result.beforeBass[(size_t) index] = b;
            slot.result.beforeKick[(size_t) index] = k;
            if (index + 1 >= windowSamples)
                slot.beforeDone = true;
        }

        ++rawStreamPos;
    }

    // Called once per rendered OUTPUT sample (channel-0 processed bass and
    // the raw kick reference, latency-aligned by the caller to the SAME
    // absolute index space as pushRawSample / the branch's physical tap).
    void pushOutputSample (float processedBass, float alignedKick) noexcept
    {
        if (! prepared)
            return;
        const float b = std::isfinite (processedBass) ? processedBass : 0.0f;
        const float k = std::isfinite (alignedKick) ? alignedKick : 0.0f;

        outputBassHistory[(size_t) outputWritePos] = b;
        outputKickHistory[(size_t) outputWritePos] = k;
        outputWritePos = (outputWritePos + 1) % outputHistoryCapacity;
        if (outputFilled < outputHistoryCapacity)
            ++outputFilled;

        for (auto& slot : slots)
        {
            if (! slot.active || slot.afterDone)
                continue;
            if (outputStreamPos < slot.afterStartSample || outputStreamPos >= slot.afterStartSample + windowSamples)
                continue;
            const int index = (int) (outputStreamPos - slot.afterStartSample);
            slot.result.afterBass[(size_t) index] = b;
            slot.result.afterKick[(size_t) index] = k;
            if (index + 1 >= windowSamples)
                slot.afterDone = true;
        }

        ++outputStreamPos;
    }

    // Starts a new capture for a confidently-matched, correction-eligible
    // event. `tapSamples` is the resolved branch's physical tap at capture
    // start (reportedLatencySamples + effectiveAbsoluteDelayMs*sr/1000,
    // rounded). Returns false (Exhausted) without mutating state when all
    // slots are already active.
    bool beginCapture (uint64_t mapGeneration, uint64_t stableStateId, DynamicSelectorBranchKind branchKind,
                       int64_t triggerSample, double tapSamples, double sampleRate) noexcept
    {
        if (! prepared || ! std::isfinite (tapSamples))
            return false;

        for (auto& slot : slots)
            if (slot.active && slot.result.stableStateId == stableStateId
                && slot.result.mapGeneration == mapGeneration)
                return true; // already capturing this identity; do not duplicate

        for (auto& slot : slots)
        {
            if (slot.active)
                continue;

            const int64_t tap = (int64_t) std::llround (tapSamples);
            slot.active = true;
            slot.beforeDone = false;
            slot.afterDone = false;
            slot.result.clearForReuse();
            slot.result.mapGeneration = mapGeneration;
            slot.result.stableStateId = stableStateId;
            slot.result.branchKind = branchKind;
            slot.result.triggerSample = triggerSample;
            slot.result.audibleTriggerSample = triggerSample + tap;
            slot.result.windowSamples = windowSamples;
            slot.result.sampleRate = sampleRate;
            slot.beforeStartSample = triggerSample - preRollSamples;
            slot.afterStartSample = triggerSample + tap - preRollSamples;

            // Backfill from the small ring histories: samples strictly before
            // the current stream position that the ring still holds.
            backfillBefore (slot);
            backfillAfter (slot);
            if (slot.beforeDone && slot.afterDone)
                finalizeSlot (slot);
            return true;
        }

        ++exhaustedRequests;
        return false;
    }

    // Discards every in-flight capture without publishing a result (transport
    // discontinuity / sidechain loss / bypass). Never affects audio.
    void discardInFlight (DynamicCaptureRejectCategory reason) noexcept
    {
        for (auto& slot : slots)
        {
            if (! slot.active)
                continue;
            slot.active = false;
            ++droppedForTransport;
            (void) reason;
        }
    }

    // Pops the oldest completed result, or returns false if none is ready.
    bool takeCompleted (DynamicRuntimeMeasurementCaptureResult& out) noexcept
    {
        if (completedCount == 0)
            return false;
        out.copyFrom (completed[(size_t) completedHead]);
        completedHead = (completedHead + 1) % kMaxConcurrentCaptures;
        --completedCount;
        return true;
    }

    // Called once per block boundary (after both push streams for the block
    // have been fed) to promote any slot that has just finished both
    // windows into the completed ring.
    void serviceCompletions() noexcept
    {
        for (auto& slot : slots)
            if (slot.active && slot.beforeDone && slot.afterDone)
                finalizeSlot (slot);
    }

private:
    static constexpr int kMaxConcurrentCaptures = DynamicRuntimeMeasurementCaptureContract::kMaxConcurrentCaptures;

    struct Slot
    {
        bool active = false;
        bool beforeDone = false;
        bool afterDone = false;
        int64_t beforeStartSample = 0;
        int64_t afterStartSample = 0;
        DynamicRuntimeMeasurementCaptureResult result;
    };

    void backfillBefore (Slot& slot) noexcept
    {
        // Fill any samples in [beforeStartSample, rawStreamPos) that the raw
        // ring still holds; the live loop in pushRawSample() supplies the
        // rest as the stream continues.
        const int64_t backfillEnd = std::min (rawStreamPos, slot.beforeStartSample + windowSamples);
        for (int64_t pos = slot.beforeStartSample; pos < backfillEnd; ++pos)
        {
            const int64_t samplesBack = rawStreamPos - 1 - pos;
            if (samplesBack < 0 || samplesBack >= rawFilled)
                continue;
            int index = rawWritePos - 1 - (int) samplesBack;
            index = ((index % rawHistoryCapacity) + rawHistoryCapacity) % rawHistoryCapacity;
            const int outIndex = (int) (pos - slot.beforeStartSample);
            if (outIndex < 0 || outIndex >= windowSamples)
                continue;
            slot.result.beforeBass[(size_t) outIndex] = rawBassHistory[(size_t) index];
            slot.result.beforeKick[(size_t) outIndex] = rawKickHistory[(size_t) index];
        }
        if (backfillEnd >= slot.beforeStartSample + windowSamples)
            slot.beforeDone = true;
    }

    void backfillAfter (Slot& slot) noexcept
    {
        const int64_t backfillEnd = std::min (outputStreamPos, slot.afterStartSample + windowSamples);
        for (int64_t pos = std::max (slot.afterStartSample, (int64_t) 0); pos < backfillEnd; ++pos)
        {
            const int64_t samplesBack = outputStreamPos - 1 - pos;
            if (samplesBack < 0 || samplesBack >= outputFilled)
                continue;
            int index = outputWritePos - 1 - (int) samplesBack;
            index = ((index % outputHistoryCapacity) + outputHistoryCapacity) % outputHistoryCapacity;
            const int outIndex = (int) (pos - slot.afterStartSample);
            if (outIndex < 0 || outIndex >= windowSamples)
                continue;
            slot.result.afterBass[(size_t) outIndex] = outputBassHistory[(size_t) index];
            slot.result.afterKick[(size_t) outIndex] = outputKickHistory[(size_t) index];
        }
        if (backfillEnd >= slot.afterStartSample + windowSamples)
            slot.afterDone = true;
    }

    void finalizeSlot (Slot& slot) noexcept
    {
        slot.result.valid = true;
        if (completedCount < kMaxConcurrentCaptures)
        {
            const int tail = (completedHead + completedCount) % kMaxConcurrentCaptures;
            completed[(size_t) tail].copyFrom (slot.result);
            ++completedCount;
        }
        slot.active = false;
    }

    bool prepared = false;
    int windowSamples = 0;
    int preRollSamples = 0;

    int rawHistoryCapacity = 0;
    std::vector<float> rawBassHistory;
    std::vector<float> rawKickHistory;
    int rawWritePos = 0;
    int rawFilled = 0;
    int64_t rawStreamPos = 0;

    int outputHistoryCapacity = 0;
    std::vector<float> outputBassHistory;
    std::vector<float> outputKickHistory;
    int outputWritePos = 0;
    int outputFilled = 0;
    int64_t outputStreamPos = 0;

    std::array<Slot, kMaxConcurrentCaptures> slots {};
    std::array<DynamicRuntimeMeasurementCaptureResult, kMaxConcurrentCaptures> completed {};
    int completedHead = 0;
    int completedCount = 0;

    uint64_t exhaustedRequests = 0;
    uint64_t droppedForTransport = 0;
};
