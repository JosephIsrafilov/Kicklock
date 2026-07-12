#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <cmath>
#include <vector>

// A completed hit window handed to the Learn worker. The audio thread never
// touches this type; the worker owns the copied bass/kick vectors.
struct LearnHitWindow
{
    int sequence = -1;
    float trackedHzAtTrigger = 0.0f;

    std::vector<float> bass;
    std::vector<float> kick;
};

// LearnHitQueue
// -------------
// Single-producer (audio thread) / single-consumer (Learn worker) queue of
// complete kick-triggered bass/kick hit windows. All storage is preallocated in
// prepare(); the audio-thread producer (pushSample) never allocates, locks,
// logs, throws, or does unbounded work. Completed slots are handed to the
// consumer through a juce::AbstractFifo, so a slot is only visible to the
// consumer after its full window has been captured (no torn windows).
//
// Trigger policy:
//   - a transient starts a capture only when accepting and not already
//     capturing;
//   - a trigger while a capture is in progress is IGNORED (never restarts) and
//     counted as an ignored overlap;
//   - if no free slot is available at trigger time the whole hit is dropped and
//     counted;
//   - stopAcceptingNewHits() stops new triggers but lets the in-progress
//     window finish;
//   - reset() clears the queue and any in-progress capture (call when the
//     producer is quiescent).
class LearnHitQueue
{
public:
    void prepare (double sampleRate,
                  int capacity = 24,
                  float preRollMs = 20.0f,
                  float postRollMs = 150.0f)
    {
        const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
        numSlots = juce::jlimit (1, 64, capacity);
        preRollSamples = juce::jmax (1, (int) std::lround ((double) preRollMs * sr / 1000.0));
        postRollSamples = juce::jmax (1, (int) std::lround ((double) postRollMs * sr / 1000.0));
        windowSamples = preRollSamples + postRollSamples;

        // The AbstractFifo can hold (totalSize - 1) items, and its indices span
        // [0, totalSize). Size the fifo at numSlots+1 (usable capacity numSlots)
        // and back it with numSlots+1 physical slots so no index is out of range.
        const int totalSize = numSlots + 1;
        slots.assign ((size_t) totalSize, Slot {});
        for (auto& s : slots)
        {
            s.bass.assign ((size_t) windowSamples, 0.0f);
            s.kick.assign ((size_t) windowSamples, 0.0f);
        }

        preRollBass.assign ((size_t) preRollSamples, 0.0f);
        preRollKick.assign ((size_t) preRollSamples, 0.0f);

        fifo.setTotalSize (totalSize);

        reset();
    }

    // Clears the queue and in-progress capture, and resets diagnostics.
    // Must be called when the producer is not concurrently pushing.
    void reset() noexcept
    {
        fifo.reset();
        capturing = false;
        inProgress.store (false, std::memory_order_relaxed);
        captureIndex = 0;
        currentSlot = -1;
        preRollWrite = 0;
        sequenceCounter = 0;

        for (auto& v : preRollBass) v = 0.0f;
        for (auto& v : preRollKick) v = 0.0f;

        accepting.store (true, std::memory_order_release);
        droppedHits.store (0, std::memory_order_relaxed);
        ignoredOverlaps.store (0, std::memory_order_relaxed);
        acceptedHits.store (0, std::memory_order_relaxed);
    }

    // -------- audio thread (producer) --------
    void pushSample (float rawBassLow,
                     float rawKickLow,
                     bool transientDetected,
                     float trackedHz) noexcept
    {
        if (transientDetected)
        {
            if (capturing)
            {
                ignoredOverlaps.fetch_add (1, std::memory_order_relaxed);
            }
            else if (accepting.load (std::memory_order_acquire))
            {
                int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
                fifo.prepareToWrite (1, start1, size1, start2, size2);

                if (size1 >= 1)
                {
                    currentSlot = start1;
                    Slot& s = slots[(size_t) currentSlot];

                    // Prepend the pre-roll ring in chronological order (oldest
                    // first). The current sample is the first post-roll sample.
                    for (int i = 0; i < preRollSamples; ++i)
                    {
                        const int idx = (preRollWrite + i) % preRollSamples;
                        s.bass[(size_t) i] = preRollBass[(size_t) idx];
                        s.kick[(size_t) i] = preRollKick[(size_t) idx];
                    }

                    s.sequence = sequenceCounter++;
                    s.trackedHzAtTrigger = trackedHz;
                    captureIndex = preRollSamples;
                    capturing = true;
                    inProgress.store (true, std::memory_order_relaxed);
                }
                else
                {
                    droppedHits.fetch_add (1, std::memory_order_relaxed);
                }
            }
        }

        if (capturing)
        {
            Slot& s = slots[(size_t) currentSlot];
            s.bass[(size_t) captureIndex] = rawBassLow;
            s.kick[(size_t) captureIndex] = rawKickLow;
            ++captureIndex;

            if (captureIndex >= windowSamples)
            {
                fifo.finishedWrite (1);           // publish the completed window
                acceptedHits.fetch_add (1, std::memory_order_relaxed);
                capturing = false;
                inProgress.store (false, std::memory_order_relaxed);
                currentSlot = -1;
            }
        }

        // Advance the pre-roll ring (always warm, so a trigger has pre-roll).
        preRollBass[(size_t) preRollWrite] = rawBassLow;
        preRollKick[(size_t) preRollWrite] = rawKickLow;
        preRollWrite = (preRollWrite + 1) % preRollSamples;
    }

    void stopAcceptingNewHits() noexcept
    {
        accepting.store (false, std::memory_order_release);
    }

    // -------- worker thread (consumer); allocation/copy allowed here --------
    bool pop (LearnHitWindow& out)
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToRead (1, start1, size1, start2, size2);
        if (size1 < 1)
            return false;

        const Slot& s = slots[(size_t) start1];
        out.sequence = s.sequence;
        out.trackedHzAtTrigger = s.trackedHzAtTrigger;
        out.bass.assign (s.bass.begin(), s.bass.end());
        out.kick.assign (s.kick.begin(), s.kick.end());

        fifo.finishedRead (1);
        return true;
    }

    // -------- diagnostics / introspection (any thread) --------
    bool hasInProgressCapture() const noexcept { return inProgress.load (std::memory_order_relaxed); }
    int getDroppedHitCount()   const noexcept { return droppedHits.load (std::memory_order_relaxed); }
    int getIgnoredOverlapCount() const noexcept { return ignoredOverlaps.load (std::memory_order_relaxed); }
    int getAcceptedHitCount()  const noexcept { return acceptedHits.load (std::memory_order_relaxed); }
    int getPendingHitCount()   const noexcept { return fifo.getNumReady(); }

    int getWindowSamples()   const noexcept { return windowSamples; }
    int getPreRollSamples()  const noexcept { return preRollSamples; }
    int getPostRollSamples() const noexcept { return postRollSamples; }
    int getCapacity()        const noexcept { return numSlots; }

private:
    struct Slot
    {
        int sequence = -1;
        float trackedHzAtTrigger = 0.0f;
        std::vector<float> bass;
        std::vector<float> kick;
    };

    // Preallocated storage.
    std::vector<Slot> slots;
    std::vector<float> preRollBass, preRollKick;
    juce::AbstractFifo fifo { 1 };

    int numSlots = 0;
    int preRollSamples = 0;
    int postRollSamples = 0;
    int windowSamples = 0;

    // Producer-thread-only state.
    bool capturing = false;
    int captureIndex = 0;
    int currentSlot = -1;
    int preRollWrite = 0;
    int sequenceCounter = 0;

    // Cross-thread flags / diagnostics.
    std::atomic<bool> accepting { true };
    std::atomic<bool> inProgress { false };
    std::atomic<int> droppedHits { 0 };
    std::atomic<int> ignoredOverlaps { 0 };
    std::atomic<int> acceptedHits { 0 };
};
