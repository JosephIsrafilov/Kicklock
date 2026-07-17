#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <type_traits>
#include <vector>

#include "../dsp/DynamicStateMap.h"

// DynamicStateMapUpdateQueue
// --------------------------
// Single-producer (message thread) / single-consumer (audio thread) queue that
// hands complete DynamicStateMap values to the audio thread. It is the Phase-7
// publication mechanism for the new DynamicStateMap runtime and is modelled
// directly on NoteMapUpdateQueue: the message thread owns
// messageOwnedDynamicStateMap and pushes it here; the audio thread owns
// activeDynamicStateMap and drains this queue at a deterministic block/chunk
// boundary.
//
// DynamicStateMap is a fixed-size, trivially-copyable POD (fixed std::array
// members, no heap storage), so both push (message thread) and pop (audio
// thread) copy the whole map with no allocation and no torn reads: a slot is
// only visible to the consumer after finishedWrite, and the AbstractFifo's
// release/acquire on the write index publishes the fully-written map
// atomically. A malformed or empty map is published as a complete empty map,
// never partially. If the queue is full, push does not block; it drops the
// update and increments a diagnostic, and the message thread keeps its pending
// publication for a later retry.
class DynamicStateMapUpdateQueue
{
public:
    void prepare (int capacity = 4)
    {
        numSlots = juce::jlimit (1, 64, capacity);
        // The AbstractFifo holds (totalSize - 1) items and indexes [0, totalSize);
        // back it with totalSize physical slots so no index is ever out of range.
        const int totalSize = numSlots + 1;
        slots.assign ((size_t) totalSize, Slot {});
        fifo.setTotalSize (totalSize);
        reset();
    }

    void reset() noexcept
    {
        fifo.reset();
        droppedUpdates.store (0, std::memory_order_relaxed);
    }

    // -------- message thread (producer) --------
    bool push (const DynamicStateMap& map) noexcept
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToWrite (1, start1, size1, start2, size2);
        if (size1 < 1)
        {
            droppedUpdates.fetch_add (1, std::memory_order_relaxed);
            return false;
        }

        slots[(size_t) start1].map = map;   // fixed-size POD copy
        fifo.finishedWrite (1);
        return true;
    }

    // -------- audio thread (consumer) --------
    bool pop (DynamicStateMap& mapOut) noexcept
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToRead (1, start1, size1, start2, size2);
        if (size1 < 1)
            return false;

        mapOut = slots[(size_t) start1].map;   // fixed-size POD copy (no heap)
        fifo.finishedRead (1);
        return true;
    }

    // -------- diagnostics (any thread) --------
    int getPendingUpdateCount() const noexcept { return fifo.getNumReady(); }
    int getDroppedUpdateCount() const noexcept { return droppedUpdates.load (std::memory_order_relaxed); }
    int getCapacity()           const noexcept { return numSlots; }

private:
    struct Slot { DynamicStateMap map; };

    static_assert (std::is_trivially_copyable_v<DynamicStateMap>,
                   "DynamicStateMap must be a fixed-size POD for RT-safe SPSC publication");

    std::vector<Slot> slots;              // allocated in prepare
    juce::AbstractFifo fifo { 1 };
    int numSlots = 0;
    std::atomic<int> droppedUpdates { 0 };
};
