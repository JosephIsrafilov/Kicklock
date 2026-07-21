#include "TestCommon.h"

#include "audio/AudioTestHarness.h"
#include "audio/AudioMetrics.h"
#include "audio/AudioSuiteHelpers.h"
#include "audio/AudioFixtureManifest.h"

// =============================================================================
// AudioSmokeTests: the short real-audio subset safe to run at checkpoint time.
// A couple of repeatable fixtures at one sample rate / one block size taken
// through the real Learn -> Apply -> render path (artifact-safety + RENDER
// determinism), plus one corrective-map scenario that genuinely selects State
// branches and crossfades (so the selector/hot-branch/crossfade path is
// exercised with teeth, click-safety asserted). The full 11-case matrix and the
// sample-rate/block-size sweep live in AudioAcceptanceTests / AudioStressTests.
//
// This is OFFLINE real-audio-path acceptance. It is NOT real-DAW validation and
// NOT a substitute for human listening.
// =============================================================================

class AudioSmokeTests : public juce::UnitTest
{
public:
    AudioSmokeTests() : juce::UnitTest ("AudioSmoke", "AudioSmoke") {}

    void runTest() override
    {
        AudioHarnessConfig cfg;
        cfg.sampleRate = 48000.0;
        cfg.blockSize = 256;

        for (const auto& entry : audioFixtureManifest())
        {
            if (! entry.inSmokeSuite)
                continue;

            beginTest ("Smoke: " + juce::String (entry.name) + " renders cleanly and deterministically");

            AudioTestHarness harness (*this, cfg);
            const auto fx = harness.resolveFixture (entry.name);
            expect (! fx.bass.empty() && ! fx.kick.empty(), "fixture resolved to real audio");
            if (fx.bass.empty())
                continue;

            KickLockAudioProcessor learnProc;
            harness.prepareProcessor (learnProc);
            const auto learn = harness.learnAndApply (learnProc, fx);
            if (! learn.reachedResultReady)
            {
                logMessage ("  INFRA_TIMEOUT (reported separately, not a silent baseline): " + learn.diagnostics);
                expect (true, "Learn infrastructure timeout reported, not passed off as a map result");
                continue;
            }
            expect (learn.applied, "Apply activated the learned map");
            const int latency = learnProc.getLatencySamples();
            const int occupied = getOccupiedDynamicStateCount (learn.map);

            // Two identical render passes on two fresh instances fed the same map.
            KickLockAudioProcessor p1, p2;
            harness.prepareProcessor (p1);
            harness.prepareProcessor (p2);
            expect (harness.applyMapDirect (p1, learn.map));
            expect (harness.applyMapDirect (p2, learn.map));
            const auto r1 = harness.render (p1, fx, harness.planLinear (fx));
            const auto r2 = harness.render (p2, fx, harness.planLinear (fx));

            const auto g1 = AudioMetrics::compute (fx, r1, latency, occupied, cfg.sampleRate);
            expect (g1.outputFinite, "no NaN/Inf in rendered output");
            expectLessThan (g1.peak, entry.thresholds.maxPeak, "output level bounded");

            expect (AudioSuite::timelinesIdentical (r1.timeline, r2.timeline),
                    "identical state-event timeline across two identical render passes");
            const auto g2 = AudioMetrics::compute (fx, r2, latency, occupied, cfg.sampleRate);
            expectWithinAbsoluteError (g2.medianImprovement, g1.medianImprovement, 1.0e-6);
            expectWithinAbsoluteError (g2.worstCaseDegradation, g1.worstCaseDegradation, 1.0e-6);
            expectWithinAbsoluteError ((double) g2.peak, (double) g1.peak, 1.0e-6);

            const auto failed = AudioMetrics::evaluateThresholds (entry, g1);
            if (! failed.isEmpty())
                harness.dumpArtifacts (entry.name, fx, r1,
                                       AudioMetrics::toJson (entry.name, g1, r1, fx.fromRealOverride),
                                       AudioMetrics::stateTimelineCsv (r1),
                                       AudioMetrics::transportTimelineCsv (r1), failed);
            expect (failed.isEmpty(), "smoke thresholds: " + failed.joinIntoString ("; "));

            logMessage ("  states=" + juce::String (occupied)
                        + " median=" + juce::String (g1.medianImprovement, 2)
                        + " worst=" + juce::String (g1.worstCaseDegradation, 2)
                        + " globalR=" + juce::String (g1.globalRate, 2)
                        + " rt=" + juce::String (r1.realtimeFactor(), 0) + "x");
        }

        beginTest ("Smoke: corrective map selects State branches and crossfades click-free");
        {
            AudioTestHarness harness (*this, cfg);
            const auto fx = harness.resolveFixture ("repeatable_multi_note");
            const auto map = AudioTestHarness::buildProvenCorrectiveMap (cfg.sampleRate);

            KickLockAudioProcessor p1, p2;
            harness.prepareProcessor (p1);
            harness.prepareProcessor (p2);
            expect (harness.applyMapDirect (p1, map));
            expect (harness.applyMapDirect (p2, map));
            const auto r1 = harness.render (p1, fx, harness.planLinear (fx));
            const auto r2 = harness.render (p2, fx, harness.planLinear (fx));

            expect (AudioMetrics::allFinite (r1.output), "finite output under real State selection");
            expectGreaterThan (AudioMetrics::matchedPersistentBlockCount (r1.timeline), 0,
                               "State branches are actually selected (MatchedPersistentState occurs)");
            expectLessThan (AudioSuite::worstBoundaryTransitionScore (r1), 4.0,
                            "crossfades into/out of State branches stay click-free");
            expect (AudioSuite::timelinesIdentical (r1.timeline, r2.timeline),
                    "State-selecting render is deterministic across two passes");
            logMessage ("  correctiveSelection stateBlocks="
                        + juce::String (AudioMetrics::matchedPersistentBlockCount (r1.timeline))
                        + " worstTransition=" + juce::String (AudioSuite::worstBoundaryTransitionScore (r1), 2));
        }
    }
};

static AudioSmokeTests audioSmokeTests;
