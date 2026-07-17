#include "TestCommon.h"

#include "DynamicSelectorScheduler.h"
#include "DynamicContinuityMixer.h"

#include <array>
#include <cmath>
#include <limits>

namespace
{
    constexpr std::array<double, 5> kSelectorSampleRates { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };

    DynamicHotBranchInfo makeBranchInfo (bool active, DynamicHotBranchKind kind, uint64_t stableId,
                                         double physicalTapSamples, bool warm)
    {
        DynamicHotBranchInfo info;
        info.active = active;
        info.kind = kind;
        info.stableStateId = stableId;
        info.physicalTapSamples = physicalTapSamples;
        info.coefficientsValid = true;
        info.warm = warm;
        return info;
    }

    // Builds a roster with Global always active/warm at a nominal 20 ms tap,
    // optionally one active State slot and/or an active Service branch.
    DynamicSelectorBranchRoster makeRoster (double sampleRate,
                                            double globalTapSamples,
                                            int activeStateSlot = -1,
                                            uint64_t activeStateId = 0,
                                            bool stateWarm = true,
                                            double stateTapSamples = -1.0,
                                            bool serviceActive = false,
                                            uint64_t serviceBoundId = 0,
                                            bool serviceWarm = true,
                                            bool serviceBindingValid = false,
                                            double serviceTapSamples = -1.0)
    {
        DynamicSelectorBranchRoster roster;
        roster.global = makeBranchInfo (true, DynamicHotBranchKind::Global, 0, globalTapSamples, true);

        for (int i = 0; i < DynamicSelectorContract::kStateSlotCount; ++i)
            roster.states[(size_t) i] = makeBranchInfo (false, DynamicHotBranchKind::State, 0, 0.0, false);

        if (activeStateSlot >= 0)
        {
            const double tap = stateTapSamples >= 0.0 ? stateTapSamples : globalTapSamples;
            roster.states[(size_t) activeStateSlot] =
                makeBranchInfo (true, DynamicHotBranchKind::State, activeStateId, tap, stateWarm);
        }

        const double svcTap = serviceTapSamples >= 0.0 ? serviceTapSamples : globalTapSamples;
        roster.service = makeBranchInfo (serviceActive, DynamicHotBranchKind::Service, 0, svcTap, serviceWarm);
        roster.serviceBoundStableStateId = serviceBoundId;
        roster.serviceBindingValid = serviceBindingValid;

        juce::ignoreUnused (sampleRate);
        return roster;
    }

    DynamicMatchResult makeMatched (uint64_t id, bool correctionAvailable = true, bool bypassed = false)
    {
        DynamicMatchResult r;
        r.decision = DynamicMatchDecision::Matched;
        r.selectedStableStateId = id;
        r.nearestStableStateId = id;
        r.nearestDistance = 0.0f;
        r.correctionAvailable = correctionAvailable;
        r.selectedBypassed = bypassed;
        r.eligibleStateCount = 1;
        return r;
    }

    DynamicMatchResult makeDecisionOnly (DynamicMatchDecision decision, uint64_t nearestId = 0)
    {
        DynamicMatchResult r;
        r.decision = decision;
        r.nearestStableStateId = nearestId;
        r.eligibleStateCount = decision == DynamicMatchDecision::NoEligibleStates ? 0 : 1;
        return r;
    }

    DynamicSelectorEvent makeEvent (int64_t triggerSample, int64_t fingerprintSamples,
                                    const DynamicMatchResult& match)
    {
        DynamicSelectorEvent event;
        event.triggerSample = triggerSample;
        event.readySample = triggerSample + fingerprintSamples;
        event.match = match;
        return event;
    }

    float gainSum (const std::array<float, DynamicSelectorContract::kBranchCount>& gains)
    {
        float sum = 0.0f;
        for (float g : gains)
            sum += g;
        return sum;
    }

    bool allGainsInRange (const std::array<float, DynamicSelectorContract::kBranchCount>& gains)
    {
        for (float g : gains)
            if (! std::isfinite (g) || g < -1.0e-6f || g > 1.0f + 1.0e-6f)
                return false;
        return true;
    }
}

class DynamicSelectorSchedulerTests : public juce::UnitTest
{
public:
    DynamicSelectorSchedulerTests() : juce::UnitTest ("DynamicSelectorScheduler", "DSP") {}

    void runTest() override
    {
        testPrepareAndTimingConstants();
        testSampleAccurateEventPlacement();
        testMatchTargetResolution();
        testHoldEventBound();
        testHoldTimeBound();
        testLookAhead();
        testLinearGainEndpoints();
        testMidFadeRetargeting();
        testRepeatedEventsOnOneSample();
        testFullBandMixer();
        testStaleIdentity();
        testServiceIdentity();
        testTransportDiscontinuity();
        testNonFiniteRecovery();
        testRealTimeSafetyStructural();
    }

    // ---------------------------------------------------------------- A ----
    void testPrepareAndTimingConstants()
    {
        beginTest ("Prepare computes valid timing constants at every supported rate");
        for (double sr : kSelectorSampleRates)
        {
            DynamicSelectorScheduler scheduler;
            expect (scheduler.prepare (sr));
            expect (scheduler.getTransitionSamples() > 0);

            const double transitionMs = 1000.0 * (double) scheduler.getTransitionSamples() / sr;
            expect (transitionMs <= (double) DynamicStateMapContract::kMaximumTransitionMs + 1.0e-6);

            const double safetyMs = 1000.0 * (double) scheduler.getSafetySamples() / sr;
            expect (safetyMs >= (double) DynamicStateMapContract::kSafetyMarginMs - 1.0e-9);

            const double holdMs = 1000.0 * (double) scheduler.getHoldSamples() / sr;
            expect (holdMs <= (double) DynamicStateMapContract::kMaximumHoldTimeMs + 1.0e-6);

            const auto window = DynamicFingerprintWindow::forSampleRate (sr);
            expect (scheduler.getFingerprintSamples() == (int64_t) window.windowSamples);

            // Exact 13 ms minimum physical tap boundary must be satisfiable:
            // fingerprint + transition + safety <= floor(sr * 13ms).
            const int64_t minOutputOffset = (int64_t) std::floor (sr * (double) DynamicHotBranchContract::kMinimumPhysicalTapMs / 1000.0);
            expect (scheduler.getFingerprintSamples() + scheduler.getTransitionSamples() + scheduler.getSafetySamples()
                    <= minOutputOffset);
        }

        beginTest ("Invalid sample rates are rejected");
        {
            DynamicSelectorScheduler scheduler;
            expect (! scheduler.prepare (0.0));
            expect (! scheduler.prepare (-48000.0));
            expect (! scheduler.prepare (std::numeric_limits<double>::quiet_NaN()));
            expect (! scheduler.prepare (100.0)); // far below any usable Nyquist for the allpass range
        }
    }

    // ---------------------------------------------------------------- B ----
    void testSampleAccurateEventPlacement()
    {
        beginTest ("Identical absolute transition position regardless of block partition");
        const double sr = 48000.0;
        const std::array<int, 5> blockSizes { 1, 7, 64, 127, 512 };

        std::array<int64_t, 5> completionSamples {};
        for (size_t b = 0; b < blockSizes.size(); ++b)
        {
            DynamicSelectorScheduler scheduler;
            expect (scheduler.prepare (sr));
            const int64_t globalTap = (int64_t) std::ceil (sr * 20.0 / 1000.0);
            auto roster = makeRoster (sr, (double) globalTap, 0, 100, true, (double) globalTap + 200.0);

            const int64_t triggerSample = 1000;
            auto event = makeEvent (triggerSample, scheduler.getFingerprintSamples(), makeMatched (100));
            expect (scheduler.submitEvent (event));

            const int64_t totalSamples = triggerSample + scheduler.getFingerprintSamples()
                + scheduler.getTransitionSamples() + 50;
            int64_t sample = 0;
            int64_t observedCompletion = -1;
            while (sample < totalSamples)
            {
                const int n = juce::jmin (blockSizes[b], (int) (totalSamples - sample));
                for (int i = 0; i < n; ++i)
                {
                    expect (scheduler.advanceSample (sample, roster));
                    if (observedCompletion < 0 && ! scheduler.getDiagnostics().fadeActive
                        && scheduler.getDiagnostics().selectedStateSlot == 0)
                        observedCompletion = sample;
                    ++sample;
                }
            }
            completionSamples[b] = observedCompletion;
        }

        for (size_t b = 1; b < blockSizes.size(); ++b)
            expect (completionSamples[b] == completionSamples[0]);
    }

    // ---------------------------------------------------------------- C ----
    void testMatchTargetResolution()
    {
        const double sr = 48000.0;
        const int64_t globalTap = (int64_t) std::ceil (sr * 20.0 / 1000.0);

        beginTest ("Matched warm persistent State is selected");
        {
            DynamicSelectorScheduler scheduler;
            expect (scheduler.prepare (sr));
            auto roster = makeRoster (sr, (double) globalTap, 2, 55, true, (double) globalTap + 100.0);
            runEventToCompletion (scheduler, roster, makeMatched (55), globalTap + 500);
            expect (scheduler.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::State);
            expect (scheduler.getDiagnostics().selectedStateSlot == 2);
            expect (scheduler.getDiagnostics().lastDecision == DynamicSelectorDiagnostic::MatchedPersistentState);
        }

        beginTest ("Matched cold/inactive persistent State falls back to Global (TargetNotWarm)");
        {
            DynamicSelectorScheduler scheduler;
            expect (scheduler.prepare (sr));
            auto roster = makeRoster (sr, (double) globalTap, 2, 55, false, (double) globalTap + 100.0);
            runEventToCompletion (scheduler, roster, makeMatched (55), globalTap + 500);
            expect (scheduler.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::Global);
            expect (scheduler.getDiagnostics().lastDecision == DynamicSelectorDiagnostic::FallbackTargetNotWarm);
        }

        beginTest ("Matched with no active State at all falls back to Global (TargetUnavailable)");
        {
            DynamicSelectorScheduler scheduler;
            expect (scheduler.prepare (sr));
            auto roster = makeRoster (sr, (double) globalTap);
            runEventToCompletion (scheduler, roster, makeMatched (55), globalTap + 500);
            expect (scheduler.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::Global);
            expect (scheduler.getDiagnostics().lastDecision == DynamicSelectorDiagnostic::FallbackTargetUnavailable);
        }

        beginTest ("Matched Service fallback when no persistent State represents the identity");
        {
            DynamicSelectorScheduler scheduler;
            expect (scheduler.prepare (sr));
            auto roster = makeRoster (sr, (double) globalTap, -1, 0, true, -1.0,
                                      true, 77, true, true, (double) globalTap + 300.0);
            runEventToCompletion (scheduler, roster, makeMatched (77), globalTap + 600);
            expect (scheduler.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::Service);
            expect (scheduler.getDiagnostics().lastDecision == DynamicSelectorDiagnostic::MatchedService);
        }

        beginTest ("Persistent State branch has priority over Service for the same identity");
        {
            DynamicSelectorScheduler scheduler;
            expect (scheduler.prepare (sr));
            auto roster = makeRoster (sr, (double) globalTap, 3, 90, true, (double) globalTap + 100.0,
                                      true, 90, true, true, (double) globalTap + 300.0);
            runEventToCompletion (scheduler, roster, makeMatched (90), globalTap + 600);
            expect (scheduler.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::State);
            expect (scheduler.getDiagnostics().selectedStateSlot == 3);
        }

        beginTest ("Matched with correction unavailable goes to Global, preserves recognized ID, no Hold");
        {
            DynamicSelectorScheduler scheduler;
            expect (scheduler.prepare (sr));
            auto roster = makeRoster (sr, (double) globalTap, 2, 55, true, (double) globalTap + 100.0);
            runEventToCompletion (scheduler, roster, makeMatched (55, false, false), globalTap + 500);
            expect (scheduler.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::Global);
            expect (scheduler.getDiagnostics().lastDecision == DynamicSelectorDiagnostic::MatchedGlobalNoCorrection);
            expect (scheduler.getDiagnostics().recognizedStableStateId == 55);
        }

        beginTest ("Matched bypassed State goes to Global, preserves recognized ID, clears Hold");
        {
            DynamicSelectorScheduler scheduler;
            expect (scheduler.prepare (sr));
            auto roster = makeRoster (sr, (double) globalTap, 2, 55, true, (double) globalTap + 100.0);
            runEventToCompletion (scheduler, roster, makeMatched (55, true, true), globalTap + 500);
            expect (scheduler.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::Global);
            expect (scheduler.getDiagnostics().lastDecision == DynamicSelectorDiagnostic::MatchedGlobalBypassed);
            expect (scheduler.getDiagnostics().recognizedStableStateId == 55);
        }

        beginTest ("Missing stable ID (zero) never resolves to a State/Service branch");
        {
            DynamicSelectorScheduler scheduler;
            expect (scheduler.prepare (sr));
            auto roster = makeRoster (sr, (double) globalTap, 2, 55, true, (double) globalTap + 100.0);
            auto match = makeDecisionOnly (DynamicMatchDecision::NoEligibleStates);
            runEventToCompletion (scheduler, roster, match, globalTap + 500);
            expect (scheduler.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::Global);
        }
    }

    // ---------------------------------------------------------------- D ----
    void testHoldEventBound()
    {
        beginTest ("Ambiguous Hold retains State for two events, expires on the third");
        const double sr = 48000.0;
        const int64_t globalTap = (int64_t) std::ceil (sr * 20.0 / 1000.0);
        DynamicSelectorScheduler scheduler;
        expect (scheduler.prepare (sr));
        auto roster = makeRoster (sr, (double) globalTap, 1, 42, true, (double) globalTap + 100.0);

        runEventToCompletion (scheduler, roster, makeMatched (42), globalTap + 500);
        expect (scheduler.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::State);

        int64_t t = scheduler.getExpectedNextSample() + 10;
        submitAndAdvanceThrough (scheduler, roster, makeDecisionOnly (DynamicMatchDecision::Ambiguous), t);
        expect (scheduler.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::State);
        expect (scheduler.getDiagnostics().lastDecision == DynamicSelectorDiagnostic::HeldAmbiguous);
        expect (scheduler.getDiagnostics().holdEventCount == 1);

        t = scheduler.getExpectedNextSample() + 10;
        submitAndAdvanceThrough (scheduler, roster, makeDecisionOnly (DynamicMatchDecision::Ambiguous), t);
        expect (scheduler.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::State);
        expect (scheduler.getDiagnostics().holdEventCount == 2);

        t = scheduler.getExpectedNextSample() + 10;
        submitAndAdvanceThrough (scheduler, roster, makeDecisionOnly (DynamicMatchDecision::Ambiguous), t);
        expect (scheduler.getDiagnostics().lastDecision == DynamicSelectorDiagnostic::FallbackAmbiguous);

        beginTest ("Unknown Hold obeys the same two-event bound");
        DynamicSelectorScheduler scheduler2;
        expect (scheduler2.prepare (sr));
        auto roster2 = makeRoster (sr, (double) globalTap, 1, 42, true, (double) globalTap + 100.0);
        runEventToCompletion (scheduler2, roster2, makeMatched (42), globalTap + 500);

        t = scheduler2.getExpectedNextSample() + 10;
        submitAndAdvanceThrough (scheduler2, roster2, makeDecisionOnly (DynamicMatchDecision::Unknown), t);
        expect (scheduler2.getDiagnostics().holdEventCount == 1);
        t = scheduler2.getExpectedNextSample() + 10;
        submitAndAdvanceThrough (scheduler2, roster2, makeDecisionOnly (DynamicMatchDecision::Unknown), t);
        expect (scheduler2.getDiagnostics().holdEventCount == 2);
        t = scheduler2.getExpectedNextSample() + 10;
        submitAndAdvanceThrough (scheduler2, roster2, makeDecisionOnly (DynamicMatchDecision::Unknown), t);
        expect (scheduler2.getDiagnostics().lastDecision == DynamicSelectorDiagnostic::FallbackUnknown);

        beginTest ("Mixed Ambiguous/Unknown events share one bounded unresolved count");
        DynamicSelectorScheduler scheduler3;
        expect (scheduler3.prepare (sr));
        auto roster3 = makeRoster (sr, (double) globalTap, 1, 42, true, (double) globalTap + 100.0);
        runEventToCompletion (scheduler3, roster3, makeMatched (42), globalTap + 500);

        t = scheduler3.getExpectedNextSample() + 10;
        submitAndAdvanceThrough (scheduler3, roster3, makeDecisionOnly (DynamicMatchDecision::Ambiguous), t);
        expect (scheduler3.getDiagnostics().holdEventCount == 1);
        t = scheduler3.getExpectedNextSample() + 10;
        submitAndAdvanceThrough (scheduler3, roster3, makeDecisionOnly (DynamicMatchDecision::Unknown), t);
        expect (scheduler3.getDiagnostics().holdEventCount == 2);
        t = scheduler3.getExpectedNextSample() + 10;
        submitAndAdvanceThrough (scheduler3, roster3, makeDecisionOnly (DynamicMatchDecision::Ambiguous), t);
        expect (scheduler3.getDiagnostics().lastDecision == DynamicSelectorDiagnostic::FallbackAmbiguous);
    }

    // ---------------------------------------------------------------- E ----
    void testHoldTimeBound()
    {
        beginTest ("Event exactly at 250 ms boundary is held; one sample after falls back");
        const double sr = 48000.0;
        const int64_t globalTap = (int64_t) std::ceil (sr * 20.0 / 1000.0);

        {
            DynamicSelectorScheduler scheduler;
            expect (scheduler.prepare (sr));
            auto roster = makeRoster (sr, (double) globalTap, 1, 42, true, (double) globalTap + 100.0);
            runEventToCompletion (scheduler, roster, makeMatched (42), globalTap + 500);

            const int64_t confidentReady = scheduler.getDiagnostics().readySample;
            const int64_t boundaryTrigger = confidentReady + scheduler.getHoldSamples() - scheduler.getFingerprintSamples();
            submitAndAdvanceThroughTrigger (scheduler, roster, makeDecisionOnly (DynamicMatchDecision::Ambiguous), boundaryTrigger);
            expect (scheduler.getDiagnostics().lastDecision == DynamicSelectorDiagnostic::HeldAmbiguous);
        }
        {
            DynamicSelectorScheduler scheduler;
            expect (scheduler.prepare (sr));
            auto roster = makeRoster (sr, (double) globalTap, 1, 42, true, (double) globalTap + 100.0);
            runEventToCompletion (scheduler, roster, makeMatched (42), globalTap + 500);

            const int64_t confidentReady = scheduler.getDiagnostics().readySample;
            const int64_t lateTrigger = confidentReady + scheduler.getHoldSamples() - scheduler.getFingerprintSamples() + 1;
            submitAndAdvanceThroughTrigger (scheduler, roster, makeDecisionOnly (DynamicMatchDecision::Ambiguous), lateTrigger);
            expect (scheduler.getDiagnostics().lastDecision == DynamicSelectorDiagnostic::FallbackAmbiguous);
        }

        beginTest ("A new confident match refreshes the Hold timer");
        {
            DynamicSelectorScheduler scheduler;
            expect (scheduler.prepare (sr));
            auto roster = makeRoster (sr, (double) globalTap, 1, 42, true, (double) globalTap + 100.0);
            runEventToCompletion (scheduler, roster, makeMatched (42), globalTap + 500);
            const int64_t firstConfident = scheduler.getDiagnostics().readySample;

            const int64_t t2 = scheduler.getExpectedNextSample() + 1000;
            submitAndAdvanceThroughTrigger (scheduler, roster, makeMatched (42), t2);
            expect (scheduler.getDiagnostics().holdEventCount == 0);
            expect (scheduler.getDiagnostics().readySample > firstConfident);
        }

        beginTest ("Bypass/no-fix clears Hold");
        {
            DynamicSelectorScheduler scheduler;
            expect (scheduler.prepare (sr));
            auto roster = makeRoster (sr, (double) globalTap, 1, 42, true, (double) globalTap + 100.0);
            runEventToCompletion (scheduler, roster, makeMatched (42), globalTap + 500);
            submitAndAdvanceThrough (scheduler, roster, makeMatched (42, true, true), scheduler.getExpectedNextSample() + 10);
            expect (scheduler.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::Global);
            expect (scheduler.getDiagnostics().holdEventCount == 0);
        }

        beginTest ("InvalidInput clears Hold");
        {
            DynamicSelectorScheduler scheduler;
            expect (scheduler.prepare (sr));
            auto roster = makeRoster (sr, (double) globalTap, 1, 42, true, (double) globalTap + 100.0);
            runEventToCompletion (scheduler, roster, makeMatched (42), globalTap + 500);
            submitAndAdvanceThrough (scheduler, roster, makeDecisionOnly (DynamicMatchDecision::InvalidInput), scheduler.getExpectedNextSample() + 10);
            expect (scheduler.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::Global);
            expect (scheduler.getDiagnostics().holdEventCount == 0);
        }
    }

    // ---------------------------------------------------------------- F ----
    void testLookAhead()
    {
        beginTest ("Exact minimum 13 ms boundary is valid; one sample late is rejected");
        for (double sr : kSelectorSampleRates)
        {
            const int64_t minOutputOffset = (int64_t) std::floor (sr * (double) DynamicHotBranchContract::kMinimumPhysicalTapMs / 1000.0);

            {
                auto roster = makeRoster (sr, (double) minOutputOffset, 0, 1, true, (double) minOutputOffset);
                DynamicSelectorScheduler s2;
                expect (s2.prepare (sr));
                auto event = makeEvent (0, s2.getFingerprintSamples(), makeMatched (1));
                expect (s2.submitEvent (event));
                for (int64_t i = 0; i < event.readySample + 1; ++i)
                    expect (s2.advanceSample (i, roster));
                expect (s2.getDiagnostics().lastDecision != DynamicSelectorDiagnostic::LateDecisionRejected);
            }
            {
                auto roster = makeRoster (sr, (double) minOutputOffset, 0, 1, true, (double) (minOutputOffset - 1));
                DynamicSelectorScheduler s3;
                expect (s3.prepare (sr));
                auto event = makeEvent (0, s3.getFingerprintSamples(), makeMatched (1));
                expect (s3.submitEvent (event));
                for (int64_t i = 0; i < event.readySample + 1; ++i)
                    expect (s3.advanceSample (i, roster));
                expect (s3.getDiagnostics().lastDecision == DynamicSelectorDiagnostic::LateDecisionRejected);
                expect (s3.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::Global);
            }
        }

        beginTest ("A no-op match to the already active State needs no new transition");
        {
            const double sr = 48000.0;
            const int64_t globalTap = (int64_t) std::ceil (sr * 20.0 / 1000.0);
            DynamicSelectorScheduler scheduler;
            expect (scheduler.prepare (sr));
            auto roster = makeRoster (sr, (double) globalTap, 0, 9, true, (double) globalTap + 200.0);
            runEventToCompletion (scheduler, roster, makeMatched (9), globalTap + 500);
            expect (! scheduler.getDiagnostics().fadeActive);

            const int64_t t2 = scheduler.getExpectedNextSample() + 5;
            submitAndAdvanceThrough (scheduler, roster, makeMatched (9), t2);
            expect (! scheduler.getDiagnostics().fadeActive);
        }
    }

    // ---------------------------------------------------------------- G ----
    void testLinearGainEndpoints()
    {
        beginTest ("Initial Global gain is exactly 1; transition endpoints are exact; sum stays 1");
        const double sr = 48000.0;
        const int64_t globalTap = (int64_t) std::ceil (sr * 20.0 / 1000.0);
        DynamicSelectorScheduler scheduler;
        expect (scheduler.prepare (sr));
        auto roster = makeRoster (sr, (double) globalTap, 0, 5, true, (double) globalTap + 300.0);

        auto initialGains = scheduler.getCurrentGains();
        expectWithinAbsoluteError (initialGains[(size_t) DynamicSelectorContract::kGlobalBranchIndex], 1.0f, 1.0e-6f);
        expectWithinAbsoluteError (gainSum (initialGains), 1.0f, 1.0e-5f);

        auto event = makeEvent (0, scheduler.getFingerprintSamples(), makeMatched (5));
        expect (scheduler.submitEvent (event));

        for (int64_t i = 0; i < event.readySample; ++i)
            expect (scheduler.advanceSample (i, roster));

        // The sample exactly at readySample begins the fade: first fade sample
        // equals the exact pre-transition vector.
        expect (scheduler.advanceSample (event.readySample, roster));
        auto firstFadeGains = scheduler.getCurrentGains();
        expectWithinAbsoluteError (firstFadeGains[(size_t) DynamicSelectorContract::kGlobalBranchIndex], 1.0f, 1.0e-6f);

        bool sawNegative = false;
        bool sawOffSum = false;
        int64_t sample = event.readySample + 1;
        while (scheduler.getDiagnostics().fadeActive)
        {
            expect (scheduler.advanceSample (sample, roster));
            const auto gains = scheduler.getCurrentGains();
            if (! allGainsInRange (gains))
                sawNegative = true;
            if (std::abs (gainSum (gains) - 1.0f) > 1.0e-4f)
                sawOffSum = true;
            ++sample;
        }
        expect (! sawNegative);
        expect (! sawOffSum);

        auto finalGains = scheduler.getCurrentGains();
        expectWithinAbsoluteError (finalGains[(size_t) (DynamicSelectorContract::kFirstStateBranchIndex)], 1.0f, 1.0e-6f);

        // After completion, no further drift: repeated advances hold the exact
        // one-hot vector.
        for (int i = 0; i < 100; ++i)
        {
            expect (scheduler.advanceSample (sample, roster));
            const auto gains = scheduler.getCurrentGains();
            expectWithinAbsoluteError (gains[(size_t) DynamicSelectorContract::kFirstStateBranchIndex], 1.0f, 1.0e-6f);
            ++sample;
        }
    }

    // ---------------------------------------------------------------- H ----
    void testMidFadeRetargeting()
    {
        beginTest ("Global -> State A -> (mid-fade) State B -> (mid-fade) Service -> Global stays continuous");
        const double sr = 48000.0;
        const int64_t globalTap = (int64_t) std::ceil (sr * 20.0 / 1000.0);
        DynamicSelectorScheduler scheduler;
        expect (scheduler.prepare (sr));
        auto roster = makeRoster (sr, (double) globalTap, 0, 11, true, (double) globalTap + 400.0,
                                  true, 22, true, true, (double) globalTap + 400.0);
        // Second State slot for State B.
        roster.states[1] = makeBranchInfo (true, DynamicHotBranchKind::State, 33, (double) globalTap + 400.0, true);

        std::array<float, DynamicSelectorContract::kBranchCount> previousGains = scheduler.getCurrentGains();
        bool discontinuity = false;
        bool offSum = false;

        auto stepAndCheck = [&] (int64_t sample)
        {
            expect (scheduler.advanceSample (sample, roster));
            const auto gains = scheduler.getCurrentGains();
            float maxJump = 0.0f;
            for (int i = 0; i < DynamicSelectorContract::kBranchCount; ++i)
                maxJump = std::max (maxJump, std::abs (gains[(size_t) i] - previousGains[(size_t) i]));
            // One transition step may move at most a bit more than 1/transitionSamples;
            // generous bound guards against a hard one-sample jump to a different branch.
            if (maxJump > 0.5f)
                discontinuity = true;
            if (std::abs (gainSum (gains) - 1.0f) > 1.0e-4f)
                offSum = true;
            previousGains = gains;
        };

        auto eventA = makeEvent (0, scheduler.getFingerprintSamples(), makeMatched (11));
        expect (scheduler.submitEvent (eventA));
        int64_t sample = 0;
        while (sample <= eventA.readySample)
            stepAndCheck (sample++);

        // Retarget mid-fade to State B (slot 1) partway through.
        const int64_t midpoint = sample + scheduler.getTransitionSamples() / 4;
        while (sample < midpoint)
            stepAndCheck (sample++);

        auto eventB = makeEvent (sample - scheduler.getFingerprintSamples(), scheduler.getFingerprintSamples(), makeMatched (33));
        expect (scheduler.submitEvent (eventB));
        while (sample <= eventB.readySample)
            stepAndCheck (sample++);

        // Retarget mid-fade to Service before that fade completes.
        const int64_t midpoint2 = sample + scheduler.getTransitionSamples() / 4;
        while (sample < midpoint2)
            stepAndCheck (sample++);

        auto eventService = makeEvent (sample - scheduler.getFingerprintSamples(), scheduler.getFingerprintSamples(), makeMatched (22));
        expect (scheduler.submitEvent (eventService));
        while (sample <= eventService.readySample)
            stepAndCheck (sample++);
        while (scheduler.getDiagnostics().fadeActive)
            stepAndCheck (sample++);

        expect (scheduler.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::Service);

        // Finally retarget to Global.
        auto eventGlobal = makeEvent (sample - scheduler.getFingerprintSamples(), scheduler.getFingerprintSamples(),
                                     makeMatched (22, false, false));
        expect (scheduler.submitEvent (eventGlobal));
        while (sample <= eventGlobal.readySample)
            stepAndCheck (sample++);
        while (scheduler.getDiagnostics().fadeActive)
            stepAndCheck (sample++);

        expect (scheduler.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::Global);
        expect (! discontinuity);
        expect (! offSum);
    }

    // ---------------------------------------------------------------- I ----
    void testRepeatedEventsOnOneSample()
    {
        beginTest ("Deterministic ordering and final target for events sharing one readySample");
        const double sr = 48000.0;
        const int64_t globalTap = (int64_t) std::ceil (sr * 20.0 / 1000.0);
        DynamicSelectorScheduler scheduler;
        expect (scheduler.prepare (sr));
        auto roster = makeRoster (sr, (double) globalTap, 0, 7, true, (double) globalTap + 400.0);
        roster.states[1] = makeBranchInfo (true, DynamicHotBranchKind::State, 8, (double) globalTap + 400.0, true);

        // The fingerprint window is fixed, so two events sharing one readySample
        // necessarily also share the same triggerSample; the only remaining tie
        // break is insertion order. Submit two fully-tied events targeting
        // different States and prove the later-inserted one wins deterministically.
        const int64_t triggerSample = 50;
        auto eventFirstInserted = makeEvent (triggerSample, scheduler.getFingerprintSamples(), makeMatched (7));
        auto eventSecondInserted = makeEvent (triggerSample, scheduler.getFingerprintSamples(), makeMatched (8));

        expect (scheduler.submitEvent (eventFirstInserted));
        expect (scheduler.submitEvent (eventSecondInserted));
        expect (scheduler.getDiagnostics().queuedEventCount == 2);

        for (int64_t i = 0; i <= eventFirstInserted.readySample; ++i)
            expect (scheduler.advanceSample (i, roster));

        // The final processed event (the later insertion, id 8, slot 1) defines
        // the final target for that sample.
        expect (scheduler.getDiagnostics().selectedStateSlot == 1);

        beginTest ("Queue exhaustion is explicit, never a silent drop");
        DynamicSelectorScheduler scheduler2;
        expect (scheduler2.prepare (sr));
        int accepted = 0;
        for (int i = 0; i < DynamicSelectorContract::kEventQueueCapacity + 2; ++i)
        {
            auto event = makeEvent ((int64_t) i * 10000 + scheduler2.getExpectedNextSample(),
                                    scheduler2.getFingerprintSamples(), makeMatched (1));
            if (scheduler2.submitEvent (event))
                ++accepted;
        }
        expect (accepted == DynamicSelectorContract::kEventQueueCapacity);
        expect (scheduler2.getDiagnostics().lastDecision == DynamicSelectorDiagnostic::QueueExhausted);
    }

    // ---------------------------------------------------------------- J ----
    void testFullBandMixer()
    {
        beginTest ("Global-only, State-only, Service-only, and weighted low mixes; high band added once");
        const double sr = 48000.0;
        const int64_t globalTapForSizing = (int64_t) std::ceil (sr * 20.0 / 1000.0);
        // Large enough to cover trigger + fingerprint window + full transition
        // with headroom, so the fade actually starts and completes in-block.
        const int numSamples = (int) (globalTapForSizing + 2000);

        for (int channels = 1; channels <= 2; ++channels)
        {
            DynamicSelectorScheduler scheduler;
            expect (scheduler.prepare (sr));
            DynamicContinuityMixer mixer;
            expect (mixer.prepare (channels));

            const int64_t globalTap = globalTapForSizing;
            auto roster = makeRoster (sr, (double) globalTap, 0, 3, true, (double) globalTap + 200.0,
                                      true, 4, true, true, (double) globalTap + 200.0);

            juce::AudioBuffer<float> globalLow (channels, numSamples);
            juce::AudioBuffer<float> serviceLow (channels, numSamples);
            juce::AudioBuffer<float> highBand (channels, numSamples);
            std::array<juce::AudioBuffer<float>, DynamicSelectorContract::kStateSlotCount> stateBuffers;
            for (auto& buf : stateBuffers)
                buf.setSize (channels, numSamples);

            for (int ch = 0; ch < channels; ++ch)
            {
                for (int i = 0; i < numSamples; ++i)
                {
                    globalLow.setSample (ch, i, 1.0f);
                    serviceLow.setSample (ch, i, 3.0f);
                    highBand.setSample (ch, i, 0.25f);
                    stateBuffers[0].setSample (ch, i, 2.0f);
                }
            }

            DynamicContinuityMixerInputs inputs;
            inputs.globalLow = &globalLow;
            inputs.serviceLow = &serviceLow;
            inputs.commonHigh = &highBand;
            for (int i = 0; i < DynamicSelectorContract::kStateSlotCount; ++i)
                inputs.stateLow[(size_t) i] = &stateBuffers[(size_t) i];

            // Global-only region (before any event). Uses its own buffer since
            // renderBlock always writes starting at output index 0.
            juce::AudioBuffer<float> initialOutput (channels, 10);
            expect (mixer.renderBlock (scheduler, roster, inputs, 10, initialOutput));
            for (int ch = 0; ch < channels; ++ch)
                for (int i = 0; i < 10; ++i)
                    expectWithinAbsoluteError (initialOutput.getSample (ch, i), 1.0f + 0.25f, 1.0e-5f);

            auto event = makeEvent (scheduler.getExpectedNextSample(), scheduler.getFingerprintSamples(), makeMatched (3));
            expect (scheduler.submitEvent (event));

            const int remaining = numSamples - 10;
            juce::AudioBuffer<float> mainOutput (channels, remaining);
            expect (mixer.renderBlock (scheduler, roster, inputs, remaining, mainOutput));

            // High band must equal exactly 0.25 on every sample regardless of
            // how many low branches are blended (never duplicated per branch).
            for (int ch = 0; ch < channels; ++ch)
                for (int i = 0; i < remaining; ++i)
                {
                    const float lowOnly = mainOutput.getSample (ch, i) - 0.25f;
                    expect (lowOnly >= -1.0e-4f && lowOnly <= 2.0f + 1.0e-4f);
                }

            // Midpoint of the fade must show a genuine weighted blend strictly
            // between the Global-only (1.25) and State-only (2.25) endpoints.
            const int64_t midpointAbsolute = event.readySample + scheduler.getTransitionSamples() / 2;
            const int midpointLocalIndex = (int) (midpointAbsolute - 10);
            if (midpointLocalIndex >= 0 && midpointLocalIndex < remaining)
                for (int ch = 0; ch < channels; ++ch)
                {
                    const float value = mainOutput.getSample (ch, midpointLocalIndex);
                    expect (value > 1.25f + 1.0e-3f && value < 2.25f - 1.0e-3f);
                }

            // State-only region: after the fade has fully completed, output must
            // equal exactly the State low sample (2.0) plus high (0.25).
            for (int ch = 0; ch < channels; ++ch)
                expectWithinAbsoluteError (mainOutput.getSample (ch, remaining - 1), 2.0f + 0.25f, 1.0e-4f);

            expect (! scheduler.getDiagnostics().fadeActive);

            // Retarget to Service and confirm a Service-only mix once settled.
            auto serviceEvent = makeEvent (scheduler.getExpectedNextSample(), scheduler.getFingerprintSamples(), makeMatched (4));
            expect (scheduler.submitEvent (serviceEvent));
            const int64_t serviceSettleSample = serviceEvent.readySample + scheduler.getTransitionSamples() + 10;
            const int serviceRenderCount = (int) (serviceSettleSample - scheduler.getExpectedNextSample() + 1);
            juce::AudioBuffer<float> serviceOutput (channels, serviceRenderCount);
            expect (mixer.renderBlock (scheduler, roster, inputs, serviceRenderCount, serviceOutput));
            for (int ch = 0; ch < channels; ++ch)
                expectWithinAbsoluteError (serviceOutput.getSample (ch, serviceRenderCount - 1), 3.0f + 0.25f, 1.0e-4f);
            expect (! scheduler.getDiagnostics().fadeActive);
            expect (scheduler.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::Service);

            expect (std::isfinite (mainOutput.getSample (0, remaining - 1)));
        }
    }

    // ---------------------------------------------------------------- L ----
    void testStaleIdentity()
    {
        beginTest ("Stale State slot identity is never routed under the old gain");
        const double sr = 48000.0;
        const int64_t globalTap = (int64_t) std::ceil (sr * 20.0 / 1000.0);
        DynamicSelectorScheduler scheduler;
        expect (scheduler.prepare (sr));
        auto roster = makeRoster (sr, (double) globalTap, 3, 100, true, (double) globalTap + 300.0);

        runEventToCompletion (scheduler, roster, makeMatched (100), globalTap + 600);
        expect (scheduler.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::State);
        expect (scheduler.isSemanticStateReferenced (100));

        // Slot 3 is silently replaced with a different stable identity.
        roster.states[3].stableStateId = 200;

        const uint64_t staleBefore = scheduler.getDiagnostics().staleReferenceCount;
        expect (scheduler.advanceSample (scheduler.getExpectedNextSample(), roster));

        expect (scheduler.getDiagnostics().staleReferenceCount > staleBefore);
        expect (scheduler.getDiagnostics().lastDecision == DynamicSelectorDiagnostic::StaleBranchReference);
        expect (scheduler.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::Global);
        expectWithinAbsoluteError (scheduler.getCurrentGains()[(size_t) DynamicSelectorContract::kGlobalBranchIndex], 1.0f, 1.0e-6f);
        expect (! scheduler.isSemanticStateReferenced (200));
    }

    // ---------------------------------------------------------------- M ----
    void testServiceIdentity()
    {
        const double sr = 48000.0;
        const int64_t globalTap = (int64_t) std::ceil (sr * 20.0 / 1000.0);

        beginTest ("Service is not selectable without a semantic binding");
        {
            DynamicSelectorScheduler scheduler;
            expect (scheduler.prepare (sr));
            auto roster = makeRoster (sr, (double) globalTap, -1, 0, true, -1.0,
                                      true, 0, true, false, (double) globalTap + 300.0);
            runEventToCompletion (scheduler, roster, makeMatched (55), globalTap + 500);
            expect (scheduler.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::Global);
        }

        beginTest ("Service binding must match the matcher-selected stable ID");
        {
            DynamicSelectorScheduler scheduler;
            expect (scheduler.prepare (sr));
            auto roster = makeRoster (sr, (double) globalTap, -1, 0, true, -1.0,
                                      true, 999, true, true, (double) globalTap + 300.0);
            runEventToCompletion (scheduler, roster, makeMatched (55), globalTap + 500);
            expect (scheduler.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::Global);
        }

        beginTest ("Changing Service binding invalidates a stale Service reference");
        {
            DynamicSelectorScheduler scheduler;
            expect (scheduler.prepare (sr));
            auto roster = makeRoster (sr, (double) globalTap, -1, 0, true, -1.0,
                                      true, 55, true, true, (double) globalTap + 300.0);
            runEventToCompletion (scheduler, roster, makeMatched (55), globalTap + 600);
            expect (scheduler.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::Service);

            roster.serviceBoundStableStateId = 66;
            expect (scheduler.advanceSample (scheduler.getExpectedNextSample(), roster));
            expect (scheduler.getDiagnostics().lastDecision == DynamicSelectorDiagnostic::StaleBranchReference);
            expect (scheduler.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::Global);
        }

        beginTest ("Persistent State wins over Service for the same identity");
        {
            DynamicSelectorScheduler scheduler;
            expect (scheduler.prepare (sr));
            auto roster = makeRoster (sr, (double) globalTap, 5, 77, true, (double) globalTap + 300.0,
                                      true, 77, true, true, (double) globalTap + 300.0);
            runEventToCompletion (scheduler, roster, makeMatched (77), globalTap + 600);
            expect (scheduler.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::State);
        }
    }

    // ---------------------------------------------------------------- N ----
    void testTransportDiscontinuity()
    {
        const double sr = 48000.0;
        const int64_t globalTap = (int64_t) std::ceil (sr * 20.0 / 1000.0);

        beginTest ("Transport reset during active Hold clears queue/Hold and snaps to Global");
        {
            DynamicSelectorScheduler scheduler;
            expect (scheduler.prepare (sr));
            auto roster = makeRoster (sr, (double) globalTap, 1, 42, true, (double) globalTap + 300.0);
            runEventToCompletion (scheduler, roster, makeMatched (42), globalTap + 500);
            submitAndAdvanceThrough (scheduler, roster, makeDecisionOnly (DynamicMatchDecision::Ambiguous), scheduler.getExpectedNextSample() + 10);
            expect (scheduler.getDiagnostics().holdEventCount == 1);

            scheduler.reportTransportDiscontinuity (DynamicSelectorTransportReason::Seek, 500000);
            expect (scheduler.getDiagnostics().lastDecision == DynamicSelectorDiagnostic::TransportReset);
            expect (scheduler.getDiagnostics().holdEventCount == 0);
            expect (scheduler.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::Global);
            expectWithinAbsoluteError (scheduler.getCurrentGains()[(size_t) DynamicSelectorContract::kGlobalBranchIndex], 1.0f, 1.0e-6f);
            expect (scheduler.getExpectedNextSample() == 500000);
        }

        beginTest ("Transport reset during an active fade cancels it and snaps to Global");
        {
            DynamicSelectorScheduler scheduler;
            expect (scheduler.prepare (sr));
            auto roster = makeRoster (sr, (double) globalTap, 0, 9, true, (double) globalTap + 400.0);
            auto event = makeEvent (0, scheduler.getFingerprintSamples(), makeMatched (9));
            expect (scheduler.submitEvent (event));
            for (int64_t i = 0; i <= event.readySample + 2; ++i)
                expect (scheduler.advanceSample (i, roster));
            expect (scheduler.getDiagnostics().fadeActive);

            scheduler.reportTransportDiscontinuity (DynamicSelectorTransportReason::LoopWrap, 0);
            expect (! scheduler.getDiagnostics().fadeActive);
            expectWithinAbsoluteError (scheduler.getCurrentGains()[(size_t) DynamicSelectorContract::kGlobalBranchIndex], 1.0f, 1.0e-6f);
        }

        beginTest ("Transport reset drops queued future events; old events cannot fire after reset");
        {
            DynamicSelectorScheduler scheduler;
            expect (scheduler.prepare (sr));
            auto roster = makeRoster (sr, (double) globalTap, 0, 9, true, (double) globalTap + 400.0);
            auto futureEvent = makeEvent (100000, scheduler.getFingerprintSamples(), makeMatched (9));
            expect (scheduler.submitEvent (futureEvent));
            expect (scheduler.getDiagnostics().queuedEventCount == 1);

            scheduler.reportTransportDiscontinuity (DynamicSelectorTransportReason::HostReset, 0);
            expect (scheduler.getDiagnostics().queuedEventCount == 0);

            // A block whose start sample does not equal the expected next
            // sample must fail explicitly rather than reinterpret timestamps.
            expect (! scheduler.advanceSample (100050, roster));
            expect (scheduler.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::Global);
        }

        beginTest ("Non-contiguous block without explicit reset is rejected");
        {
            DynamicSelectorScheduler scheduler;
            expect (scheduler.prepare (sr));
            auto roster = makeRoster (sr, (double) globalTap);
            expect (scheduler.advanceSample (0, roster));
            expect (! scheduler.advanceSample (5, roster));
        }
    }

    // ---------------------------------------------------------------- O ----
    void testNonFiniteRecovery()
    {
        beginTest ("Non-finite branch/high samples are substituted with zero and counted; output stays finite");
        const double sr = 48000.0;
        DynamicSelectorScheduler scheduler;
        expect (scheduler.prepare (sr));
        DynamicContinuityMixer mixer;
        expect (mixer.prepare (1));
        auto roster = makeRoster (sr, (double) std::ceil (sr * 20.0 / 1000.0));

        const int numSamples = 8;
        juce::AudioBuffer<float> globalLow (1, numSamples);
        juce::AudioBuffer<float> serviceLow (1, numSamples);
        juce::AudioBuffer<float> highBand (1, numSamples);
        std::array<juce::AudioBuffer<float>, DynamicSelectorContract::kStateSlotCount> stateBuffers;
        for (auto& buf : stateBuffers)
            buf.setSize (1, numSamples);

        globalLow.clear();
        serviceLow.clear();
        highBand.clear();
        for (auto& buf : stateBuffers)
            buf.clear();

        globalLow.setSample (0, 2, std::numeric_limits<float>::quiet_NaN());
        highBand.setSample (0, 4, std::numeric_limits<float>::infinity());
        globalLow.setSample (0, 6, 0.5f);
        highBand.setSample (0, 6, 0.25f);

        DynamicContinuityMixerInputs inputs;
        inputs.globalLow = &globalLow;
        inputs.serviceLow = &serviceLow;
        inputs.commonHigh = &highBand;
        for (int i = 0; i < DynamicSelectorContract::kStateSlotCount; ++i)
            inputs.stateLow[(size_t) i] = &stateBuffers[(size_t) i];

        juce::AudioBuffer<float> output (1, numSamples);
        expect (mixer.renderBlock (scheduler, roster, inputs, numSamples, output));

        for (int i = 0; i < numSamples; ++i)
            expect (std::isfinite (output.getSample (0, i)));

        expect (mixer.getNonFiniteBranchSampleCount() >= 1);
        expect (mixer.getNonFiniteHighSampleCount() >= 1);
        // Later finite material recovers exactly.
        expectWithinAbsoluteError (output.getSample (0, 6), 0.75f, 1.0e-5f);

        beginTest ("Corrupted internal gain state collapses safely to Global");
        scheduler.test_corruptCurrentGain (DynamicSelectorContract::kGlobalBranchIndex,
                                          std::numeric_limits<float>::quiet_NaN());
        expect (scheduler.advanceSample (scheduler.getExpectedNextSample(), roster));
        expect (scheduler.getDiagnostics().nonFiniteGainRecoveryCount >= 1);
        expectWithinAbsoluteError (scheduler.getCurrentGains()[(size_t) DynamicSelectorContract::kGlobalBranchIndex], 1.0f, 1.0e-6f);
    }

    // ---------------------------------------------------------------- P ----
    void testRealTimeSafetyStructural()
    {
        beginTest ("Sustained operation across submit/advance/mix stays bounded and finite (no allocator instrumentation available; structural fixed-array design verified by construction)");
        const double sr = 48000.0;
        DynamicSelectorScheduler scheduler;
        expect (scheduler.prepare (sr));
        DynamicContinuityMixer mixer;
        expect (mixer.prepare (2));

        auto roster = makeRoster (sr, (double) std::ceil (sr * 20.0 / 1000.0), 0, 1, true,
                                  (double) std::ceil (sr * 20.0 / 1000.0) + 200.0);

        const int numSamples = 32;
        juce::AudioBuffer<float> globalLow (2, numSamples);
        juce::AudioBuffer<float> serviceLow (2, numSamples);
        juce::AudioBuffer<float> highBand (2, numSamples);
        std::array<juce::AudioBuffer<float>, DynamicSelectorContract::kStateSlotCount> stateBuffers;
        for (auto& buf : stateBuffers)
            buf.setSize (2, numSamples);
        globalLow.clear(); serviceLow.clear(); highBand.clear();
        for (auto& buf : stateBuffers) buf.clear();

        DynamicContinuityMixerInputs inputs;
        inputs.globalLow = &globalLow;
        inputs.serviceLow = &serviceLow;
        inputs.commonHigh = &highBand;
        for (int i = 0; i < DynamicSelectorContract::kStateSlotCount; ++i)
            inputs.stateLow[(size_t) i] = &stateBuffers[(size_t) i];
        juce::AudioBuffer<float> output (2, numSamples);

        for (int block = 0; block < 200; ++block)
        {
            if (block % 20 == 0)
            {
                auto event = makeEvent (scheduler.getExpectedNextSample(), scheduler.getFingerprintSamples(),
                                        (block % 40 == 0) ? makeMatched (1) : makeDecisionOnly (DynamicMatchDecision::Ambiguous));
                scheduler.submitEvent (event);
            }
            expect (mixer.renderBlock (scheduler, roster, inputs, numSamples, output));
        }

        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < numSamples; ++i)
                expect (std::isfinite (output.getSample (ch, i)));
    }

private:
    void runEventToCompletion (DynamicSelectorScheduler& scheduler,
                              const DynamicSelectorBranchRoster& roster,
                              const DynamicMatchResult& match,
                              int64_t extraSamples)
    {
        auto event = makeEvent (scheduler.getExpectedNextSample(), scheduler.getFingerprintSamples(), match);
        expect (scheduler.submitEvent (event));
        const int64_t endSample = event.readySample + extraSamples;
        for (int64_t i = scheduler.getExpectedNextSample(); i <= endSample; ++i)
            expect (scheduler.advanceSample (i, roster));
    }

    void submitAndAdvanceThrough (DynamicSelectorScheduler& scheduler,
                                 const DynamicSelectorBranchRoster& roster,
                                 const DynamicMatchResult& match,
                                 int64_t triggerSample)
    {
        submitAndAdvanceThroughTrigger (scheduler, roster, match, triggerSample);
    }

    void submitAndAdvanceThroughTrigger (DynamicSelectorScheduler& scheduler,
                                        const DynamicSelectorBranchRoster& roster,
                                        const DynamicMatchResult& match,
                                        int64_t triggerSample)
    {
        auto event = makeEvent (triggerSample, scheduler.getFingerprintSamples(), match);
        expect (scheduler.submitEvent (event));
        for (int64_t i = scheduler.getExpectedNextSample(); i <= event.readySample; ++i)
            expect (scheduler.advanceSample (i, roster));
    }
};

static DynamicSelectorSchedulerTests dynamicSelectorSchedulerTests;
