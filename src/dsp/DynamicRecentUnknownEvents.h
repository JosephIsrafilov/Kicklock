#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <type_traits>

#include "DynamicFingerprintExtractor.h"
#include "DynamicFingerprintMatcher.h"

// =============================================================================
// Phase 12, Section 15: Recent Unknown Events.
//
// Two-stage, thread-safety-explicit design:
//
//   1. Audio thread produces a fixed-size DynamicRecentUnknownRawEvent per
//      Unknown/Ambiguous match decision (DynamicProductionRuntime buffers a
//      small same-thread ring internally; PluginProcessor drains it once per
//      block into a DynamicBoundedSpscQueue<DynamicRecentUnknownRawEvent> -
//      the exact same generic SPSC template already used for measurement
//      captures). No allocation, no lock, on the audio thread.
//
//   2. The message thread alone owns DynamicRecentUnknownEventLog: it drains
//      the queue and calls ingest(), which coalesces repeatable similar
//      events into a bounded (kMaxRecentUnknownClusters) list of clusters.
//      The audio thread never touches this log - there is no synchronization
//      inside it because there is only ever one thread using it.
// =============================================================================

namespace DynamicRecentUnknownContract
{
    inline constexpr int kMaxRecentUnknownClusters = 32;
    // Normalized [0,1] fingerprint distance (see dynamicFingerprintDistanceV1)
    // below which two events are considered the same repeatable conflict
    // rather than two distinct ones.
    inline constexpr float kCoalesceDistanceThreshold = 0.12f;
}

enum class DynamicRecentUnknownOutcome : uint8_t
{
    Unknown = 0,
    Ambiguous = 1
};

// Bounded, fixed-size, audio-thread-producible raw event. No allocation.
struct DynamicRecentUnknownRawEvent
{
    bool valid = false;
    DynamicFingerprintPrototype fingerprint;
    DynamicRecentUnknownOutcome outcome = DynamicRecentUnknownOutcome::Unknown;
    float nearestDistance = 0.0f;
    int64_t triggerSample = -1;
    uint64_t mapGeneration = 0;
};

static_assert (std::is_trivially_copyable_v<DynamicRecentUnknownRawEvent>,
               "raw recent-unknown event must be a fixed-size POD for RT-safe hand-off");

// Message-thread-owned, non-persistent coalesced cluster. eventId is an
// internal monotonic identifier - never a stableStateId, and never
// serialized (Recent Unknown history is explicitly non-persistent per the
// frozen contract).
struct DynamicRecentUnknownCluster
{
    uint64_t eventId = 0;
    DynamicFingerprintPrototype prototype;
    DynamicRecentUnknownOutcome outcome = DynamicRecentUnknownOutcome::Unknown;
    uint32_t repeatCount = 0;
    float bestDistance = 0.0f;
    int64_t lastTriggerSample = -1;
    uint64_t lastMapGeneration = 0;
};

// Pure, message-thread-only coalescing log. No JUCE dependency; unit-testable
// exactly like DynamicStateMap.h's own validators. Bounded at
// kMaxRecentUnknownClusters entries - a genuinely new, distinct cluster is
// dropped (with a diagnostic counter) once full rather than overwriting an
// existing one, so a burst of unrelated noise cannot evict a real repeatable
// cluster the producer is about to promote.
class DynamicRecentUnknownEventLog
{
public:
    bool ingest (const DynamicRecentUnknownRawEvent& event) noexcept
    {
        if (! event.valid || ! isValidDynamicFingerprintPrototype (event.fingerprint))
            return false;

        int bestIndex = -1;
        float bestDistance = std::numeric_limits<float>::infinity();
        for (int i = 0; i < count; ++i)
        {
            const float distance = dynamicFingerprintDistanceV1 (event.fingerprint, clusters[(size_t) i].prototype);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestIndex = i;
            }
        }

        if (bestIndex >= 0 && bestDistance <= DynamicRecentUnknownContract::kCoalesceDistanceThreshold)
        {
            auto& cluster = clusters[(size_t) bestIndex];
            ++cluster.repeatCount;
            cluster.bestDistance = std::min (cluster.bestDistance, event.nearestDistance);
            cluster.lastTriggerSample = event.triggerSample;
            cluster.lastMapGeneration = event.mapGeneration;
            cluster.outcome = event.outcome;
            return true;
        }

        if (count >= DynamicRecentUnknownContract::kMaxRecentUnknownClusters)
        {
            ++overflowCount;
            return false;
        }

        auto& cluster = clusters[(size_t) count];
        cluster.eventId = nextEventId++;
        cluster.prototype = event.fingerprint;
        cluster.outcome = event.outcome;
        cluster.repeatCount = 1;
        cluster.bestDistance = event.nearestDistance;
        cluster.lastTriggerSample = event.triggerSample;
        cluster.lastMapGeneration = event.mapGeneration;
        ++count;
        return true;
    }

    int getClusterCount() const noexcept { return count; }
    const DynamicRecentUnknownCluster& getCluster (int index) const noexcept { return clusters[(size_t) index]; }
    uint64_t getOverflowCount() const noexcept { return overflowCount; }

    // Removes one cluster by eventId (Ignore, or consumed by Create Manual
    // State). Preserves the relative order of the remaining clusters.
    bool removeCluster (uint64_t eventId) noexcept
    {
        for (int i = 0; i < count; ++i)
        {
            if (clusters[(size_t) i].eventId != eventId)
                continue;
            for (int j = i; j + 1 < count; ++j)
                clusters[(size_t) j] = clusters[(size_t) (j + 1)];
            --count;
            clusters[(size_t) count] = DynamicRecentUnknownCluster {};
            return true;
        }
        return false;
    }

    void clear() noexcept
    {
        for (auto& cluster : clusters)
            cluster = DynamicRecentUnknownCluster {};
        count = 0;
        overflowCount = 0;
    }

private:
    std::array<DynamicRecentUnknownCluster, DynamicRecentUnknownContract::kMaxRecentUnknownClusters> clusters {};
    int count = 0;
    uint64_t nextEventId = 1;
    uint64_t overflowCount = 0;
};

// Small same-thread (audio-thread-only) staging ring: DynamicProductionRuntime
// pushes here at match time; PluginProcessor pops from here at the same
// block's drain point, on the same (audio) thread, before handing raw events
// across to the message thread via the SPSC queue. Not itself cross-thread -
// no atomics needed.
class DynamicRecentUnknownStagingRing
{
public:
    static constexpr int kCapacity = 8;

    bool push (const DynamicRecentUnknownRawEvent& event) noexcept
    {
        if (count >= kCapacity)
        {
            ++droppedCount;
            return false;
        }
        const int tail = (head + count) % kCapacity;
        slots[(size_t) tail] = event;
        ++count;
        return true;
    }

    bool pop (DynamicRecentUnknownRawEvent& out) noexcept
    {
        if (count == 0)
            return false;
        out = slots[(size_t) head];
        head = (head + 1) % kCapacity;
        --count;
        return true;
    }

    uint64_t getDroppedCount() const noexcept { return droppedCount; }

private:
    std::array<DynamicRecentUnknownRawEvent, kCapacity> slots {};
    int head = 0;
    int count = 0;
    uint64_t droppedCount = 0;
};
