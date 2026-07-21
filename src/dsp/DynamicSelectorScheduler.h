#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

#include "DynamicSelectorTypes.h"

// =============================================================================
// Phase 6: standalone, allocation-free selector scheduler.
//
// Consumes DynamicMatchResult decisions (already computed by the pure Phase-3
// matcher) plus a read-only Phase-5 branch roster, and produces a
// sample-accurate, ten-position low-band gain vector. It does not extract
// fingerprints, compute match distances, resolve packages, or touch
// PluginProcessor. It never connects to the product audio callback.
// =============================================================================

class DynamicSelectorScheduler
{
public:
    bool prepare (double sampleRateIn) noexcept
    {
        prepared = false;
        if (! DynamicAllpassPoleDomain::isValidSampleRate (sampleRateIn))
            return false;

        const auto windowLayout = DynamicFingerprintWindow::forSampleRate (sampleRateIn);
        fingerprintSamples = (int64_t) windowLayout.windowSamples;

        const int64_t nominalTransition = (int64_t) std::floor (
            sampleRateIn * (double) DynamicStateMapContract::kMaximumTransitionMs / 1000.0);
        const int64_t requiredSafety = (int64_t) std::ceil (
            sampleRateIn * (double) DynamicStateMapContract::kSafetyMarginMs / 1000.0);
        const int64_t minOutputOffset = (int64_t) std::floor (
            sampleRateIn * (double) DynamicHotBranchContract::kMinimumPhysicalTapMs / 1000.0);

        if (nominalTransition <= 0 || requiredSafety <= 0 || minOutputOffset <= 0)
            return false;

        const int64_t candidateTransition = minOutputOffset - fingerprintSamples - requiredSafety;
        transitionSamples = std::min (nominalTransition, candidateTransition);
        safetySamples = requiredSafety;

        if (transitionSamples <= 0)
            return false;

        holdSamples = (int64_t) std::floor (
            sampleRateIn * (double) DynamicStateMapContract::kMaximumHoldTimeMs / 1000.0);
        if (holdSamples <= 0)
            return false;

        sampleRate = sampleRateIn;
        prepared = true;
        if (! reset (0))
        {
            // Unreachable in practice (0 is never INT64_MAX), but keep the
            // prepared/reset contract honest if that policy ever changes.
            prepared = false;
            return false;
        }
        return true;
    }

    // Resets to Global with an explicit absolute-sample origin. Clears events,
    // Hold, and any active fade. Does not touch Phase-5 branch DSP state.
    //
    // int64 overflow policy: originSample == INT64_MAX is rejected so that at
    // least one further sample (originSample + 1) always remains a
    // representable expectedNextSample. Rejection leaves all prior state
    // (gains, active target, Hold, queued events, expected sample position)
    // completely unmutated.
    bool reset (int64_t originSample) noexcept
    {
        if (originSample == std::numeric_limits<int64_t>::max())
            return false;

        queueCount = 0;
        nextInsertionSequence = 0;

        activeTarget = makeGlobalSelectorBranchRef();
        boundStableId.fill (0);

        fadeActive = false;
        fadeStartSample = 0;
        fadeLength = 0;
        fadePosition = 0;
        currentGains.fill (0.0f);
        currentGains[(size_t) DynamicSelectorContract::kGlobalBranchIndex] = 1.0f;
        fadeStartGains = currentGains;
        targetGains = currentGains;

        unresolvedHoldCount = 0;
        lastConfidentReadySample = originSample;

        expectedNextSample = originSample;

        diagnostics = DynamicSelectorDiagnostics {};
        diagnostics.safetySamples = safetySamples;
        return true;
    }

    bool isPrepared() const noexcept { return prepared; }
    int64_t getTransitionSamples() const noexcept { return transitionSamples; }
    int64_t getSafetySamples() const noexcept { return safetySamples; }
    int64_t getFingerprintSamples() const noexcept { return fingerprintSamples; }
    int64_t getHoldSamples() const noexcept { return holdSamples; }
    int64_t getExpectedNextSample() const noexcept { return expectedNextSample; }

    // Queues one observation. Rejects invalid timestamps, stale events already
    // behind the scheduler's current sample, and events beyond fixed capacity.
    // Never overwrites an unconsumed event.
    bool submitEvent (const DynamicSelectorEvent& event) noexcept
    {
        if (! prepared)
            return false;

        if (event.readySample <= event.triggerSample
            || event.triggerSample < 0
            || event.triggerSample > std::numeric_limits<int64_t>::max() - fingerprintSamples
            || event.readySample - event.triggerSample != fingerprintSamples)
        {
            diagnostics.lastDecision = DynamicSelectorDiagnostic::InvalidEvent;
            ++diagnostics.invalidEventCount;
            return false;
        }

        if (event.readySample < expectedNextSample)
        {
            diagnostics.lastDecision = DynamicSelectorDiagnostic::InvalidEvent;
            ++diagnostics.invalidEventCount;
            return false;
        }

        if (queueCount >= DynamicSelectorContract::kEventQueueCapacity)
        {
            diagnostics.lastDecision = DynamicSelectorDiagnostic::QueueExhausted;
            return false;
        }

        // Deterministic sorted insert: smaller readySample first, then smaller
        // triggerSample, then insertion order. Capacity is tiny and fixed, so a
        // linear shift is allocation-free and bounded.
        int insertAt = queueCount;
        for (int i = 0; i < queueCount; ++i)
        {
            const auto& existing = queue[(size_t) i];
            const bool laterEvent =
                event.readySample < existing.event.readySample
                || (event.readySample == existing.event.readySample
                    && event.triggerSample < existing.event.triggerSample);
            if (laterEvent)
            {
                insertAt = i;
                break;
            }
        }
        for (int i = queueCount; i > insertAt; --i)
            queue[(size_t) i] = queue[(size_t) (i - 1)];

        queue[(size_t) insertAt].event = event;
        queue[(size_t) insertAt].insertionSequence = nextInsertionSequence++;
        ++queueCount;
        diagnostics.queuedEventCount = queueCount;
        return true;
    }

    // Explicit transport-discontinuity reset. Clears pending events, Hold, the
    // active fade, and snaps gains to Global. Updates the expected absolute
    // sample origin. Does not reset DynamicHotBranchEngine.
    //
    // Returns false (rejecting newOriginSample == INT64_MAX per reset()'s
    // overflow policy) without mutating any state, including the diagnostics'
    // last decision.
    bool reportTransportDiscontinuity (DynamicSelectorTransportReason reason,
                                       int64_t newOriginSample) noexcept
    {
        (void) reason;
        if (! prepared)
            return false;
        if (! reset (newOriginSample))
            return false;
        diagnostics.lastDecision = DynamicSelectorDiagnostic::TransportReset;
        return true;
    }

    // Advances exactly one absolute sample. `absoluteSample` must equal
    // getExpectedNextSample(); otherwise this call fails explicitly and the
    // caller must issue a transport-discontinuity reset.
    //
    // int64 overflow policy: absoluteSample == INT64_MAX is rejected before
    // any mutation, since expectedNextSample = absoluteSample + 1 would
    // otherwise overflow. A sample at INT64_MAX - 1 may still be processed
    // once, leaving expectedNextSample at exactly INT64_MAX (representable);
    // the scheduler simply cannot advance any further after that.
    bool advanceSample (int64_t absoluteSample, const DynamicSelectorBranchRoster& roster) noexcept
    {
        if (! prepared
            || absoluteSample == std::numeric_limits<int64_t>::max()
            || absoluteSample != expectedNextSample
            || ! isValidDynamicSelectorBranchRoster (roster))
            return false;

        while (queueCount > 0 && queue[0].event.readySample == absoluteSample)
        {
            const DynamicSelectorEvent event = queue[0].event;
            for (int i = 1; i < queueCount; ++i)
                queue[(size_t) (i - 1)] = queue[(size_t) i];
            --queueCount;
            diagnostics.queuedEventCount = queueCount;

            processEvent (event, roster);
        }

        checkStaleReferences (roster);
        computeSampleGains();

        expectedNextSample = absoluteSample + 1;
        return true;
    }

    const std::array<float, DynamicSelectorContract::kBranchCount>& getCurrentGains() const noexcept
    {
        return currentGains;
    }

    const DynamicSelectorDiagnostics& getDiagnostics() const noexcept { return diagnostics; }

    // Test-only hook: injects a non-finite value directly into the live gain
    // vector so the non-finite recovery path (see sanitizeGainVector()) can be
    // exercised deterministically. Never called from production code.
    void test_corruptCurrentGain (int index, float value) noexcept
    {
        if (index >= 0 && index < DynamicSelectorContract::kBranchCount)
            currentGains[(size_t) index] = value;
    }

    bool isStateSlotReferenced (int slot) const noexcept
    {
        using namespace DynamicSelectorContract;
        if (slot < 0 || slot >= kStateSlotCount)
            return false;
        return isBranchIndexLive (kFirstStateBranchIndex + slot);
    }

    bool isSemanticStateReferenced (uint64_t stableStateId) const noexcept
    {
        using namespace DynamicSelectorContract;
        if (stableStateId == 0)
            return false;
        for (int slot = 0; slot < kStateSlotCount; ++slot)
            if (boundStableId[(size_t) (kFirstStateBranchIndex + slot)] == stableStateId
                && isStateSlotReferenced (slot))
                return true;
        if (boundStableId[(size_t) kServiceBranchIndex] == stableStateId
            && isBranchIndexLive (kServiceBranchIndex))
            return true;
        return false;
    }

private:
    struct QueuedEvent
    {
        DynamicSelectorEvent event;
        uint64_t insertionSequence = 0;
    };

    // Resolves one event's target branch per the frozen match-to-target policy
    // and Hold policy, then either begins a transition, refreshes Hold, or
    // rejects a late decision. Never mutates activeTarget/Hold when rejected.
    void processEvent (const DynamicSelectorEvent& event,
                       const DynamicSelectorBranchRoster& roster) noexcept
    {
        diagnostics.triggerSample = event.triggerSample;
        diagnostics.readySample = event.readySample;

        DynamicSelectorBranchRef resolvedTarget = makeGlobalSelectorBranchRef();
        DynamicSelectorDiagnostic decisionDiag = DynamicSelectorDiagnostic::FallbackNoEligibleStates;
        uint64_t recognizedId = 0;
        bool isConfidentCorrection = false;
        bool clearsHold = true;
        bool isHoldCandidate = false;
        bool isHeldEvent = false;

        const auto& match = event.match;
        switch (match.decision)
        {
            case DynamicMatchDecision::Matched:
            {
                recognizedId = match.selectedStableStateId;
                if (match.selectedNeutralSafe)
                {
                    // The measurement gate proved this identity's Global
                    // fallback is harmful: route to the shared Neutral branch
                    // unconditionally, exactly like MatchedGlobalNoCorrection
                    // routes to Global for an ordinary no-correction identity.
                    resolvedTarget = makeNeutralSelectorBranchRef();
                    decisionDiag = DynamicSelectorDiagnostic::MatchedNeutralSafe;
                }
                else if (match.correctionAvailable && ! match.selectedBypassed)
                {
                    const int warmSlot = findWarmActiveStateSlotByStableId (roster, match.selectedStableStateId);
                    if (warmSlot >= 0)
                    {
                        resolvedTarget = { DynamicSelectorBranchKind::State, warmSlot, match.selectedStableStateId };
                        decisionDiag = DynamicSelectorDiagnostic::MatchedPersistentState;
                        isConfidentCorrection = true;
                        clearsHold = false;
                    }
                    else if (roster.serviceBindingValid && roster.service.active && roster.service.warm
                             && roster.serviceBoundStableStateId == match.selectedStableStateId)
                    {
                        resolvedTarget = { DynamicSelectorBranchKind::Service, -1, match.selectedStableStateId };
                        decisionDiag = DynamicSelectorDiagnostic::MatchedService;
                        isConfidentCorrection = true;
                        clearsHold = false;
                    }
                    else
                    {
                        const bool existsInactiveOrCold =
                            findActiveStateSlotByStableId (roster, match.selectedStableStateId) >= 0
                            || (roster.serviceBindingValid && roster.service.active
                                && roster.serviceBoundStableStateId == match.selectedStableStateId);
                        decisionDiag = existsInactiveOrCold
                            ? DynamicSelectorDiagnostic::FallbackTargetNotWarm
                            : DynamicSelectorDiagnostic::FallbackTargetUnavailable;
                    }
                }
                else if (! match.correctionAvailable)
                {
                    decisionDiag = DynamicSelectorDiagnostic::MatchedGlobalNoCorrection;
                }
                else
                {
                    decisionDiag = DynamicSelectorDiagnostic::MatchedGlobalBypassed;
                }
                break;
            }
            case DynamicMatchDecision::Ambiguous:
            case DynamicMatchDecision::Unknown:
            {
                const bool isAmbiguous = match.decision == DynamicMatchDecision::Ambiguous;
                if (isHoldValid (event, roster))
                {
                    resolvedTarget = activeTarget;
                    decisionDiag = isAmbiguous ? DynamicSelectorDiagnostic::HeldAmbiguous
                                               : DynamicSelectorDiagnostic::HeldUnknown;
                    isHoldCandidate = true;
                    isHeldEvent = true;
                    clearsHold = false;
                }
                else
                {
                    decisionDiag = isAmbiguous ? DynamicSelectorDiagnostic::FallbackAmbiguous
                                               : DynamicSelectorDiagnostic::FallbackUnknown;
                }
                break;
            }
            case DynamicMatchDecision::InvalidInput:
                decisionDiag = DynamicSelectorDiagnostic::FallbackInvalidInput;
                break;
            case DynamicMatchDecision::NoEligibleStates:
                decisionDiag = DynamicSelectorDiagnostic::FallbackNoEligibleStates;
                break;
        }

        diagnostics.lastDecision = decisionDiag;
        diagnostics.recognizedStableStateId = recognizedId;

        const bool isNoOp = dynamicSelectorBranchRefsEqual (resolvedTarget, activeTarget);

        if (isNoOp)
        {
            if (isConfidentCorrection)
            {
                lastConfidentReadySample = event.readySample;
                unresolvedHoldCount = 0;
            }
            else if (isHoldCandidate)
            {
                unresolvedHoldCount = std::min (unresolvedHoldCount + 1, DynamicSelectorContract::kMaximumHoldEvents);
            }
            else if (clearsHold)
            {
                unresolvedHoldCount = 0;
            }
            updateSelectedDiagnostics();
            return;
        }

        // A real target change: validate the sample-accurate look-ahead deadline
        // before mutating any selection or Hold state.
        const int targetBranchIndex = dynamicSelectorBranchIndex (resolvedTarget);
        const double targetPhysicalTap = branchPhysicalTapSamples (resolvedTarget, roster);
        if (targetBranchIndex < 0 || ! std::isfinite (targetPhysicalTap) || targetPhysicalTap < 0.0)
        {
            diagnostics.lastDecision = DynamicSelectorDiagnostic::LateDecisionRejected;
            ++diagnostics.lateDecisionCount;
            return;
        }

        const int64_t targetEarliestOffset = (int64_t) std::floor (targetPhysicalTap);
        if (event.triggerSample > std::numeric_limits<int64_t>::max() - targetEarliestOffset)
        {
            diagnostics.lastDecision = DynamicSelectorDiagnostic::LateDecisionRejected;
            ++diagnostics.lateDecisionCount;
            return;
        }
        const int64_t targetDeadlineSample = event.triggerSample + targetEarliestOffset;
        diagnostics.targetDeadlineSample = targetDeadlineSample;

        if (event.readySample > std::numeric_limits<int64_t>::max() - transitionSamples - safetySamples)
        {
            diagnostics.lastDecision = DynamicSelectorDiagnostic::LateDecisionRejected;
            ++diagnostics.lateDecisionCount;
            return;
        }
        const int64_t completionSample = event.readySample + transitionSamples + safetySamples;

        if (completionSample > targetDeadlineSample)
        {
            diagnostics.lastDecision = DynamicSelectorDiagnostic::LateDecisionRejected;
            ++diagnostics.lateDecisionCount;
            return;
        }

        // Deadline satisfied: begin the transition from the exact current gain
        // vector, preserving whatever bound identities already apply to every
        // other branch index.
        fadeStartGains = currentGains;
        targetGains.fill (0.0f);
        targetGains[(size_t) targetBranchIndex] = 1.0f;
        boundStableId[(size_t) targetBranchIndex] = resolvedTarget.stableStateId;
        fadeStartSample = event.readySample;
        fadeLength = transitionSamples;
        fadePosition = 0;
        fadeActive = true;

        activeTarget = resolvedTarget;

        if (isConfidentCorrection)
        {
            lastConfidentReadySample = event.readySample;
            unresolvedHoldCount = 0;
        }
        else if (isHeldEvent)
        {
            unresolvedHoldCount = std::min (unresolvedHoldCount + 1, DynamicSelectorContract::kMaximumHoldEvents);
        }
        else if (clearsHold)
        {
            unresolvedHoldCount = 0;
        }

        updateSelectedDiagnostics();
    }

    bool isHoldValid (const DynamicSelectorEvent& event, const DynamicSelectorBranchRoster& roster) const noexcept
    {
        if (activeTarget.kind == DynamicSelectorBranchKind::Global || activeTarget.stableStateId == 0)
            return false;
        if (unresolvedHoldCount >= DynamicSelectorContract::kMaximumHoldEvents)
            return false;
        if (event.readySample < lastConfidentReadySample)
            return false;
        if (event.readySample - lastConfidentReadySample > holdSamples)
            return false;

        if (activeTarget.kind == DynamicSelectorBranchKind::State)
        {
            if (activeTarget.stateSlot < 0 || activeTarget.stateSlot >= DynamicSelectorContract::kStateSlotCount)
                return false;
            const auto& state = roster.states[(size_t) activeTarget.stateSlot];
            return state.active && state.warm && state.stableStateId == activeTarget.stableStateId;
        }
        if (activeTarget.kind == DynamicSelectorBranchKind::Service)
        {
            return roster.serviceBindingValid && roster.service.active && roster.service.warm
                && roster.serviceBoundStableStateId == activeTarget.stableStateId;
        }
        return false;
    }

    static double branchPhysicalTapSamples (const DynamicSelectorBranchRef& ref,
                                            const DynamicSelectorBranchRoster& roster) noexcept
    {
        switch (ref.kind)
        {
            case DynamicSelectorBranchKind::Global:
                return roster.global.physicalTapSamples;
            case DynamicSelectorBranchKind::State:
                if (ref.stateSlot < 0 || ref.stateSlot >= DynamicSelectorContract::kStateSlotCount)
                    return std::numeric_limits<double>::quiet_NaN();
                return roster.states[(size_t) ref.stateSlot].physicalTapSamples;
            case DynamicSelectorBranchKind::Service:
                return roster.service.physicalTapSamples;
            case DynamicSelectorBranchKind::Neutral:
                return roster.neutral.physicalTapSamples;
        }
        return std::numeric_limits<double>::quiet_NaN();
    }

    // Single source of truth for "is this gain-vector index currently live"
    // (contributing audio now, or about to via an in-progress fade).
    //
    // currentGains alone is sufficient once settled: after a fade completes,
    // computeSampleGains() canonicalizes fadeStartGains to currentGains and
    // clears boundStableId for every index that settled to zero, so a
    // completed fade's old source is never treated as live again (this is
    // the fix for the ghost-reference bug: checkStaleReferences() used to
    // read fadeStartGains unconditionally, so it kept seeing the pre-transition
    // branch as "referenced" forever after the fade had already settled
    // elsewhere).
    //
    // While a fade is active, targetGains/fadeStartGains are also consulted.
    // In this implementation processEvent() and checkStaleReferences() always
    // see the same roster snapshot within one advanceSample() call, so a
    // fade's destination is already validated (warm/active) at the instant
    // the fade starts and currentGains reaches a detectable nonzero value
    // within the same call's computeSampleGains(); the extra targetGains/
    // fadeStartGains checks are therefore defensive redundancy (e.g. against
    // a future refactor that separates resolution from staleness checking
    // across different roster snapshots) rather than covering a reachable gap
    // today. They cost nothing and keep the policy correct under that change.
    bool isBranchIndexLive (int index) const noexcept
    {
        using namespace DynamicSelectorContract;
        if (currentGains[(size_t) index] > kGainEpsilon)
            return true;
        if (! fadeActive)
            return false;
        return targetGains[(size_t) index] > kGainEpsilon
            || fadeStartGains[(size_t) index] > kGainEpsilon;
    }

    // Emergency safety net: if the identity bound to any currently-live
    // gain index no longer matches the live roster, the whole selection
    // collapses immediately to Global rather than routing a replaced branch's
    // audio under a stale gain.
    void checkStaleReferences (const DynamicSelectorBranchRoster& roster) noexcept
    {
        using namespace DynamicSelectorContract;
        bool stale = false;

        for (int slot = 0; slot < kStateSlotCount; ++slot)
        {
            const int index = kFirstStateBranchIndex + slot;
            if (boundStableId[(size_t) index] == 0 || ! isBranchIndexLive (index))
                continue;
            const auto& state = roster.states[(size_t) slot];
            if (! (state.active && state.stableStateId == boundStableId[(size_t) index]))
                stale = true;
        }

        if (boundStableId[(size_t) kServiceBranchIndex] != 0 && isBranchIndexLive (kServiceBranchIndex))
        {
            if (! (roster.serviceBindingValid && roster.service.active
                   && roster.serviceBoundStableStateId == boundStableId[(size_t) kServiceBranchIndex]))
                stale = true;
        }

        if (! stale)
            return;

        ++diagnostics.staleReferenceCount;
        diagnostics.lastDecision = DynamicSelectorDiagnostic::StaleBranchReference;

        activeTarget = makeGlobalSelectorBranchRef();
        boundStableId.fill (0);
        fadeActive = false;
        fadeLength = 0;
        fadePosition = 0;
        currentGains.fill (0.0f);
        currentGains[(size_t) kGlobalBranchIndex] = 1.0f;
        fadeStartGains = currentGains;
        targetGains = currentGains;
        unresolvedHoldCount = 0;

        updateSelectedDiagnostics();
    }

    void computeSampleGains() noexcept
    {
        if (! fadeActive)
        {
            diagnostics.fadeActive = false;
            sanitizeGainVector();
            return;
        }

        double t = 1.0;
        if (fadeLength > 1)
            t = (double) fadePosition / (double) (fadeLength - 1);

        for (int i = 0; i < DynamicSelectorContract::kBranchCount; ++i)
        {
            const float start = fadeStartGains[(size_t) i];
            const float target = targetGains[(size_t) i];
            float gain = (float) (start + t * (target - start));
            if (! std::isfinite (gain))
                gain = target;
            currentGains[(size_t) i] = std::clamp (gain, 0.0f, 1.0f);
        }
        sanitizeGainVector();

        ++fadePosition;
        if (fadePosition >= fadeLength)
        {
            currentGains = targetGains;
            // Canonicalize the fade-start snapshot to the settled vector so a
            // completed fade's old source is never read as "referenced" again
            // (see isBranchIndexLive()), and drop bound identities for every
            // index that settled to zero gain so a later roster change to
            // that slot/Service binding cannot trigger a false stale collapse.
            fadeStartGains = currentGains;
            for (int i = 0; i < DynamicSelectorContract::kBranchCount; ++i)
                if (currentGains[(size_t) i] <= DynamicSelectorContract::kGainEpsilon)
                    boundStableId[(size_t) i] = 0;
            fadeActive = false;
        }

        diagnostics.fadeActive = fadeActive;
        diagnostics.fadeStartSample = fadeStartSample;
        diagnostics.fadePosition = fadePosition;
        diagnostics.fadeLength = fadeLength;
    }

    // Non-finite gain state is an internal error: collapse safely to Global
    // rather than propagate NaN/Inf into the continuity mixer.
    void sanitizeGainVector() noexcept
    {
        float sum = 0.0f;
        bool allFinite = true;
        for (float gain : currentGains)
        {
            if (! std::isfinite (gain))
            {
                allFinite = false;
                break;
            }
            sum += gain;
        }
        if (allFinite && std::isfinite (sum) && sum > 0.0f)
            return;

        ++diagnostics.nonFiniteGainRecoveryCount;

        activeTarget = makeGlobalSelectorBranchRef();
        boundStableId.fill (0);
        fadeActive = false;
        fadeLength = 0;
        fadePosition = 0;
        currentGains.fill (0.0f);
        currentGains[(size_t) DynamicSelectorContract::kGlobalBranchIndex] = 1.0f;
        fadeStartGains = currentGains;
        targetGains = currentGains;
        unresolvedHoldCount = 0;
        updateSelectedDiagnostics();
    }

    void updateSelectedDiagnostics() noexcept
    {
        diagnostics.selectedSemanticStateId = activeTarget.stableStateId;
        diagnostics.selectedBranchKind = activeTarget.kind;
        diagnostics.selectedStateSlot = activeTarget.stateSlot;
        diagnostics.holdEventCount = unresolvedHoldCount;
        // Overflow-safe: both operands are absolute sample positions bounded
        // well under INT64_MAX by the reset()/advanceSample() overflow guards,
        // and expectedNextSample only ever increases from lastConfidentReadySample
        // forward, but this stays explicit and defined even if that invariant
        // is ever violated rather than risking UB on the subtraction.
        diagnostics.holdAgeSamples = (expectedNextSample >= lastConfidentReadySample)
            ? (expectedNextSample - lastConfidentReadySample) : 0;
    }

    bool prepared = false;
    double sampleRate = 0.0;
    int64_t fingerprintSamples = 0;
    int64_t transitionSamples = 0;
    int64_t safetySamples = 0;
    int64_t holdSamples = 0;

    std::array<QueuedEvent, DynamicSelectorContract::kEventQueueCapacity> queue {};
    int queueCount = 0;
    uint64_t nextInsertionSequence = 0;

    DynamicSelectorBranchRef activeTarget = makeGlobalSelectorBranchRef();
    std::array<uint64_t, DynamicSelectorContract::kBranchCount> boundStableId {};

    bool fadeActive = false;
    int64_t fadeStartSample = 0;
    int64_t fadeLength = 0;
    int64_t fadePosition = 0;
    std::array<float, DynamicSelectorContract::kBranchCount> fadeStartGains {};
    std::array<float, DynamicSelectorContract::kBranchCount> targetGains {};
    std::array<float, DynamicSelectorContract::kBranchCount> currentGains {};

    uint32_t unresolvedHoldCount = 0;
    int64_t lastConfidentReadySample = 0;

    int64_t expectedNextSample = 0;

    DynamicSelectorDiagnostics diagnostics;
};
