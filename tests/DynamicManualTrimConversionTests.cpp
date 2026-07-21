#include "TestCommon.h"
#include "dsp/DynamicManualTrimConversion.h"

class DynamicManualTrimConversionTests : public juce::UnitTest
{
public:
    DynamicManualTrimConversionTests()
        : juce::UnitTest ("DynamicManualTrimConversion", "Dynamic State Map") {}

    void runTest() override
    {
        beginTest ("Zero trim leaves the effective frequency/Q equal to the source package");
        {
            const DynamicZonePackage source { 0.5f, 100.0f, 0.9f };
            double freq = 0.0, q = 0.0;
            expect (DynamicManualTrimConversion::effectiveFrequencyQ (
                source, makeZeroDynamicManualTrim(), 48000.0, freq, q));
            expectWithinAbsoluteError (freq, 100.0, 1.0e-6);
            expectWithinAbsoluteError (q, 0.9, 1.0e-6);
        }

        beginTest ("Frequency/semitone round trip: trim -> effective -> trim is stable");
        {
            // Values chosen so 120 Hz * 2^(semitones/12) stays comfortably
            // inside [30, 220] Hz - this test proves the round trip is
            // stable, not the separately-tested clamp-at-the-boundary
            // behavior (see "stays within the frozen ... bounds" below).
            const DynamicZonePackage source { 0.0f, 120.0f, 0.8f };
            for (const float semitones : { -12.0f, -3.0f, 0.0f, 5.0f, 6.0f })
            {
                DynamicManualTrim trim;
                trim.frequencyTrimSemitones = semitones;

                double freq = 0.0, q = 0.0;
                expect (DynamicManualTrimConversion::effectiveFrequencyQ (source, trim, 48000.0, freq, q));

                float recoveredSemitones = 0.0f, recoveredDamping = 0.0f;
                expect (DynamicManualTrimConversion::manualTrimForEffectiveFrequencyQ (
                    source, freq, q, 48000.0, recoveredSemitones, recoveredDamping));
                expectWithinAbsoluteError (recoveredSemitones, semitones, 0.01f);
            }
        }

        beginTest ("Q/log-pole-damping round trip: trim -> effective -> trim is stable");
        {
            // Values chosen to stay comfortably inside this frequency's valid
            // log-pole-damping envelope (Q in [0.35, 2.0]) - this test proves
            // the round trip is stable, not the separately-tested clamp-at-
            // the-envelope-edge behavior.
            const DynamicZonePackage source { 0.0f, 100.0f, 0.7f };
            for (const float dampingTrim : { -0.5f, -0.2f, 0.0f, 0.2f, 0.5f })
            {
                DynamicManualTrim trim;
                trim.logPoleDampingTrim = dampingTrim;

                double freq = 0.0, q = 0.0;
                expect (DynamicManualTrimConversion::effectiveFrequencyQ (source, trim, 48000.0, freq, q));

                float recoveredSemitones = 0.0f, recoveredDamping = 0.0f;
                expect (DynamicManualTrimConversion::manualTrimForEffectiveFrequencyQ (
                    source, freq, q, 48000.0, recoveredSemitones, recoveredDamping));
                expectWithinAbsoluteError (recoveredDamping, dampingTrim, 0.01f);
            }
        }

        beginTest ("Effective frequency stays within the frozen allpass frequency bounds");
        {
            const DynamicZonePackage source { 0.0f, 40.0f, 0.7f };
            DynamicManualTrim trim;
            trim.frequencyTrimSemitones = -24.0f; // maximum downward trim
            double freq = 0.0, q = 0.0;
            expect (DynamicManualTrimConversion::effectiveFrequencyQ (source, trim, 48000.0, freq, q));
            expect (freq >= (double) DynamicStateMapContract::kAllpassFrequencyMinHz);
            expect (freq <= (double) DynamicStateMapContract::kAllpassFrequencyMaxHz);
        }

        beginTest ("Effective Q stays within the frozen Q bounds after a large damping trim");
        {
            const DynamicZonePackage source { 0.0f, 100.0f, 1.0f };
            DynamicManualTrim trim;
            trim.logPoleDampingTrim = 2.0f; // maximum trim
            double freq = 0.0, q = 0.0;
            expect (DynamicManualTrimConversion::effectiveFrequencyQ (source, trim, 48000.0, freq, q));
            expect (q >= (double) DynamicStateMapContract::kAllpassQMin);
            expect (q <= (double) DynamicStateMapContract::kAllpassQMax);
        }

        beginTest ("Sample-rate edge cases and invalid inputs are rejected, not silently wrong");
        {
            const DynamicZonePackage source { 0.0f, 100.0f, 0.7f };
            double freq = 0.0, q = 0.0;
            expect (! DynamicManualTrimConversion::effectiveFrequencyQ (
                source, makeZeroDynamicManualTrim(), -48000.0, freq, q));
            expect (! DynamicManualTrimConversion::effectiveFrequencyQ (
                source, makeZeroDynamicManualTrim(), 0.0, freq, q));

            float semitones = 0.0f, damping = 0.0f;
            expect (! DynamicManualTrimConversion::manualTrimForEffectiveFrequencyQ (
                source, 100.0, 0.7, 0.0, semitones, damping));
            // Desired frequency outside the valid allpass range is rejected,
            // not clamped-then-silently-wrong.
            expect (! DynamicManualTrimConversion::manualTrimForEffectiveFrequencyQ (
                source, 5.0, 0.7, 48000.0, semitones, damping));
        }

        beginTest ("manualTrimForEffectiveFrequencyQ output stays within the frozen manual-trim bounds");
        {
            const DynamicZonePackage source { 0.0f, 200.0f, 0.4f };
            float semitones = 0.0f, damping = 0.0f;
            // A tiny target frequency relative to the source would demand an
            // enormous downward semitone shift; the result must still clamp
            // to the frozen manual-trim bounds rather than overflow them.
            expect (DynamicManualTrimConversion::manualTrimForEffectiveFrequencyQ (
                source, 30.0, 2.0, 48000.0, semitones, damping));
            expect (semitones >= DynamicStateMapContract::kManualFrequencyTrimMinSemitones);
            expect (semitones <= DynamicStateMapContract::kManualFrequencyTrimMaxSemitones);
            expect (damping >= DynamicStateMapContract::kManualLogPoleDampingTrimMin);
            expect (damping <= DynamicStateMapContract::kManualLogPoleDampingTrimMax);
        }
    }
};

static DynamicManualTrimConversionTests dynamicManualTrimConversionTests;
