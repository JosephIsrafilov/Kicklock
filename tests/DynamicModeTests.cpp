#include "TestCommon.h"

// Phase -1 stabilization tests.
//
// T0 — high-band transparency: proves the hidden sidechain-reactive
//      DynamicTransientEQ has been removed, so the high band no longer changes
//      with the kick sidechain.
// T1 — Static golden baseline: a deterministic post-stabilization render of a
//      representative Static fixture matrix, pinned to golden RMS signatures and
//      checked for determinism and block-size invariance.
// T2 — Parameter / A-B serialization completeness: every ParameterSnapshot field
//      (including crossover enable/frequency, delay interpolation and pitch
//      follow) survives a full getStateInformation -> setStateInformation round
//      trip for both compare slots.

namespace
{
    // Deterministic Static material. The same bass is used for every fixture so
    // golden RMS differences reflect the processing alone. It deliberately
    // carries strong high-band content (a 3 kHz tone plus a periodic transient)
    // so the high-band transparency test is meaningful.
    float staticBassSample (int idx, double sampleRate) noexcept
    {
        const double t = (double) idx / sampleRate;
        const double beat = std::fmod (t, 0.5); // ~120 BPM period
        const float fundamental = 0.60f * (float) std::sin (kTwoPi * 55.0 * t);
        const float octave      = 0.20f * (float) std::sin (kTwoPi * 110.0 * t);
        const float highTone    = 0.15f * (float) std::sin (kTwoPi * 3000.0 * t);
        const float env         = (float) std::exp (-beat * 22.0);
        const float transient   = 0.30f * env * (float) std::sin (kTwoPi * 1500.0 * t);
        return fundamental + octave + highTone + transient;
    }

    // A sharp, decaying, broadband-ish kick click. Only used to prove the high
    // band ignores it; its exact shape does not matter for the Static golden.
    float kickSample (int idx, double sampleRate) noexcept
    {
        const double t = (double) idx / sampleRate;
        const double beat = std::fmod (t, 0.5);
        const float env = (float) std::exp (-beat * 55.0);
        return env * (0.9f * (float) std::sin (kTwoPi * 60.0 * t)
                      + 0.6f * (float) std::sin (kTwoPi * 2500.0 * t));
    }

    std::vector<float> renderStaticCore (const MultibandPhaseCore::Params& params,
                                         double sampleRate,
                                         int blockSize,
                                         int totalSamples,
                                         bool withKick)
    {
        MultibandPhaseCore core;
        core.prepare (sampleRate, blockSize, 1, 20.0f);

        juce::AudioBuffer<float> main (1, blockSize);
        juce::AudioBuffer<float> sidechain (1, blockSize);
        std::vector<float> output ((size_t) totalSamples, 0.0f);

        for (int offset = 0; offset < totalSamples; offset += blockSize)
        {
            const int n = std::min (blockSize, totalSamples - offset);
            main.setSize (1, n, false, false, true);
            sidechain.setSize (1, n, false, false, true);

            for (int i = 0; i < n; ++i)
            {
                main.setSample (0, i, staticBassSample (offset + i, sampleRate));
                sidechain.setSample (0, i, withKick ? kickSample (offset + i, sampleRate) : 0.0f);
            }

            core.process (main, sidechain, params, n);

            for (int i = 0; i < n; ++i)
                output[(size_t) (offset + i)] = main.getSample (0, i);
        }

        return output;
    }

    double rmsOf (const std::vector<float>& x) noexcept
    {
        double sum = 0.0;
        for (const float v : x)
            sum += (double) v * (double) v;
        return std::sqrt (sum / (double) std::max<size_t> (1, x.size()));
    }

    double maxAbsDiff (const std::vector<float>& a, const std::vector<float>& b) noexcept
    {
        const size_t n = std::min (a.size(), b.size());
        double m = 0.0;
        for (size_t i = 0; i < n; ++i)
            m = std::max (m, (double) std::abs (a[i] - b[i]));
        return m;
    }

    bool allFinite (const std::vector<float>& x) noexcept
    {
        for (const float v : x)
            if (! std::isfinite (v))
                return false;
        return true;
    }

    void setFloatParam (KickLockAudioProcessor& processor, const char* id, float value)
    {
        if (auto* param = processor.apvts.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 (value));
    }

    void setBoolParam (KickLockAudioProcessor& processor, const char* id, bool value)
    {
        if (auto* param = processor.apvts.getParameter (id))
            param->setValueNotifyingHost (value ? 1.0f : 0.0f);
    }

    float rawParam (KickLockAudioProcessor& processor, const char* id)
    {
        if (const auto* value = processor.apvts.getRawParameterValue (id))
            return value->load();
        return 0.0f;
    }

    // A distinctive full parameter state, used to prove every field round-trips.
    struct FullState
    {
        float delayMs;
        bool  polarity;
        bool  phaseEnabled;
        float freqHz;
        float q;
        float stages;       // rotatorStages raw (0..2)
        bool  crossoverOn;
        float crossoverHz;
        float delayInterp;  // 0 or 1
        bool  pitchTrack;
    };

    void applyFullState (KickLockAudioProcessor& processor, const FullState& s)
    {
        setFloatParam (processor, "delay_ms", s.delayMs);
        setFloatParam (processor, "delayMs", s.delayMs);
        setBoolParam  (processor, "polarity_invert", s.polarity);
        setBoolParam  (processor, "polarityInvert", s.polarity);
        setBoolParam  (processor, "allpass_enable", s.phaseEnabled);
        setBoolParam  (processor, "phaseFilterEnabled", s.phaseEnabled);
        setFloatParam (processor, "allpass_freq", s.freqHz);
        setFloatParam (processor, "rotatorFreq", s.freqHz);
        setFloatParam (processor, "rotatorQ", s.q);
        setFloatParam (processor, "rotatorStages", s.stages);
        setBoolParam  (processor, "crossover_enable", s.crossoverOn);
        setFloatParam (processor, "crossover_freq", s.crossoverHz);
        setFloatParam (processor, "delayInterp", s.delayInterp);
        setBoolParam  (processor, "pitch_track", s.pitchTrack);
    }
}

class HighBandTransparencyTests : public juce::UnitTest
{
public:
    HighBandTransparencyTests() : juce::UnitTest ("HighBandTransparency", "Stabilization") {}

    void runTest() override
    {
        beginTest ("T0: high band does not react to the kick sidechain (neutral bass path)");
        {
            MultibandPhaseCore::Params params;
            params.crossoverEnabled = true;
            params.crossoverHz = 150.0f;
            params.userDelayMs = 0.0f;
            params.polarityInvert = false;
            params.allpassEnabled = false;

            const double sr = 48000.0;
            const int total = (int) std::round (sr * 1.0);

            const auto silent = renderStaticCore (params, sr, 256, total, false);
            const auto kicked = renderStaticCore (params, sr, 256, total, true);

            // The bass carries real high-band energy, so the fixture is
            // non-trivial and the old transient EQ would have modulated it.
            expectGreaterThan (rmsOf (silent), 1.0e-3, "fixture should be non-trivial");

            // With the hidden transient EQ removed the core reads no sidechain,
            // so the two renders must be bit-for-bit identical.
            expectLessThan (maxAbsDiff (silent, kicked), 1.0e-7,
                            "high band must be independent of the kick sidechain");
        }

        beginTest ("T0: high band stays sidechain-independent while low-band correction is active");
        {
            MultibandPhaseCore::Params params;
            params.crossoverEnabled = true;
            params.crossoverHz = 150.0f;
            params.userDelayMs = 2.0f;
            params.polarityInvert = true;
            params.allpassEnabled = true;
            params.allpassFreqHz = 70.0f;
            params.allpassQ = 1.2f;
            params.allpassStages = 3;

            const double sr = 48000.0;
            const int total = (int) std::round (sr * 1.0);

            const auto silent = renderStaticCore (params, sr, 256, total, false);
            const auto kicked = renderStaticCore (params, sr, 256, total, true);

            expectLessThan (maxAbsDiff (silent, kicked), 1.0e-7,
                            "correction path must not make the output depend on the sidechain");
        }

        beginTest ("T0: full processor main output is independent of the kick sidechain");
        {
            const double sr = 48000.0;
            const int blockSize = 256;
            const int total = (int) std::round (sr * 0.8);

            auto renderProcessor = [sr, blockSize, total] (bool withKick)
            {
                KickLockAudioProcessor processor;
                processor.enableAllBuses();
                processor.setRateAndBufferSizeDetails (sr, blockSize);
                processor.prepareToPlay (sr, blockSize);
                // Crossover ON is the path that used to feed the transient EQ.
                if (auto* p = processor.apvts.getParameter ("crossover_enable"))
                    p->setValueNotifyingHost (1.0f);

                std::vector<float> out ((size_t) total, 0.0f);
                for (int offset = 0; offset < total; offset += blockSize)
                {
                    const int n = std::min (blockSize, total - offset);
                    juce::AudioBuffer<float> buffer (juce::jmax (processor.getTotalNumInputChannels(),
                                                                 processor.getTotalNumOutputChannels()),
                                                     n);
                    buffer.clear();
                    for (int i = 0; i < n; ++i)
                    {
                        const float bass = staticBassSample (offset + i, sr);
                        const float kick = withKick ? kickSample (offset + i, sr) : 0.0f;
                        buffer.setSample (0, i, bass);
                        if (buffer.getNumChannels() > 1) buffer.setSample (1, i, bass);
                        if (buffer.getNumChannels() > 2) buffer.setSample (2, i, kick);
                        if (buffer.getNumChannels() > 3) buffer.setSample (3, i, kick);
                    }

                    juce::MidiBuffer midi;
                    processor.processBlock (buffer, midi);

                    for (int i = 0; i < n; ++i)
                        out[(size_t) (offset + i)] = buffer.getSample (0, i);
                }
                return out;
            };

            const auto silent = renderProcessor (false);
            const auto kicked = renderProcessor (true);

            expectGreaterThan (rmsOf (silent), 1.0e-3, "processor fixture should be non-trivial");
            expectLessThan (maxAbsDiff (silent, kicked), 1.0e-6,
                            "processor main output must not depend on the kick sidechain");
        }
    }
};

class StaticGoldenBaselineTests : public juce::UnitTest
{
public:
    StaticGoldenBaselineTests() : juce::UnitTest ("StaticGoldenBaseline", "Stabilization") {}

    struct Fixture
    {
        const char* name;
        MultibandPhaseCore::Params params;
        double goldenRms;   // pinned post-stabilization signature (48 kHz, block 256)
    };

    std::vector<Fixture> makeFixtures() const
    {
        auto make = [] (bool xover, float delayMs, bool pol, bool ap, float freq,
                        float q, int stages)
        {
            MultibandPhaseCore::Params p;
            p.crossoverEnabled = xover;
            p.crossoverHz = 150.0f;
            p.userDelayMs = delayMs;
            p.polarityInvert = pol;
            p.allpassEnabled = ap;
            p.allpassFreqHz = freq;
            p.allpassQ = q;
            p.allpassStages = stages;
            return p;
        };

        // Golden RMS values are filled from a first post-stabilization run and
        // pinned here. Tolerance is generous enough to survive cross-platform
        // float variation but tight enough to catch a real Static regression;
        // T0 is the sharp guard for the transient-EQ removal specifically.
        return {
            { "neutral",           make (true,   0.0f, false, false,  50.0f, 0.7f, 2), 0.45597210 },
            { "delay+3ms",         make (true,   3.0f, false, false,  50.0f, 0.7f, 2), 0.44108530 },
            { "delay-3ms",         make (true,  -3.0f, false, false,  50.0f, 0.7f, 2), 0.44260485 },
            { "polarity-invert",   make (true,   0.0f, true,  false,  50.0f, 0.7f, 2), 0.42692361 },
            { "allpass-2stage",    make (true,   0.0f, false, true,   60.0f, 1.0f, 2), 0.43639122 },
            { "allpass-3stage",    make (true,   0.0f, false, true,   60.0f, 1.0f, 3), 0.42648037 },
            { "allpass-4stage",    make (true,   0.0f, false, true,   60.0f, 1.0f, 4), 0.43875166 },
            { "crossover-off-ap",  make (false,  0.0f, false, true,   60.0f, 1.0f, 2), 0.45283569 },
        };
    }

    void runTest() override
    {
        const double sr = 48000.0;
        const int blockSize = 256;
        const int total = (int) std::round (sr * 1.0);
        const auto fixtures = makeFixtures();

        beginTest ("T1: Static render is deterministic and finite");
        {
            for (const auto& f : fixtures)
            {
                const auto a = renderStaticCore (f.params, sr, blockSize, total, false);
                const auto b = renderStaticCore (f.params, sr, blockSize, total, false);
                expect (allFinite (a), juce::String (f.name) + ": output must be finite");
                expect (maxAbsDiff (a, b) == 0.0,
                        juce::String (f.name) + ": repeated render must be bit-exact");
            }
        }

        beginTest ("T1: Static render matches the golden post-stabilization baseline");
        {
            for (const auto& f : fixtures)
            {
                const auto out = renderStaticCore (f.params, sr, blockSize, total, false);
                const double rms = rmsOf (out);
                logMessage (juce::String (f.name) + " RMS=" + juce::String (rms, 8));
                // Golden captured on the post-stabilization build. Same-platform
                // renders are bit-exact (see the determinism test); this 0.2%
                // relative band absorbs cross-platform float math while still
                // catching a real Static regression (which shifts RMS by whole
                // percent). T0 is the sharp guard for the transient-EQ removal.
                const double tol = 2.0e-3 * std::max (1.0e-6, f.goldenRms);
                expectWithinAbsoluteError (rms, f.goldenRms, tol,
                                           juce::String (f.name) + ": golden RMS drift");
            }
        }

        beginTest ("T1: Static render is invariant to host block size");
        {
            // Constant parameters -> the internally-chunked core must produce the
            // same result regardless of how the host splits the stream.
            const auto& f = fixtures[4]; // allpass-2stage
            const auto ref = renderStaticCore (f.params, sr, 256, total, false);
            for (int bs : { 32, 64, 2048 })
            {
                const auto other = renderStaticCore (f.params, sr, bs, total, false);
                expectLessThan (maxAbsDiff (ref, other), 1.0e-6,
                                "block size " + juce::String (bs) + " must match the 256 reference");
            }
        }

        beginTest ("T1: Static render is finite across sample rates");
        {
            const auto& f = fixtures[4]; // allpass-2stage
            for (double rate : { 44100.0, 48000.0, 96000.0 })
            {
                const int n = (int) std::round (rate * 0.5);
                const auto out = renderStaticCore (f.params, rate, 256, n, false);
                expect (allFinite (out), "sr=" + juce::String (rate, 0) + " must be finite");
                expectGreaterThan (rmsOf (out), 1.0e-3, "sr=" + juce::String (rate, 0));
            }
        }
    }
};

class ParameterSerializationCompletenessTests : public juce::UnitTest
{
public:
    ParameterSerializationCompletenessTests()
        : juce::UnitTest ("ParameterSerializationCompleteness", "Stabilization") {}

    void expectStateMatches (const FullState& s, KickLockAudioProcessor& p)
    {
        expectWithinAbsoluteError (rawParam (p, "delay_ms"), s.delayMs, 0.02f, "delay_ms");
        expectWithinAbsoluteError (rawParam (p, "polarity_invert"), s.polarity ? 1.0f : 0.0f, 1.0e-6f, "polarity");
        expectWithinAbsoluteError (rawParam (p, "allpass_enable"), s.phaseEnabled ? 1.0f : 0.0f, 1.0e-6f, "allpass_enable");
        expectWithinAbsoluteError (rawParam (p, "allpass_freq"), s.freqHz, 0.05f, "allpass_freq");
        expectWithinAbsoluteError (rawParam (p, "rotatorQ"), s.q, 0.02f, "rotatorQ");
        expectWithinAbsoluteError (rawParam (p, "rotatorStages"), s.stages, 1.0e-6f, "rotatorStages");
        expectWithinAbsoluteError (rawParam (p, "crossover_enable"), s.crossoverOn ? 1.0f : 0.0f, 1.0e-6f, "crossover_enable");
        expectWithinAbsoluteError (rawParam (p, "crossover_freq"), s.crossoverHz, 0.05f, "crossover_freq");
        expectWithinAbsoluteError (rawParam (p, "delayInterp"), s.delayInterp, 1.0e-6f, "delayInterp");
        expectWithinAbsoluteError (rawParam (p, "pitch_track"), s.pitchTrack ? 1.0f : 0.0f, 1.0e-6f, "pitch_track");
    }

    void runTest() override
    {
        const FullState slotA {
            -3.25f, true,  true,  180.0f, 2.5f, 1.0f, true,  90.0f,  1.0f, true
        };
        const FullState slotB {
            5.00f,  false, false, 60.0f,  0.9f, 2.0f, false, 200.0f, 0.0f, false
        };

        beginTest ("T2: every ParameterSnapshot field round-trips through save/reload");
        {
            juce::MemoryBlock stateData;
            {
                KickLockAudioProcessor source;
                source.setRateAndBufferSizeDetails (kSampleRate, 512);
                source.prepareToPlay (kSampleRate, 512);

                // Configure slot 0, then switch to slot 1 and configure it. The
                // switch captures slot 0 into the compare state.
                applyFullState (source, slotA);
                source.selectCompareSlot (1);
                applyFullState (source, slotB);

                source.getStateInformation (stateData);
                expectEquals (source.getActiveCompareSlot(), 1);
            }

            // Restore into a fresh instance.
            KickLockAudioProcessor restored;
            restored.setRateAndBufferSizeDetails (kSampleRate, 512);
            restored.prepareToPlay (kSampleRate, 512);
            restored.setStateInformation (stateData.getData(), (int) stateData.getSize());

            // Active slot (1) must restore verbatim on the live parameters.
            expectEquals (restored.getActiveCompareSlot(), 1);
            expectStateMatches (slotB, restored);

            // The inactive slot (0) must have survived persistence in full —
            // this is what the incomplete serialization used to lose (crossover
            // enable/frequency, delay interpolation and pitch follow).
            restored.selectCompareSlot (0);
            expectStateMatches (slotA, restored);

            // Switching back must still hold slot 1.
            restored.selectCompareSlot (1);
            expectStateMatches (slotB, restored);
        }

        beginTest ("T2: Analyze rollback bundle restores the full pre-Analyze state");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (kSampleRate, 2048);
            processor.prepareToPlay (kSampleRate, 2048);

            applyFullState (processor, slotA);

            expect (! processor.hasRevertSnapshot());
            // Directly exercise the rollback bundle without needing captured
            // material: capture, mutate, restore.
            processor.ensureRevertBundleCapturedForTesting();
            expect (processor.hasRevertSnapshot());

            applyFullState (processor, slotB);
            expect (processor.revertLatestFix());
            expect (! processor.hasRevertSnapshot());
            expectStateMatches (slotA, processor);

            // A second revert with no captured bundle is a no-op.
            expect (! processor.revertLatestFix());
        }
    }
};

static HighBandTransparencyTests highBandTransparencyTests;
static StaticGoldenBaselineTests staticGoldenBaselineTests;
static ParameterSerializationCompletenessTests parameterSerializationCompletenessTests;
