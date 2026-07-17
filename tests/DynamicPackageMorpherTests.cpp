#include "TestCommon.h"

#include "DynamicAllpassPoleDomain.h"
#include "DynamicPackageMorpher.h"

#include <array>
#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>

namespace
{
    constexpr std::array<double, 5> kSampleRates { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };

    DynamicFingerprintPrototype validFingerprint()
    {
        DynamicFingerprintPrototype fingerprint;
        fingerprint.valid = true;
        fingerprint.featureCount = 1;
        return fingerprint;
    }

    DynamicState autoState (uint64_t id, bool stable = true, bool learned = true)
    {
        DynamicState state;
        state.occupied = true;
        state.stableStateId = id;
        state.fingerprint = validFingerprint();
        state.origin = DynamicStateOrigin::Auto;
        state.evidence = stable ? DynamicStateEvidence::Stable : DynamicStateEvidence::Candidate;
        state.hitCount = stable ? DynamicStateMapContract::kStableAutoMinimumRepeatableHits
                                : DynamicStateMapContract::kCandidateMinimumRepeatableHits;
        state.repeatability = 0.9f;
        state.ambiguity = 0.1f;
        state.hasLearnedPackage = learned;
        state.learnedPackage = { -1.5f, 160.0f, 1.4f };
        return state;
    }

    DynamicState manualState (uint64_t id, DynamicStateEvidence evidence = DynamicStateEvidence::Candidate)
    {
        auto state = autoState (id, false, false);
        state.origin = DynamicStateOrigin::Manual;
        state.evidence = evidence;
        state.hitCount = DynamicStateMapContract::kManualMinimumRepeatableHits;
        state.hasManualBasePackage = true;
        state.manualBasePackage = { -1.5f, 160.0f, 1.4f };
        return state;
    }

    DynamicStateMap mapWith (const DynamicState& state)
    {
        DynamicStateMap map = makeEmptyDynamicStateMap();
        map.valid = true;
        map.calibration = { true, 0.25f, 0.125f };
        map.globalBase.globalBaseDelayMs = 1.0f;
        map.globalBase.globalAllpassFreqHz = 80.0f;
        map.globalBase.globalAllpassQ = 0.5f;
        map.globalBase.polarityInvert = true;
        map.globalBase.crossoverEnabled = false;
        map.globalBase.crossoverHz = 230.0f;
        map.globalBase.allpassEnabled = true;
        map.globalBase.allpassStages = 4;
        map.globalBase.delayInterpolationIndex = 1;
        map.states[0] = state;
        map.nextStateId = state.stableStateId + 1u;
        return map;
    }

    bool isGlobalResult (const DynamicPackageResolution& result, const DynamicStateMap& map)
    {
        return result.effectiveAbsoluteDelayMs == (double) map.globalBase.globalBaseDelayMs
            && result.morphedDelayDeltaMs == 0.0
            && result.effectiveAllpassFreqHz == (double) map.globalBase.globalAllpassFreqHz
            && result.effectiveAllpassQ == (double) map.globalBase.globalAllpassQ;
    }

    double magnitude (const DynamicAllpassCoefficients& c, double omega)
    {
        const double cosine = std::cos (omega);
        const double sine = std::sin (omega);
        const double cosine2 = std::cos (2.0 * omega);
        const double sine2 = std::sin (2.0 * omega);
        const double nr = c.b0 + c.b1 * cosine + c.b2 * cosine2;
        const double ni = -c.b1 * sine - c.b2 * sine2;
        const double dr = 1.0 + c.a1 * cosine + c.a2 * cosine2;
        const double di = -c.a1 * sine - c.a2 * sine2;
        return std::sqrt ((nr * nr + ni * ni) / (dr * dr + di * di));
    }
}

class DynamicPackageMorpherTests : public juce::UnitTest
{
public:
    DynamicPackageMorpherTests() : juce::UnitTest ("DynamicPackageMorpher", "DSP") {}

    void runTest() override
    {
        using namespace DynamicAllpassPoleDomain;
        static_assert (std::is_trivially_copyable_v<DynamicAllpassCoefficients>);
        static_assert (std::is_trivially_copyable_v<DynamicPackageResolution>);

        beginTest ("Pole coordinates round-trip exact Dynamic ranges at supported rates");
        {
            for (const double sampleRate : kSampleRates)
            {
                for (const double frequency : { 30.0, 61.25, 100.0, 220.0 })
                {
                    for (const double q : { 0.35, 0.7, 1.35, 2.0 })
                    {
                        DynamicAllpassPoleCoordinates coordinates;
                        DynamicAllpassFrequencyQ roundTrip;
                        expect (frequencyQToPoleCoordinates (frequency, q, sampleRate, coordinates));
                        expect (poleCoordinatesToFrequencyQ (coordinates.log2FrequencyHz,
                                                             coordinates.logPoleDamping,
                                                             sampleRate,
                                                             roundTrip));
                        expectWithinAbsoluteError (roundTrip.frequencyHz, frequency, 1.0e-9);
                        expectWithinAbsoluteError (roundTrip.q, q, 1.0e-9);
                        expect (std::isfinite (coordinates.logPoleDamping));
                    }
                }
            }

            DynamicAllpassPoleCoordinates ignored;
            expect (! isValidSampleRate (0.0));
            expect (! isValidSampleRate (-48000.0));
            expect (! isValidSampleRate (440.0));
            expect (! isValidSampleRate (std::numeric_limits<double>::quiet_NaN()));
            expect (! isValidSampleRate (std::numeric_limits<double>::infinity()));
            expect (! frequencyQToPoleCoordinates (30.0, 0.7, 0.0, ignored));
            expect (! frequencyQToPoleCoordinates (std::numeric_limits<double>::quiet_NaN(), 0.7, 48000.0, ignored));
            expect (! frequencyQToPoleCoordinates (100.0, std::numeric_limits<double>::infinity(), 48000.0, ignored));
        }

        beginTest ("Normalized coefficients match JUCE makeAllPass and preserve unity magnitude");
        {
            for (const double sampleRate : kSampleRates)
            {
                for (const double frequency : { 30.0, 100.0, 220.0 })
                {
                    for (const double q : { 0.35, 0.7, 2.0 })
                    {
                        DynamicAllpassCoefficients coefficients;
                        expect (makeCoefficients (frequency, q, sampleRate, coefficients));
                        expect (isValidCoefficients (coefficients));
                        expectWithinAbsoluteError (coefficients.b0, coefficients.a2, 0.0);
                        expectWithinAbsoluteError (coefficients.b1, coefficients.a1, 0.0);
                        expectWithinAbsoluteError (coefficients.b2, 1.0, 0.0);

                        // ArrayCoefficients layout: b0, b1, b2, a0, a1, a2 (a0 normalized to 1).
                        const auto reference = juce::dsp::IIR::ArrayCoefficients<float>::makeAllPass (
                            sampleRate, (float) frequency, (float) q);
                        expectWithinAbsoluteError (coefficients.b0, (double) reference[0], 2.0e-6);
                        expectWithinAbsoluteError (coefficients.b1, (double) reference[1], 2.0e-6);
                        expectWithinAbsoluteError (coefficients.b2, (double) reference[2], 2.0e-6);
                        expectWithinAbsoluteError (coefficients.a1, (double) reference[4], 2.0e-6);
                        expectWithinAbsoluteError (coefficients.a2, (double) reference[5], 2.0e-6);
                        for (const double omega : { 0.0, 0.1, 0.7, 1.3, 2.8 })
                            expectWithinAbsoluteError (magnitude (coefficients, omega), 1.0, 1.0e-11);

                        DynamicAllpassCoefficients repeated;
                        expect (makeCoefficients (frequency, q, sampleRate, repeated));
                        expect (repeated.a1 == coefficients.a1 && repeated.a2 == coefficients.a2);
                    }
                }
            }
        }

        beginTest ("Strength endpoints clamp as specified and retain exact Global and source values");
        {
            const auto map = mapWith (autoState (11));
            const auto zero = resolveDynamicPackage (map, 11, 0.0, 48000.0);
            expect (zero.valid && zero.decision == DynamicPackageDecision::StateCorrection);
            expect (isGlobalResult (zero, map));
            expect (! zero.correctionApplied);

            const auto one = resolveDynamicPackage (map, 11, 1.0, 48000.0);
            expect (one.valid && one.correctionApplied);
            expect (one.trimmedStateDelayDeltaMs == (double) map.states[0].learnedPackage.delayDeltaMs);
            expect (one.morphedDelayDeltaMs == (double) map.states[0].learnedPackage.delayDeltaMs);
            expect (one.effectiveAllpassFreqHz == (double) map.states[0].learnedPackage.allpassFreqHz);
            expect (one.effectiveAllpassQ == (double) map.states[0].learnedPackage.allpassQ);

            const auto below = resolveDynamicPackage (map, 11, -1.0, 48000.0);
            const auto above = resolveDynamicPackage (map, 11, 2.0, 48000.0);
            expect (below.valid && below.effectiveStrength == 0.0 && isGlobalResult (below, map));
            expect (above.valid && above.effectiveStrength == 1.0
                    && above.effectiveAllpassFreqHz == one.effectiveAllpassFreqHz);
            expect (! resolveDynamicPackage (map, 11, std::numeric_limits<double>::quiet_NaN(), 48000.0).valid);
        }

        beginTest ("Midpoint uses geometric frequency and geometric pole damping, never raw Q");
        {
            auto state = autoState (12);
            state.learnedPackage = { 0.0f, 200.0f, 1.8f };
            auto map = mapWith (state);
            map.globalBase.globalAllpassFreqHz = 50.0f;
            map.globalBase.globalAllpassQ = 0.4f;
            const auto result = resolveDynamicPackage (map, 12, 0.5, 48000.0);
            expect (result.valid);

            // Hand-calc must use the same float storage the resolver reads, so
            // double promotion matches the production path bit-for-bit.
            DynamicAllpassPoleCoordinates globalCoordinates, stateCoordinates;
            expect (frequencyQToPoleCoordinates ((double) map.globalBase.globalAllpassFreqHz,
                                                 (double) map.globalBase.globalAllpassQ,
                                                 48000.0,
                                                 globalCoordinates));
            expect (frequencyQToPoleCoordinates ((double) map.states[0].learnedPackage.allpassFreqHz,
                                                 (double) map.states[0].learnedPackage.allpassQ,
                                                 48000.0,
                                                 stateCoordinates));
            const double midLog2Frequency = 0.5 * (globalCoordinates.log2FrequencyHz
                                                   + stateCoordinates.log2FrequencyHz);
            const double midLogPoleDamping = 0.5 * (globalCoordinates.logPoleDamping
                                                    + stateCoordinates.logPoleDamping);
            DynamicAllpassFrequencyQ expected;
            expect (poleCoordinatesToFrequencyQ (midLog2Frequency, midLogPoleDamping, 48000.0, expected));
            expectWithinAbsoluteError (result.effectiveAllpassFreqHz, expected.frequencyHz, 1.0e-12);
            expectWithinAbsoluteError (result.effectiveAllpassQ, expected.q, 1.0e-12);
            expect (std::abs (result.effectiveAllpassQ - 1.1) > 1.0e-3,
                    "raw-Q midpoint must not define package morphing");
            expectWithinAbsoluteError (result.effectiveAllpassFreqHz, 100.0, 1.0e-4);
        }

        beginTest ("Manual trim is applied before Strength and respects frozen frequency and damping bounds");
        {
            auto state = manualState (13);
            state.manualBasePackage = { 1.0f, 100.0f, 0.7f };
            state.manualTrim = { 0.5f, 12.0f, 0.0f };
            const auto before = state;
            auto map = mapWith (state);
            const auto doubled = resolveDynamicPackage (map, 13, 1.0, 48000.0);
            expect (doubled.valid && doubled.sourceWasManual);
            expectWithinAbsoluteError (doubled.effectiveAllpassFreqHz, 200.0, 1.0e-9);
            expectWithinAbsoluteError (doubled.trimmedStateDelayDeltaMs, 1.5, 1.0e-9);
            expect (map.states[0].manualBasePackage.allpassFreqHz == before.manualBasePackage.allpassFreqHz
                    && map.states[0].manualTrim.frequencyTrimSemitones == before.manualTrim.frequencyTrimSemitones);

            map.states[0].manualTrim.frequencyTrimSemitones = -12.0f;
            const auto halved = resolveDynamicPackage (map, 13, 1.0, 48000.0);
            expectWithinAbsoluteError (halved.effectiveAllpassFreqHz, 50.0, 1.0e-9);
            map.states[0].manualTrim.frequencyTrimSemitones = 24.0f;
            const auto highClamp = resolveDynamicPackage (map, 13, 1.0, 48000.0);
            expect (highClamp.valid && highClamp.frequencyTrimClamped);
            expectWithinAbsoluteError (highClamp.effectiveAllpassFreqHz, 220.0, 1.0e-9);
            map.states[0].manualTrim.frequencyTrimSemitones = -24.0f;
            const auto lowClamp = resolveDynamicPackage (map, 13, 1.0, 48000.0);
            expect (lowClamp.valid && lowClamp.frequencyTrimClamped);
            expectWithinAbsoluteError (lowClamp.effectiveAllpassFreqHz, 30.0, 1.0e-9);

            map.states[0].manualTrim = { 0.0f, 0.0f, (float) std::log (2.0) };
            const auto doubledDamping = resolveDynamicPackage (map, 13, 1.0, 48000.0);
            DynamicAllpassPoleCoordinates sourceCoordinates, doubledCoordinates;
            expect (frequencyQToPoleCoordinates (100.0, 0.7, 48000.0, sourceCoordinates));
            expect (frequencyQToPoleCoordinates (doubledDamping.effectiveAllpassFreqHz,
                                                 doubledDamping.effectiveAllpassQ,
                                                 48000.0,
                                                 doubledCoordinates));
            expectWithinAbsoluteError (std::exp (doubledCoordinates.logPoleDamping),
                                               2.0 * std::exp (sourceCoordinates.logPoleDamping), 1.0e-8);
            map.states[0].manualTrim.logPoleDampingTrim = 2.0f;
            const auto dampingClamp = resolveDynamicPackage (map, 13, 1.0, 48000.0);
            expect (dampingClamp.valid && dampingClamp.dampingTrimClamped);
        }

        beginTest ("State policy is stable-ID based and preserves Global no-correction behavior");
        {
            auto stableAutoNoFix = autoState (21, true, false);
            auto noFixMap = mapWith (stableAutoNoFix);
            const auto noFix = resolveDynamicPackage (noFixMap, 21, 1.0, 48000.0);
            expect (noFix.valid && noFix.decision == DynamicPackageDecision::GlobalRecognizedNoCorrection
                    && noFix.selectedStableStateId == 21u && ! noFix.correctionAvailable
                    && isGlobalResult (noFix, noFixMap));

            auto bypassed = autoState (22);
            bypassed.bypassed = true;
            const auto bypassMap = mapWith (bypassed);
            const auto bypass = resolveDynamicPackage (bypassMap, 22, 1.0, 48000.0);
            expect (bypass.valid && bypass.decision == DynamicPackageDecision::GlobalBypassed
                    && bypass.selectedBypassed && bypass.selectedStableStateId == 22u
                    && isGlobalResult (bypass, bypassMap));

            const auto missing = resolveDynamicPackage (bypassMap, 999, 1.0, 48000.0);
            const auto zeroId = resolveDynamicPackage (bypassMap, 0, 1.0, 48000.0);
            expect (missing.valid && missing.decision == DynamicPackageDecision::GlobalNoSelection
                    && missing.selectedStableStateId == 0 && isGlobalResult (missing, bypassMap));
            expect (zeroId.valid && zeroId.decision == DynamicPackageDecision::GlobalNoSelection);

            auto candidateMap = mapWith (autoState (23, false));
            expect (resolveDynamicPackage (candidateMap, 23, 1.0, 48000.0).decision
                    == DynamicPackageDecision::GlobalNoSelection);

            auto manualMap = mapWith (manualState (24));
            expect (resolveDynamicPackage (manualMap, 24, 1.0, 48000.0).valid);
            std::swap (manualMap.states[0], manualMap.states[3]);
            expect (resolveDynamicPackage (manualMap, 24, 1.0, 48000.0).valid);

            auto duplicate = mapWith (autoState (25));
            duplicate.states[1] = duplicate.states[0];
            expect (! resolveDynamicPackage (duplicate, 25, 1.0, 48000.0).valid);
        }

        beginTest ("Global-only metadata, full-target look-ahead and disabled allpass metadata remain unchanged");
        {
            auto state = autoState (31);
            state.learnedPackage = { -3.0f, 160.0f, 1.4f };
            auto map = mapWith (state);
            map.globalBase.globalBaseDelayMs = -4.0f;
            map.globalBase.allpassEnabled = false;
            const auto valid = resolveDynamicPackage (map, 31, 1.0, 48000.0);
            expect (valid.valid);
            expectWithinAbsoluteError (valid.effectiveAbsoluteDelayMs, -7.0, 1.0e-11);
            expect (valid.polarityInvert == map.globalBase.polarityInvert
                    && valid.allpassStages == map.globalBase.allpassStages
                    && valid.crossoverEnabled == map.globalBase.crossoverEnabled
                    && valid.crossoverHz == (double) map.globalBase.crossoverHz
                    && ! valid.allpassEnabled
                    && valid.delayInterpolationIndex == map.globalBase.delayInterpolationIndex);

            // Source + Manual delay trim must stay inside the frozen +/-3 ms State range.
            // Strength 0 must still reject a full target that violates the stored budget.
            auto invalidTarget = map;
            invalidTarget.states[0].learnedPackage.delayDeltaMs = -3.0f;
            invalidTarget.states[0].manualTrim.delayTrimMs = -0.001f;
            expect (! resolveDynamicPackage (invalidTarget, 31, 0.0, 48000.0).valid,
                    "Strength must not weaken full-target delay validation");
            expect (! resolveDynamicPackage (invalidTarget, 31, 1.0, 48000.0).valid);

            auto disabled = map;
            disabled.states[0].enabled = false;
            const auto disabledResult = resolveDynamicPackage (disabled, 31, 1.0, 48000.0);
            expect (disabledResult.valid && disabledResult.decision == DynamicPackageDecision::GlobalNoSelection);
        }

        beginTest ("Repeated lookup, conversion, coefficients and resolution are deterministic fixed-size operations");
        {
            const auto map = mapWith (autoState (41));
            const auto baseline = resolveDynamicPackage (map, 41, 0.37, 96000.0);
            expect (baseline.valid);
            for (int i = 0; i < 10000; ++i)
            {
                const auto* state = findDynamicStateByStableId (map, 41);
                DynamicAllpassPoleCoordinates coordinates;
                DynamicAllpassCoefficients coefficients;
                const auto result = resolveDynamicPackage (map, 41, 0.37, 96000.0);
                expect (state != nullptr);
                expect (frequencyQToPoleCoordinates (100.0, 0.7, 96000.0, coordinates));
                expect (makeCoefficients (100.0, 0.7, 96000.0, coefficients));
                expect (result.valid && result.effectiveAllpassQ == baseline.effectiveAllpassQ);
            }
        }
    }
};

static DynamicPackageMorpherTests dynamicPackageMorpherTests;
