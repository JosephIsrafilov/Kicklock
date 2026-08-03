#include "TestCommon.h"
#include "ui/DynamicStateEditThrottle.h"

class DynamicStateEditThrottleTests : public juce::UnitTest
{
public:
    DynamicStateEditThrottleTests()
        : juce::UnitTest ("DynamicStateEditThrottle", "Dynamic State Map") {}

    void runTest() override
    {
        beginTest ("No pending edit initially");
        {
            DynamicStateEditThrottle throttle;
            expect (! throttle.hasPendingEdit());
        }

        beginTest ("setDraft marks a pending edit; takePending clears it and returns the value");
        {
            DynamicStateEditThrottle throttle;
            const DynamicManualTrim trim { 1.5f, -2.0f, 0.3f };
            throttle.setDraft (trim);
            expect (throttle.hasPendingEdit());

            const auto taken = throttle.takePending();
            expect (! throttle.hasPendingEdit());
            expectEquals (taken.delayTrimMs, 1.5f);
            expectEquals (taken.frequencyTrimSemitones, -2.0f);
            expectEquals (taken.logPoleDampingTrim, 0.3f);
        }

        beginTest ("Rapid successive setDraft calls: only the newest value survives (no queue)");
        {
            DynamicStateEditThrottle throttle;
            for (int i = 0; i < 50; ++i)
                throttle.setDraft ({ (float) i * 0.01f, 0.0f, 0.0f });
            expect (throttle.hasPendingEdit());
            const auto taken = throttle.takePending();
            expectWithinAbsoluteError (taken.delayTrimMs, 0.49f, 1.0e-5f);
            expect (! throttle.hasPendingEdit());
        }

        beginTest ("cancel() discards a pending draft without publishing");
        {
            DynamicStateEditThrottle throttle;
            throttle.setDraft ({ 2.0f, 0.0f, 0.0f });
            throttle.cancel();
            expect (! throttle.hasPendingEdit());
        }
    }
};

static DynamicStateEditThrottleTests dynamicStateEditThrottleTests;
