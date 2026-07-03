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
#include "PhaseFixEngine.h"
#include "PluginProcessor.h"
#include "util/HitCaptureBuffer.h"
#include "util/RawCaptureBuffer.h"
#include "ui/ScopeVisuals.h"
#include "ui/StatusHelpers.h"

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
class PhaseFixEngineTests : public juce::UnitTest
{
public:
    PhaseFixEngineTests() : juce::UnitTest ("PhaseFixEngine", "DSP") {}

    void runTest() override
    {
        constexpr int n = 8192;

        auto fillBurst = [] (std::vector<float>& bass, std::vector<float>& kick,
                             int bassOffset, int kickOffset, double freq,
                             bool invertBass = false)
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
                    const float v = (float) (std::sin (kTwoPi * freq * t) * std::exp (-t * 5.0));
                    bass[(size_t) i] = invertBass ? -v : v;
                }

                if (ki >= 0)
                {
                    const double t = (double) ki / kSampleRate;
                    kick[(size_t) i] = (float) (std::sin (kTwoPi * freq * t) * std::exp (-t * 5.0));
                }
            }
        };

        beginTest ("Partial improvement classification stays actionable");
        {
            PhaseFixResult r;
            r.valid = true;
            r.enoughSignal = true;
            r.beforeMatchPercent = 43.0f;
            r.afterMatchPercent = 63.0f;
            r.improvementPercent = 20.0f;
            r.confidence = 0.46f;
            PhaseFixEngine::updateDerivedResultFields (r);

            expectEquals ((int) r.quality, (int) PhaseFixQuality::PartialImprovement);
            expect (PhaseFixEngine::canApply (r));
            expect (r.optionalApplyAllowed);
            expect (r.message.containsIgnoreCase ("partial fix"));
        }

        beginTest ("Small improvement stays in no useful change");
        {
            PhaseFixResult r;
            r.valid = true;
            r.enoughSignal = true;
            r.beforeMatchPercent = 52.0f;
            r.afterMatchPercent = 55.0f;
            r.improvementPercent = 3.0f;
            r.confidence = 0.58f;
            PhaseFixEngine::updateDerivedResultFields (r);

            expectEquals ((int) r.quality, (int) PhaseFixQuality::NoUsefulChange);
            expect (! PhaseFixEngine::canApply (r));
            expect (r.message.containsIgnoreCase ("no useful bass-path fix"));
        }

        beginTest ("Inverted bass recommends polarity invert");
        {
            std::vector<float> bass ((size_t) n), kick ((size_t) n);
            fillBurst (bass, kick, 1000, 1000, 70.0, true);

            const auto r = PhaseFixEngine::analyze (bass.data(), kick.data(), n, kSampleRate, 10.0f);

            expect (r.valid);
            expect (r.enoughSignal);
            expect (r.bassPolarityInvert);
            expectGreaterThan (r.afterMatchPercent, r.beforeMatchPercent + 40.0f);
            expectEquals ((int) r.quality, (int) PhaseFixQuality::StrongImprovement);
            expect (PhaseFixEngine::canApply (r));
        }

        beginTest ("Bass too early recommends positive bass delay");
        {
            std::vector<float> bass ((size_t) n), kick ((size_t) n);
            fillBurst (bass, kick, 1000, 1040, 65.0);

            const auto r = PhaseFixEngine::analyze (bass.data(), kick.data(), n, kSampleRate, 10.0f);

            expect (r.valid);
            expectGreaterThan (r.bassDelayMs, 0.5f);
            expectLessThan (r.bassDelayMs, 1.2f);
            expectGreaterThan (r.afterMatchPercent, 95.0f);
            expectEquals ((int) r.quality, (int) PhaseFixQuality::StrongImprovement);
            expect (! r.requiresTimelineMove);
        }

        beginTest ("Large bass delay is not auto-applied");
        {
            constexpr int largeLag = 1920; // 40 ms at 48 kHz
            std::vector<float> bass ((size_t) n), kick ((size_t) n);
            fillBurst (bass, kick, 1000, 1000 + largeLag, 65.0);

            const auto r = PhaseFixEngine::analyze (bass.data(), kick.data(), n, kSampleRate, 5.0f);

            expect (r.valid);
            expectEquals ((int) r.quality, (int) PhaseFixQuality::LargeTimingOffset);
            expect (! PhaseFixEngine::canApply (r));
            expectLessThan (r.bassDelayMs, 5.1f);
            expectGreaterThan (r.detectedTimingOffsetMs, 30.0f);
        }

        beginTest ("Delay penalty prefers the smaller delay when the match is similar");
        {
            std::vector<float> bass ((size_t) n, 0.0f), kick ((size_t) n, 0.0f);

            for (int i = 0; i < n; ++i)
            {
                const double t = (double) i / kSampleRate;
                bass[(size_t) i] = (float) std::sin (kTwoPi * 50.0 * t);

                const int shifted = i - 48; // ~1 ms
                if (shifted >= 0)
                    kick[(size_t) i] = (float) std::sin (kTwoPi * 50.0 * (double) shifted / kSampleRate);
            }

            const auto r = PhaseFixEngine::analyze (bass.data(), kick.data(), n, kSampleRate, 25.0f);

            expect (r.valid);
            expectLessThan (r.bassDelayMs, 5.0f);
        }

        beginTest ("Already aligned does not recommend unnecessary apply");
        {
            std::vector<float> bass ((size_t) n), kick ((size_t) n);
            fillBurst (bass, kick, 1000, 1000, 80.0);

            const auto r = PhaseFixEngine::analyze (bass.data(), kick.data(), n, kSampleRate, 10.0f);

            expect (r.valid);
            expectGreaterThan (r.beforeMatchPercent, 85.0f);
            expectLessThan (r.improvementPercent, 5.0f);
            expectEquals ((int) r.quality, (int) PhaseFixQuality::AlreadyGood);
            expect (r.message.containsIgnoreCase ("already close"));
            expect (! PhaseFixEngine::canApply (r));
        }

        beginTest ("Phase offset recommends phase filter when useful");
        {
            std::vector<float> bass ((size_t) n, 0.0f), kick ((size_t) n, 0.0f);

            AllpassPhaseRotator rot;
            rot.prepare (kSampleRate, 4);
            rot.setParameters (80.0f, 2.0f);

            for (int i = 0; i < n; ++i)
            {
                const double t = (double) i / kSampleRate;
                const double fadeIn = std::min (1.0, t * 200.0);
                const double env = fadeIn * std::exp (-t * 0.4);
                const float x = (float) (env * (std::sin (kTwoPi * 50.0 * t)
                                      + 0.8 * std::sin (kTwoPi * 90.0 * t + 0.3)
                                      + 0.6 * std::sin (kTwoPi * 150.0 * t + 1.1)));
                bass[(size_t) i] = x;
                kick[(size_t) i] = rot.processSample (x);
            }

            const auto r = PhaseFixEngine::analyze (bass.data(), kick.data(), n, kSampleRate, 10.0f);

            const auto phaseDebug = "phase result: before=" + juce::String (r.beforeMatchPercent, 2)
                                  + " after=" + juce::String (r.afterMatchPercent, 2)
                                  + " improve=" + juce::String (r.improvementPercent, 2)
                                  + " delay=" + juce::String (r.bassDelayMs, 2)
                                  + " phase=" + juce::String (r.phaseFilterEnabled ? "on" : "off")
                                  + " msg=" + r.message;
            expect (r.valid);
            expect (r.phaseFilterEnabled, phaseDebug);
            expect (r.afterMatchPercent > r.beforeMatchPercent + 5.0f, phaseDebug);
        }

        beginTest ("Optional phase refinement is kept when overall match is already good");
        {
            std::vector<float> bass ((size_t) n, 0.0f), kick ((size_t) n, 0.0f);

            AllpassPhaseRotator rot;
            rot.prepare (kSampleRate, 3);
            rot.setParameters (100.0f, 2.0f);

            for (int i = 0; i < n; ++i)
            {
                const double t = (double) i / kSampleRate;
                const double env = std::exp (-t * 0.35);
                const float alignedLow = (float) (0.95 * env * std::sin (kTwoPi * 45.0 * t));
                const float alignedHigh = (float) (0.35 * env * std::sin (kTwoPi * 160.0 * t + 0.2));
                const float conflict = (float) (0.22 * env * std::sin (kTwoPi * 100.0 * t));

                bass[(size_t) i] = alignedLow + alignedHigh + conflict;
                kick[(size_t) i] = alignedLow + alignedHigh + rot.processSample (conflict);
            }

            const auto r = PhaseFixEngine::analyze (bass.data(), kick.data(), n, kSampleRate, 10.0f);

            expect (r.valid);
            expectGreaterThan (r.beforeMatchPercent, 85.0f);
            expectEquals ((int) r.quality, (int) PhaseFixQuality::AlreadyGood);
            expect (r.phaseFilterEnabled);
            expect (r.optionalApplyAllowed);
            expect (! PhaseFixEngine::canApply (r));
            expect (r.message.containsIgnoreCase ("optional phase filter"));
        }

        beginTest ("Bass late suggests timeline movement instead of negative delay");
        {
            std::vector<float> bass ((size_t) n), kick ((size_t) n);
            fillBurst (bass, kick, 1040, 1000, 70.0);

            const auto r = PhaseFixEngine::analyze (bass.data(), kick.data(), n, kSampleRate, 10.0f);

            expect (r.valid);
            expect (r.bassDelayMs >= 0.0f);
            expect (r.requiresTimelineMove);
            expectEquals ((int) r.quality, (int) PhaseFixQuality::TimelineMoveRequired);
            expectGreaterThan (r.suggestedKickMoveMs, 0.5f);
            expect (! PhaseFixEngine::canApply (r));
        }

        beginTest ("Low signal returns enoughSignal=false");
        {
            std::vector<float> bass ((size_t) n, 0.0f), kick ((size_t) n, 0.0f);

            const auto r = PhaseFixEngine::analyze (bass.data(), kick.data(), n, kSampleRate, 10.0f);

            expect (! r.enoughSignal);
            expectEquals ((int) r.quality, (int) PhaseFixQuality::NotEnoughSignal);
            expect (r.message.containsIgnoreCase ("waiting"));
            expect (! PhaseFixEngine::canApply (r));
        }

        beginTest ("High-frequency click does not dominate low-end fix");
        {
            std::vector<float> bass ((size_t) n, 0.0f), kick ((size_t) n, 0.0f);
            const int lag = 40;

            for (int i = 0; i < n; ++i)
            {
                const double t = (double) i / kSampleRate;
                const double click = 4.0 * std::sin (kTwoPi * 5000.0 * t) * std::exp (-t * 200.0);
                const double kickLow = std::sin (kTwoPi * 60.0 * t) * std::exp (-t * 5.0);
                const int bi = i - lag;
                const double bassLow = bi >= 0
                    ? std::sin (kTwoPi * 60.0 * (double) bi / kSampleRate) * std::exp (-(double) bi / kSampleRate * 5.0)
                    : 0.0;

                kick[(size_t) i] = (float) (click + kickLow);
                bass[(size_t) i] = (float) (click + bassLow);
            }

            const auto r = PhaseFixEngine::analyze (bass.data(), kick.data(), n, kSampleRate, 10.0f);

            expect (r.valid);
            expect (r.requiresTimelineMove);
            expectGreaterThan (r.suggestedKickMoveMs, 0.5f);
        }
    }
};

static PhaseFixEngineTests phaseFixEngineTestsInstance;

//==============================================================================
class ScopeVisualTests : public juce::UnitTest
{
public:
    ScopeVisualTests() : juce::UnitTest ("ScopeVisuals", "UI Helpers") {}

    void runTest() override
    {
        beginTest ("Phase relation classifies constructive, destructive, and silent buckets");
        {
            expectEquals ((int) classifyPhaseRelation (0.4f, 0.2f), (int) PhaseRelation::Constructive);
            expectEquals ((int) classifyPhaseRelation (-0.5f, 0.1f), (int) PhaseRelation::Destructive);
            expectEquals ((int) classifyPhaseRelation (1.0e-7f, 0.2f), (int) PhaseRelation::Silent);
        }

        beginTest ("Peak delta is positive when bass transient is later than kick");
        {
            std::vector<float> bass (128, 0.0f), kick (128, 0.0f);
            kick[20] = 1.0f;
            bass[60] = 1.0f;

            const float deltaMs = calculatePeakDeltaMs (bass.data(), kick.data(), (int) bass.size(), kSampleRate);
            expectWithinAbsoluteError (deltaMs, (float) (40.0 / kSampleRate * 1000.0), 0.001f);
        }

        beginTest ("Peak delta is negative when bass transient is earlier than kick");
        {
            std::vector<float> bass (128, 0.0f), kick (128, 0.0f);
            bass[18] = 1.0f;
            kick[58] = 1.0f;

            const float deltaMs = calculatePeakDeltaMs (bass.data(), kick.data(), (int) bass.size(), kSampleRate);
            expectWithinAbsoluteError (deltaMs, (float) (-40.0 / kSampleRate * 1000.0), 0.001f);
        }

        beginTest ("Sample and millisecond conversion stay consistent");
        {
            expectWithinAbsoluteError (samplesToMs (480, kSampleRate), 10.0f, 1.0e-6f);
            expectEquals (msToSamples (10.0f, kSampleRate), 480);
        }

        beginTest ("BPM grid conversion maps note values to milliseconds");
        {
            expectWithinAbsoluteError ((float) bpmToQuarterMs (120.0), 500.0f, 1.0e-6f);
            expectWithinAbsoluteError ((float) gridDivisionToMs (120.0, GridDivision::Quarter), 500.0f, 1.0e-6f);
            expectWithinAbsoluteError ((float) gridDivisionToMs (120.0, GridDivision::Eighth), 250.0f, 1.0e-6f);
            expectWithinAbsoluteError ((float) gridDivisionToMs (120.0, GridDivision::Sixteenth), 125.0f, 1.0e-6f);
            expectWithinAbsoluteError ((float) gridDivisionToMs (120.0, GridDivision::Bar), 2000.0f, 1.0e-6f);
        }

        beginTest ("Visible scope samples follow sample rate, decimation, zoom, and grid");
        {
            expectEquals (calculateVisibleScopeSamples (8192, kSampleRate, 24, 1.0f,
                                                        GridDivision::Sixteenth, true, 120.0),
                          250);
            expectEquals (calculateVisibleScopeSamples (8192, kSampleRate, 24, 2.0f,
                                                        GridDivision::Sixteenth, true, 120.0),
                          125);
            expectEquals (calculateVisibleScopeSamples (8192, kSampleRate, 24, 2.0f,
                                                        GridDivision::Milliseconds, false, 0.0),
                          4096);
        }

        beginTest ("Visual offset shifts the display index by decimated samples");
        {
            const int unshifted = resolveDisplayHistoryIndex (100, 2048, 1800, 12, 0, 4);
            const int shifted = resolveDisplayHistoryIndex (100, 2048, 1800, 12, 24, 4);
            const int expected = wrapHistoryIndex (unshifted + 6, 2048);

            expectEquals (visualOffsetToDisplaySamples (24, 4), 6);
            expectEquals (shifted, expected);
        }

        beginTest ("Visual offset shifts bass relative to kick only");
        {
            const auto unshifted = resolveRelativeDisplayHistoryIndices (100, 2048, 1800, 12, 0, 4);
            const auto shifted = resolveRelativeDisplayHistoryIndices (100, 2048, 1800, 12, 24, 4);

            expectEquals (shifted.kickIndex, unshifted.kickIndex);
            expectEquals (shifted.bassIndex, wrapHistoryIndex (unshifted.bassIndex + 6, 2048));
            expectEquals (shifted.bassIndex - shifted.kickIndex, 6);
        }

        beginTest ("Visual offset wraps bass index without moving kick index");
        {
            const auto unshifted = resolveRelativeDisplayHistoryIndices (2038, 2048, 0, 8, 0, 2);
            const auto shifted = resolveRelativeDisplayHistoryIndices (2038, 2048, 0, 8, 12, 2);

            expectEquals (shifted.kickIndex, unshifted.kickIndex);
            expectEquals (shifted.bassIndex, wrapHistoryIndex (unshifted.bassIndex + 6, 2048));
        }
    }
};

static ScopeVisualTests scopeVisualTestsInstance;

//==============================================================================
class CaptureBufferTests : public juce::UnitTest
{
public:
    CaptureBufferTests() : juce::UnitTest ("CaptureBuffers", "DSP") {}

    void runTest() override
    {
        beginTest ("Raw snapshot returns only filled samples before ring is full");
        {
            RawCaptureBuffer buffer;
            buffer.prepare (8);

            for (int i = 0; i < 3; ++i)
                buffer.push ((float) (10 + i), (float) (20 + i));

            buffer.publishSnapshot();

            std::vector<float> bass, kick;
            const int count = buffer.snapshot (bass, kick);

            expectEquals (count, 3);
            expectEquals ((int) bass.size(), 3);
            expectEquals ((int) kick.size(), 3);
            expectWithinAbsoluteError (bass[0], 10.0f, 1.0e-7f);
            expectWithinAbsoluteError (bass[2], 12.0f, 1.0e-7f);
            expectWithinAbsoluteError (kick[0], 20.0f, 1.0e-7f);
            expectWithinAbsoluteError (kick[2], 22.0f, 1.0e-7f);
        }

        beginTest ("Raw snapshot stays chronological after ring wraps");
        {
            RawCaptureBuffer buffer;
            buffer.prepare (4);

            for (int i = 0; i < 6; ++i)
                buffer.push ((float) i, (float) (100 + i));

            buffer.publishSnapshot();

            std::vector<float> bass, kick;
            const int count = buffer.snapshot (bass, kick);

            expectEquals (count, 4);
            expectWithinAbsoluteError (bass[0], 2.0f, 1.0e-7f);
            expectWithinAbsoluteError (bass[3], 5.0f, 1.0e-7f);
            expectWithinAbsoluteError (kick[0], 102.0f, 1.0e-7f);
            expectWithinAbsoluteError (kick[3], 105.0f, 1.0e-7f);
        }

        beginTest ("Hit snapshot returns the latest completed published window");
        {
            HitCaptureBuffer buffer;
            buffer.prepare (1000.0, 2.0f, 3.0f);

            buffer.pushSample (1.0f, 11.0f, false);
            buffer.pushSample (2.0f, 12.0f, false);
            buffer.pushSample (3.0f, 13.0f, true);
            buffer.pushSample (4.0f, 14.0f, false);
            buffer.pushSample (5.0f, 15.0f, false);

            std::vector<float> bass, kick;
            const int count = buffer.snapshotLatest (bass, kick);

            expectEquals (count, 5);
            expectEquals (buffer.getSequence(), 1);
            expectWithinAbsoluteError (bass[0], 1.0f, 1.0e-7f);
            expectWithinAbsoluteError (bass[4], 5.0f, 1.0e-7f);
            expectWithinAbsoluteError (kick[0], 11.0f, 1.0e-7f);
            expectWithinAbsoluteError (kick[4], 15.0f, 1.0e-7f);
        }
    }
};

static CaptureBufferTests captureBufferTestsInstance;

//==============================================================================
class StatusHelperTests : public juce::UnitTest
{
public:
    StatusHelperTests() : juce::UnitTest ("StatusHelpers", "UI Helpers") {}

    void runTest() override
    {
        beginTest ("Sidechain status distinguishes missing, quiet, and active states");
        {
            expectEquals ((int) classifySidechainSignalStatus (false, 0.5f, 0.5f),
                          (int) SidechainSignalStatus::WaitingForSidechain);
            expectEquals ((int) classifySidechainSignalStatus (true, 1.0e-6f, 0.5f),
                          (int) SidechainSignalStatus::SignalTooLow);
            expectEquals ((int) classifySidechainSignalStatus (true, 0.2f, 0.15f),
                          (int) SidechainSignalStatus::SidechainActive);
        }

        beginTest ("Workflow status distinguishes monitoring, fix ready, and processing");
        {
            expectEquals ((int) classifyProcessingWorkflowStatus (true, false),
                          (int) ProcessingWorkflowStatus::MonitoringDryInput);
            expectEquals ((int) classifyProcessingWorkflowStatus (true, true),
                          (int) ProcessingWorkflowStatus::FixReady);
            expectEquals ((int) classifyProcessingWorkflowStatus (false, true),
                          (int) ProcessingWorkflowStatus::ProcessingBass);
        }
    }
};

static StatusHelperTests statusHelperTestsInstance;

//==============================================================================
class KickLockProcessorTransparencyTests : public juce::UnitTest
{
public:
    KickLockProcessorTransparencyTests() : juce::UnitTest ("KickLockProcessorTransparency", "Processor") {}

    void runTest() override
    {
        beginTest ("Default audio pass-through without sidechain signal");
        {
            KickLockAudioProcessor processor;
            processor.setRateAndBufferSizeDetails (kSampleRate, 512);
            processor.prepareToPlay (kSampleRate, 512);
            expect (processor.isBassProcessingNeutral());

            juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                         processor.getTotalNumOutputChannels()),
                                             512);
            juce::MidiBuffer midi;
            fillBass (buffer, 512);

            const auto expected = copyMainChannels (buffer, 2, 512);
            processor.processBlock (buffer, midi);
            expectMainMatches (buffer, expected, 2, 512, 1.0e-7f);
        }

        beginTest ("Default audio pass-through with sidechain");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 4096);
            processor.prepareToPlay (kSampleRate, 4096);
            expect (processor.isBassProcessingNeutral());

            juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                         processor.getTotalNumOutputChannels()),
                                             4096);
            juce::MidiBuffer midi;
            fillBass (buffer, 4096);
            fillKickSidechain (buffer, 4096, 0);

            const auto expected = copyMainChannels (buffer, 2, 4096);
            processor.processBlock (buffer, midi);
            expectMainMatches (buffer, expected, 2, 4096, 1.0e-7f);
        }

        beginTest ("Analyze recommend-only does not change parameters");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 8192);
            processor.prepareToPlay (kSampleRate, 8192);

            feedAnalyzableInvertedSignal (processor, 8192);

            const float delayBefore = rawParam (processor, "delayMs");
            const float polarityBefore = rawParam (processor, "polarityInvert");
            const float phaseBefore = rawParam (processor, "phaseFilterEnabled");
            const float freqBefore = rawParam (processor, "rotatorFreq");
            const float qBefore = rawParam (processor, "rotatorQ");
            const float stagesBefore = rawParam (processor, "rotatorStages");

            (void) processor.analyzeFix();
            (void) processor.analyzeAndApply (AnalyzeMode::RecommendOnly);

            expectWithinAbsoluteError (rawParam (processor, "delayMs"), delayBefore, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "polarityInvert"), polarityBefore, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "phaseFilterEnabled"), phaseBefore, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "rotatorFreq"), freqBefore, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "rotatorQ"), qBefore, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "rotatorStages"), stagesBefore, 1.0e-7f);
            expect (processor.isBassProcessingNeutral());
        }

        beginTest ("Analyze preserves phase state when phase filter is already on");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 8192);
            processor.prepareToPlay (kSampleRate, 8192);

            setBoolParam (processor, "phaseFilterEnabled", true);
            feedAnalyzableInvertedSignal (processor, 8192);

            (void) processor.analyzeFix();
            (void) processor.analyzeAndApply (AnalyzeMode::RecommendOnly);

            expectWithinAbsoluteError (rawParam (processor, "phaseFilterEnabled"), 1.0f, 1.0e-7f);
        }

        beginTest ("Phase filter off stays transparent when rotator settings change");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 4096);
            processor.prepareToPlay (kSampleRate, 4096);

            setBoolParam (processor, "phaseFilterEnabled", false);
            setFloatParam (processor, "rotatorFreq", 90.0f);
            setFloatParam (processor, "rotatorQ", 4.0f);
            setFloatParam (processor, "rotatorStages", 0.0f);

            juce::AudioBuffer<float> firstBuffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                              processor.getTotalNumOutputChannels()),
                                                  4096);
            juce::MidiBuffer midi;
            fillBass (firstBuffer, 4096);
            const auto firstExpected = copyMainChannels (firstBuffer, 2, 4096);
            processor.processBlock (firstBuffer, midi);
            expectMainMatches (firstBuffer, firstExpected, 2, 4096, 1.0e-7f);

            setFloatParam (processor, "rotatorFreq", 1400.0f);
            setFloatParam (processor, "rotatorQ", 0.5f);
            setFloatParam (processor, "rotatorStages", 2.0f);

            juce::AudioBuffer<float> secondBuffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                               processor.getTotalNumOutputChannels()),
                                                   4096);
            fillBass (secondBuffer, 4096);
            const auto secondExpected = copyMainChannels (secondBuffer, 2, 4096);
            processor.processBlock (secondBuffer, midi);
            expectMainMatches (secondBuffer, secondExpected, 2, 4096, 1.0e-7f);
        }

        beginTest ("Apply Fix changes only safe bass-path parameters");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 8192);
            processor.prepareToPlay (kSampleRate, 8192);
            feedAnalyzableInvertedSignal (processor, 8192);

            const auto fix = processor.analyzeFix();
            expect (PhaseFixEngine::canApply (fix), fix.message);

            const bool applied = processor.applyLatestFix();
            expect (applied);
            expectGreaterOrEqual (rawParam (processor, "delayMs"), 0.0f);
            expectWithinAbsoluteError (rawParam (processor, "polarityInvert"),
                                       fix.bassPolarityInvert ? 1.0f : 0.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "phaseFilterEnabled"),
                                       fix.phaseFilterEnabled ? 1.0f : 0.0f, 1.0e-7f);

            if (fix.phaseFilterEnabled)
            {
                expectWithinAbsoluteError (rawParam (processor, "rotatorFreq"), fix.phaseFilterFreqHz, 0.01f);
                expectWithinAbsoluteError (rawParam (processor, "rotatorQ"), fix.phaseFilterQ, 0.01f);
                expectWithinAbsoluteError (rawParam (processor, "rotatorStages"),
                                           (float) (fix.phaseFilterStages - 2), 1.0e-7f);
            }
        }

        beginTest ("Apply Fix disables stale phase filter when the new fix does not use it");
        {
            KickLockAudioProcessor processor;
            processor.setRateAndBufferSizeDetails (kSampleRate, 512);
            processor.prepareToPlay (kSampleRate, 512);

            setBoolParam (processor, "phaseFilterEnabled", true);
            setFloatParam (processor, "rotatorFreq", 90.0f);

            PhaseFixResult fix;
            fix.valid = true;
            fix.enoughSignal = true;
            fix.bassPolarityInvert = true;
            fix.phaseFilterEnabled = false;
            fix.beforeMatchPercent = 40.0f;
            fix.afterMatchPercent = 92.0f;
            fix.improvementPercent = 52.0f;
            fix.confidence = 0.85f;
            PhaseFixEngine::updateDerivedResultFields (fix);
            processor.setLatestFixResultForTesting (fix);

            expect (processor.applyLatestFix());
            expectWithinAbsoluteError (rawParam (processor, "phaseFilterEnabled"), 0.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "polarityInvert"), 1.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "rotatorFreq"), 90.0f, 0.01f);
        }

        beginTest ("Apply Fix can write an optional phase refinement");
        {
            KickLockAudioProcessor processor;
            processor.setRateAndBufferSizeDetails (kSampleRate, 512);
            processor.prepareToPlay (kSampleRate, 512);

            PhaseFixResult fix;
            fix.valid = true;
            fix.enoughSignal = true;
            fix.phaseFilterEnabled = true;
            fix.phaseFilterFreqHz = 100.0f;
            fix.phaseFilterQ = 2.0f;
            fix.phaseFilterStages = 3;
            fix.beforeMatchPercent = 88.0f;
            fix.afterMatchPercent = 90.0f;
            fix.improvementPercent = 2.0f;
            fix.confidence = 0.72f;
            PhaseFixEngine::updateDerivedResultFields (fix);
            processor.setLatestFixResultForTesting (fix);

            expectEquals ((int) fix.quality, (int) PhaseFixQuality::AlreadyGood);
            expect (! PhaseFixEngine::canApply (fix));
            expect (fix.optionalApplyAllowed);
            expect (processor.applyLatestFix());
            expectWithinAbsoluteError (rawParam (processor, "phaseFilterEnabled"), 1.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "rotatorFreq"), 100.0f, 0.01f);
        }

        beginTest ("Apply Fix enables phase filter and writes rotator settings when recommended");
        {
            KickLockAudioProcessor processor;
            processor.setRateAndBufferSizeDetails (kSampleRate, 512);
            processor.prepareToPlay (kSampleRate, 512);

            PhaseFixResult fix;
            fix.valid = true;
            fix.enoughSignal = true;
            fix.phaseFilterEnabled = true;
            fix.phaseFilterFreqHz = 90.0f;
            fix.phaseFilterQ = 2.0f;
            fix.phaseFilterStages = 3;
            fix.beforeMatchPercent = 45.0f;
            fix.afterMatchPercent = 88.0f;
            fix.improvementPercent = 43.0f;
            fix.confidence = 0.82f;
            PhaseFixEngine::updateDerivedResultFields (fix);
            processor.setLatestFixResultForTesting (fix);

            expect (processor.applyLatestFix());
            expectWithinAbsoluteError (rawParam (processor, "phaseFilterEnabled"), 1.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "rotatorFreq"), 90.0f, 0.01f);
            expectWithinAbsoluteError (rawParam (processor, "rotatorQ"), 2.0f, 0.01f);
                expectWithinAbsoluteError (rawParam (processor, "rotatorStages"), 1.0f, 1.0e-7f);
        }

        beginTest ("Predicted after score stays close to verified after score");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 2048);
            processor.prepareToPlay (kSampleRate, 2048);

            feedHitSeries (processor, { 0, 0, 0, 0 }, { 40, 40, 40, 40 }, { false, false, false, false }, 2048);

            const auto predicted = processor.analyzeFix();
            expect (PhaseFixEngine::canApply (predicted), predicted.message);
            expect (processor.applyLatestFix());

            const auto verified = processor.getLatestFixResult();
            expectGreaterThan (verified.verifiedAfterMatchPercent, 0.0f);
            expectLessThan (verified.verificationDeltaPercent, 10.0f);
        }

        beginTest ("Multi-hit stable analysis aggregates consistent hits");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 2048);
            processor.prepareToPlay (kSampleRate, 2048);

            feedHitSeries (processor, { 0, 0, 0, 0 }, { 40, 40, 40, 40 }, { false, false, false, false }, 2048);

            const auto fix = processor.analyzeFix();
            expectGreaterThan (fix.contributingHits, 1);
            expect (! fix.unstableRecommendation);
            expect (PhaseFixEngine::canApply (fix), fix.message);
            expectGreaterThan (fix.bassDelayMs, 0.5f);
            expectLessThan (fix.bassDelayMs, 1.2f);
        }

        beginTest ("Multi-hit unstable analysis rejects conflicting hits");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 2048);
            processor.prepareToPlay (kSampleRate, 2048);

            feedHitSeries (processor, { 0, 40, 0, 40 }, { 40, 0, 40, 0 }, { false, true, false, true }, 2048);

            const auto fix = processor.analyzeFix();
            expectGreaterThan (fix.contributingHits, 1);
            expectEquals ((int) fix.quality, (int) PhaseFixQuality::Unstable);
            expect (! PhaseFixEngine::canApply (fix));
        }

        beginTest ("Negative bass delay is clamped while visual offset may stay negative");
        {
            KickLockAudioProcessor processor;
            processor.setRateAndBufferSizeDetails (kSampleRate, 512);
            processor.prepareToPlay (kSampleRate, 512);

            expectWithinAbsoluteError (processor.apvts.getParameter ("delayMs")->convertFrom0to1 (0.0f),
                                       0.0f, 1.0e-7f);
            expectEquals (formatBassDelayMs (-0.0001f), juce::String ("0.00 ms"));
            expectEquals (formatBassDelayMs (-6.0f), juce::String ("0.00 ms"));

            setFloatParam (processor, "visualOffsetSamples", -128.0f);
            expectWithinAbsoluteError (rawParam (processor, "visualOffsetSamples"), -128.0f, 1.0e-7f);

            PhaseFixResult fix;
            fix.valid = true;
            fix.enoughSignal = true;
            fix.bassPolarityInvert = true;
            fix.bassDelayMs = -3.25f;
            fix.beforeMatchPercent = 40.0f;
            fix.afterMatchPercent = 90.0f;
            fix.improvementPercent = 50.0f;
            fix.confidence = 0.9f;
            PhaseFixEngine::updateDerivedResultFields (fix);
            processor.setLatestFixResultForTesting (fix);

            expect (processor.applyLatestFix());
            expectWithinAbsoluteError (rawParam (processor, "delayMs"), 0.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParam (processor, "visualOffsetSamples"), -128.0f, 1.0e-7f);
        }

        beginTest ("Visual offset does not change analyzer output");
        {
            KickLockAudioProcessor baseline;
            baseline.enableAllBuses();
            baseline.setRateAndBufferSizeDetails (kSampleRate, 8192);
            baseline.prepareToPlay (kSampleRate, 8192);
            feedAnalyzableInvertedSignal (baseline, 8192);
            const auto baselineFix = baseline.analyzeFix();

            KickLockAudioProcessor shifted;
            shifted.enableAllBuses();
            shifted.setRateAndBufferSizeDetails (kSampleRate, 8192);
            shifted.prepareToPlay (kSampleRate, 8192);
            setFloatParam (shifted, "visualOffsetSamples", -256.0f);
            feedAnalyzableInvertedSignal (shifted, 8192);
            const auto shiftedFix = shifted.analyzeFix();

            expectWithinAbsoluteError (shiftedFix.bassDelayMs, baselineFix.bassDelayMs, 1.0e-4f);
            expectEquals (shiftedFix.phaseFilterEnabled, baselineFix.phaseFilterEnabled);
            expectEquals (shiftedFix.requiresTimelineMove, baselineFix.requiresTimelineMove);
        }

        beginTest ("Dry and processed meters split pre and post correction");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 20000);
            processor.prepareToPlay (kSampleRate, 20000);

            feedAlignedSignal (processor, 20000);
            const float neutralDry = processor.dryInputMatchPercent.load();
            const float neutralProcessed = processor.processedMatchPercent.load();
            expectWithinAbsoluteError (neutralDry, neutralProcessed, 0.5f);
            expectWithinAbsoluteError (processor.correlationPercent.load(), neutralDry, 0.5f);

            if (auto* polarity = processor.apvts.getParameter ("polarityInvert"))
                polarity->setValueNotifyingHost (1.0f);

            feedAlignedSignal (processor, 20000);
            const float correctedDry = processor.dryInputMatchPercent.load();
            const float correctedProcessed = processor.processedMatchPercent.load();

            expectGreaterThan (correctedDry, 80.0f);
            expectLessThan (correctedProcessed, correctedDry - 20.0f);
            expectWithinAbsoluteError (processor.correlationPercent.load(), correctedProcessed, 0.5f);
        }
    }

private:
    static void setBoolParam (KickLockAudioProcessor& processor, const char* id, bool value)
    {
        if (auto* param = processor.apvts.getParameter (id))
            param->setValueNotifyingHost (value ? 1.0f : 0.0f);
    }

    static void setFloatParam (KickLockAudioProcessor& processor, const char* id, float value)
    {
        if (auto* param = processor.apvts.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 (value));
    }

    static float rawParam (KickLockAudioProcessor& processor, const char* id)
    {
        if (const auto* value = processor.apvts.getRawParameterValue (id))
            return value->load();

        return 0.0f;
    }

    static void fillBass (juce::AudioBuffer<float>& buffer, int numSamples)
    {
        buffer.clear();
        for (int i = 0; i < numSamples; ++i)
        {
            const double t = (double) i / kSampleRate;
            buffer.setSample (0, i, (float) (0.4 * std::sin (kTwoPi * 70.0 * t)));
            if (buffer.getNumChannels() > 1)
                buffer.setSample (1, i, (float) (0.25 * std::sin (kTwoPi * 90.0 * t + 0.2)));
        }
    }

    static void fillKickSidechain (juce::AudioBuffer<float>& buffer, int numSamples, int offset)
    {
        if (buffer.getNumChannels() < 4)
            return;

        for (int i = 0; i < numSamples; ++i)
        {
            const int k = i - offset;
            const float v = k >= 0
                ? (float) (0.7 * std::sin (kTwoPi * 70.0 * (double) k / kSampleRate))
                : 0.0f;
            buffer.setSample (2, i, v);
            buffer.setSample (3, i, v);
        }
    }

    static std::array<std::vector<float>, 2> copyMainChannels (const juce::AudioBuffer<float>& buffer,
                                                              int channels,
                                                              int numSamples)
    {
        std::array<std::vector<float>, 2> copy;
        for (int ch = 0; ch < channels; ++ch)
        {
            copy[(size_t) ch].resize ((size_t) numSamples);
            for (int i = 0; i < numSamples; ++i)
                copy[(size_t) ch][(size_t) i] = buffer.getSample (ch, i);
        }

        return copy;
    }

    void expectMainMatches (const juce::AudioBuffer<float>& buffer,
                            const std::array<std::vector<float>, 2>& expected,
                            int channels,
                            int numSamples,
                            float tolerance)
    {
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < numSamples; ++i)
                expectWithinAbsoluteError (buffer.getSample (ch, i), expected[(size_t) ch][(size_t) i], tolerance);
    }

    static void feedAlignedSignal (KickLockAudioProcessor& processor, int numSamples)
    {
        juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                     processor.getTotalNumOutputChannels()),
                                         numSamples);
        buffer.clear();

        for (int i = 0; i < numSamples; ++i)
        {
            const double t = (double) i / kSampleRate;
            const float v = (float) (0.6 * std::sin (kTwoPi * 70.0 * t));
            buffer.setSample (0, i, v);
            buffer.setSample (1, i, v);
            if (buffer.getNumChannels() > 2)
                buffer.setSample (2, i, v);
            if (buffer.getNumChannels() > 3)
                buffer.setSample (3, i, v);
        }

        juce::MidiBuffer midi;
        processor.processBlock (buffer, midi);
    }

    static void feedAnalyzableInvertedSignal (KickLockAudioProcessor& processor, int blockSize)
    {
        constexpr int totalSamples = 96000;
        constexpr int offset = 1000;

        for (int start = 0; start < totalSamples; start += blockSize)
        {
            const int numSamples = std::min (blockSize, totalSamples - start);
            juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                         processor.getTotalNumOutputChannels()),
                                             numSamples);
            buffer.clear();

            for (int i = 0; i < numSamples; ++i)
            {
                const int sample = start + i;
                const int eventSample = sample - offset;

                const float kick = eventSample >= 0
                    ? (float) (std::sin (kTwoPi * 70.0 * (double) eventSample / kSampleRate) * 0.8)
                    : 0.0f;
                const float bass = -kick;

                buffer.setSample (0, i, bass);
                buffer.setSample (1, i, bass);
                if (buffer.getNumChannels() > 2)
                    buffer.setSample (2, i, kick);
                if (buffer.getNumChannels() > 3)
                    buffer.setSample (3, i, kick);
            }

            juce::MidiBuffer midi;
            processor.processBlock (buffer, midi);
        }
    }

    static void feedHitSeries (KickLockAudioProcessor& processor,
                               const std::vector<int>& bassDelays,
                               const std::vector<int>& kickDelays,
                               const std::vector<bool>& invertBass,
                               int blockSize)
    {
        const int hitCount = std::min ({ (int) bassDelays.size(),
                                         (int) kickDelays.size(),
                                         (int) invertBass.size() });
        const int hitSpacing = 12000;
        const int eventLength = 5000;
        const int startOffset = 1500;
        const int totalSamples = startOffset + hitCount * hitSpacing + eventLength;

        for (int start = 0; start < totalSamples; start += blockSize)
        {
            const int numSamples = std::min (blockSize, totalSamples - start);
            juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                         processor.getTotalNumOutputChannels()),
                                             numSamples);
            buffer.clear();

            for (int i = 0; i < numSamples; ++i)
            {
                const int sample = start + i;
                float bass = 0.0f;
                float kick = 0.0f;

                for (int hit = 0; hit < hitCount; ++hit)
                {
                    const int base = startOffset + hit * hitSpacing;
                    const int bassIndex = sample - (base + bassDelays[(size_t) hit]);
                    const int kickIndex = sample - (base + kickDelays[(size_t) hit]);

                    if (bassIndex >= 0 && bassIndex < eventLength)
                    {
                        const float env = (float) std::exp (-6.0 * (double) bassIndex / kSampleRate);
                        const float value = env * (float) std::sin (kTwoPi * 70.0 * (double) bassIndex / kSampleRate);
                        bass += invertBass[(size_t) hit] ? -value : value;
                    }

                    if (kickIndex >= 0 && kickIndex < eventLength)
                    {
                        const float env = (float) std::exp (-6.0 * (double) kickIndex / kSampleRate);
                        kick += env * (float) std::sin (kTwoPi * 70.0 * (double) kickIndex / kSampleRate);
                    }
                }

                buffer.setSample (0, i, bass);
                if (buffer.getNumChannels() > 1)
                    buffer.setSample (1, i, bass);
                if (buffer.getNumChannels() > 2)
                    buffer.setSample (2, i, kick);
                if (buffer.getNumChannels() > 3)
                    buffer.setSample (3, i, kick);
            }

            juce::MidiBuffer midi;
            processor.processBlock (buffer, midi);
        }
    }
};

static KickLockProcessorTransparencyTests kickLockProcessorTransparencyTestsInstance;

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
