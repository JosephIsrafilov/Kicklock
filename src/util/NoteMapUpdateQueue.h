#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <vector>

#include "../dsp/NotePhaseMap.h"

// NoteMapUpdateQueue
// ------------------
// Single-producer (message thread) / single-consumer (audio thread) queue that
// hands complete NotePhaseMapSnapshot values to the audio thread. The worker
// never publishes to the audio thread directly: it hands its result to the
// message thread, which pushes here. The audio thread owns its own activeNoteMap
// and drains this queue at the top of each block.
//
// NotePhaseMapSnapshot is a fixed-size, trivially-copyable POD (no heap members),
// so both push (message thread) and pop (audio thread) copy the whole snapshot
// with no allocation and no torn reads: a slot is only visible to the consumer
// after finishedWrite, and the AbstractFifo's release/acquire on the write index
// publishes the fully-written snapshot atomically. If the queue is full, push
// does not block; it drops the update and increments a diagnostic (the message
// thread keeps its pending publication and retries on the next tick).
class NoteMapUpdateQueue
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
    bool push (const NotePhaseMapSnapshot& snapshot) noexcept
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToWrite (1, start1, size1, start2, size2);
        if (size1 < 1)
        {
            droppedUpdates.fetch_add (1, std::memory_order_relaxed);
            return false;
        }

        slots[(size_t) start1].snapshot = snapshot;   // fixed-size POD copy
        fifo.finishedWrite (1);
        return true;
    }

    // -------- audio thread (consumer) --------
    bool pop (NotePhaseMapSnapshot& snapshotOut) noexcept
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToRead (1, start1, size1, start2, size2);
        if (size1 < 1)
            return false;

        snapshotOut = slots[(size_t) start1].snapshot;   // fixed-size POD copy (no heap)
        fifo.finishedRead (1);
        return true;
    }

    // -------- diagnostics (any thread) --------
    int getPendingUpdateCount() const noexcept { return fifo.getNumReady(); }
    int getDroppedUpdateCount() const noexcept { return droppedUpdates.load (std::memory_order_relaxed); }
    int getCapacity()           const noexcept { return numSlots; }

private:
    struct Slot { NotePhaseMapSnapshot snapshot; };

    std::vector<Slot> slots;              // allocated in prepare
    juce::AbstractFifo fifo { 1 };
    int numSlots = 0;
    std::atomic<int> droppedUpdates { 0 };
};
