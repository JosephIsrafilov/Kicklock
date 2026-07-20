#include "TestCommon.h"
#include "DynamicStateEditTransaction.h"

namespace
{
    DynamicFingerprintPrototype validFingerprint()
    {
        DynamicFingerprintPrototype fingerprint;
        fingerprint.valid = true;
        fingerprint.featureCount = 4;
        fingerprint.features[0] = -0.8f;
        fingerprint.features[1] = 0.25f;
        fingerprint.features[2] = -0.15f;
        fingerprint.features[3] = 0.5f;
        return fingerprint;
    }

    DynamicState autoState (uint64_t stateId, uint32_t hitCount, DynamicStateEvidence evidence)
    {
        DynamicState state;
        state.occupied = true;
        state.stableStateId = stateId;
        state.fingerprint = validFingerprint();
        state.hasLearnedPackage = true;
        state.learnedPackage = { 0.5f, 100.0f, 0.7f };
        state.hitCount = hitCount;
        state.repeatability = 0.85f;
        state.ambiguity = 0.1f;
        state.evidence = evidence;
        return state;
    }

    DynamicState manualState (uint64_t stateId, uint32_t hitCount = 3)
    {
        DynamicState state;
        state.occupied = true;
        state.stableStateId = stateId;
        state.fingerprint = validFingerprint();
        state.origin = DynamicStateOrigin::Manual;
        state.hasManualBasePackage = true;
        state.manualBasePackage = { 0.0f, 100.0f, 0.7f };
        state.hitCount = hitCount;
        state.repeatability = 0.8f;
        state.ambiguity = 0.1f;
        return state;
    }

    DynamicStateMap baseMap()
    {
        DynamicStateMap map = makeEmptyDynamicStateMap();
        map.valid = true;
        map.calibration = { true, 0.20f, 0.05f };
        map.globalBase.globalBaseDelayMs = -4.0f;
        map.globalBase.globalAllpassFreqHz = 100.0f;
        map.globalBase.globalAllpassQ = 0.7f;
        map.nextStateId = 1;
        return map;
    }

    bool stateFieldsEqual (const DynamicState& a, const DynamicState& b)
    {
        return a.occupied == b.occupied && a.stableStateId == b.stableStateId
            && a.origin == b.origin && a.evidence == b.evidence
            && a.hasLearnedPackage == b.hasLearnedPackage
            && a.learnedPackage.delayDeltaMs == b.learnedPackage.delayDeltaMs
            && a.learnedPackage.allpassFreqHz == b.learnedPackage.allpassFreqHz
            && a.learnedPackage.allpassQ == b.learnedPackage.allpassQ
            && a.hasManualBasePackage == b.hasManualBasePackage
            && a.manualBasePackage.delayDeltaMs == b.manualBasePackage.delayDeltaMs
            && a.manualBasePackage.allpassFreqHz == b.manualBasePackage.allpassFreqHz
            && a.manualBasePackage.allpassQ == b.manualBasePackage.allpassQ
            && a.manualTrim.delayTrimMs == b.manualTrim.delayTrimMs
            && a.manualTrim.frequencyTrimSemitones == b.manualTrim.frequencyTrimSemitones
            && a.manualTrim.logPoleDampingTrim == b.manualTrim.logPoleDampingTrim
            && a.enabled == b.enabled && a.bypassed == b.bypassed
            && a.hitCount == b.hitCount && a.repeatability == b.repeatability
            && a.ambiguity == b.ambiguity;
    }
}

class DynamicStateEditTransactionTests : public juce::UnitTest
{
public:
    DynamicStateEditTransactionTests()
        : juce::UnitTest ("DynamicStateEditTransaction", "Dynamic State Map") {}

    void runTest() override
    {
        beginTest ("setManualTrim applies to the target Auto State only");
        {
            auto map = baseMap();
            map.states[0] = autoState (1, 5, DynamicStateEvidence::Stable);
            map.states[1] = autoState (2, 5, DynamicStateEvidence::Stable);

            const DynamicManualTrim trim { 1.0f, 2.0f, -0.5f };
            const auto result = setManualTrim (map, 1, trim);
            expect (result.success);
            expect (result.map.states[0].manualTrim.delayTrimMs == 1.0f);
            expect (result.map.states[0].manualTrim.frequencyTrimSemitones == 2.0f);
            expect (result.map.states[0].manualTrim.logPoleDampingTrim == -0.5f);
            expect (stateFieldsEqual (result.map.states[1], map.states[1]));
        }

        beginTest ("setManualTrim rejects invalid trim and unknown id without mutating the map");
        {
            auto map = baseMap();
            map.states[0] = autoState (1, 5, DynamicStateEvidence::Stable);

            const auto badTrim = setManualTrim (map, 1, { 10.0f, 0.0f, 0.0f });
            expect (! badTrim.success);
            expect (badTrim.reason == DynamicStateEditRejectionReason::InvalidTrim);
            expect (stateFieldsEqual (badTrim.map.states[0], map.states[0]));

            const auto missing = setManualTrim (map, 999, { 0.5f, 0.0f, 0.0f });
            expect (! missing.success);
            expect (missing.reason == DynamicStateEditRejectionReason::StateNotFound);
        }

        beginTest ("setManualTrim rejects a trim that pushes effective delay out of range");
        {
            auto map = baseMap();
            auto state = autoState (1, 5, DynamicStateEvidence::Stable);
            state.learnedPackage.delayDeltaMs = 2.9f;
            map.states[0] = state;

            const auto result = setManualTrim (map, 1, { 0.5f, 0.0f, 0.0f });
            expect (! result.success);
            expect (result.reason == DynamicStateEditRejectionReason::MapValidationFailed);
            expect (result.map.states[0].manualTrim.delayTrimMs == 0.0f);
        }

        beginTest ("setManualTrim also applies to Manual States");
        {
            auto map = baseMap();
            map.states[0] = manualState (1);
            const auto result = setManualTrim (map, 1, { -0.5f, 1.0f, 0.2f });
            expect (result.success);
            expect (result.map.states[0].manualTrim.delayTrimMs == -0.5f);
        }

        beginTest ("resetToLearned requires Auto origin and zeroes trim only");
        {
            auto map = baseMap();
            auto state = autoState (1, 5, DynamicStateEvidence::Stable);
            state.manualTrim = { 1.0f, 1.0f, 0.1f };
            map.states[0] = state;

            const auto result = resetToLearned (map, 1);
            expect (result.success);
            expect (isZeroDynamicManualTrim (result.map.states[0].manualTrim));
            expect (result.map.states[0].hasLearnedPackage);

            map.states[0] = manualState (1);
            const auto rejected = resetToLearned (map, 1);
            expect (! rejected.success);
            expect (rejected.reason == DynamicStateEditRejectionReason::RequiresAutoOrigin);
        }

        beginTest ("resetToGlobal clears the learned package but preserves identity");
        {
            auto map = baseMap();
            map.states[0] = autoState (1, 5, DynamicStateEvidence::Stable);

            const auto result = resetToGlobal (map, 1);
            expect (result.success);
            expect (! result.map.states[0].hasLearnedPackage);
            expect (result.map.states[0].stableStateId == 1);
            expect (result.map.states[0].fingerprint.valid);

            map.states[0] = manualState (1);
            const auto rejected = resetToGlobal (map, 1);
            expect (! rejected.success);
            expect (rejected.reason == DynamicStateEditRejectionReason::RequiresAutoOrigin);
        }

        beginTest ("setEnabled(false) also clears bypassed to preserve validity");
        {
            auto map = baseMap();
            auto state = autoState (1, 5, DynamicStateEvidence::Stable);
            state.bypassed = true;
            map.states[0] = state;

            const auto result = setEnabled (map, 1, false);
            expect (result.success);
            expect (! result.map.states[0].enabled);
            expect (! result.map.states[0].bypassed);
        }

        beginTest ("setBypassed on a disabled State fails map validation atomically");
        {
            auto map = baseMap();
            auto state = autoState (1, 5, DynamicStateEvidence::Stable);
            state.enabled = false;
            map.states[0] = state;

            const auto result = setBypassed (map, 1, true);
            expect (! result.success);
            expect (result.reason == DynamicStateEditRejectionReason::MapValidationFailed);
            expect (! result.map.states[0].bypassed);
        }

        beginTest ("promoteToManual converts an eligible Auto State exactly per Section 10");
        {
            auto map = baseMap();
            map.globalBase.globalAllpassFreqHz = 123.0f;
            map.globalBase.globalAllpassQ = 0.9f;
            map.states[0] = autoState (1, 5, DynamicStateEvidence::Stable);

            const auto result = promoteToManual (map, 1);
            expect (result.success);
            const auto& promoted = result.map.states[0];
            expect (promoted.origin == DynamicStateOrigin::Manual);
            expect (promoted.stableStateId == 1);
            expect (! promoted.hasLearnedPackage);
            expect (promoted.hasManualBasePackage);
            expect (promoted.manualBasePackage.delayDeltaMs == 0.0f);
            expect (promoted.manualBasePackage.allpassFreqHz == 123.0f);
            expect (promoted.manualBasePackage.allpassQ == 0.9f);
            expect (isZeroDynamicManualTrim (promoted.manualTrim));
            expect (promoted.hitCount == 5);
        }

        beginTest ("promoteToManual rejects insufficient evidence, invalid fingerprint, and already-Manual");
        {
            auto map = baseMap();
            map.states[0] = autoState (1, 2, DynamicStateEvidence::Candidate);
            const auto tooFewHits = promoteToManual (map, 1);
            expect (! tooFewHits.success);
            expect (tooFewHits.reason == DynamicStateEditRejectionReason::InsufficientEvidence);

            auto badFingerprintMap = baseMap();
            auto badState = autoState (1, 5, DynamicStateEvidence::Stable);
            badState.fingerprint.valid = false;
            badFingerprintMap.states[0] = badState;
            const auto badFingerprint = promoteToManual (badFingerprintMap, 1);
            expect (! badFingerprint.success);
            expect (badFingerprint.reason == DynamicStateEditRejectionReason::InvalidFingerprint);

            auto alreadyManualMap = baseMap();
            alreadyManualMap.states[0] = manualState (1);
            const auto alreadyManual = promoteToManual (alreadyManualMap, 1);
            expect (! alreadyManual.success);
            expect (alreadyManual.reason == DynamicStateEditRejectionReason::AlreadyManual);
        }

        beginTest ("removeManualState frees the slot without recycling nextStateId or touching other States");
        {
            auto map = baseMap();
            map.states[0] = manualState (1);
            map.states[1] = autoState (2, 5, DynamicStateEvidence::Stable);
            map.nextStateId = 3;

            const auto result = removeManualState (map, 1);
            expect (result.success);
            expect (! result.map.states[0].occupied);
            expect (result.map.nextStateId == 3);
            expect (stateFieldsEqual (result.map.states[1], map.states[1]));

            const auto rejectAuto = removeManualState (map, 2);
            expect (! rejectAuto.success);
            expect (rejectAuto.reason == DynamicStateEditRejectionReason::RequiresManualOrigin);
        }

        beginTest ("createManualState allocates a monotonic id and never recycles it");
        {
            auto map = baseMap();
            map.nextStateId = 5;

            DynamicManualStateCreationRequest request;
            request.fingerprint = validFingerprint();
            request.hitCount = 4;
            request.evidence = DynamicStateEvidence::Stable;

            const auto result = createManualState (map, request);
            expect (result.success);
            expect (result.map.nextStateId == 6);
            const int slot = DynamicStateEditDetail::findOccupiedSlotByStableId (result.map, 5);
            expect (slot == 0);
            expect (result.map.states[(size_t) slot].origin == DynamicStateOrigin::Manual);
            expect (result.map.states[(size_t) slot].hasManualBasePackage);

            DynamicManualStateCreationRequest tooFewHits;
            tooFewHits.fingerprint = validFingerprint();
            tooFewHits.hitCount = 1;
            const auto rejected = createManualState (map, tooFewHits);
            expect (! rejected.success);
            expect (rejected.reason == DynamicStateEditRejectionReason::InsufficientEvidence);
            expect (result.map.nextStateId == 6); // unaffected by the rejected attempt
        }

        beginTest ("createManualState rejects when all 8 slots are occupied");
        {
            auto map = baseMap();
            for (int i = 0; i < DynamicStateMapContract::kMaxPersistentStates; ++i)
                map.states[(size_t) i] = autoState ((uint64_t) i + 1, 5, DynamicStateEvidence::Stable);
            map.nextStateId = 9;

            DynamicManualStateCreationRequest request;
            request.fingerprint = validFingerprint();
            request.hitCount = 4;
            const auto result = createManualState (map, request);
            expect (! result.success);
            expect (result.reason == DynamicStateEditRejectionReason::NoFreeCapacity);
        }
    }
};

static DynamicStateEditTransactionTests dynamicStateEditTransactionTests;
