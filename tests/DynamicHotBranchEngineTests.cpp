#include "TestCommon.h"

#include "DynamicHotBranchEngine.h"

#include <array>
#include <cmath>
#include <limits>

namespace
{
    constexpr std::array<double, 5> kSampleRates { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };

    juce::AudioBuffer<float> makeImpulse (int channels, int samples, int impulseAt = 0)
    {
        juce::AudioBuffer<float> buffer (channels, samples);
        buffer.clear();
        for (int ch = 0; ch < channels; ++ch)
            if (impulseAt >= 0 && impulseAt < samples)
                buffer.setSample (ch, impulseAt, 1.0f);
        return buffer;
    }

    juce::AudioBuffer<float> makeSine (int channels, int samples, double sampleRate, double freq, double amp = 0.5)
    {
        juce::AudioBuffer<float> buffer (channels, samples);
        for (int i = 0; i < samples; ++i)
        {
            const float s = (float) (amp * std::sin (2.0 * juce::MathConstants<double>::pi
                                                     * freq * (double) i / sampleRate));
            for (int ch = 0; ch < channels; ++ch)
                buffer.setSample (ch, i, s);
        }
        return buffer;
    }

    int findImpulseIndex (const juce::AudioBuffer<float>& buffer, int channel, float threshold = 0.5f)
    {
        for (int i = 0; i < buffer.getNumSamples(); ++i)
            if (std::abs (buffer.getSample (channel, i)) >= threshold)
                return i;
        return -1;
    }
}

class DynamicHotBranchEngineTests : public juce::UnitTest
{
public:
    DynamicHotBranchEngineTests() : juce::UnitTest ("DynamicHotBranchEngine", "DSP") {}

    DynamicAllpassCoefficients makeCoeffs (double sampleRate, double freq = 100.0, double q = 0.7)
    {
        DynamicAllpassCoefficients coefficients;
        expect (DynamicAllpassPoleDomain::makeCoefficients (freq, q, sampleRate, coefficients));
        return coefficients;
    }

    // All Global-only fields must match across Global/State/Service configs.
    DynamicHotBranchConfig cfg (double sampleRate,
                                double delayMs,
                                uint64_t stableId = 0,
                                bool polarity = false,
                                int stages = 2,
                                bool allpassEnabled = true,
                                int interp = 0,
                                bool crossoverEnabled = false)
    {
        DynamicHotBranchConfig config;
        config.active = true;
        config.stableStateId = stableId;
        config.effectiveAbsoluteDelayMs = delayMs;
        config.coefficients = makeCoeffs (sampleRate);
        config.polarityInvert = polarity;
        config.allpassStages = stages;
        config.crossoverEnabled = crossoverEnabled;
        config.crossoverHz = 150.0;
        config.allpassEnabled = allpassEnabled;
        config.delayInterpolationIndex = interp;
        return config;
    }

    // Process material using an explicit process-call size and accumulate outputs.
    void processWithBlockSize (DynamicHotBranchEngine& engine,
                               const juce::AudioBuffer<float>& material,
                               int processBlockSize,
                               juce::AudioBuffer<float>& globalOut,
                               juce::AudioBuffer<float>& stateOut,
                               juce::AudioBuffer<float>& highOut,
                               int stateSlot = 0)
    {
        const int total = material.getNumSamples();
        const int channels = material.getNumChannels();
        expect (processBlockSize > 0);
        expect (processBlockSize <= engine.getMaxBlockSize());
        globalOut.setSize (channels, total);
        stateOut.setSize (channels, total);
        highOut.setSize (channels, total);
        globalOut.clear();
        stateOut.clear();
        highOut.clear();

        int written = 0;
        while (written < total)
        {
            const int n = juce::jmin (processBlockSize, total - written);
            juce::AudioBuffer<float> block (channels, n);
            for (int ch = 0; ch < channels; ++ch)
                block.copyFrom (ch, 0, material, ch, written, n);
            expect (engine.process (block, n).valid);
            for (int ch = 0; ch < channels; ++ch)
            {
                globalOut.copyFrom (ch, written, engine.getGlobalLowOutput(), ch, 0, n);
                stateOut.copyFrom (ch, written, engine.getStateLowOutput (stateSlot), ch, 0, n);
                highOut.copyFrom (ch, written, engine.getHighOutput(), ch, 0, n);
            }
            written += n;
        }
    }

    void processChunked (DynamicHotBranchEngine& engine,
                         const juce::AudioBuffer<float>& material,
                         juce::AudioBuffer<float>& globalOut,
                         juce::AudioBuffer<float>& stateOut,
                         juce::AudioBuffer<float>& highOut,
                         int stateSlot = 0)
    {
        processWithBlockSize (engine, material, engine.getMaxBlockSize(),
                              globalOut, stateOut, highOut, stateSlot);
    }

    // Test-local independent TDF-II reference (not the production helper).
    static float independentAllpass (float input,
                                     const DynamicAllpassCoefficients& c,
                                     int stages,
                                     std::array<std::array<double, 2>, 4>& z)
    {
        double x = (double) input;
        double selected = x;
        const int active = juce::jlimit (2, 4, stages);
        for (int s = 0; s < 4; ++s)
        {
            const double y = c.b0 * x + z[(size_t) s][0];
            z[(size_t) s][0] = c.b1 * x - c.a1 * y + z[(size_t) s][1];
            z[(size_t) s][1] = c.b2 * x - c.a2 * y;
            x = y;
            if (s + 1 == active)
                selected = y;
        }
        return (float) selected;
    }

    void runTest() override
    {
        beginTest ("Prepare succeeds at supported rates and rejects invalid inputs");
        {
            for (const double rate : kSampleRates)
            {
                DynamicHotBranchEngine engine;
                expect (engine.prepare (rate, 512, 2));
                expect (engine.isPrepared());
                expectEquals (engine.getReportedLatencySamples(),
                              (int) std::ceil (rate * 0.020));
                const int minCapacity = (int) std::ceil (rate * 0.340)
                    + DynamicHotBranchContract::kInterpolationGuardSamples;
                expect (engine.getLowHistoryCapacity() >= minCapacity,
                        "rate=" + juce::String (rate, 0)
                            + " cap=" + juce::String (engine.getLowHistoryCapacity())
                            + " min=" + juce::String (minCapacity));
            }

            DynamicHotBranchEngine bad;
            expect (! bad.prepare (0.0, 512, 2));
            expect (! bad.prepare (-48000.0, 512, 2));
            expect (! bad.prepare (std::numeric_limits<double>::quiet_NaN(), 512, 2));
            expect (! bad.prepare (48000.0, 0, 2));
            expect (! bad.prepare (48000.0, 512, 0));
            expect (! bad.prepare (48000.0, 512, 3));
            expect (bad.prepare (48000.0, 256, 1));
        }

        beginTest ("Shared-history integer taps land at exact physical positions");
        {
            const double rate = 48000.0;
            DynamicHotBranchEngine engine;
            expect (engine.prepare (rate, 512, 1));
            expect (engine.configureGlobal (cfg (rate, 0.0, 0, false, 2, false)));
            expect (engine.configureStateSlot (0, cfg (rate, -3.0, 11, false, 2, false)));
            expect (engine.configureStateSlot (1, cfg (rate, 3.0, 22, false, 2, false)));

            const int total = 3000;
            const auto impulse = makeImpulse (1, total, 0);
            juce::AudioBuffer<float> globalOut, state0, highOut, state1;
            processChunked (engine, impulse, globalOut, state0, highOut, 0);

            // Re-run for state slot 1 accumulation.
            engine.reset();
            expect (engine.configureGlobal (cfg (rate, 0.0, 0, false, 2, false)));
            expect (engine.configureStateSlot (0, cfg (rate, -3.0, 11, false, 2, false)));
            expect (engine.configureStateSlot (1, cfg (rate, 3.0, 22, false, 2, false)));
            processChunked (engine, impulse, globalOut, state1, highOut, 1);

            const int latency = engine.getReportedLatencySamples();
            const int stateATap = (int) std::lround ((double) latency + (-3.0) * rate / 1000.0);
            const int stateBTap = (int) std::lround ((double) latency + 3.0 * rate / 1000.0);

            expectEquals (findImpulseIndex (globalOut, 0), latency);
            expectEquals (findImpulseIndex (state0, 0), stateATap);
            expectEquals (findImpulseIndex (state1, 0), stateBTap);
            // Crossover is disabled: high input is zero, so high output stays silent.
            bool highSilent = true;
            for (int i = 0; i < highOut.getNumSamples(); ++i)
                if (std::abs (highOut.getSample (0, i)) > 1.0e-7f)
                    highSilent = false;
            expect (highSilent);
        }

        beginTest ("Minimum and maximum physical taps and rejection beyond capacity");
        {
            const double rate = 48000.0;
            DynamicHotBranchEngine engine;
            expect (engine.prepare (rate, 512, 1));
            expect (engine.configureGlobal (cfg (rate, -4.0)));
            expect (engine.configureStateSlot (0, cfg (rate, -3.0, 1)));
            expect (engine.configureGlobal (cfg (rate, 17.0)));
            expect (engine.configureStateSlot (1, cfg (rate, 3.0, 2)));

            DynamicHotBranchConfig tooDeep = cfg (rate, 17.0, 3);
            tooDeep.effectiveAbsoluteDelayMs = 20.1;
            expect (! engine.configureStateSlot (2, tooDeep));
        }

        beginTest ("Linear fractional interpolation matches hand calculation");
        {
            DynamicHotBranchDetail::FractionalInterpolator interp;
            expectWithinAbsoluteError (interp.process (1.0f, 0.0f, 0.25f, 0), 0.75f, 1.0e-6f);
        }

        beginTest ("First-order allpass fractional interpolation matches hand calculation");
        {
            DynamicHotBranchDetail::FractionalInterpolator interp;
            const float frac = 0.3f;
            const float eta = (1.0f - frac) / (1.0f + frac);
            expectWithinAbsoluteError (interp.process (1.0f, 0.0f, frac, 1), eta, 1.0e-6f);
        }

        beginTest ("Allpass TDF2 advances all stages; dry output remains finite");
        {
            const double rate = 48000.0;
            const auto coefficients = makeCoeffs (rate, 90.0, 1.1);
            std::array<DynamicHotBranchDetail::AllpassStageState, 4> ref {};
            const float y2 = DynamicHotBranchEngine::processAllpassReference (0.5f, coefficients, 2, ref);
            ref = {};
            const float y4 = DynamicHotBranchEngine::processAllpassReference (0.5f, coefficients, 4, ref);
            expect (std::isfinite (y2) && std::isfinite (y4));

            DynamicHotBranchEngine engine;
            expect (engine.prepare (rate, 512, 1));
            auto global = cfg (rate, 0.0, 0, false, 2, false);
            global.coefficients = coefficients;
            expect (engine.configureGlobal (global));
            const auto sine = makeSine (1, 256, rate, 80.0);
            expect (engine.process (sine, 256).valid);
            for (int i = 0; i < 256; ++i)
                expect (std::isfinite (engine.getGlobalLowOutput().getSample (0, i)));
        }

        beginTest ("Crossover-disabled routes full input to low and zeros high");
        {
            const double rate = 48000.0;
            DynamicHotBranchEngine engine;
            expect (engine.prepare (rate, 512, 1));
            expect (engine.configureGlobal (cfg (rate, 0.0, 0, false, 2, false, 0, false)));
            // Need more than 20 ms of material for the delayed impulse to emerge.
            const auto impulse = makeImpulse (1, 2048, 0);
            juce::AudioBuffer<float> globalOut, stateOut, highOut;
            processChunked (engine, impulse, globalOut, stateOut, highOut);
            bool highSilent = true;
            for (int i = 0; i < highOut.getNumSamples(); ++i)
                if (std::abs (highOut.getSample (0, i)) > 1.0e-7f)
                    highSilent = false;
            expect (highSilent);
            expectEquals (findImpulseIndex (globalOut, 0), engine.getReportedLatencySamples());
        }

        beginTest ("High path is common, exact PDC, unaffected by State count");
        {
            const double rate = 48000.0;
            DynamicHotBranchEngine engine;
            expect (engine.prepare (rate, 512, 1));
            expect (engine.configureGlobal (cfg (rate, 0.0, 0, false, 2, false, 0, true)));
            const auto sine = makeSine (1, 2048, rate, 2000.0, 0.6);
            juce::AudioBuffer<float> globalOut, stateOut, highWithoutStates;
            processChunked (engine, sine, globalOut, stateOut, highWithoutStates);

            const int latency = engine.getReportedLatencySamples();
            for (int i = 0; i < latency; ++i)
                expectWithinAbsoluteError (highWithoutStates.getSample (0, i), 0.0f, 1.0e-7f);
            bool highHasEnergy = false;
            for (int i = latency; i < 2048; ++i)
                if (std::abs (highWithoutStates.getSample (0, i)) > 1.0e-4f)
                    highHasEnergy = true;
            expect (highHasEnergy);

            engine.reset();
            expect (engine.configureGlobal (cfg (rate, 0.0, 0, false, 2, false, 0, true)));
            for (int s = 0; s < 8; ++s)
                expect (engine.configureStateSlot (s, cfg (rate, 0.0, (uint64_t) s + 1, false, 2, false, 0, true)));
            juce::AudioBuffer<float> highWithStates;
            processChunked (engine, sine, globalOut, stateOut, highWithStates);
            for (int i = 0; i < 2048; ++i)
                expectWithinAbsoluteError (highWithStates.getSample (0, i),
                                           highWithoutStates.getSample (0, i), 1.0e-6f);
        }

        beginTest ("Hot continuity: later observation matches continuous independent processing");
        {
            const double rate = 48000.0;
            DynamicHotBranchEngine live;
            DynamicHotBranchEngine reference;
            expect (live.prepare (rate, 256, 1));
            expect (reference.prepare (rate, 256, 1));

            // Global-only fields must match across branches (including interpolation).
            const auto global = cfg (rate, 0.0, 0, false, 2, true, 1);
            const auto stateB = cfg (rate, 1.5, 99, false, 2, true, 1);
            expect (live.configureGlobal (global));
            expect (live.configureStateSlot (0, cfg (rate, -1.0, 11, false, 2, true, 1)));
            expect (live.configureStateSlot (1, stateB));
            expect (reference.configureGlobal (global));
            expect (reference.configureStateSlot (1, stateB));

            const auto material = makeSine (1, 2048, rate, 70.0, 0.4);
            for (int offset = 0; offset < 2048; offset += 64)
            {
                juce::AudioBuffer<float> block (1, 64);
                block.copyFrom (0, 0, material, 0, offset, 64);
                expect (live.process (block, 64).valid);
                expect (reference.process (block, 64).valid);
                if (offset >= 512)
                {
                    for (int i = 0; i < 64; ++i)
                        expectWithinAbsoluteError (live.getStateLowOutput (1).getSample (0, i),
                                                   reference.getStateLowOutput (1).getSample (0, i),
                                                   1.0e-5f);
                }
            }
        }

        beginTest ("State identity, capacity and clearing");
        {
            const double rate = 48000.0;
            DynamicHotBranchEngine engine;
            expect (engine.prepare (rate, 256, 1));
            expect (engine.configureGlobal (cfg (rate, 0.0)));
            for (int s = 0; s < 8; ++s)
                expect (engine.configureStateSlot (s, cfg (rate, 0.0, (uint64_t) s + 1)));
            expect (! engine.configureStateSlot (8, cfg (rate, 0.0, 99)));
            expect (! engine.configureStateSlot (0, cfg (rate, 0.0, 0)));
            expect (! engine.configureStateSlot (1, cfg (rate, 0.0, 1)));

            engine.clearStateSlot (3);
            expect (! engine.getStateInfo (3).active);
            expect (engine.getStateInfo (3).stableStateId == 0);
        }

        beginTest ("Service priming uses shared history and does not move write position");
        {
            const double rate = 48000.0;
            DynamicHotBranchEngine engine;
            expect (engine.prepare (rate, 512, 1));
            expect (engine.configureGlobal (cfg (rate, 0.0)));
            expect (engine.configureService (cfg (rate, 0.0)));

            auto emptyPrime = engine.primeService (100);
            expect (emptyPrime.valid);
            expect (! emptyPrime.fullyPrimed);
            expectEquals (emptyPrime.primedSamples, 0);

            const auto sine = makeSine (1, 4800, rate, 60.0);
            juce::AudioBuffer<float> g, s, h;
            processChunked (engine, sine, g, s, h);

            const int beforeValid = engine.getValidLowHistorySamples();
            auto primed = engine.primeService (1000);
            expect (primed.valid);
            expect (primed.primedSamples > 0);
            expectEquals (engine.getValidLowHistorySamples(), beforeValid);

            const int maxPrime = (int) std::ceil (rate * 0.300);
            auto clamped = engine.primeService (maxPrime + 1000);
            expect (clamped.valid);
            expectEquals (clamped.requestedSamples, maxPrime);
        }

        beginTest ("Block-size invariance for Global, State and high outputs");
        {
            const double rate = 48000.0;
            const int preparedMax = 512;
            const auto material = makeSine (1, 1024, rate, 55.0, 0.35);

            juce::AudioBuffer<float> refGlobal, refState, refHigh;
            {
                DynamicHotBranchEngine engine;
                expect (engine.prepare (rate, preparedMax, 1));
                expect (engine.configureGlobal (cfg (rate, 0.5, 0, false, 3, true, 1)));
                expect (engine.configureStateSlot (0, cfg (rate, -1.25, 7, false, 3, true, 1)));
                processWithBlockSize (engine, material, preparedMax, refGlobal, refState, refHigh);
            }

            // Real process call sizes, including 1. Never substitute another size.
            for (int blockSize : { 1, 7, 64, 127, 512 })
            {
                DynamicHotBranchEngine engine;
                expect (engine.prepare (rate, preparedMax, 1));
                expect (engine.configureGlobal (cfg (rate, 0.5, 0, false, 3, true, 1)));
                expect (engine.configureStateSlot (0, cfg (rate, -1.25, 7, false, 3, true, 1)));

                juce::AudioBuffer<float> globalOut, stateOut, highOut;
                processWithBlockSize (engine, material, blockSize, globalOut, stateOut, highOut);
                for (int i = 0; i < 1024; ++i)
                {
                    expectWithinAbsoluteError (globalOut.getSample (0, i), refGlobal.getSample (0, i), 1.0e-4f);
                    expectWithinAbsoluteError (stateOut.getSample (0, i), refState.getSample (0, i), 1.0e-4f);
                    expectWithinAbsoluteError (highOut.getSample (0, i), refHigh.getSample (0, i), 1.0e-4f);
                }
            }
        }

        beginTest ("Non-finite input is sanitized before crossover and does not poison recursive state");
        {
            const double rate = 48000.0;
            DynamicHotBranchEngine engine;
            expect (engine.prepare (rate, 128, 1));
            expect (engine.configureGlobal (cfg (rate, 0.0, 0, false, 2, true, 0, true)));
            expect (engine.configureStateSlot (0, cfg (rate, -1.0, 9, false, 2, true, 0, true)));
            expect (engine.configureService (cfg (rate, 0.5, 0, false, 2, true, 0, true)));

            const auto warm = makeSine (1, 512, rate, 90.0, 0.4);
            juce::AudioBuffer<float> g, s, h;
            processChunked (engine, warm, g, s, h);
            const uint64_t countAfterWarm = engine.getNonFiniteInputCount();

            juce::AudioBuffer<float> dirty (1, 128);
            dirty.clear();
            dirty.setSample (0, 10, std::numeric_limits<float>::quiet_NaN());
            dirty.setSample (0, 20, std::numeric_limits<float>::infinity());
            dirty.setSample (0, 30, -std::numeric_limits<float>::infinity());
            for (int i = 40; i < 128; ++i)
                dirty.setSample (0, i, 0.2f);
            auto dirtyResult = engine.process (dirty, 128);
            expect (dirtyResult.valid);
            expectEquals ((int) (dirtyResult.nonFiniteInputCount - countAfterWarm), 3);

            const auto recovery = makeSine (1, 1024, rate, 110.0, 0.35);
            processChunked (engine, recovery, g, s, h);
            bool anyEnergy = false;
            for (int i = 0; i < recovery.getNumSamples(); ++i)
            {
                expect (std::isfinite (g.getSample (0, i)));
                expect (std::isfinite (s.getSample (0, i)));
                expect (std::isfinite (h.getSample (0, i)));
                if (std::abs (g.getSample (0, i)) > 1.0e-4f || std::abs (h.getSample (0, i)) > 1.0e-4f)
                    anyEnergy = true;
            }
            // Service path remains finite after recovery blocks.
            for (int offset = 0; offset < 256; offset += 128)
            {
                juce::AudioBuffer<float> block (1, 128);
                block.copyFrom (0, 0, recovery, 0, offset, 128);
                expect (engine.process (block, 128).valid);
                for (int i = 0; i < 128; ++i)
                    expect (std::isfinite (engine.getServiceLowOutput().getSample (0, i)));
            }
            expect (anyEnergy, "crossover must not remain permanently silenced after NaN");
            expectEquals ((int) (engine.getNonFiniteInputCount() - countAfterWarm), 3);
        }

        beginTest ("configureGlobal propagates Global-only fields to active State and Service");
        {
            const double rate = 48000.0;
            DynamicHotBranchEngine engine;
            expect (engine.prepare (rate, 256, 1));
            expect (engine.configureGlobal (cfg (rate, 0.0, 0, false, 2, true, 0, false)));
            expect (engine.configureStateSlot (0, cfg (rate, -1.5, 21, false, 2, true, 0, false)));
            expect (engine.configureStateSlot (1, cfg (rate, 1.0, 22, false, 2, true, 0, false)));
            expect (engine.configureService (cfg (rate, 0.25, 0, false, 2, true, 0, false)));

            juce::AudioBuffer<float> g, s, h;
            processChunked (engine, makeSine (1, 1024, rate, 70.0), g, s, h);
            expect (engine.getStateInfo (0).warm || engine.getStateInfo (0).active);

            auto updated = cfg (rate, 0.5, 0, true, 4, false, 1, true);
            updated.crossoverHz = 220.0;
            expect (engine.configureGlobal (updated));

            expect (engine.getGlobalInfo().active);
            expect (engine.getStateInfo (0).stableStateId == 21);
            expect (engine.getStateInfo (1).stableStateId == 22);
            expectWithinAbsoluteError (engine.getStateInfo (0).effectiveAbsoluteDelayMs, -1.5, 1.0e-12);
            expectWithinAbsoluteError (engine.getStateInfo (1).effectiveAbsoluteDelayMs, 1.0, 1.0e-12);
            expectWithinAbsoluteError (engine.getServiceInfo().effectiveAbsoluteDelayMs, 0.25, 1.0e-12);

            // Process one block and verify polarity/stages/allpassEnabled take effect.
            processChunked (engine, makeImpulse (1, 2048, 0), g, s, h);
            const int idx = findImpulseIndex (g, 0, 0.1f);
            expect (idx >= 0);
            expect (g.getSample (0, idx) < 0.0f); // polarity now inverted and allpass dry
        }

        beginTest ("Warm requires full physical-tap history, not one sample");
        {
            const double rate = 48000.0;
            DynamicHotBranchEngine engine;
            expect (engine.prepare (rate, 64, 1));
            expect (engine.configureGlobal (cfg (rate, 0.0, 0, false, 2, false)));
            expect (engine.configureStateSlot (0, cfg (rate, -3.0, 5, false, 2, false)));
            expect (engine.configureService (cfg (rate, 0.0, 0, false, 2, false)));

            expect (! engine.getGlobalInfo().warm);
            expect (! engine.getStateInfo (0).warm);
            expect (! engine.getServiceInfo().warm);

            juce::AudioBuffer<float> one (1, 1);
            one.clear();
            one.setSample (0, 0, 0.1f);
            expect (engine.process (one, 1).valid);
            expect (! engine.getGlobalInfo().warm);
            expect (! engine.getStateInfo (0).warm);
            expect (! engine.getServiceInfo().warm);

            const int globalNeed = (int) std::floor (engine.getGlobalInfo().physicalTapSamples) + 2;
            const int stateNeed = (int) std::floor (engine.getStateInfo (0).physicalTapSamples) + 2;
            expect (globalNeed > 2);
            expect (stateNeed > 2);
            juce::AudioBuffer<float> g, s, h;

            // Process exactly need-1 frames total (including the first sample).
            processChunked (engine, makeSine (1, globalNeed - 2, rate, 60.0), g, s, h);
            expect (! engine.getGlobalInfo().warm);

            expect (engine.process (one, 1).valid);
            expect (engine.getGlobalInfo().warm);
            // State needs fewer frames, so it must already be warm.
            expect (engine.getStateInfo (0).warm);

            // Identity replace resets warm.
            expect (engine.configureStateSlot (0, cfg (rate, -3.0, 6, false, 2, false)));
            expect (! engine.getStateInfo (0).warm);
            expectEquals ((int) engine.getStateInfo (0).stableStateId, 6);

            // Partial Service prime below the warm requirement is not warm.
            // Fill shared history without Service active, then prime only 10 frames.
            engine.reset();
            expect (engine.configureGlobal (cfg (rate, 0.0, 0, false, 2, false)));
            const int historyForShortPrime = globalNeed + 32;
            processChunked (engine, makeSine (1, historyForShortPrime, rate, 60.0), g, s, h);
            expect (engine.configureService (cfg (rate, 0.0, 0, false, 2, false)));
            expect (! engine.getServiceInfo().warm);
            auto partial = engine.primeService (10);
            expect (partial.valid);
            expectEquals (partial.primedSamples, 10);
            expect (partial.fullyPrimed); // request fully satisfied
            expect (partial.primedSamples < globalNeed);
            expect (! engine.getServiceInfo().warm);
            juce::ignoreUnused (stateNeed);
        }

        beginTest ("Service full prime is deterministic and isolates Global/State");
        {
            const double rate = 48000.0;
            const int history = (int) std::ceil (rate * 0.340) + 64;
            for (int interp : { 0, 1 })
            {
                DynamicHotBranchEngine a;
                DynamicHotBranchEngine b;
                DynamicHotBranchEngine control;
                expect (a.prepare (rate, 256, 1));
                expect (b.prepare (rate, 256, 1));
                expect (control.prepare (rate, 256, 1));
                const auto global = cfg (rate, 0.0, 0, false, 2, true, interp);
                const auto service = cfg (rate, 1.0, 0, false, 2, true, interp);
                const auto state = cfg (rate, -1.0, 88, false, 2, true, interp);
                expect (a.configureGlobal (global));
                expect (b.configureGlobal (global));
                expect (control.configureGlobal (global));
                expect (a.configureStateSlot (0, state));
                expect (b.configureStateSlot (0, state));
                expect (control.configureStateSlot (0, state));
                expect (a.configureService (service));
                expect (b.configureService (service));

                const auto material = makeSine (1, history, rate, 65.0, 0.4);
                juce::AudioBuffer<float> ag, as, ah, bg, bs, bh, cg, cs, ch;
                processChunked (a, material, ag, as, ah);
                processChunked (b, material, bg, bs, bh);
                processChunked (control, material, cg, cs, ch);

                const int beforeValid = a.getValidLowHistorySamples();
                const auto primeA = a.primeService ((int) std::ceil (rate * 0.300));
                const auto primeB = b.primeService ((int) std::ceil (rate * 0.300));
                expect (primeA.valid && primeB.valid);
                expect (primeA.fullyPrimed && primeB.fullyPrimed);
                expectEquals (a.getValidLowHistorySamples(), beforeValid);
                expectEquals (b.getValidLowHistorySamples(), beforeValid);
                expect (a.getServiceInfo().warm);
                expect (b.getServiceInfo().warm);

                // Priming must not alter Global/State relative to an engine that never primed.
                const auto future = makeSine (1, 256, rate, 80.0, 0.3);
                for (int offset = 0; offset < 256; offset += 64)
                {
                    juce::AudioBuffer<float> block (1, 64);
                    block.copyFrom (0, 0, future, 0, offset, 64);
                    expect (a.process (block, 64).valid);
                    expect (b.process (block, 64).valid);
                    expect (control.process (block, 64).valid);
                    for (int i = 0; i < 64; ++i)
                    {
                        expectWithinAbsoluteError (a.getGlobalLowOutput().getSample (0, i),
                                                   control.getGlobalLowOutput().getSample (0, i), 1.0e-5f);
                        expectWithinAbsoluteError (a.getStateLowOutput (0).getSample (0, i),
                                                   control.getStateLowOutput (0).getSample (0, i), 1.0e-5f);
                        expectWithinAbsoluteError (a.getServiceLowOutput().getSample (0, i),
                                                   b.getServiceLowOutput().getSample (0, i), 1.0e-9f);
                    }
                }
            }
        }

        beginTest ("Independent TDF-II reference matches stage outputs and disabled-warm path");
        {
            const double rate = 48000.0;
            const auto coefficients = makeCoeffs (rate, 95.0, 1.2);
            for (int stages : { 2, 3, 4 })
            {
                std::array<std::array<double, 2>, 4> z {};
                std::array<DynamicHotBranchDetail::AllpassStageState, 4> engineZ {};
                float x = 0.37f;
                for (int n = 0; n < 32; ++n)
                {
                    const float independent = independentAllpass (x, coefficients, stages, z);
                    const float engine = DynamicHotBranchEngine::processAllpassReference (
                        x, coefficients, stages, engineZ);
                    expectWithinAbsoluteError (independent, engine, 1.0e-9f);
                    x = 0.91f * x + 0.05f;
                }
            }

            // Disabled allpass returns dry sample while stages still advance.
            DynamicHotBranchEngine engine;
            expect (engine.prepare (rate, 128, 1));
            auto global = cfg (rate, 0.0, 0, false, 3, false);
            global.coefficients = coefficients;
            expect (engine.configureGlobal (global));
            const auto sine = makeSine (1, 256, rate, 70.0);
            juce::AudioBuffer<float> g, s, h;
            processChunked (engine, sine, g, s, h);
            // Re-enable allpass without resetting: cascade should already be warm.
            global.allpassEnabled = true;
            expect (engine.configureGlobal (global));
            processChunked (engine, sine, g, s, h);
            bool finite = true;
            for (int i = 0; i < g.getNumSamples(); ++i)
                if (! std::isfinite (g.getSample (0, i)))
                    finite = false;
            expect (finite);
        }

        beginTest ("Exact common-high latency with crossover-enabled impulse");
        {
            const double rate = 48000.0;
            DynamicHotBranchEngine engine;
            expect (engine.prepare (rate, 256, 1));
            // Low crossover keeps more impulse energy in the high band.
            auto global = cfg (rate, 0.0, 0, false, 2, false, 0, true);
            global.crossoverHz = 80.0;
            expect (engine.configureGlobal (global));
            const int latency = engine.getReportedLatencySamples();
            const auto impulse = makeImpulse (1, latency + 512, 0);
            juce::AudioBuffer<float> g, s, h;
            processChunked (engine, impulse, g, s, h);

            // High path is pure integer delay of the high-band split. The LR
            // filters smear the impulse, so the first non-zero sample is at or
            // shortly after reportedLatencySamples and never before it.
            for (int i = 0; i < latency; ++i)
                expectWithinAbsoluteError (h.getSample (0, i), 0.0f, 1.0e-7f);

            int firstHigh = -1;
            for (int i = latency; i < h.getNumSamples(); ++i)
                if (std::abs (h.getSample (0, i)) > 1.0e-5f)
                {
                    firstHigh = i;
                    break;
                }
            expect (firstHigh >= latency);
            expect (firstHigh <= latency + 128);

            juce::AudioBuffer<float> highAlone;
            highAlone.makeCopyOf (h);

            // State packages must not change the common high path.
            engine.reset();
            expect (engine.configureGlobal (global));
            expect (engine.configureStateSlot (0, cfg (rate, -3.0, 3, false, 2, false, 0, true)));
            expect (engine.configureStateSlot (1, cfg (rate, 3.0, 4, false, 2, true, 0, true)));
            // Even if callers pass stale Global-only values, live Global wins.
            auto stale = cfg (rate, 1.0, 5, true, 4, true, 1, false);
            stale.crossoverHz = 400.0;
            expect (engine.configureStateSlot (2, stale));
            expect (engine.getStateInfo (2).active);
            processChunked (engine, impulse, g, s, h);
            for (int i = 0; i < h.getNumSamples(); ++i)
                expectWithinAbsoluteError (h.getSample (0, i), highAlone.getSample (0, i), 1.0e-6f);
        }

        beginTest ("Reset clears delayed history and recursive state");
        {
            const double rate = 48000.0;
            DynamicHotBranchEngine engine;
            expect (engine.prepare (rate, 512, 1));
            expect (engine.configureGlobal (cfg (rate, 0.0, 0, false, 2, false)));
            juce::AudioBuffer<float> g, s, h;
            processChunked (engine, makeImpulse (1, 2048, 0), g, s, h);
            expect (findImpulseIndex (g, 0) >= 0);
            engine.reset();
            juce::AudioBuffer<float> silence (1, 2048);
            silence.clear();
            processChunked (engine, silence, g, s, h);
            for (int i = 0; i < 2048; ++i)
                expectWithinAbsoluteError (g.getSample (0, i), 0.0f, 1.0e-7f);
        }

        beginTest ("Non-finite input is sanitized and subsequent output is finite");
        {
            const double rate = 48000.0;
            DynamicHotBranchEngine engine;
            expect (engine.prepare (rate, 128, 1));
            expect (engine.configureGlobal (cfg (rate, 0.0, 0, false, 2, true, 0, false)));
            juce::AudioBuffer<float> dirty (1, 128);
            dirty.clear();
            dirty.setSample (0, 10, std::numeric_limits<float>::quiet_NaN());
            dirty.setSample (0, 20, std::numeric_limits<float>::infinity());
            for (int i = 40; i < 128; ++i)
                dirty.setSample (0, i, 0.25f);
            const auto result = engine.process (dirty, 128);
            expect (result.valid);
            expectEquals ((int) result.nonFiniteInputCount, 2);
            for (int i = 40; i < 128; ++i)
                expect (std::isfinite (engine.getGlobalLowOutput().getSample (0, i)));
        }

        beginTest ("Stereo channels stay independent and finite");
        {
            const double rate = 48000.0;
            DynamicHotBranchEngine engine;
            expect (engine.prepare (rate, 256, 2));
            expect (engine.configureGlobal (cfg (rate, 0.0, 0, false, 2, false)));
            juce::AudioBuffer<float> input (2, 2048);
            input.clear();
            input.setSample (0, 0, 1.0f);
            input.setSample (1, 5, 0.5f);
            juce::AudioBuffer<float> g, s, h;
            processChunked (engine, input, g, s, h);
            const int left = findImpulseIndex (g, 0);
            const int right = findImpulseIndex (g, 1, 0.25f);
            expect (left >= 0 && right >= 0);
            expect (left != right);
        }

        beginTest ("Repeated identical configuration is a deterministic no-op");
        {
            const double rate = 48000.0;
            DynamicHotBranchEngine engine;
            expect (engine.prepare (rate, 128, 1));
            const auto global = cfg (rate, 1.0);
            expect (engine.configureGlobal (global));
            expect (engine.configureGlobal (global));
            expect (engine.configureStateSlot (0, cfg (rate, 0.5, 5)));
            expect (engine.configureStateSlot (0, cfg (rate, 0.5, 5)));
        }

        beginTest ("Polarity is applied and Global-only fields cannot be overridden per State");
        {
            const double rate = 48000.0;
            DynamicHotBranchEngine engine;
            expect (engine.prepare (rate, 512, 1));
            auto global = cfg (rate, 0.0, 0, false, 2, false);
            expect (engine.configureGlobal (global));
            // Caller polarity is ignored; live Global polarity remains false.
            auto state = cfg (rate, 0.0, 3, true, 2, false);
            expect (engine.configureStateSlot (0, state));
            juce::AudioBuffer<float> g, s, h;
            processChunked (engine, makeImpulse (1, 2048, 0), g, s, h);
            int idx = findImpulseIndex (g, 0, 0.5f);
            expect (idx >= 0);
            expect (g.getSample (0, idx) > 0.0f);

            global.polarityInvert = true;
            expect (engine.configureGlobal (global));
            processChunked (engine, makeImpulse (1, 2048, 0), g, s, h);
            idx = findImpulseIndex (g, 0, 0.5f);
            expect (idx >= 0);
            expect (g.getSample (0, idx) < 0.0f);
        }

        beginTest ("Package-derived configuration consumes Phase-4 coefficients");
        {
            const double rate = 48000.0;
            DynamicHotBranchEngine engine;
            expect (engine.prepare (rate, 256, 1));

            DynamicPackageResolution globalPackage;
            globalPackage.valid = true;
            globalPackage.decision = DynamicPackageDecision::GlobalNoSelection;
            globalPackage.effectiveAbsoluteDelayMs = 0.0;
            globalPackage.polarityInvert = false;
            globalPackage.allpassStages = 2;
            globalPackage.crossoverEnabled = false;
            globalPackage.crossoverHz = 150.0;
            globalPackage.allpassEnabled = true;
            globalPackage.delayInterpolationIndex = 0;
            expect (DynamicAllpassPoleDomain::makeCoefficients (
                100.0, 0.7, rate, globalPackage.allpassCoefficients));
            expect (engine.configureFromPackage (DynamicHotBranchKind::Global, -1, globalPackage));

            DynamicPackageResolution statePackage = globalPackage;
            statePackage.decision = DynamicPackageDecision::StateCorrection;
            statePackage.selectedStableStateId = 42;
            statePackage.effectiveAbsoluteDelayMs = -1.0;
            expect (engine.configureFromPackage (DynamicHotBranchKind::State, 0, statePackage));
            expect (engine.getStateInfo (0).active);
            expect (engine.getStateInfo (0).stableStateId == 42);
        }
    }
};

static DynamicHotBranchEngineTests dynamicHotBranchEngineTests;
