#include "TestCommon.h"

#include "audio/AudioTestHarness.h"
#include "audio/AudioMetrics.h"
#include "audio/AudioSuiteHelpers.h"

#include <algorithm>
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
    }

private:
    void sampleRateBlockSizeMatrix()
    {
        const double rates[] = { 44100.0, 48000.0, 96000.0, 192000.0 };
        const int blockSizes[] = { 1, 7, 64, 127, 512, 2048 };

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
                expect (AudioMetrics::allFinite (r.output),
                        "sr=" + juce::String (rate, 0) + " block=" + juce::String (blockSize) + ": finite output");
                expectLessThan (AudioMetrics::peakAbs (r.output), 6.0f,
                                "sr=" + juce::String (rate, 0) + " block=" + juce::String (blockSize) + ": bounded output level");
                expectLessThan (AudioSuite::worstBoundaryTransitionScore (r), 5.0,
                                "sr=" + juce::String (rate, 0) + " block=" + juce::String (blockSize) + ": crossfades stay click-safe");

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
        cfg.blockSizeSequence = { 1, 7, 64, 127, 512, 2048 };
        cfg.blockSize = 512; // used only for Learn-side feed; irrelevant here (no Learn)

        AudioTestHarness harness (*this, cfg);
        const auto fx = harness.resolveFixture ("repeatable_multi_note");
        const auto map = AudioTestHarness::buildProvenCorrectiveMap (cfg.sampleRate);

        KickLockAudioProcessor proc;
        harness.prepareProcessor (proc);
        expect (harness.applyMapDirect (proc, map));
        const auto r = harness.render (proc, fx, harness.planLinear (fx));

        expect (AudioMetrics::allFinite (r.output), "finite output under a variable block-size sequence");
        expectGreaterThan (AudioMetrics::matchedPersistentBlockCount (r.timeline), 0,
                           "State branches still engage under variable block partitioning");
        expectLessThan (AudioSuite::worstBoundaryTransitionScore (r), 5.0,
                        "crossfades stay click-safe under variable block partitioning");

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
};

static AudioStressTests audioStressTests;
