#include "TestCommon.h"

#include "DynamicHotBranchEngine.h"

#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

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

    void renderFull (DynamicHotBranchEngine& engine,
                     const juce::AudioBuffer<float>& material,
                     int blockSize,
                     juce::AudioBuffer<float>& globalOut,
                     juce::AudioBuffer<float>& stateOut,
                     juce::AudioBuffer<float>& highOut)
    {
        const int total = material.getNumSamples();
        globalOut.setSize (1, total);
        stateOut.setSize (1, total);
        highOut.setSize (1, total);
        int written = 0;
        while (written < total)
        {
            const int n = juce::jmin (blockSize, total - written);
            juce::AudioBuffer<float> block (1, n);
            block.copyFrom (0, 0, material, 0, written, n);
            expect (engine.process (block, n).valid);
            globalOut.copyFrom (0, written, engine.getGlobalLowOutput(), 0, 0, n);
            stateOut.copyFrom (0, written, engine.getStateLowOutput (0), 0, 0, n);
            highOut.copyFrom (0, written, engine.getHighOutput(), 0, 0, n);
            written += n;
        }
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
                const int minCapacity = (int) std::ceil (rate * 0.340) + 8;
                expect (engine.getLowHistoryCapacity() >= minCapacity);
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
            expect (engine.prepare (rate, 2048, 1));
            // Keep allpass dry so the impulse peak remains a clean unit sample.
            expect (engine.configureGlobal (cfg (rate, 0.0, 0, false, 2, false)));
            expect (engine.configureStateSlot (0, cfg (rate, -3.0, 11, false, 2, false)));
            expect (engine.configureStateSlot (1, cfg (rate, 3.0, 22, false, 2, false)));

            const int total = 3000;
            const auto impulse = makeImpulse (1, total, 0);
            expect (engine.process (impulse, total).valid);

            const int latency = engine.getReportedLatencySamples();
            const int stateATap = (int) std::lround ((double) latency + (-3.0) * rate / 1000.0);
            const int stateBTap = (int) std::lround ((double) latency + 3.0 * rate / 1000.0);

            expectEquals (findImpulseIndex (engine.getGlobalLowOutput(), 0), latency);
            expectEquals (findImpulseIndex (engine.getStateLowOutput (0), 0), stateATap);
            expectEquals (findImpulseIndex (engine.getStateLowOutput (1), 0), stateBTap);
            expectEquals (findImpulseIndex (engine.getHighOutput(), 0), latency);
        }

        beginTest ("Minimum and maximum physical taps and rejection beyond capacity");
        {
            const double rate = 48000.0;
            DynamicHotBranchEngine engine;
            expect (engine.prepare (rate, 4096, 1));
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
            expect (engine.process (makeImpulse (1, 512, 0), 512).valid);
            bool highSilent = true;
            for (int i = 0; i < 512; ++i)
                if (std::abs (engine.getHighOutput().getSample (0, i)) > 1.0e-7f)
                    highSilent = false;
            expect (highSilent);
            expectEquals (findImpulseIndex (engine.getGlobalLowOutput(), 0),
                          engine.getReportedLatencySamples());
        }

        beginTest ("High path is common, exact PDC, unaffected by State count");
        {
            const double rate = 48000.0;
            DynamicHotBranchEngine engine;
            expect (engine.prepare (rate, 2048, 1));
            // Crossover on so high-band energy exists; PDC is pure integer delay.
            expect (engine.configureGlobal (cfg (rate, 0.0, 0, false, 2, false, 0, true)));
            const auto sine = makeSine (1, 2048, rate, 2000.0, 0.6);
            expect (engine.process (sine, 2048).valid);
            const int latency = engine.getReportedLatencySamples();
            // First latency samples of high must be the cleared history (zero).
            for (int i = 0; i < latency; ++i)
                expectWithinAbsoluteError (engine.getHighOutput().getSample (0, i), 0.0f, 1.0e-7f);
            bool highHasEnergy = false;
            for (int i = latency; i < 2048; ++i)
                if (std::abs (engine.getHighOutput().getSample (0, i)) > 1.0e-4f)
                    highHasEnergy = true;
            expect (highHasEnergy);

            juce::AudioBuffer<float> highWithoutStates (1, 2048);
            highWithoutStates.makeCopyOf (engine.getHighOutput());

            engine.reset();
            expect (engine.configureGlobal (cfg (rate, 0.0, 0, false, 2, false, 0, true)));
            for (int s = 0; s < 8; ++s)
                expect (engine.configureStateSlot (s, cfg (rate, 0.0, (uint64_t) s + 1, false, 2, false, 0, true)));
            expect (engine.process (sine, 2048).valid);
            for (int i = 0; i < 2048; ++i)
                expectWithinAbsoluteError (engine.getHighOutput().getSample (0, i),
                                           highWithoutStates.getSample (0, i), 1.0e-6f);
        }

        beginTest ("Hot continuity: later observation matches continuous independent processing");
        {
            const double rate = 48000.0;
            DynamicHotBranchEngine live;
            DynamicHotBranchEngine reference;
            expect (live.prepare (rate, 256, 1));
            expect (reference.prepare (rate, 256, 1));

            const auto global = cfg (rate, 0.0, 0, false, 2, true, 0);
            const auto stateB = cfg (rate, 1.5, 99, false, 2, true, 1);
            expect (live.configureGlobal (global));
            expect (live.configureStateSlot (0, cfg (rate, -1.0, 11)));
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
            for (int offset = 0; offset < 4800; offset += 512)
            {
                const int n = juce::jmin (512, 4800 - offset);
                juce::AudioBuffer<float> block (1, n);
                block.copyFrom (0, 0, sine, 0, offset, n);
                expect (engine.process (block, n).valid);
            }

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
            const auto material = makeSine (1, 1024, rate, 55.0, 0.35);

            juce::AudioBuffer<float> refGlobal, refState, refHigh;
            {
                DynamicHotBranchEngine engine;
                expect (engine.prepare (rate, 1024, 1));
                expect (engine.configureGlobal (cfg (rate, 0.5)));
                expect (engine.configureStateSlot (0, cfg (rate, -1.25, 7, false, 3, true, 1)));
                renderFull (engine, material, 64, refGlobal, refState, refHigh);
            }

            for (int blockSize : { 1, 7, 127, 512 })
            {
                DynamicHotBranchEngine engine;
                expect (engine.prepare (rate, 1024, 1));
                expect (engine.configureGlobal (cfg (rate, 0.5)));
                expect (engine.configureStateSlot (0, cfg (rate, -1.25, 7, false, 3, true, 1)));
                juce::AudioBuffer<float> globalOut, stateOut, highOut;
                renderFull (engine, material, blockSize, globalOut, stateOut, highOut);
                for (int i = 0; i < 1024; ++i)
                {
                    expectWithinAbsoluteError (globalOut.getSample (0, i), refGlobal.getSample (0, i), 1.0e-4f);
                    expectWithinAbsoluteError (stateOut.getSample (0, i), refState.getSample (0, i), 1.0e-4f);
                    expectWithinAbsoluteError (highOut.getSample (0, i), refHigh.getSample (0, i), 1.0e-4f);
                }
            }
        }

        beginTest ("Reset clears delayed history and recursive state");
        {
            const double rate = 48000.0;
            DynamicHotBranchEngine engine;
            expect (engine.prepare (rate, 512, 1));
            expect (engine.configureGlobal (cfg (rate, 0.0)));
            expect (engine.process (makeImpulse (1, 512, 0), 512).valid);
            expect (findImpulseIndex (engine.getGlobalLowOutput(), 0) >= 0);
            engine.reset();
            juce::AudioBuffer<float> silence (1, 512);
            silence.clear();
            expect (engine.process (silence, 512).valid);
            for (int i = 0; i < 512; ++i)
                expectWithinAbsoluteError (engine.getGlobalLowOutput().getSample (0, i), 0.0f, 1.0e-7f);
        }

        beginTest ("Non-finite input is sanitized and subsequent output is finite");
        {
            const double rate = 48000.0;
            DynamicHotBranchEngine engine;
            expect (engine.prepare (rate, 128, 1));
            expect (engine.configureGlobal (cfg (rate, 0.0)));
            juce::AudioBuffer<float> dirty (1, 128);
            dirty.clear();
            dirty.setSample (0, 10, std::numeric_limits<float>::quiet_NaN());
            dirty.setSample (0, 20, std::numeric_limits<float>::infinity());
            for (int i = 40; i < 128; ++i)
                dirty.setSample (0, i, 0.25f);
            const auto result = engine.process (dirty, 128);
            expect (result.valid);
            expect (result.nonFiniteInputCount >= 2);
            for (int i = 40; i < 128; ++i)
                expect (std::isfinite (engine.getGlobalLowOutput().getSample (0, i)));
        }

        beginTest ("Stereo channels stay independent and finite");
        {
            const double rate = 48000.0;
            DynamicHotBranchEngine engine;
            expect (engine.prepare (rate, 256, 2));
            expect (engine.configureGlobal (cfg (rate, 0.0, 0, false, 2, false)));
            juce::AudioBuffer<float> input (2, 256);
            input.clear();
            input.setSample (0, 0, 1.0f);
            input.setSample (1, 5, 0.5f);
            expect (engine.process (input, 256).valid);
            const int left = findImpulseIndex (engine.getGlobalLowOutput(), 0);
            const int right = findImpulseIndex (engine.getGlobalLowOutput(), 1, 0.25f);
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
            expect (engine.prepare (rate, 1024, 1));
            auto global = cfg (rate, 0.0, 0, false, 2, false);
            expect (engine.configureGlobal (global));
            auto state = cfg (rate, 0.0, 3, true, 2, false);
            expect (! engine.configureStateSlot (0, state));

            global.polarityInvert = true;
            expect (engine.configureGlobal (global));
            state.polarityInvert = true;
            expect (engine.configureStateSlot (0, state));
            expect (engine.process (makeImpulse (1, 1024, 0), 1024).valid);
            const int idx = findImpulseIndex (engine.getGlobalLowOutput(), 0, 0.5f);
            expect (idx >= 0);
            expect (engine.getGlobalLowOutput().getSample (0, idx) < 0.0f);
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
