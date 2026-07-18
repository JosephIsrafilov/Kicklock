#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <type_traits>
#include <vector>

#include "../dsp/DynamicRuntimeSnapshot.h"

// DynamicSnapshotPublisher
// ------------------------
// Publishes DynamicRuntimeSnapshot from one audio-thread writer to one
// UI/message-thread reader, genuinely race-free (not merely timing-assumed):
// built directly on juce::AbstractFifo, the same proven single-producer/
// single-consumer primitive already used throughout this codebase
// (DynamicStateMapUpdateQueue, NoteMapUpdateQueue, DynamicMeasurementResultQueue).
// The writer never touches a slot the fifo reports as still unread; the
// reader only ever copies a slot the fifo has confirmed is fully written and
// not concurrently being written. No mutex, no allocation after prepare().
//
// This is a "drain to latest" queue, not a slot ring: publishIfDue() enqueues
// (dropping + counting on overflow, never blocking); read() drains every
// queued snapshot and returns the newest one, caching it so a read() with
// nothing new queued still returns the last known snapshot. Contract: read()
// is for exactly one logical reader (the message/UI thread); concurrent
// callers of read() from multiple threads are not supported, matching every
// other SPSC queue in this codebase.
class DynamicSnapshotPublisher
{
public:
    static_assert (std::is_trivially_copyable_v<DynamicRuntimeSnapshot>,
                   "DynamicRuntimeSnapshot must stay a fixed-size, trivially-copyable value");

    void prepare() noexcept
    {
        const int totalSize = kCapacity + 1;
        slots.assign ((size_t) totalSize, DynamicRuntimeSnapshot {});
        fifo.setTotalSize (totalSize);
        fifo.reset();
        cachedLatest = DynamicRuntimeSnapshot {};
        sequenceCounter = 0;
        lastPublishedBlockCounter = -1;
        droppedCount = 0;
    }

    // Publishes at most once per distinct `blockCounter` value (the caller's
    // monotonic per-process()-call counter), so a caller that accidentally
    // invokes this once per sample cannot spam the queue. Audio thread only.
    // Never blocks: a full queue (the reader has fallen behind) drops the
    // snapshot and increments a fixed diagnostic rather than overwriting an
    // unread slot or waiting.
    bool publishIfDue (DynamicRuntimeSnapshot snapshot, int64_t blockCounter) noexcept
    {
        if (blockCounter == lastPublishedBlockCounter)
            return false;
        lastPublishedBlockCounter = blockCounter;
        snapshot.sequence = ++sequenceCounter;

        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToWrite (1, start1, size1, start2, size2);
        if (size1 < 1)
        {
            ++droppedCount;
            return false;
        }
        slots[(size_t) start1] = snapshot;
        fifo.finishedWrite (1);
        return true;
    }

    // Message/UI thread only. Drains every queued snapshot and returns the
    // newest; if nothing new has been published since the last read(),
    // returns the cached last-known snapshot (never a stale partial one).
    // `const` because the mutation is purely an internal read-side cache,
    // matching the read-only contract every caller already relies on.
    DynamicRuntimeSnapshot read() const noexcept
    {
        DynamicRuntimeSnapshot popped;
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        while (true)
        {
            fifo.prepareToRead (1, start1, size1, start2, size2);
            if (size1 < 1)
                break;
            popped = slots[(size_t) start1];
            fifo.finishedRead (1);
            cachedLatest = popped;
        }
        return cachedLatest;
    }

    uint64_t getDroppedCount() const noexcept { return droppedCount; }

private:
    static constexpr int kCapacity = 4;

    mutable std::vector<DynamicRuntimeSnapshot> slots;
    mutable juce::AbstractFifo fifo { 1 };
    mutable DynamicRuntimeSnapshot cachedLatest {};
    uint64_t sequenceCounter = 0;
    int64_t lastPublishedBlockCounter = -1;
    uint64_t droppedCount = 0;
};
