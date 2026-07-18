#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <type_traits>
#include <vector>

#include "../dsp/DynamicRuntimeMeasurementCapture.h"
#include "../dsp/DynamicMeasurementScorer.h"
#include "../dsp/DynamicSelectorTypes.h"

// DynamicMeasurementResultQueue
// ------------------------------
// Two bounded SPSC queues for Phase 9 runtime verification, both modelled
// directly on DynamicStateMapUpdateQueue / NoteMapUpdateQueue's established
// juce::AbstractFifo pattern:
//
//   DynamicMeasurementCaptureQueue: audio thread (producer) -> measurement
//   worker (consumer). Carries completed, fixed-size capture windows. Push
//   never blocks; a full queue drops the capture and increments a fixed
//   diagnostic rather than overwriting an in-flight publish.
//
//   DynamicMeasurementScoreQueue: measurement worker (producer) -> audio
//   thread (consumer). Carries the small scored-result value. The audio
//   thread drains this at a block boundary only, never inside the per-sample
//   loop, and rejects a result whose mapGeneration no longer matches the
//   live map.
//
// DynamicMeasurementScoreQueue's payload (DynamicMeasurementScoredCapture) is
// a small, fixed-size, trivially-copyable value: push/pop there are plain
// memcpy-style copies. DynamicMeasurementCaptureQueue's payload
// (DynamicRuntimeMeasurementCaptureResult) instead owns four std::vector
// windows sized once in prepare() via `templateValue`; push/pop then only
// ever copy ELEMENTS into already-sized destination vectors (ordinary
// vector::operator= does not reallocate when the destination's capacity
// already covers the source), so neither queue allocates after prepare().
// Both are guarded by AbstractFifo's own release/acquire fencing: no mutex,
// no torn reads.
template <typename PayloadType>
class DynamicBoundedSpscQueue
{
public:
    void prepare (int capacity, const PayloadType& templateValue = PayloadType {}) noexcept
    {
        numSlots = juce::jlimit (1, 64, capacity);
        const int totalSize = numSlots + 1;
        slots.assign ((size_t) totalSize, templateValue);
        fifo.setTotalSize (totalSize);
        reset();
    }

    void reset() noexcept
    {
        fifo.reset();
        droppedCount.store (0, std::memory_order_relaxed);
    }

    // -------- producer --------
    bool push (const PayloadType& value) noexcept
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToWrite (1, start1, size1, start2, size2);
        if (size1 < 1)
        {
            droppedCount.fetch_add (1, std::memory_order_relaxed);
            return false;
        }
        slots[(size_t) start1] = value;
        fifo.finishedWrite (1);
        return true;
    }

    // -------- consumer --------
    bool pop (PayloadType& out) noexcept
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToRead (1, start1, size1, start2, size2);
        if (size1 < 1)
            return false;
        out = slots[(size_t) start1];
        fifo.finishedRead (1);
        return true;
    }

    // -------- diagnostics (any thread) --------
    int getPendingCount() const noexcept { return fifo.getNumReady(); }
    int getDroppedCount() const noexcept { return droppedCount.load (std::memory_order_relaxed); }
    int getCapacity() const noexcept { return numSlots; }

private:
    std::vector<PayloadType> slots;
    juce::AbstractFifo fifo { 1 };
    int numSlots = 0;
    std::atomic<int> droppedCount { 0 };
};

// One small scored result handed back from the measurement worker to the
// audio thread.
struct DynamicMeasurementScoredCapture
{
    bool valid = false;
    uint64_t mapGeneration = 0;
    uint64_t stableStateId = 0;
    DynamicSelectorBranchKind branchKind = DynamicSelectorBranchKind::Global;
    int64_t triggerSample = -1;
    DynamicMeasurementScore score;
};

static_assert (std::is_trivially_copyable_v<DynamicMeasurementScoredCapture>,
               "scored capture result must be a fixed-size POD for RT-safe SPSC publication");

using DynamicMeasurementCaptureQueue = DynamicBoundedSpscQueue<DynamicRuntimeMeasurementCaptureResult>;
using DynamicMeasurementScoreQueue = DynamicBoundedSpscQueue<DynamicMeasurementScoredCapture>;

namespace DynamicMeasurementQueueContract
{
    inline constexpr int kCaptureQueueCapacity = DynamicRuntimeMeasurementCaptureContract::kMaxConcurrentCaptures;
    inline constexpr int kScoreQueueCapacity = 8;
}
