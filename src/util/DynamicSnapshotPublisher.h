#pragma once

#include <array>
#include <atomic>
#include <type_traits>

#include "../dsp/DynamicRuntimeSnapshot.h"

// DynamicSnapshotPublisher
// ------------------------
// Fixed N-way buffer publishing DynamicRuntimeSnapshot from one audio-thread
// writer to any number of UI/message-thread readers, tear-free. The writer
// always advances to the slot after the currently-published one and only
// then publishes the new index (release); a reader loads the index
// (acquire) and copies that whole slot by value. With 4 slots and a
// publish cadence of at most once per host process block (never once per
// sample - see publishIfDue()), a reader's copy of this small fixed struct
// completes long before the writer could wrap back onto the slot it is
// reading, so no reader ever observes a partially-written snapshot or a mix
// of two generations. No mutex, no allocation after construction.
class DynamicSnapshotPublisher
{
public:
    static_assert (std::is_trivially_copyable_v<DynamicRuntimeSnapshot>,
                   "DynamicRuntimeSnapshot must stay a fixed-size, trivially-copyable value");

    void prepare() noexcept
    {
        for (auto& slot : slots)
            slot = DynamicRuntimeSnapshot {};
        latestIndex.store (0, std::memory_order_relaxed);
        sequenceCounter.store (0, std::memory_order_relaxed);
        lastPublishedBlockCounter = -1;
    }

    // Publishes at most once per distinct `blockCounter` value (the caller's
    // monotonic per-process()-call counter), so a caller that accidentally
    // invokes this once per sample cannot spam the buffer ring.
    bool publishIfDue (DynamicRuntimeSnapshot snapshot, int64_t blockCounter) noexcept
    {
        if (blockCounter == lastPublishedBlockCounter)
            return false;
        lastPublishedBlockCounter = blockCounter;

        const int current = latestIndex.load (std::memory_order_relaxed);
        const int next = (current + 1) % kSlotCount;
        snapshot.sequence = sequenceCounter.fetch_add (1, std::memory_order_relaxed) + 1;
        slots[(size_t) next] = snapshot;
        latestIndex.store (next, std::memory_order_release);
        return true;
    }

    DynamicRuntimeSnapshot read() const noexcept
    {
        const int index = latestIndex.load (std::memory_order_acquire);
        return slots[(size_t) index];
    }

private:
    static constexpr int kSlotCount = 4;

    std::array<DynamicRuntimeSnapshot, kSlotCount> slots {};
    std::atomic<int> latestIndex { 0 };
    std::atomic<uint64_t> sequenceCounter { 0 };
    int64_t lastPublishedBlockCounter = -1;
};
