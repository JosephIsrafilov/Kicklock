#include "TestCommon.h"

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
class MultibandPhaseModuleTests : public juce::UnitTest
{
public:
    MultibandPhaseModuleTests() : juce::UnitTest ("MultibandPhaseModules", "DSP") {}

    void runTest() override
    {
        beginTest ("LR crossover produces finite low/high bands");
        {
            constexpr int n = 1024;
            juce::AudioBuffer<float> in (2, n), low (2, n), high (2, n);
            for (int i = 0; i < n; ++i)
            {
                const float s = (float) (0.4 * std::sin (kTwoPi * 80.0 * (double) i / kSampleRate)
                                       + 0.2 * std::sin (kTwoPi * 2000.0 * (double) i / kSampleRate));
                in.setSample (0, i, s);
                in.setSample (1, i, s);
            }

            LinkwitzRileyCrossover crossover;
            crossover.prepare ({ kSampleRate, (juce::uint32) n, 2 });
            crossover.setCrossoverFrequency (150.0f);
            crossover.split (in, low, high, n);

            float sumAbs = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                expect (std::isfinite (low.getSample (0, i)));
                expect (std::isfinite (high.getSample (0, i)));
                sumAbs += std::abs (low.getSample (0, i)) + std::abs (high.getSample (0, i));
            }
            expectGreaterThan (sumAbs, 1.0f);
        }

        beginTest ("Transient envelope emits attack/hold/release window");
        {
            TransientEnvelopeFollower follower;
            follower.prepare (1000.0);
            follower.setWindow (2.0f, 4.0f, 20.0f);
            follower.setTriggerRatio (2.0f);

            float peak = 0.0f;
            for (int i = 0; i < 80; ++i)
                peak = juce::jmax (peak, follower.processSample (i == 10 ? 1.0f : 0.0f));

            expectGreaterThan (peak, 0.8f);
        }

        beginTest ("Transient envelope opens on a decaying kick tone");
        {
            TransientEnvelopeFollower follower;
            follower.prepare (kSampleRate);
            follower.setWindow (2.0f, 18.0f, 80.0f);
            follower.setTriggerRatio (1.6f);

            float peak = 0.0f;
            for (int i = 0; i < 4096; ++i)
            {
                const double t = (double) i / kSampleRate;
                const float kick = (float) (std::sin (kTwoPi * 60.0 * t) * std::exp (-t * 35.0));
                peak = juce::jmax (peak, follower.processSample (kick));
            }

            expectGreaterThan (peak, 0.8f);
        }

        beginTest ("Dynamic EQ can increase high-band peak when envelope is open");
        {
            DynamicHighBandEQ eq;
            eq.prepare ({ kSampleRate, 512, 2 });
            eq.setFrequency (3200.0f);
            eq.setQ (4.0f);
            eq.setMaxBoostDb (9.0f);

            float pre = 0.0f, post = 0.0f;
            for (int i = 0; i < 2048; ++i)
            {
                const float x = (float) (0.2 * std::sin (kTwoPi * 3200.0 * (double) i / kSampleRate));
                pre = juce::jmax (pre, std::abs (x));
                post = juce::jmax (post, std::abs (eq.processSample (0, x, 1.0f, 1.0f)));
            }

            expectGreaterThan (post, pre);
        }

        beginTest ("Transient health reports positive delta when post peak is larger");
        {
            TransientHealthMeter meter;
            meter.prepare (kSampleRate, 80.0f);
            meter.pushBlock (0.25f, 0.5f);
            expectGreaterThan (meter.getPostPeak(), meter.getPrePeak());
            expectGreaterThan (meter.getHealthDb(), 0.0f);
        }

        beginTest ("Multiband core reports fixed 20 ms latency");
        {
            MultibandPhaseCore core;
            core.prepare (kSampleRate, 512, 2, 20.0f);
            expectEquals (core.reportLatencySamples(), (int) std::ceil (kSampleRate * 0.020));

            juce::AudioBuffer<float> main (2, 512), kick (2, 512);
            main.clear();
            kick.clear();
            MultibandPhaseCore::Params params;
            params.dynEqAmount = 0.0f;
            params.allpassEnabled = false;
            params.polarityInvert = false;
            core.process (main, kick, params, 512);
            expectEquals (core.reportLatencySamples(), (int) std::ceil (kSampleRate * 0.020));
            expectWithinAbsoluteError (core.getHealthMeter().getHealthDb(), 0.0f, 0.01f);
        }
    }
};

static MultibandPhaseModuleTests multibandPhaseModuleTestsInstance;

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

        // P4: the fast lag-0 path must agree with the full FFT path's
        // beforeMatch (both are the zero-offset 30-120 Hz correlation).
        beginTest ("matchAtZeroLag agrees with the FFT beforeMatch within 1 point");
        {
            std::vector<float> bass ((size_t) n, 0.0f), kick ((size_t) n, 0.0f);
            const int lag = 60;
            for (int i = 0; i < n; ++i)
            {
                kick[(size_t) i] = event (i);
                if (i - lag >= 0)
                    bass[(size_t) i] = event (i - lag);
            }

            const auto full = AlignmentAnalyzer::analyze (bass.data(), kick.data(), n, kSampleRate, 50.0f);
            const float fast = AlignmentAnalyzer::matchAtZeroLag (bass.data(), kick.data(), n, kSampleRate);

            expect (full.valid);
            expectWithinAbsoluteError (fast, full.beforeMatch, 1.0f);
        }

        // P4: skipping the rotator grid must not change the lag/polarity result
        // (only whether a rotator is recommended), and must be no slower.
        beginTest ("searchRotator=false matches lag and polarity of the full search");
        {
            std::vector<float> bass ((size_t) n, 0.0f), kick ((size_t) n, 0.0f);
            const int lag = 40;
            for (int i = 0; i < n; ++i)
            {
                kick[(size_t) i] = event (i);
                if (i - lag >= 0)
                    bass[(size_t) i] = event (i - lag);
            }

            const auto withRot = AlignmentAnalyzer::analyze (bass.data(), kick.data(), n, kSampleRate,
                                                             50.0f, 30.0f, 120.0f, 16384, true);
            const auto noRot   = AlignmentAnalyzer::analyze (bass.data(), kick.data(), n, kSampleRate,
                                                             50.0f, 30.0f, 120.0f, 16384, false);

            expect (withRot.valid && noRot.valid);
            expectWithinAbsoluteError (noRot.delayMs, withRot.delayMs, 1.0e-4f);
            expect (noRot.invertPolarity == withRot.invertPolarity);
            expect (! noRot.adjustRotator, "rotator must stay off when the grid is skipped");
        }
    }
};

static AlignmentAnalyzerTests alignmentAnalyzerTestsInstance;

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

            // 4-band plan: band[1] = LOW (60-120) holds the in-phase 60 Hz;
            // band[2] = LOW MID (120-250) holds the inverted 150 Hz.
            expectGreaterThan (result.bands[1].correlation, 0.75f);
            expectLessThan (result.bands[2].correlation, -0.40f);
            expectGreaterThan (result.confidence, 0.1f);
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

        beginTest ("Small improvement still leaves an auditionable suggestion");
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
            expect (r.optionalApplyAllowed);
            expect (r.message.containsIgnoreCase ("apply to audition"));
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

        beginTest ("Analyze publishes the same after score as the rendered candidate");
        {
            std::vector<float> bass ((size_t) n), kick ((size_t) n);
            fillBurst (bass, kick, 1000, 1120, 65.0);

            const auto r = PhaseFixEngine::analyze (bass.data(), kick.data(), n, kSampleRate, 10.0f);

            PhaseFixRenderSettings settings;
            settings.bassPolarityInvert = r.bassPolarityInvert;
            settings.bassDelayMs = r.bassDelayMs;
            settings.phaseFilterEnabled = r.phaseFilterEnabled;
            settings.phaseFilterFreqHz = r.phaseFilterFreqHz;
            settings.phaseFilterQ = r.phaseFilterQ;
            settings.phaseFilterStages = r.phaseFilterStages;

            const auto rendered = PhaseFixEngine::scoreSettings (bass.data(), kick.data(), n, kSampleRate,
                                                                 settings,
                                                                 PhaseFixEngine::absoluteManualMaxDelayMs);

            expect (r.valid);
            expectWithinAbsoluteError (r.predictedAfterMatchPercent, rendered.matchPercent, 0.01f);
        }

        beginTest ("Render candidate honours negative bass delay");
        {
            std::vector<float> bass ((size_t) n, 0.0f);
            bass[1000] = 1.0f;

            PhaseFixRenderSettings settings;
            settings.bassDelayMs = -2.0f;

            std::vector<float> rendered;
            PhaseFixEngine::renderCandidate (bass.data(), n, kSampleRate, settings, rendered, 20.0f);

            int peak = 0;
            for (int i = 1; i < n; ++i)
                if (std::abs (rendered[(size_t) i]) > std::abs (rendered[(size_t) peak]))
                    peak = i;

            expectEquals (peak, 904);
        }

        beginTest ("Analyze confidence is measured, not forced to 100 percent");
        {
            std::vector<float> bass ((size_t) n, 0.0f), kick ((size_t) n, 0.0f);
            constexpr float amp = 0.02f;

            for (int i = 0; i < n; ++i)
            {
                const double t = (double) i / kSampleRate;
                const float v = amp * (float) std::sin (kTwoPi * 70.0 * t);
                bass[(size_t) i] = -v;
                kick[(size_t) i] = v;
            }

            const auto r = PhaseFixEngine::analyze (bass.data(), kick.data(), n, kSampleRate, 10.0f);

            expect (r.valid);
            expectGreaterThan (r.confidence, 0.0f);
            expectLessThan (r.confidence, 0.99f);
        }

        beginTest ("Bass too early recommends positive bass delay");
        {
            // ~2.5 ms offset (120 samples at 48 kHz): before ~76%, so the fix is
            // a genuine Strong improvement, not a cosmetic nudge on an already-
            // aligned pair (P2 honest classification).
            std::vector<float> bass ((size_t) n), kick ((size_t) n);
            fillBurst (bass, kick, 1000, 1120, 65.0);

            const auto r = PhaseFixEngine::analyze (bass.data(), kick.data(), n, kSampleRate, 10.0f);

            expect (r.valid);
            expectGreaterThan (r.bassDelayMs, 2.0f);
            expectLessThan (r.bassDelayMs, 3.2f);
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

        beginTest ("Already aligned offers only optional apply");
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
            expect (r.optionalApplyAllowed);
        }

        beginTest ("Phase offset returns an applicable low-end fix");
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
            expect (r.afterMatchPercent > r.beforeMatchPercent + 5.0f, phaseDebug);
            expect (r.applyAllowed || r.optionalApplyAllowed, phaseDebug);
        }

        beginTest ("Already good without useful phase refinement still offers optional apply");
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
            expect (! r.phaseFilterEnabled);
            expect (r.optionalApplyAllowed);
            expect (! PhaseFixEngine::canApply (r));
            expect (r.message.containsIgnoreCase ("already close"));
        }

        beginTest ("Bass late recommends signed negative delay inside the PDC budget");
        {
            std::vector<float> bass ((size_t) n), kick ((size_t) n);
            fillBurst (bass, kick, 1040, 1000, 70.0);

            const auto r = PhaseFixEngine::analyze (bass.data(), kick.data(), n, kSampleRate, 10.0f);

            expect (r.valid);
            expectLessThan (r.bassDelayMs, -0.5f);
            expect (! r.requiresTimelineMove);
            expect (r.optionalApplyAllowed);
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
            expectLessThan (r.bassDelayMs, -0.5f);
            expect (r.applyAllowed || r.optionalApplyAllowed);
        }

        // P2: an already well-aligned pair must not be sold a fix, and the
        // rotator must not enable itself on a negligible correlation gain.
        beginTest ("Already >=95% aligned reads AlreadyGood with no rotator");
        {
            std::vector<float> bass ((size_t) n), kick ((size_t) n);
            fillBurst (bass, kick, 1000, 1000, 70.0); // perfectly aligned

            const auto r = PhaseFixEngine::analyze (bass.data(), kick.data(), n, kSampleRate, 10.0f);

            expect (r.valid);
            expectGreaterThan (r.beforeMatchPercent, 95.0f);
            expectEquals ((int) r.quality, (int) PhaseFixQuality::AlreadyGood);
            expect (! r.phaseFilterEnabled, "rotator enabled on an already-aligned pair");
            expect (! PhaseFixEngine::canApply (r));
            expect (r.optionalApplyAllowed);
        }

        // P2: a genuine ~2 ms timing error is a Strong fix, and the recovered
        // delay lands close to the true offset. (The exponential burst decay
        // biases the correlation peak by ~0.3 ms, so the tolerance reflects the
        // engine's real accuracy on a transient rather than an idealised tone.)
        beginTest ("A 2 ms offset classifies Strong with a close delay estimate");
        {
            const int lag = (int) std::lround (kSampleRate * 2.0 / 1000.0); // 2 ms
            std::vector<float> bass ((size_t) n), kick ((size_t) n);
            fillBurst (bass, kick, 1000, 1000 + lag, 65.0); // kick late -> +delay bass

            const auto r = PhaseFixEngine::analyze (bass.data(), kick.data(), n, kSampleRate, 10.0f);

            expect (r.valid);
            expectEquals ((int) r.quality, (int) PhaseFixQuality::StrongImprovement);
            expectWithinAbsoluteError (r.bassDelayMs, 2.0f, 0.4f);
            expect (PhaseFixEngine::canApply (r), r.message);
        }

        // P2: deterministic peak tie-breaking - the same window analyzed 20x
        // must yield identical polarity and delay every time (no flip-flopping).
        beginTest ("Repeated analysis is bit-stable over 20 runs");
        {
            std::vector<float> bass ((size_t) n), kick ((size_t) n);
            fillBurst (bass, kick, 1000, 1000 + 96, 65.0); // ~2 ms, kick late

            const auto first = PhaseFixEngine::analyze (bass.data(), kick.data(), n, kSampleRate, 10.0f);
            for (int i = 0; i < 20; ++i)
            {
                const auto r = PhaseFixEngine::analyze (bass.data(), kick.data(), n, kSampleRate, 10.0f);
                expectWithinAbsoluteError (r.bassDelayMs, first.bassDelayMs, 1.0e-6f);
                expect (r.bassPolarityInvert == first.bassPolarityInvert);
                expect (r.phaseFilterEnabled == first.phaseFilterEnabled);
            }
        }
    }
};

static PhaseFixEngineTests phaseFixEngineTestsInstance;

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

        beginTest ("Hit snapshot does not return torn windows under concurrent handoff");
        {
            HitCaptureBuffer buffer;
            buffer.prepare (1000.0, 4.0f, 4.0f);

            std::atomic<bool> done { false };
            std::atomic<bool> torn { false };

            std::thread writer ([&]
            {
                for (int seq = 1; seq <= 400; ++seq)
                {
                    const float bass = (float) seq;
                    const float kick = 10000.0f + (float) seq;

                    for (int i = 0; i < 4; ++i)
                        buffer.pushSample (bass, kick, false);

                    buffer.pushSample (bass, kick, true);

                    for (int i = 0; i < 4; ++i)
                        buffer.pushSample (bass, kick, false);
                }

                done.store (true, std::memory_order_release);
            });

            while (! done.load (std::memory_order_acquire))
            {
                std::vector<float> bass, kick;
                const int count = buffer.snapshotLatest (bass, kick);
                if (count <= 0)
                {
                    std::this_thread::yield();
                    continue;
                }

                const float expectedBass = bass[0];
                const float expectedKick = kick[0];
                for (int i = 0; i < count; ++i)
                {
                    if (bass[(size_t) i] != expectedBass || kick[(size_t) i] != expectedKick)
                    {
                        torn.store (true, std::memory_order_relaxed);
                        break;
                    }
                }
            }

            writer.join();
            expect (! torn.load (std::memory_order_relaxed), "snapshot returned mixed samples from different hit windows");
        }
    }
};

static CaptureBufferTests captureBufferTestsInstance;

