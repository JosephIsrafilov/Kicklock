#include "TestCommon.h"

#include "DynamicReleaseFixture.h"

#include "dsp/DynamicProductionRuntime.h"

#include <chrono>

namespace
{
    constexpr double kMedianRealtimeRatioLimit = 0.75;
    constexpr double kUpperRealtimeRatioLimit = 1.00;
    constexpr int kWarmupPasses = 2;
    constexpr int kMeasuredPasses = 7;

    DynamicFingerprintPrototype performancePrototype (float seed)
    {
        DynamicFingerprintPrototype result;
        result.valid = true;
        result.featureCount = DynamicFingerprintContract::kFeatureCount;
        for (int feature = 0; feature < DynamicFingerprintContract::kFeatureCount; ++feature)
            result.features[(size_t) feature] = std::clamp (seed + 0.004f * (float) feature, -0.9f, 0.9f);
        return result;
    }

    DynamicStateMap performanceMap (int stateCount, bool worstCase)
    {
        DynamicStateMap map;
        map.valid = true;
        map.globalBase.globalBaseDelayMs = 0.0f;
        map.globalBase.crossoverEnabled = true;
        map.globalBase.crossoverHz = 150.0f;
        map.globalBase.allpassEnabled = true;
        map.globalBase.globalAllpassFreqHz = 100.0f;
        map.globalBase.globalAllpassQ = 0.8f;
        map.globalBase.allpassStages = worstCase ? 4 : 2;
        map.globalBase.learnedSampleRate = worstCase ? 192000.0 : 48000.0;
        map.calibration = { true, 0.2f, 0.02f };
        map.nextStateId = 100;
        for (int slot = 0; slot < stateCount; ++slot)
        {
            auto& state = map.states[(size_t) slot];
            state.occupied = true;
            state.stableStateId = (uint64_t) (slot + 1);
            state.fingerprint = performancePrototype (-0.8f + 0.2f * (float) slot);
            state.hasLearnedPackage = true;
            state.learnedPackage = { -1.5f + 0.3f * (float) slot, 70.0f + 12.0f * (float) slot, 0.8f };
            state.origin = DynamicStateOrigin::Auto;
            state.evidence = DynamicStateEvidence::Stable;
            state.hitCount = 5;
            state.repeatability = 0.9f;
            state.ambiguity = 0.05f;
        }
        map.nextStateId = (uint64_t) stateCount + 1;
        return map;
    }

    bool processFixture (DynamicProductionRuntime& runtime, const DynamicReleaseFixture& fixture,
                         int blockSize, double strength)
    {
        juce::AudioBuffer<float> input (1, blockSize);
        juce::AudioBuffer<float> output (1, blockSize);
        for (int offset = 0; offset < (int) fixture.bass.size(); offset += blockSize)
        {
            const int count = std::min (blockSize, (int) fixture.bass.size() - offset);
            for (int sample = 0; sample < count; ++sample)
                input.setSample (0, sample, fixture.bass[(size_t) (offset + sample)]);
            if (! runtime.process (input, fixture.bass.data() + offset, fixture.kick.data() + offset,
                                  true, strength, output, count))
                return false;
            for (int sample = 0; sample < count; ++sample)
                if (! std::isfinite (output.getSample (0, sample)))
                    return false;
        }
        return true;
    }

    bool processStaticFixture (KickLockAudioProcessor& processor, const DynamicReleaseFixture& fixture,
                               int blockSize, juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
    {
        const int channels = buffer.getNumChannels();
        for (int offset = 0; offset < (int) fixture.bass.size(); offset += blockSize)
        {
            const int count = std::min (blockSize, (int) fixture.bass.size() - offset);
            buffer.clear();
            for (int sample = 0; sample < count; ++sample)
            {
                const float bass = fixture.bass[(size_t) (offset + sample)];
                const float kick = fixture.kick[(size_t) (offset + sample)];
                buffer.setSample (0, sample, bass);
                if (channels > 1) buffer.setSample (1, sample, bass);
                if (channels > 2) buffer.setSample (2, sample, kick);
                if (channels > 3) buffer.setSample (3, sample, kick);
            }
            processor.processBlock (buffer, midi);
            for (int sample = 0; sample < count; ++sample)
                if (! std::isfinite (buffer.getSample (0, sample)))
                    return false;
        }
        return true;
    }

    struct PerformanceSample
    {
        double representedSeconds = 0.0;
        double medianSeconds = 0.0;
        double upperSeconds = 0.0;
        bool processed = true;
    };

    template <typename Render>
    PerformanceSample measure (Render&& render, double representedSeconds)
    {
        bool processed = true;
        for (int warmup = 0; warmup < kWarmupPasses; ++warmup)
            processed = render() && processed;
        std::array<double, kMeasuredPasses> seconds {};
        for (int pass = 0; pass < kMeasuredPasses; ++pass)
        {
            const auto start = std::chrono::steady_clock::now();
            processed = render() && processed;
            seconds[(size_t) pass] = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
        }
        std::sort (seconds.begin(), seconds.end());
        return { representedSeconds, seconds[seconds.size() / 2], seconds[(seconds.size() * 6) / 7], processed };
    }

    bool skipTimedAssertions() noexcept
    {
        return juce::SystemStats::getEnvironmentVariable ("KICKLOCK_SKIP_TIMED_ASSERTS", {}) == "1";
    }
}

class DynamicPerformanceGateTests : public juce::UnitTest
{
public:
    DynamicPerformanceGateTests() : juce::UnitTest ("DynamicPerformanceGates", "Performance") {}

    void runTest() override
    {
        beginTest ("Release runtime batches retain real-time headroom across Dynamic scenarios");
        if (skipTimedAssertions())
        {
            logMessage ("KICKLOCK_SKIP_TIMED_ASSERTS=1: wall-clock Performance assertions skipped; finiteness still runs.");
        }

       #if ! defined (NDEBUG)
        logMessage ("Debug build: timing thresholds are skipped; Release builds enforce them.");
       #endif

        const auto logAndAssert = [this] (const juce::String& scenarioLabel, double rate, int block,
                                          const PerformanceSample& sample)
        {
            const double medianRatio = sample.medianSeconds / sample.representedSeconds;
            const double upperRatio = sample.upperSeconds / sample.representedSeconds;
            logMessage (scenarioLabel + " sr=" + juce::String (rate, 0)
                        + " block=" + juce::String (block)
                        + " audio=" + juce::String (sample.representedSeconds, 3)
                        + " median=" + juce::String (sample.medianSeconds, 4)
                        + " upper=" + juce::String (sample.upperSeconds, 4)
                        + " medianRatio=" + juce::String (medianRatio, 4)
                        + " upperRatio=" + juce::String (upperRatio, 4)
                        + " thresholds=" + juce::String (kMedianRealtimeRatioLimit, 2)
                        + "/" + juce::String (kUpperRealtimeRatioLimit, 2));
            expect (sample.processed, "pre-generated runtime batch remains finite and processable");
            expect (std::isfinite (medianRatio) && std::isfinite (upperRatio));
           #if defined (NDEBUG)
            if (! skipTimedAssertions())
            {
                expectLessThan (medianRatio, kMedianRealtimeRatioLimit,
                                "median batch processing retains explicit Release headroom");
                expectLessThan (upperRatio, kUpperRealtimeRatioLimit,
                                "robust upper batch processing remains faster than real time");
            }
           #endif
        };

        const auto staticFixture = makeDynamicReleaseFixture (48000.0);
        KickLockAudioProcessor staticProcessor;
        staticProcessor.enableAllBuses();
        staticProcessor.setRateAndBufferSizeDetails (48000.0, 256);
        staticProcessor.prepareToPlay (48000.0, 256);
        juce::AudioBuffer<float> staticBuffer (
            juce::jmax (staticProcessor.getTotalNumInputChannels(), staticProcessor.getTotalNumOutputChannels()), 256);
        juce::MidiBuffer staticMidi;
        const auto staticSample = measure ([&]
        {
            return processStaticFixture (staticProcessor, staticFixture, 256, staticBuffer, staticMidi);
        }, (double) staticFixture.bass.size() / staticFixture.sampleRate);
        logAndAssert ("Static baseline", 48000.0, 256, staticSample);

        struct Scenario { const char* name; double rate; int block; int states; bool worstCase; };
        const std::array<Scenario, 4> scenarios {{
            { "Dynamic Global-only", 48000.0, 256, 0, false },
            { "Dynamic eight hot States", 48000.0, 256, 8, false },
            { "Worst case State matching", 192000.0, 32, 8, true },
            { "Service-capable package set", 96000.0, 64, 8, true }
        }};

        for (const auto& scenario : scenarios)
        {
            const auto fixture = makeDynamicReleaseFixture (scenario.rate);
            DynamicProductionRuntime runtime;
            expect (runtime.prepare (scenario.rate, scenario.block, 1));
            runtime.activateMap (performanceMap (scenario.states, scenario.worstCase));
            const auto sample = measure ([&]
            {
                return processFixture (runtime, fixture, scenario.block, 1.0);
            }, (double) fixture.bass.size() / scenario.rate);

            logAndAssert (scenario.name, scenario.rate, scenario.block, sample);
        }
    }
};

static DynamicPerformanceGateTests dynamicPerformanceGateTests;
