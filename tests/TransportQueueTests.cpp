#include "TestCommon.h"

#include "LearnHitQueue.h"
#include "NoteMapUpdateQueue.h"
#include "NotePhaseMap.h"

#include <cstdlib>
#include <thread>

// =============================================================================
// Phase 2 tests: RT-safe transport infrastructure.
//   T5  - LearnHitQueue (SPSC hit-window queue)
//   T10 - NoteMapUpdateQueue (SPSC map publication)
//   plus concurrent stress and processor plumbing integration.
// =============================================================================

// Flag-gated global allocation counter. It only counts while g_allocTracking is
// set (off by default), so it never perturbs the rest of the binary; the tracked
// regions are the tight audio-thread queue operations.
namespace
{
    std::atomic<long> g_allocCount { 0 };
    std::atomic<bool> g_allocTracking { false };
}

// Override only the scalar operators (std::vector / std::string / std::allocator
// route through ::operator new(size_t)); leaving the array forms at their
// defaults keeps new[]/delete[] matched. GCC's -Wmismatched-new-delete is a
// false positive here: replacing the global operator new/delete with malloc/free
// is a correctly matched pair, but the heuristic flags the free() calls.
#if defined(__GNUC__) && ! defined(__clang__)
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new (std::size_t n)
{
    if (g_allocTracking.load (std::memory_order_relaxed))
        g_allocCount.fetch_add (1, std::memory_order_relaxed);
    if (n == 0) n = 1;
    void* p = std::malloc (n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}
void operator delete (void* p) noexcept { std::free (p); }
void operator delete (void* p, std::size_t) noexcept { std::free (p); }
#if defined(__GNUC__) && ! defined(__clang__)
 #pragma GCC diagnostic pop
#endif

namespace
{
    NotePhaseMapSnapshot makeValidMap (float crossoverHz = 140.0f, float delayTag = -2.3f)
    {
        NotePhaseMapSnapshot m = NoteMap::makeEmptyNoteMap();
        m.valid = true;
        m.base.delayMs = delayTag;
        m.base.crossoverHz = crossoverHz;
        m.base.allpassStages = 3;
        m.base.learnedSampleRate = 48000.0;
        m.global.learned = true;
        m.global.fundamentalHz = 55.0f;
        m.global.allpassFreqHz = 90.0f;
        m.global.allpassQ = 1.1f;
        m.global.confidence = 0.7f;
        m.global.hitCount = 24;
        m.global.rotatorHelps = true;

        NoteEntry e;
        e.learned = true;
        e.fundamentalHz = 55.0f;
        e.allpassFreqHz = 80.0f;
        e.allpassQ = 1.0f;
        e.confidence = 0.82f;
        e.fundamentalSpreadRatio = 0.04f;
        e.hitCount = 6;
        e.rotatorHelps = true;
        m.notes[(size_t) NotePhaseMapSnapshot::indexForMidi (33)] = e;
        return m;
    }

    // Drives a LearnHitQueue with a global ramp: bass[g] = g, kick[g] = g + 0.5,
    // triggering at chosen global sample indices.
    struct RampDriver
    {
        LearnHitQueue& q;
        int g = 0;
        void feedTo (int endGlobal, const std::vector<int>& triggers, float hz = 55.0f)
        {
            while (g < endGlobal)
            {
                const bool trig = std::find (triggers.begin(), triggers.end(), g) != triggers.end();
                q.pushSample ((float) g, (float) g + 0.5f, trig, trig ? hz : 0.0f);
                ++g;
            }
        }
    };
}

//============================================================================//
// T5 - LearnHitQueue
//============================================================================//
class LearnHitQueueTests : public juce::UnitTest
{
public:
    LearnHitQueueTests() : juce::UnitTest ("LearnHitQueue", "Phase2") {}

    void runTest() override
    {
        beginTest ("Window sizing matches ~20 ms pre + ~150 ms post");
        {
            LearnHitQueue q;
            q.prepare (48000.0, 24, 20.0f, 150.0f);
            expectEquals (q.getPreRollSamples(), 960);
            expectEquals (q.getPostRollSamples(), 7200);
            expectEquals (q.getWindowSamples(), 8160);
            expectEquals (q.getCapacity(), 24);
        }

        beginTest ("A captured window carries the correct pre/post content and pitch metadata");
        {
            LearnHitQueue q;
            q.prepare (48000.0, 24, 20.0f, 150.0f);
            const int pre = q.getPreRollSamples();
            const int post = q.getPostRollSamples();
            const int win = q.getWindowSamples();

            RampDriver d { q };
            d.feedTo (pre, {});                    // warm the pre-roll ring with [0..pre-1]
            d.feedTo (pre + post, { pre }, 55.0f); // trigger at global index `pre`, then complete

            expectEquals (q.getPendingHitCount(), 1);
            expectEquals (q.getAcceptedHitCount(), 1);

            LearnHitWindow w;
            expect (q.pop (w));
            expectEquals ((int) w.bass.size(), win);
            expectEquals ((int) w.kick.size(), win);
            expectEquals (w.sequence, 0);
            expectWithinAbsoluteError (w.trackedHzAtTrigger, 55.0f, 1.0e-6f);

            // Window covers global [0 .. win-1]; bass[k]=k, kick[k]=k+0.5.
            expectWithinAbsoluteError (w.bass[0], 0.0f, 1.0e-4f);
            expectWithinAbsoluteError (w.bass[(size_t) pre], (float) pre, 1.0e-4f);       // trigger sample
            expectWithinAbsoluteError (w.bass[(size_t) (win - 1)], (float) (win - 1), 1.0e-4f);
            expectWithinAbsoluteError (w.kick[(size_t) pre], (float) pre + 0.5f, 1.0e-4f);

            expect (! q.pop (w), "queue is now empty");
        }

        beginTest ("FIFO ordering, full/empty and overflow drop the whole hit");
        {
            LearnHitQueue q;
            q.prepare (1000.0, 3, 2.0f, 8.0f);   // pre=2, post=8, win=10, capacity=3
            const int win = q.getWindowSamples();
            const int pre = q.getPreRollSamples();

            // Three hits, triggers spaced by `win` so windows are contiguous and
            // never overlap.
            RampDriver d { q };
            d.feedTo (pre + 3 * win, { pre, pre + win, pre + 2 * win });
            expectEquals (q.getPendingHitCount(), 3);
            expectEquals (q.getAcceptedHitCount(), 3);
            expectEquals (q.getDroppedHitCount(), 0);

            // Queue is full: a further trigger drops the whole hit.
            d.feedTo (pre + 3 * win + win, { pre + 3 * win });
            expectEquals (q.getDroppedHitCount(), 1);
            expectEquals (q.getPendingHitCount(), 3);   // unchanged

            LearnHitWindow w;
            for (int expected = 0; expected < 3; ++expected)
            {
                expect (q.pop (w));
                expectEquals (w.sequence, expected);     // FIFO order
                expectEquals ((int) w.bass.size(), win);
            }
            expect (! q.pop (w), "empty after draining");
        }

        beginTest ("Ring wraparound preserves FIFO order");
        {
            LearnHitQueue q;
            q.prepare (1000.0, 3, 2.0f, 8.0f);
            const int win = q.getWindowSamples();
            const int pre = q.getPreRollSamples();

            RampDriver d { q };
            d.feedTo (pre + 3 * win, { pre, pre + win, pre + 2 * win }); // seq 0,1,2 (full)

            LearnHitWindow w;
            expect (q.pop (w)); expectEquals (w.sequence, 0);
            expect (q.pop (w)); expectEquals (w.sequence, 1);            // freed two slots

            // Two more hits reuse the wrapped slots.
            d.feedTo (d.g + 2 * win + pre, { d.g + pre, d.g + pre + win }); // seq 3,4

            expect (q.pop (w)); expectEquals (w.sequence, 2);
            expect (q.pop (w)); expectEquals (w.sequence, 3);
            expect (q.pop (w)); expectEquals (w.sequence, 4);
            expect (! q.pop (w));
        }

        beginTest ("Overlapping trigger is ignored, not restarted");
        {
            LearnHitQueue q;
            q.prepare (1000.0, 3, 2.0f, 8.0f);   // win=10, pre=2
            const int win = q.getWindowSamples();
            const int pre = q.getPreRollSamples();

            // Trigger at g=2 starts a window; a second trigger at g=4 arrives
            // mid-capture and must be ignored (not restart the window).
            RampDriver d { q };
            d.feedTo (win + 2, { 2, 4 });

            expectEquals (q.getIgnoredOverlapCount(), 1);
            expectEquals (q.getAcceptedHitCount(), 1);
            expectEquals (q.getPendingHitCount(), 1);

            LearnHitWindow w;
            expect (q.pop (w));
            // The window belongs to the FIRST trigger: its sample at index `pre`
            // is global 2, and the tracked pitch is the first trigger's value.
            expectWithinAbsoluteError (w.bass[(size_t) pre], 2.0f, 1.0e-4f);
        }

        beginTest ("stopAcceptingNewHits blocks new triggers but finishes the current window");
        {
            LearnHitQueue q;
            q.prepare (1000.0, 3, 2.0f, 8.0f);
            const int win = q.getWindowSamples();
            const int pre = q.getPreRollSamples();

            RampDriver d { q };
            d.feedTo (pre, {});
            // Start a capture, then stop accepting mid-window.
            d.feedTo (pre + 3, { pre });   // trigger at pre; capture in progress
            expect (q.hasInProgressCapture());
            q.stopAcceptingNewHits();
            d.feedTo (pre + win, {});       // finish the window
            expectEquals (q.getAcceptedHitCount(), 1);   // in-progress window still completed
            expect (! q.hasInProgressCapture());

            // A new trigger after stop does not start a capture.
            d.feedTo (d.g + win, { d.g + 1 });
            expectEquals (q.getAcceptedHitCount(), 1);
        }

        beginTest ("pushSample performs no allocations on the audio thread");
        {
            LearnHitQueue q;
            q.prepare (48000.0, 24, 20.0f, 150.0f);
            for (int i = 0; i < q.getPreRollSamples(); ++i)
                q.pushSample (0.0f, 0.0f, false, 0.0f);   // warm

            g_allocCount.store (0);
            g_allocTracking.store (true);
            for (int i = 0; i < 200000; ++i)
            {
                const bool trig = (i % 9000 == 0);       // spaced > window, no overlap
                q.pushSample ((float) i, (float) i, trig, 55.0f);
            }
            g_allocTracking.store (false);

            expectEquals ((int) g_allocCount.load(), 0, "no allocation in pushSample");
            expectGreaterThan (q.getAcceptedHitCount(), 0);   // it really did work
        }
    }
};

//============================================================================//
// T10 - NoteMapUpdateQueue
//============================================================================//
class NoteMapUpdateQueueTests : public juce::UnitTest
{
public:
    NoteMapUpdateQueueTests() : juce::UnitTest ("NoteMapUpdateQueue", "Phase2") {}

    void runTest() override
    {
        beginTest ("Message -> audio publication transfers a complete snapshot");
        {
            NoteMapUpdateQueue q;
            q.prepare (4);
            expectEquals (q.getCapacity(), 4);

            const auto m = makeValidMap (137.0f, -1.75f);
            expect (q.push (m));
            expectEquals (q.getPendingUpdateCount(), 1);

            NotePhaseMapSnapshot out;
            expect (q.pop (out));
            expect (out.valid);
            expectWithinAbsoluteError (out.base.crossoverHz, 137.0f, 1.0e-4f);
            expectWithinAbsoluteError (out.base.delayMs, -1.75f, 1.0e-4f);
            expectWithinAbsoluteError (out.global.allpassFreqHz, m.global.allpassFreqHz, 1.0e-4f);
            const int idx = NotePhaseMapSnapshot::indexForMidi (33);
            expect (out.notes[(size_t) idx].learned);
            expectWithinAbsoluteError (out.notes[(size_t) idx].allpassFreqHz, 80.0f, 1.0e-4f);
            expect (! q.pop (out), "empty after draining");
        }

        beginTest ("FIFO order, capacity 4, and overflow drops the update");
        {
            NoteMapUpdateQueue q;
            q.prepare (4);
            for (int i = 0; i < 4; ++i)
                expect (q.push (makeValidMap (100.0f + (float) i, (float) i)));
            expectEquals (q.getPendingUpdateCount(), 4);

            // Fifth push overflows.
            expect (! q.push (makeValidMap (200.0f, 9.0f)));
            expectEquals (q.getDroppedUpdateCount(), 1);
            expectEquals (q.getPendingUpdateCount(), 4);

            NotePhaseMapSnapshot out;
            for (int i = 0; i < 4; ++i)
            {
                expect (q.pop (out));
                expectWithinAbsoluteError (out.base.delayMs, (float) i, 1.0e-4f); // FIFO order
            }
        }

        beginTest ("Draining keeps the latest complete snapshot");
        {
            NoteMapUpdateQueue q;
            q.prepare (4);
            q.push (makeValidMap (110.0f, 1.0f));
            q.push (makeValidMap (120.0f, 2.0f));
            q.push (makeValidMap (130.0f, 3.0f));

            NotePhaseMapSnapshot active;
            int drained = 0;
            NotePhaseMapSnapshot tmp;
            while (q.pop (tmp)) { active = tmp; ++drained; }
            expectEquals (drained, 3);
            expectWithinAbsoluteError (active.base.crossoverHz, 130.0f, 1.0e-4f); // last wins
            expectEquals (q.getPendingUpdateCount(), 0);
        }

        beginTest ("push/pop perform no allocations");
        {
            NoteMapUpdateQueue q;
            q.prepare (4);
            const auto m = makeValidMap();
            NotePhaseMapSnapshot out;

            g_allocCount.store (0);
            g_allocTracking.store (true);
            for (int round = 0; round < 1000; ++round)
            {
                q.push (m);
                q.pop (out);
            }
            g_allocTracking.store (false);
            expectEquals ((int) g_allocCount.load(), 0, "no allocation in push/pop");
        }
    }
};

//============================================================================//
// Concurrent stress (SPSC ownership / no torn windows)
//============================================================================//
class TransportStressTests : public juce::UnitTest
{
public:
    TransportStressTests() : juce::UnitTest ("TransportStress", "Phase2") {}

    void runTest() override
    {
        beginTest ("LearnHitQueue SPSC stress keeps FIFO order and intact windows");
        {
            LearnHitQueue q;
            q.prepare (8000.0, 24, 5.0f, 20.0f);   // pre=40, post=160, win=200
            const int pre = q.getPreRollSamples();
            const int win = q.getWindowSamples();
            const int numHits = 600;

            std::atomic<bool> producerDone { false };
            std::atomic<bool> torn { false };
            std::vector<int> received;
            received.reserve ((size_t) numHits);

            std::thread producer ([&]
            {
                int g = 0;
                for (int h = 0; h < numHits; ++h)
                {
                    const int trigger = g + pre;
                    for (int k = 0; k < win; ++k)
                    {
                        const bool trig = (g == trigger);
                        q.pushSample ((float) g, (float) g + 0.5f, trig, (float) h);
                        ++g;
                    }
                }
                producerDone.store (true);
            });

            std::thread consumer ([&]
            {
                LearnHitWindow w;
                auto drainOne = [&]
                {
                    if (! q.pop (w))
                        return false;
                    if ((int) w.bass.size() != win)
                        torn.store (true);
                    // Each window is a contiguous ramp; a torn window would break it.
                    else
                        for (int k = 1; k < win; ++k)
                            if (std::abs ((w.bass[(size_t) k] - w.bass[(size_t) (k - 1)]) - 1.0f) > 1.0e-3f)
                            { torn.store (true); break; }
                    received.push_back (w.sequence);
                    return true;
                };

                while (! producerDone.load())
                    if (! drainOne())
                        std::this_thread::yield();
                while (drainOne()) {}
            });

            producer.join();
            consumer.join();

            expect (! torn.load(), "no torn windows");
            expectEquals ((int) received.size(), q.getAcceptedHitCount());
            expectEquals (q.getAcceptedHitCount() + q.getDroppedHitCount(), numHits);
            // Accepted hits carry contiguous sequences delivered strictly in order.
            bool ordered = true;
            for (size_t i = 0; i < received.size(); ++i)
                if (received[i] != (int) i)
                    ordered = false;
            expect (ordered, "FIFO order preserved under stress");
            expectGreaterThan ((int) received.size(), 0);
        }

        beginTest ("NoteMapUpdateQueue SPSC stress delivers complete snapshots in order");
        {
            NoteMapUpdateQueue q;
            q.prepare (4);
            const int numUpdates = 2000;

            std::atomic<bool> producerDone { false };
            std::atomic<bool> corrupt { false };
            std::vector<int> received;

            std::thread producer ([&]
            {
                for (int i = 0; i < numUpdates; ++i)
                    while (! q.push (makeValidMap (100.0f, (float) i)))
                        std::this_thread::yield();   // retry until it fits (message-thread style)
                producerDone.store (true);
            });

            std::thread consumer ([&]
            {
                NotePhaseMapSnapshot out;
                auto drainOne = [&]
                {
                    if (! q.pop (out))
                        return false;
                    if (! out.valid || out.notes[(size_t) NotePhaseMapSnapshot::indexForMidi (33)].learned == false)
                        corrupt.store (true);
                    received.push_back ((int) std::lround (out.base.delayMs));
                    return true;
                };
                while (! producerDone.load())
                    if (! drainOne())
                        std::this_thread::yield();
                while (drainOne()) {}
            });

            producer.join();
            consumer.join();

            expect (! corrupt.load(), "no torn / partial snapshots");
            expectEquals ((int) received.size(), numUpdates);
            bool ordered = true;
            for (size_t i = 0; i < received.size(); ++i)
                if (received[i] != (int) i)
                    ordered = false;
            expect (ordered, "FIFO order preserved");
        }
    }
};

//============================================================================//
// Processor transport plumbing integration
//============================================================================//
class TransportProcessorTests : public juce::UnitTest
{
public:
    TransportProcessorTests() : juce::UnitTest ("TransportProcessor", "Phase2") {}

    void runTest() override
    {
        beginTest ("Message->audio map publication is drained into the audio-owned active map");
        {
            KickLockAudioProcessor p;
            p.enableAllBuses();
            p.setRateAndBufferSizeDetails (kSampleRate, 512);
            p.prepareToPlay (kSampleRate, 512);

            expect (! p.getActiveNoteMapForTesting().valid, "no active map initially");

            const auto m = makeValidMap (142.0f, -3.1f);
            expect (p.publishNoteMapForTesting (m));
            expectEquals (p.getPendingMapUpdates(), 1);

            juce::AudioBuffer<float> buffer (juce::jmax (p.getTotalNumInputChannels(),
                                                         p.getTotalNumOutputChannels()), 512);
            buffer.clear();
            juce::MidiBuffer midi;
            p.processBlock (buffer, midi);

            expectEquals (p.getPendingMapUpdates(), 0, "drained by processBlock");
            expect (p.getActiveNoteMapForTesting().valid);
            expectWithinAbsoluteError (p.getActiveNoteMapForTesting().base.crossoverHz, 142.0f, 1.0e-3f);
        }

        beginTest ("Learn capture stays inert until activated, then delivers hits");
        {
            const double sr = kSampleRate;
            const int block = 512;

            auto run = [sr, block] (bool learnActive)
            {
                KickLockAudioProcessor p;
                p.enableAllBuses();
                p.setRateAndBufferSizeDetails (sr, block);
                p.prepareToPlay (sr, block);
                if (auto* c = p.apvts.getParameter ("crossover_enable")) c->setValueNotifyingHost (0.0f);
                p.setLearnActiveForTesting (learnActive);

                const int totalSamples = (int) std::round (sr * 0.6);   // 600 ms
                int produced = 0;
                for (int offset = 0; offset < totalSamples; offset += block)
                {
                    const int n = std::min (block, totalSamples - offset);
                    juce::AudioBuffer<float> buffer (juce::jmax (p.getTotalNumInputChannels(),
                                                                 p.getTotalNumOutputChannels()), n);
                    buffer.clear();
                    for (int i = 0; i < n; ++i)
                    {
                        const int g = offset + i;
                        const double t = (double) g / sr;
                        const float bass = 0.4f * (float) std::sin (kTwoPi * 55.0 * t);
                        // A sharp kick burst every 200 ms.
                        const int inBeat = g % (int) std::round (sr * 0.2);
                        const float env = std::exp (-(float) inBeat / (0.004f * (float) sr));
                        const float kick = env * (float) std::sin (kTwoPi * 60.0 * t);
                        buffer.setSample (0, i, bass);
                        if (buffer.getNumChannels() > 1) buffer.setSample (1, i, bass);
                        if (buffer.getNumChannels() > 2) buffer.setSample (2, i, kick);
                        if (buffer.getNumChannels() > 3) buffer.setSample (3, i, kick);
                    }
                    juce::MidiBuffer midi;
                    p.processBlock (buffer, midi);
                    juce::ignoreUnused (produced);
                }
                return p.getLearnAcceptedHits();
            };

            expectEquals (run (false), 0, "inactive: no Learn captures");
            expectGreaterOrEqual (run (true), 1, "active: at least one hit captured");
        }
    }
};

static LearnHitQueueTests learnHitQueueTests;
static NoteMapUpdateQueueTests noteMapUpdateQueueTests;
static TransportStressTests transportStressTests;
static TransportProcessorTests transportProcessorTests;
