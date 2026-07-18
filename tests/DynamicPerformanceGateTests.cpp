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
    constexpr int kBatchCount = kWarmupPasses + kMeasuredPasses;

    struct HostBlock
    {
        juce::AudioBuffer<float> input;
        juce::AudioBuffer<float> output;
        std::vector<float> bass;
        std::vector<float> kick;
        int samples = 0;

        HostBlock (int blockSize, int count)
            : input (2, blockSize), output (2, blockSize), bass ((size_t) count), kick ((size_t) count), samples (count) {}
    };

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
        map.calibration = { true, 1.0f, 0.001f };
        for (int slot = 0; slot < stateCount; ++slot)
        {
            auto& state = map.states[(size_t) slot];
            state.occupied = true;
            state.stableStateId = (uint64_t) (slot + 1);
            state.fingerprint = performancePrototype (-0.75f + 0.18f * (float) slot);
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

    std::vector<HostBlock> makeHostBlocks (const DynamicReleaseFixture& fixture, int blockSize)
    {
        std::vector<HostBlock> blocks;
        blocks.reserve ((fixture.bass.size() + (size_t) blockSize - 1) / (size_t) blockSize);
        for (int offset = 0; offset < (int) fixture.bass.size(); offset += blockSize)
        {
            const int count = std::min (blockSize, (int) fixture.bass.size() - offset);
            blocks.emplace_back (blockSize, count);
            auto& block = blocks.back();
            for (int sample = 0; sample < count; ++sample)
            {
                const float bass = fixture.bass[(size_t) (offset + sample)];
                const float kick = fixture.kick[(size_t) (offset + sample)];
                block.bass[(size_t) sample] = bass;
                block.kick[(size_t) sample] = kick;
                block.input.setSample (0, sample, bass);
                block.input.setSample (1, sample, bass);
            }
        }
        return blocks;
    }

    struct RuntimeBatch
    {
        DynamicProductionRuntime runtime;
        std::vector<HostBlock> blocks;
        bool finite = true;

        bool prepareAndWarm (const DynamicReleaseFixture& fixture, int blockSize,
                             const DynamicStateMap& map, double strength)
        {
            blocks = makeHostBlocks (fixture, blockSize);
            if (! runtime.prepare (fixture.sampleRate, blockSize, 2))
                return false;
            runtime.activateMap (map);
            return render (strength) && render (strength);
        }

        bool render (double strength) noexcept
        {
            finite = true;
            for (auto& block : blocks)
            {
                if (! runtime.process (block.input, block.bass.data(), block.kick.data(), true,
                                       strength, block.output, block.samples))
                    return false;
                for (int channel = 0; channel < 2; ++channel)
                    for (int sample = 0; sample < block.samples; ++sample)
                        if (! std::isfinite (block.output.getSample (channel, sample)))
                            finite = false;
            }
            return finite;
        }
    };

    struct StaticBatch
    {
        KickLockAudioProcessor processor;
        std::vector<HostBlock> blocks;
        juce::MidiBuffer midi;
        bool finite = true;

        bool prepareAndWarm (const DynamicReleaseFixture& fixture, int blockSize)
        {
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (fixture.sampleRate, blockSize);
            processor.prepareToPlay (fixture.sampleRate, blockSize);
            blocks = makeHostBlocks (fixture, blockSize);
            for (auto& block : blocks)
            {
                block.input.setSize (juce::jmax (processor.getTotalNumInputChannels(), processor.getTotalNumOutputChannels()),
                                     block.input.getNumSamples(), false, true, true);
                for (int sample = 0; sample < block.samples; ++sample)
                {
                    block.input.setSample (0, sample, block.bass[(size_t) sample]);
                    if (block.input.getNumChannels() > 1) block.input.setSample (1, sample, block.bass[(size_t) sample]);
                    if (block.input.getNumChannels() > 2) block.input.setSample (2, sample, block.kick[(size_t) sample]);
                    if (block.input.getNumChannels() > 3) block.input.setSample (3, sample, block.kick[(size_t) sample]);
                }
            }
            return render() && render();
        }

        bool render() noexcept
        {
            finite = true;
            for (auto& block : blocks)
            {
                block.input.clear();
                for (int sample = 0; sample < block.samples; ++sample)
                {
                    block.input.setSample (0, sample, block.bass[(size_t) sample]);
                    if (block.input.getNumChannels() > 1) block.input.setSample (1, sample, block.bass[(size_t) sample]);
                    if (block.input.getNumChannels() > 2) block.input.setSample (2, sample, block.kick[(size_t) sample]);
                    if (block.input.getNumChannels() > 3) block.input.setSample (3, sample, block.kick[(size_t) sample]);
                }
                processor.processBlock (block.input, midi);
                for (int channel = 0; channel < 2; ++channel)
                    for (int sample = 0; sample < block.samples; ++sample)
                        if (! std::isfinite (block.input.getSample (channel, sample)))
                            finite = false;
            }
            return finite;
        }
    };

    struct MeasurementBatch
    {
        KickLockAudioProcessor processor;
        std::vector<HostBlock> blocks;
        juce::MidiBuffer midi;
        bool finite = true;

        bool prepareAndWarm (const DynamicReleaseFixture& fixture, int blockSize, const DynamicStateMap& map)
        {
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (fixture.sampleRate, blockSize);
            processor.prepareToPlay (fixture.sampleRate, blockSize);
            if (auto* mode = processor.apvts.getParameter ("correction_mode"))
                mode->setValueNotifyingHost (mode->convertTo0to1 (1.0f));
            if (auto* strength = processor.apvts.getParameter ("dynamic_strength"))
                strength->setValueNotifyingHost (strength->convertTo0to1 (1.0f));
            if (! processor.publishDynamicStateMapForTesting (map))
                return false;
            blocks = makeHostBlocks (fixture, blockSize);
            for (auto& block : blocks)
                block.input.setSize (juce::jmax (processor.getTotalNumInputChannels(), processor.getTotalNumOutputChannels()),
                                     block.input.getNumSamples(), false, true, true);
            return render() && render();
        }

        bool render() noexcept
        {
            finite = true;
            for (auto& block : blocks)
            {
                block.input.clear();
                for (int sample = 0; sample < block.samples; ++sample)
                {
                    block.input.setSample (0, sample, block.bass[(size_t) sample]);
                    if (block.input.getNumChannels() > 1) block.input.setSample (1, sample, block.bass[(size_t) sample]);
                    if (block.input.getNumChannels() > 2) block.input.setSample (2, sample, block.kick[(size_t) sample]);
                    if (block.input.getNumChannels() > 3) block.input.setSample (3, sample, block.kick[(size_t) sample]);
                }
                processor.processBlock (block.input, midi);
                for (int channel = 0; channel < 2; ++channel)
                    for (int sample = 0; sample < block.samples; ++sample)
                        if (! std::isfinite (block.input.getSample (channel, sample)))
                            finite = false;
            }
            return finite;
        }
    };

    struct PerformanceSample
    {
        double representedSeconds = 0.0;
        double medianSeconds = 0.0;
        double upperSeconds = 0.0;
        bool processed = true;
    };

    template <typename Batch, typename Render>
    PerformanceSample measure (std::array<Batch, kBatchCount>& batches, Render&& render,
                               double representedSeconds)
    {
        std::array<double, kMeasuredPasses> seconds {};
        bool processed = true;
        for (int pass = 0; pass < kWarmupPasses; ++pass)
            processed = render (batches[(size_t) pass]) && processed;
        for (int pass = 0; pass < kMeasuredPasses; ++pass)
        {
            const auto start = std::chrono::steady_clock::now();
            processed = render (batches[(size_t) (kWarmupPasses + pass)]) && processed;
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
        beginTest ("Release runtime batches retain real-time headroom with preallocated stereo host blocks");
        if (skipTimedAssertions())
            logMessage ("KICKLOCK_SKIP_TIMED_ASSERTS=1: wall-clock Performance assertions skipped; preallocated correctness coverage still runs.");
       #if ! defined (NDEBUG)
        logMessage ("Debug build: timing thresholds are skipped; Release builds enforce them.");
       #endif

        const auto logAndAssert = [this] (const juce::String& label, double rate, int block,
                                          const PerformanceSample& sample, const DynamicProductionDiagnostics* diagnostics)
        {
            const double medianRatio = sample.medianSeconds / sample.representedSeconds;
            const double upperRatio = sample.upperSeconds / sample.representedSeconds;
            logMessage (label + " sr=" + juce::String (rate, 0)
                        + " block=" + juce::String (block)
                        + " medianRatio=" + juce::String (medianRatio, 4)
                        + " upperRatio=" + juce::String (upperRatio, 4));
            expect (sample.processed, "preallocated batch remains finite and processable");
            expect (std::isfinite (medianRatio) && std::isfinite (upperRatio));
            if (diagnostics != nullptr)
            {
                expectGreaterThan ((int64_t) diagnostics->completedObservations, (int64_t) 0,
                                  "scenario processes real production fingerprint observations");
            }
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
        std::array<StaticBatch, kBatchCount> staticBatches;
        for (auto& batch : staticBatches)
            expect (batch.prepareAndWarm (staticFixture, 256));
        const auto staticSample = measure (staticBatches, [] (StaticBatch& batch) { return batch.render(); },
                                           (double) staticFixture.bass.size() / staticFixture.sampleRate);
        logAndAssert ("Static baseline", 48000.0, 256, staticSample, nullptr);

        struct Scenario { const char* name; double rate; int block; int states; bool worstCase; bool expectState; bool expectService; };
        const std::array<Scenario, 4> scenarios {{
            { "Dynamic Global-only", 48000.0, 256, 0, false, false, false },
            { "Dynamic eight hot States", 48000.0, 256, 8, false, true, false },
            { "Worst-case State matching", 192000.0, 32, 8, true, true, false },
            { "Service prime/replay", 96000.0, 64, 1, true, true, true }
        }};

        for (const auto& scenario : scenarios)
        {
            const auto fixture = makeDynamicReleaseFixture (scenario.rate);
            const auto map = performanceMap (scenario.states, scenario.worstCase);
            std::array<RuntimeBatch, kBatchCount> batches;
            for (auto& batch : batches)
            {
                expect (batch.prepareAndWarm (fixture, scenario.block, map, 1.0));
                if (scenario.expectService)
                {
                    // Service is only selected for an eligible identity whose
                    // persistent slot is cold/unavailable. Force that bounded
                    // production branch condition, then prime it before timing.
                    batch.runtime.getEngineForTesting().clearStateSlot (0);
                    expect (batch.render (1.0));
                }
            }
            const auto sample = measure (batches, [] (RuntimeBatch& batch) { return batch.render (1.0); },
                                         (double) fixture.bass.size() / fixture.sampleRate);
            const auto diagnostics = batches.back().runtime.getDiagnostics();
            logAndAssert (scenario.name, scenario.rate, scenario.block, sample, &diagnostics);
            if (scenario.states == 0)
                expect (diagnostics.selectedBranchKind == DynamicSelectorBranchKind::Global,
                        "Global-only scenario never selects a persistent correction");
            if (scenario.expectState)
                expect (diagnostics.selectedBranchKind == DynamicSelectorBranchKind::State
                        || diagnostics.serviceBindings > 0,
                        "eligible Dynamic scenario reaches a persistent State or Service branch");
            if (scenario.expectService)
            {
                expectGreaterThan ((int64_t) diagnostics.servicePrimes, (int64_t) 0,
                                  "Service scenario exercises the bounded prime/replay path");
                expect (diagnostics.selectedBranchKind == DynamicSelectorBranchKind::Service,
                        "Service scenario reaches the actual Service branch before timing");
            }
        }

        beginTest ("Measurement callback batch reaches the processor worker handoff without timed allocations");
        const auto measurementFixture = makeDynamicReleaseFixture (48000.0);
        const auto measurementMap = performanceMap (1, false);
        std::array<MeasurementBatch, kBatchCount> measurementBatches;
        for (auto& batch : measurementBatches)
            expect (batch.prepareAndWarm (measurementFixture, 128, measurementMap));
        const auto measurementSample = measure (measurementBatches,
                                                [] (MeasurementBatch& batch) { return batch.render(); },
                                                (double) measurementFixture.bass.size() / measurementFixture.sampleRate);
        logAndAssert ("Runtime measurement capture", 48000.0, 128, measurementSample, nullptr);
        auto& measurementProcessor = measurementBatches.back().processor;
        expectGreaterThan ((int64_t) measurementProcessor.getDynamicProductionDiagnosticsForTesting().completedObservations,
                           (int64_t) 0, "measurement scenario completes real runtime observations");
        bool verified = false;
        for (int attempt = 0; attempt < 250 && ! verified; ++attempt)
        {
            const auto summary = measurementProcessor.getDynamicVerifiedMeasurementForTesting (1);
            verified = summary.availability == DynamicMeasurementAvailability::Available
                    || summary.availability == DynamicMeasurementAvailability::InsufficientMaterial;
            if (! verified)
            {
                juce::Thread::sleep (2);
                expect (measurementBatches.back().render());
            }
        }
        expect (verified, "measurement scenario reaches the worker handoff and fresh Verified sidecar");
    }
};

static DynamicPerformanceGateTests dynamicPerformanceGateTests;
