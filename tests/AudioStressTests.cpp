#include "TestCommon.h"

#include "audio/AudioTestHarness.h"
#include "audio/AudioMetrics.h"
#include "audio/AudioSuiteHelpers.h"

#include <algorithm>
#include <cmath>
#include <future>
#include <vector>

// =============================================================================
// AudioStressTests: the long-running sample-rate x block-size matrix, plus
// variable block-size sequences. NOT part of the default ctest run or the fast
// checkpoint suite (see tests/CMakeLists.txt) - run explicitly.
//
// Uses the proven corrective map (real State selection/crossfade engaged) on
// one representative fixture rather than the full 11-case matrix at every
// rate/block-size combination: real async Learn per combination across 4 rates
// x 6 block sizes would be prohibitively slow for a suite meant to be run
// somewhat regularly, and Learn-path correctness is already covered by
// AudioAcceptanceTests at 48 kHz. This suite's job is exclusively DSP/selector
// robustness (finite output, bounded level, click-free selection/crossfades,
// deterministic decisions) across the full rate/block-size breadth, plus
// variable-block-size partitioning stability.
//
// This is offline real-audio-path robustness testing, not real-DAW validation.
// =============================================================================

class AudioStressTests : public juce::UnitTest
{
public:
    AudioStressTests() : juce::UnitTest ("AudioStress", "AudioStress") {}

    void runTest() override
    {
        sampleRateBlockSizeMatrix();
        variableBlockSizeSequence();
        hundredLoopWraps();
        concurrentInstances();
    }

private:
    // Release-stress limits. These are product contracts, declared here before
    // the matrix is executed; they are not adjusted from observed results.
    static constexpr float kMaximumOutputPeak = 6.0f;
    static constexpr double kMaximumTransitionScore = 5.0;
    static constexpr float kMaximumAbsoluteDc = 0.05f;
    static constexpr double kMinimumRealtimeFactor = 1.0;

    static bool hasSubnormalSample (const std::vector<float>& samples)
    {
        for (const auto sample : samples)
            if (std::fpclassify (sample) == FP_SUBNORMAL)
                return true;
        return false;
    }

    static float absoluteMean (const std::vector<float>& samples)
    {
        if (samples.empty())
            return 0.0f;

        double sum = 0.0;
        for (const auto sample : samples)
            sum += sample;
        return (float) std::abs (sum / (double) samples.size());
    }

    void assertReleaseStressContract (const RenderResult& render, const juce::String& label)
    {
        expect (AudioMetrics::allFinite (render.output), label + ": finite output");
        expect (! hasSubnormalSample (render.output), label + ": no subnormal output samples");
        expectLessThan (AudioMetrics::peakAbs (render.output), kMaximumOutputPeak,
                        label + ": bounded output level");
        expectLessThan (absoluteMean (render.output), kMaximumAbsoluteDc,
                        label + ": no unexpected DC offset");
        expectLessThan (AudioSuite::worstBoundaryTransitionScore (render), kMaximumTransitionScore,
                        label + ": crossfades stay click-safe");
        expectGreaterThan (render.realtimeFactor(), kMinimumRealtimeFactor,
                           label + ": processing remains faster than real time");
    }

    void sampleRateBlockSizeMatrix()
    {
        const double rates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };
        const int blockSizes[] = { 32, 64, 128, 256, 512, 1024, 2048 };

        for (const double rate : rates)
        {
            for (const int blockSize : blockSizes)
            {
                beginTest ("Stress: sr=" + juce::String (rate, 0) + " block=" + juce::String (blockSize));

                AudioHarnessConfig cfg;
                cfg.sampleRate = rate;
                cfg.blockSize = blockSize;

                AudioTestHarness harness (*this, cfg);
                const auto fx = harness.resolveFixture ("repeatable_multi_note");
                expect (! fx.bass.empty(), "fixture resolves at this sample rate");
                if (fx.bass.empty())
                    continue;

                const auto map = AudioTestHarness::buildProvenCorrectiveMap (rate);
                KickLockAudioProcessor proc;
                harness.prepareProcessor (proc);
                expect (harness.applyMapDirect (proc, map), "corrective map applied");

                const auto r = harness.render (proc, fx, harness.planLinear (fx));
                const auto label = "sr=" + juce::String (rate, 0) + " block=" + juce::String (blockSize);
                assertReleaseStressContract (r, label);

                logMessage ("  stateBlocks=" + juce::String (AudioMetrics::matchedPersistentBlockCount (r.timeline))
                            + " worstTransition=" + juce::String (AudioSuite::worstBoundaryTransitionScore (r), 2)
                            + " rt=" + juce::String (r.realtimeFactor(), 0) + "x");
            }
        }
    }

    void variableBlockSizeSequence()
    {
        beginTest ("Stress: variable block-size sequence stays finite and click-safe (block partitioning invariance)");

        AudioHarnessConfig cfg;
        cfg.sampleRate = 48000.0;
        cfg.blockSizeSequence = { 32, 64, 128, 256, 512, 1024, 2048 };
        cfg.blockSize = 512; // used only for Learn-side feed; irrelevant here (no Learn)

        AudioTestHarness harness (*this, cfg);
        const auto fx = harness.resolveFixture ("repeatable_multi_note");
        const auto map = AudioTestHarness::buildProvenCorrectiveMap (cfg.sampleRate);

        KickLockAudioProcessor proc;
        harness.prepareProcessor (proc);
        expect (harness.applyMapDirect (proc, map));
        const auto r = harness.render (proc, fx, harness.planLinear (fx));

        assertReleaseStressContract (r, "variable legal block-size sequence");
        expectGreaterThan (AudioMetrics::matchedPersistentBlockCount (r.timeline), 0,
                           "State branches still engage under variable block partitioning");

        // Same input rendered again with a FIXED block size: the set of
        // resolved decisions (which identities ever get selected) should match,
        // proving block partitioning doesn't change what the selector decides,
        // even though exact block boundaries (and therefore per-block timeline
        // rows) differ.
        AudioHarnessConfig fixedCfg = cfg;
        fixedCfg.blockSizeSequence.clear();
        fixedCfg.blockSize = 256;
        AudioTestHarness fixedHarness (*this, fixedCfg);
        KickLockAudioProcessor proc2;
        fixedHarness.prepareProcessor (proc2);
        expect (fixedHarness.applyMapDirect (proc2, map));
        const auto r2 = fixedHarness.render (proc2, fx, fixedHarness.planLinear (fx));

        std::vector<uint64_t> idsVariable, idsFixed;
        for (const auto& e : r.timeline) if (e.selectedSemanticStateId != 0) idsVariable.push_back (e.selectedSemanticStateId);
        for (const auto& e : r2.timeline) if (e.selectedSemanticStateId != 0) idsFixed.push_back (e.selectedSemanticStateId);
        std::sort (idsVariable.begin(), idsVariable.end()); idsVariable.erase (std::unique (idsVariable.begin(), idsVariable.end()), idsVariable.end());
        std::sort (idsFixed.begin(), idsFixed.end()); idsFixed.erase (std::unique (idsFixed.begin(), idsFixed.end()), idsFixed.end());
        expect (idsVariable == idsFixed, "the same set of States is selectable regardless of block partitioning");

        logMessage ("  variableBlockStates=" + juce::String ((int) idsVariable.size())
                    + " fixedBlockStates=" + juce::String ((int) idsFixed.size()));
    }

    void hundredLoopWraps()
    {
        beginTest ("Stress: 100 loop wraps retain monotonic timing, bounded output, and State selection");

        AudioHarnessConfig cfg;
        cfg.sampleRate = 48000.0;
        cfg.blockSize = 256;
        AudioTestHarness harness (*this, cfg);
        const auto fx = harness.resolveFixture ("loop_wrap_first_kick");
        const auto map = AudioTestHarness::buildProvenCorrectiveMap (cfg.sampleRate);

        KickLockAudioProcessor processor;
        harness.prepareProcessor (processor);
        expect (harness.applyMapDirect (processor, map));
        const auto render = harness.render (processor, fx, harness.planLoop (fx, 100));

        assertReleaseStressContract (render, "100 loop wraps");
        expectEquals (AudioMetrics::internalTimestampBackwardSteps (render.timeline), 0,
                      "loop wraps never rewind the production monotonic timestamp");
        expectGreaterThan (AudioMetrics::matchedPersistentBlockCount (render.timeline), 0,
                           "State branches remain active across the endurance loop");
        logMessage ("  processedSeconds=" + juce::String (render.processedSeconds, 2)
                    + " realtimeFactor=" + juce::String (render.realtimeFactor(), 1)
                    + " stateBlocks=" + juce::String (AudioMetrics::matchedPersistentBlockCount (render.timeline)));
    }

    void concurrentInstances()
    {
        beginTest ("Stress: concurrent processors keep Dynamic State selection independent");

        AudioHarnessConfig cfg;
        cfg.sampleRate = 48000.0;
        cfg.blockSizeSequence = { 32, 128, 512, 2048 };
        cfg.blockSize = 512;
        AudioTestHarness harness (*this, cfg);
        const auto fx = harness.resolveFixture ("repeatable_multi_note");
        const auto map = AudioTestHarness::buildProvenCorrectiveMap (cfg.sampleRate);

        auto renderInstance = [&harness, &fx, &map] ()
        {
            KickLockAudioProcessor processor;
            harness.prepareProcessor (processor);
            const bool applied = harness.applyMapDirect (processor, map);
            auto render = harness.render (processor, fx, harness.planLinear (fx));
            return std::make_pair (applied, std::move (render));
        };

        auto first = std::async (std::launch::async, renderInstance);
        auto second = std::async (std::launch::async, renderInstance);
        const auto left = first.get();
        const auto right = second.get();

        expect (left.first && right.first, "each instance accepts its own State map publication");
        assertReleaseStressContract (left.second, "concurrent instance A");
        assertReleaseStressContract (right.second, "concurrent instance B");
        expect (AudioSuite::timelinesIdentical (left.second.timeline, right.second.timeline),
                "independent instances produce the same deterministic selection timeline");
        expectGreaterThan (AudioMetrics::matchedPersistentBlockCount (left.second.timeline), 0,
                           "instance A selected a State");
        expectGreaterThan (AudioMetrics::matchedPersistentBlockCount (right.second.timeline), 0,
                           "instance B selected a State");
    }
};

static AudioStressTests audioStressTests;
