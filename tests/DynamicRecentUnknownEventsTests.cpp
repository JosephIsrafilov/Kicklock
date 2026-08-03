#include "TestCommon.h"
#include "dsp/DynamicRecentUnknownEvents.h"

namespace
{
    DynamicFingerprintPrototype prototypeFor (float seed)
    {
        DynamicFingerprintPrototype p;
        p.valid = true;
        p.featureCount = (uint8_t) DynamicStateMapContract::kMaxFingerprintFeatures;
        for (int i = 0; i < DynamicStateMapContract::kMaxFingerprintFeatures; ++i)
            p.features[(size_t) i] = std::clamp (seed + 0.01f * (float) i, -0.99f, 0.99f);
        return p;
    }

    DynamicRecentUnknownRawEvent eventFor (float seed, int64_t triggerSample, uint64_t mapGeneration = 1)
    {
        DynamicRecentUnknownRawEvent event;
        event.valid = true;
        event.fingerprint = prototypeFor (seed);
        event.outcome = DynamicRecentUnknownOutcome::Unknown;
        event.nearestDistance = 0.5f;
        event.triggerSample = triggerSample;
        event.mapGeneration = mapGeneration;
        return event;
    }

    // 32 distinct 3-element subsets of {0..7}, each rendered as a fingerprint
    // with those three feature positions at +0.99 and the rest at -0.99.
    // Any two distinct same-size subsets differ in at least 2 positions, so
    // the normalized L1 distance between any two of these prototypes is at
    // least 2*1.98/(2*8) = 0.2475 - comfortably above
    // kCoalesceDistanceThreshold (0.12), guaranteeing all 32 are genuinely
    // distinct clusters rather than relying on a fragile margin.
    constexpr int kDistinctSubsets[32][3] = {
        {0,1,2}, {0,1,3}, {0,1,4}, {0,1,5}, {0,1,6}, {0,1,7},
        {0,2,3}, {0,2,4}, {0,2,5}, {0,2,6}, {0,2,7},
        {0,3,4}, {0,3,5}, {0,3,6}, {0,3,7},
        {0,4,5}, {0,4,6}, {0,4,7},
        {0,5,6}, {0,5,7}, {0,6,7},
        {1,2,3}, {1,2,4}, {1,2,5}, {1,2,6}, {1,2,7},
        {1,3,4}, {1,3,5}, {1,3,6}, {1,3,7},
        {1,4,5}, {1,4,6}
    };

    DynamicFingerprintPrototype distinctPrototype (int index)
    {
        DynamicFingerprintPrototype p;
        p.valid = true;
        p.featureCount = 8;
        for (int i = 0; i < 8; ++i)
            p.features[(size_t) i] = -0.99f;
        for (int slot = 0; slot < 3; ++slot)
            p.features[(size_t) kDistinctSubsets[index][slot]] = 0.99f;
        return p;
    }

    DynamicRecentUnknownRawEvent distinctEvent (int index, int64_t triggerSample)
    {
        DynamicRecentUnknownRawEvent event;
        event.valid = true;
        event.fingerprint = distinctPrototype (index);
        event.outcome = DynamicRecentUnknownOutcome::Unknown;
        event.nearestDistance = 0.5f;
        event.triggerSample = triggerSample;
        event.mapGeneration = 1;
        return event;
    }
}

class DynamicRecentUnknownEventsTests : public juce::UnitTest
{
public:
    DynamicRecentUnknownEventsTests()
        : juce::UnitTest ("DynamicRecentUnknownEvents", "Dynamic State Map") {}

    void runTest() override
    {
        beginTest ("Repeatable similar events coalesce into one cluster with an incrementing repeat count");
        {
            DynamicRecentUnknownEventLog log;
            expect (log.ingest (eventFor (0.2f, 1000)));
            expect (log.ingest (eventFor (0.202f, 2000))); // within coalesce threshold
            expect (log.ingest (eventFor (0.198f, 3000)));

            expectEquals (log.getClusterCount(), 1);
            const auto cluster = log.getCluster (0);
            expectEquals ((int) cluster.repeatCount, 3);
            expectEquals ((int64_t) cluster.lastTriggerSample, (int64_t) 3000);
        }

        beginTest ("Distinct fingerprints form separate clusters");
        {
            DynamicRecentUnknownEventLog log;
            expect (log.ingest (eventFor (0.2f, 1000)));
            expect (log.ingest (eventFor (-0.6f, 2000)));
            expectEquals (log.getClusterCount(), 2);
        }

        beginTest ("A single arbitrary Unknown event never becomes a persistent State on its own");
        {
            // This is a log-level property check: ingest() only ever records
            // repeat counts - the decision to create a Manual State (Section
            // 15's "at least 3 repeatable hits") is enforced entirely by
            // DynamicStateEditTransaction::createManualState(), which is
            // covered separately in DynamicStateEditTransactionTests.cpp.
            // Here we just confirm a single event's repeatCount is 1, not
            // silently inflated.
            DynamicRecentUnknownEventLog log;
            log.ingest (eventFor (0.3f, 500));
            expectEquals ((int) log.getCluster (0).repeatCount, 1);
        }

        beginTest ("Overflow beyond capacity is dropped with a diagnostic, not silently evicting an existing cluster");
        {
            DynamicRecentUnknownEventLog log;
            for (int i = 0; i < DynamicRecentUnknownContract::kMaxRecentUnknownClusters; ++i)
                expect (log.ingest (distinctEvent (i, i * 1000)));
            expectEquals (log.getClusterCount(), DynamicRecentUnknownContract::kMaxRecentUnknownClusters);

            const auto firstClusterBefore = log.getCluster (0);
            // {1,4,7}: not among the 32 chosen subsets above and far (Hamming
            // distance >= 2) from every one of them.
            DynamicFingerprintPrototype farPrototype;
            farPrototype.valid = true;
            farPrototype.featureCount = 8;
            for (int i = 0; i < 8; ++i)
                farPrototype.features[(size_t) i] = -0.99f;
            farPrototype.features[1] = 0.99f;
            farPrototype.features[4] = 0.99f;
            farPrototype.features[7] = 0.99f;
            DynamicRecentUnknownRawEvent farEvent;
            farEvent.valid = true;
            farEvent.fingerprint = farPrototype;
            farEvent.outcome = DynamicRecentUnknownOutcome::Unknown;
            farEvent.nearestDistance = 0.5f;
            farEvent.triggerSample = 999999;
            farEvent.mapGeneration = 1;
            const bool accepted = log.ingest (farEvent);
            expect (! accepted);
            expectEquals (log.getClusterCount(), DynamicRecentUnknownContract::kMaxRecentUnknownClusters);
            expectEquals ((uint64_t) log.getOverflowCount(), (uint64_t) 1);
            const auto firstClusterAfter = log.getCluster (0);
            expectEquals ((int64_t) firstClusterAfter.lastTriggerSample, (int64_t) firstClusterBefore.lastTriggerSample);
        }

        beginTest ("removeCluster removes exactly the requested cluster and preserves the others");
        {
            DynamicRecentUnknownEventLog log;
            log.ingest (eventFor (0.1f, 100));
            log.ingest (eventFor (0.4f, 200));
            log.ingest (eventFor (0.7f, 300));
            expectEquals (log.getClusterCount(), 3);

            const auto middleId = log.getCluster (1).eventId;
            expect (log.removeCluster (middleId));
            expectEquals (log.getClusterCount(), 2);
            expect (log.getCluster (0).lastTriggerSample == 100);
            expect (log.getCluster (1).lastTriggerSample == 300);

            expect (! log.removeCluster (middleId), "removing an already-removed id fails, not a no-op success");
        }

        beginTest ("clear() empties the log and resets overflow diagnostics");
        {
            DynamicRecentUnknownEventLog log;
            log.ingest (eventFor (0.1f, 100));
            log.clear();
            expectEquals (log.getClusterCount(), 0);
            expectEquals ((uint64_t) log.getOverflowCount(), (uint64_t) 0);
        }

        beginTest ("An invalid event is rejected without mutating the log");
        {
            DynamicRecentUnknownEventLog log;
            DynamicRecentUnknownRawEvent invalidEvent;
            invalidEvent.valid = false;
            expect (! log.ingest (invalidEvent));
            expectEquals (log.getClusterCount(), 0);
        }
    }
};

static DynamicRecentUnknownEventsTests dynamicRecentUnknownEventsTests;
