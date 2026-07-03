#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <iostream>

#include "CorrelationMeter.h"
#include "FractionalDelayLine.h"
#include "AllpassPhaseRotator.h"
#include "AlignmentAnalyzer.h"
#include "AnalyzerInstruction.h"
#include "TransientDetector.h"
#include "MultiBandCorrelation.h"
#include "PerHitAnalyzer.h"

namespace
{
    constexpr double kSampleRate = 48000.0;
    constexpr double kTwoPi = juce::MathConstants<double>::twoPi;
}

//==============================================================================
class CorrelationMeterTests : public juce::UnitTest
{
public:
    CorrelationMeterTests() : juce::UnitTest ("CorrelationMeter", "DSP") {}

    void runTest() override
    {
        beginTest ("Identical signals converge to high correlation");
        {
            CorrelationMeter meter;
            meter.prepare (kSampleRate, 1024);

            const double freq = 1000.0;
            for (int i = 0; i < 20000; ++i)
            {
                const float a = (float) std::sin (kTwoPi * freq * (double) i / kSampleRate);
                meter.pushSample (a, a);
            }

            expectGreaterThan (meter.getCorrelation(), 95.0f);
        }

        beginTest ("Inverted signals converge to low correlation");
        {
            CorrelationMeter meter;
            meter.prepare (kSampleRate, 1024);

            const double freq = 1000.0;
            for (int i = 0; i < 20000; ++i)
            {
                const float a = (float) std::sin (kTwoPi * freq * (double) i / kSampleRate);
                meter.pushSample (a, -a);
            }

            expectLessThan (meter.getCorrelation(), 5.0f);
        }
    }
};

static CorrelationMeterTests correlationMeterTestsInstance;

//==============================================================================
class FractionalDelayLineTests : public juce::UnitTest
{
public:
    FractionalDelayLineTests() : juce::UnitTest ("FractionalDelayLine", "DSP") {}

    void runTest() override
    {
        beginTest ("Integer delay via Linear interpolation places impulse correctly");
        {
            FractionalDelayLine delay;
            delay.prepare (kSampleRate, 50.0f);
            delay.setInterpolationType (InterpolationType::Linear);
            delay.setDelaySamples (5.0f);

            const int numSamples = 32;
            std::vector<float> output ((size_t) numSamples, 0.0f);

            for (int i = 0; i < numSamples; ++i)
            {
                const float input = (i == 0) ? 1.0f : 0.0f;
                output[(size_t) i] = delay.processSample (input);
            }

            for (int i = 0; i < numSamples; ++i)
            {
                if (i == 5)
                    expectWithinAbsoluteError (output[(size_t) i], 1.0f, 1.0e-4f);
                else
                    expectWithinAbsoluteError (output[(size_t) i], 0.0f, 1.0e-4f);
            }
        }

        beginTest ("Allpass interpolation produces finite, roughly-delayed output");
        {
            FractionalDelayLine delay;
            delay.prepare (kSampleRate, 50.0f);
            delay.setInterpolationType (InterpolationType::Allpass);
            delay.setDelaySamples (5.0f);

            const int numSamples = 64;
            int peakIndex = -1;
            float peakValue = 0.0f;

            for (int i = 0; i < numSamples; ++i)
            {
                const float input = (i == 0) ? 1.0f : 0.0f;
                const float y = delay.processSample (input);

                expect (std::isfinite (y));

                if (std::abs (y) > peakValue)
                {
                    peakValue = std::abs (y);
                    peakIndex = i;
                }
            }

            // allpass smears energy, so allow a wider tolerance window around
            // the expected integer delay of 5 samples
            expect (peakIndex >= 3 && peakIndex <= 7);
            expectGreaterThan (peakValue, 0.3f);
        }
    }
};

static FractionalDelayLineTests fractionalDelayLineTestsInstance;

//==============================================================================
class AllpassPhaseRotatorTests : public juce::UnitTest
{
public:
    AllpassPhaseRotatorTests() : juce::UnitTest ("AllpassPhaseRotator", "DSP") {}

    void runTest() override
    {
        for (int numStages : { 2, 3, 4 })
        {
            beginTest ("Unity gain and finite output with activeStages = " + juce::String (numStages));

            AllpassPhaseRotator rotator;
            rotator.prepare (kSampleRate, numStages);
            rotator.setParameters (500.0f, 0.7f);

            const double freq = 500.0;
            const int numSamples = (int) (kSampleRate / freq) * 20; // ~20 periods

            double inputSumSq = 0.0;
            double outputSumSq = 0.0;

            for (int i = 0; i < numSamples; ++i)
            {
                const float input = (float) std::sin (kTwoPi * freq * (double) i / kSampleRate);
                const float output = rotator.processSample (input);

                expect (std::isfinite (output));

                inputSumSq += (double) input * (double) input;
                outputSumSq += (double) output * (double) output;
            }

            const double inputRms = std::sqrt (inputSumSq / (double) numSamples);
            const double outputRms = std::sqrt (outputSumSq / (double) numSamples);

            expectWithinAbsoluteError (outputRms, inputRms, inputRms * 0.1);
        }
    }
};

static AllpassPhaseRotatorTests allpassPhaseRotatorTestsInstance;

//==============================================================================
class AlignmentAnalyzerTests : public juce::UnitTest
{
public:
    AlignmentAnalyzerTests() : juce::UnitTest ("AlignmentAnalyzer", "DSP") {}

    void runTest() override
    {
        // Build a shared "event" signal (a decaying sine burst) and place it in
        // the two channels at a known relative offset, then check the analyzer
        // recovers that offset and the correct polarity.
        const int   n        = 16384;
        const double freq    = 80.0; // bass-ish
        auto event = [freq] (int i)
        {
            const double t = (double) i / kSampleRate;
            return (float) (std::sin (kTwoPi * freq * t) * std::exp (-t * 6.0));
        };

        beginTest ("Recovers a known lag: kick leads bass -> negative signed recommendation");
        {
            // Bass event starts 40 samples LATER than the kick event. The kick
            // leads, so the analyzer returns a negative signed timing
            // recommendation. The processor/UI layer must not process the
            // sidechain with that value.
            const int lag = 40;
            std::vector<float> bass ((size_t) n, 0.0f), kick ((size_t) n, 0.0f);
            for (int i = 0; i < n; ++i)
            {
                kick[(size_t) i] = event (i);
                if (i - lag >= 0)
                    bass[(size_t) i] = event (i - lag);
            }

            const auto r = AlignmentAnalyzer::analyze (bass.data(), kick.data(), n, kSampleRate, 50.0f);

            expect (r.valid);
            expect (! r.invertPolarity);
            const float expectedMs = (float) (-lag / kSampleRate * 1000.0);
            expectWithinAbsoluteError (r.delayMs, expectedMs, 0.1f);
            expectGreaterThan (r.afterMatch, r.beforeMatch);
            expectGreaterThan (r.afterMatch, 90.0f);
        }

        beginTest ("Ignores out-of-band interference (the fix for random-feeling results)");
        {
            // The realistic failure case: both signals share a loud broadband
            // transient (a 5 kHz 'click', well above the kick/bass overlap band)
            // that lines up at lag 0, while the actual LOW-END thump we care about
            // is offset. A full-range correlation would lock onto the click and
            // report ~0 delay; band-limiting to 30-120 Hz must recover the true
            // low-end lag instead.
            const int    lag     = 40;
            const double lowFreq = 60.0;
            const double hiFreq  = 5000.0; // out of band

            auto thump = [lowFreq] (int i)
            {
                const double t = (double) i / kSampleRate;
                return std::sin (kTwoPi * lowFreq * t) * std::exp (-t * 5.0);
            };
            auto click = [hiFreq] (int i)
            {
                const double t = (double) i / kSampleRate;
                return std::sin (kTwoPi * hiFreq * t) * std::exp (-t * 200.0);
            };

            std::vector<float> bass ((size_t) n, 0.0f), kick ((size_t) n, 0.0f);
            for (int i = 0; i < n; ++i)
            {
                // Identical loud click at the same time in both (would dominate a
                // broadband correlation and pin the peak to lag 0).
                const double sharedClick = 4.0 * click (i);

                kick[(size_t) i] = (float) (sharedClick + thump (i));
                if (i - lag >= 0)
                    bass[(size_t) i] = (float) (sharedClick + thump (i - lag));
                else
                    bass[(size_t) i] = (float) sharedClick;
            }

            const auto r = AlignmentAnalyzer::analyze (bass.data(), kick.data(), n, kSampleRate, 50.0f);

            expect (r.valid);
            // Must recover the low-end lag, NOT the click's zero offset.
            const float expectedMs = (float) (-lag / kSampleRate * 1000.0);
            expectWithinAbsoluteError (r.delayMs, expectedMs, 0.15f);
        }

        beginTest ("Detects inverted polarity");
        {
            // Same event, aligned, but the bass is phase-inverted. Analyzer
            // should recommend a polarity flip and ~zero delay.
            std::vector<float> bass ((size_t) n, 0.0f), kick ((size_t) n, 0.0f);
            for (int i = 0; i < n; ++i)
            {
                kick[(size_t) i] = event (i);
                bass[(size_t) i] = -event (i);
            }

            const auto r = AlignmentAnalyzer::analyze (bass.data(), kick.data(), n, kSampleRate, 50.0f);

            expect (r.valid);
            expect (r.invertPolarity);
            expectWithinAbsoluteError (r.delayMs, 0.0f, 0.1f);
            expectLessThan (r.beforeMatch, 5.0f);   // fighting before
            expectGreaterThan (r.afterMatch, 95.0f); // aligned after the flip
        }

        beginTest ("Silent input is reported invalid");
        {
            std::vector<float> bass ((size_t) n, 0.0f), kick ((size_t) n, 0.0f);
            const auto r = AlignmentAnalyzer::analyze (bass.data(), kick.data(), n, kSampleRate, 50.0f);
            expect (! r.valid);
        }

        beginTest ("Rotator search improves a phase offset delay alone cannot fix");
        {
            // The plugin rotates the BASS, so the fixable case is a kick that is
            // itself phase-shifted relative to the bass: passing the bass
            // forward through the same allpass brings it toward the kick. (The
            // reverse -- rotating the bass to undo a shift already on the bass --
            // would need an unstable inverse allpass and is not fixable.)
            //
            // Build kick = allpass(shared event), bass = shared event. A pure
            // delay can't reconcile the frequency-dependent phase, but the
            // rotator search should find a setting that raises the match.
            // Genuine rotator case: broadband (noise) low content, where the
            // kick is a phase-rotated copy of the bass. Because noise has no
            // periodicity for a cross-correlation to exploit, NO single delay
            // can realign it -- delay+polarity plateaus. But rotating the bass
            // by the SAME allpass reproduces the kick's phase, so the search
            // should find matching rotator settings and fire.
            juce::Random rng (1234);
            std::vector<float> bass ((size_t) n, 0.0f), kick ((size_t) n, 0.0f);

            AllpassPhaseRotator rot;
            rot.prepare (kSampleRate, 4);
            rot.setParameters (80.0f, 2.0f);

            for (int i = 0; i < n; ++i)
            {
                const double t   = (double) i / kSampleRate;
                const double env = std::exp (-t * 3.0);
                const float  x   = (float) ((rng.nextDouble() * 2.0 - 1.0) * env);
                bass[(size_t) i] = x;
                kick[(size_t) i] = rot.processSample (x);
            }

            const auto r = AlignmentAnalyzer::analyze (bass.data(), kick.data(), n, kSampleRate, 50.0f);

            expect (r.valid);
            expect (r.adjustRotator);
            expect (r.rotatorStages >= 2 && r.rotatorStages <= 4);
            expectGreaterThan (r.afterMatch, r.beforeMatch);
        }
    }
};

static AlignmentAnalyzerTests alignmentAnalyzerTestsInstance;

//==============================================================================
class AnalyzerInstructionTests : public juce::UnitTest
{
public:
    AnalyzerInstructionTests() : juce::UnitTest ("AnalyzerInstruction", "DSP") {}

    void runTest() override
    {
        beginTest ("Negative analyzer delay becomes move-kick instruction");
        {
            AlignmentResult result;
            result.valid = true;
            result.delayMs = -1.25f;
            result.beforeMatch = 45.0f;
            result.afterMatch = 96.0f;

            const auto instruction = AnalyzerInstructionBuilder::build (result);

            expectEquals ((int) instruction.action, (int) AlignmentAction::RecommendMoveKick);
            expectWithinAbsoluteError (instruction.delayMs, -1.25f, 0.001f);
            expect (instruction.message.containsIgnoreCase ("move kick later"));
        }

        beginTest ("Positive analyzer delay can be applied as bass delay");
        {
            AlignmentResult result;
            result.valid = true;
            result.delayMs = 2.5f;
            result.beforeMatch = 50.0f;
            result.afterMatch = 98.0f;

            const auto instruction = AnalyzerInstructionBuilder::build (result);

            expectEquals ((int) instruction.action, (int) AlignmentAction::ApplyBassDelay);
            expectWithinAbsoluteError (instruction.delayMs, 2.5f, 0.001f);
            expect (instruction.message.containsIgnoreCase ("delay bass"));
        }

        beginTest ("Silence becomes waiting instruction");
        {
            AlignmentResult result;
            const auto instruction = AnalyzerInstructionBuilder::build (result);

            expectEquals ((int) instruction.action, (int) AlignmentAction::NotEnoughSignal);
            expectEquals (instruction.confidence, 0.0f);
            expect (instruction.message.containsIgnoreCase ("waiting"));
        }
    }
};

static AnalyzerInstructionTests analyzerInstructionTestsInstance;

//==============================================================================
class Stage2AnalysisTests : public juce::UnitTest
{
public:
    Stage2AnalysisTests() : juce::UnitTest ("Stage2Analysis", "DSP") {}

    void runTest() override
    {
        beginTest ("Transient detector gates repeated hits during holdoff");
        {
            TransientDetector detector;
            detector.prepare (1000.0);
            detector.setThreshold (0.01f);
            detector.setMinimumEnergyGate (0.005f);
            detector.setAttackReleaseMs (1.0f, 20.0f);
            detector.setHoldoffMs (100.0f);

            int detections = 0;
            for (int i = 0; i < 500; ++i)
            {
                const bool isHit = i == 50 || i == 80 || i == 220;
                if (detector.processSample (isHit ? 1.0f : 0.0f))
                    ++detections;
            }

            expectEquals (detections, 2);
        }

        beginTest ("Multi-band scorer reports separate band polarity");
        {
            constexpr int n = 4096;
            std::vector<float> bass ((size_t) n, 0.0f), kick ((size_t) n, 0.0f);

            for (int i = 0; i < n; ++i)
            {
                const double t = (double) i / kSampleRate;
                const float low = (float) std::sin (kTwoPi * 60.0 * t);
                const float high = (float) std::sin (kTwoPi * 150.0 * t);

                bass[(size_t) i] = low + 0.5f * high;
                kick[(size_t) i] = low - 0.5f * high;
            }

            const auto result = MultiBandCorrelation::analyze (bass.data(), kick.data(), n, kSampleRate);

            expectGreaterThan (result.bands[1].correlation, 0.75f); // 50-80 Hz
            expectLessThan (result.bands[3].correlation, -0.40f);   // 120-200 Hz
            expectGreaterThan (result.confidence, 0.1f);
        }

        beginTest ("Per-hit analyzer can produce different delay recommendations");
        {
            constexpr int n = 8192;
            constexpr int lag = 40;

            auto fillHit = [] (std::vector<float>& bass, std::vector<float>& kick,
                               int bassOffset, int kickOffset, double freq)
            {
                std::fill (bass.begin(), bass.end(), 0.0f);
                std::fill (kick.begin(), kick.end(), 0.0f);

                for (int i = 0; i < (int) bass.size(); ++i)
                {
                    const int bi = i - bassOffset;
                    const int ki = i - kickOffset;

                    if (bi >= 0)
                    {
                        const double t = (double) bi / kSampleRate;
                        bass[(size_t) i] = (float) (std::sin (kTwoPi * freq * t) * std::exp (-t * 5.0));
                    }

                    if (ki >= 0)
                    {
                        const double t = (double) ki / kSampleRate;
                        kick[(size_t) i] = (float) (std::sin (kTwoPi * freq * t) * std::exp (-t * 5.0));
                    }
                }
            };

            std::vector<float> bass ((size_t) n, 0.0f), kick ((size_t) n, 0.0f);

            fillHit (bass, kick, 1040, 1000, 60.0);
            const auto first = PerHitAnalyzer::analyzeHit (bass.data(), kick.data(), n, kSampleRate, 1);

            fillHit (bass, kick, 1000, 1040, 90.0);
            const auto second = PerHitAnalyzer::analyzeHit (bass.data(), kick.data(), n, kSampleRate, 2);

            expect (first.valid);
            expect (second.valid);
            expectLessThan (first.instruction.delayMs, -0.5f);
            expectGreaterThan (second.instruction.delayMs, 0.5f);
        }
    }
};

static Stage2AnalysisTests stage2AnalysisTestsInstance;

//==============================================================================
int main (int, char**)
{
    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);
    runner.runAllTests();

    int failures = 0;

    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        const auto* result = runner.getResult (i);
        if (result == nullptr)
            continue;

        std::cout << "[" << (result->failures > 0 ? "FAIL" : "ok  ") << "] "
                  << result->unitTestName.toStdString() << " / "
                  << result->subcategoryName.toStdString()
                  << "  (" << result->passes << " passed, "
                  << result->failures << " failed)" << std::endl;

        for (const auto& msg : result->messages)
            if (msg.isNotEmpty())
                std::cout << "        " << msg.toStdString() << std::endl;

        failures += result->failures;
    }

    std::cout << "TOTAL FAILURES: " << failures << std::endl;
    return failures > 0 ? 1 : 0;
}
