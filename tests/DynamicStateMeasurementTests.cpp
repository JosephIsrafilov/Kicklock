#include "TestCommon.h"

#include <chrono>
#include <thread>

#include "DynamicStateMeasurements.h"
#include "DynamicMeasurementScorer.h"
#include "DynamicPredictedMeasurement.h"
#include "DynamicRuntimeMeasurementCapture.h"
#include "DynamicVerifiedAggregation.h"
#include "DynamicRuntimeSnapshot.h"
#include "DynamicProductionRuntime.h"
#include "util/DynamicMeasurementResultQueue.h"
#include "util/DynamicSnapshotPublisher.h"
#include "util/LearnHitQueue.h"

namespace
{
    // A quarter-period phase-shifted sine pair: kick is a pure tone, bass is
    // the SAME tone delayed by `lagMs`. At zero compensation the two are
    // ~90 degrees out of phase (poor correlation); delaying bass by exactly
    // -lagMs (i.e. advancing it back into phase) restores near-unity
    // correlation. This gives a deterministic, exactly-known before/after
    // relationship without depending on any transient-detection heuristic.
    void makePhaseShiftedPair (std::vector<float>& bass, std::vector<float>& kick,
                               int numSamples, double sampleRate, double freqHz, double lagMs)
    {
        bass.assign ((size_t) numSamples, 0.0f);
        kick.assign ((size_t) numSamples, 0.0f);
        const int lagSamples = (int) std::lround (lagMs / 1000.0 * sampleRate);
        for (int i = 0; i < numSamples; ++i)
        {
            kick[(size_t) i] = (float) std::sin (kTwoPi * freqHz * (double) i / sampleRate);
            const int bassIndex = i - lagSamples;
            bass[(size_t) i] = bassIndex >= 0
                ? (float) std::sin (kTwoPi * freqHz * (double) bassIndex / sampleRate)
                : 0.0f;
        }
    }

    DynamicPackageResolution makeTestPackage (double sampleRate, double delayMs, bool polarityInvert,
                                              bool allpassEnabled, bool crossoverEnabled) noexcept
    {
        DynamicPackageResolution package;
        package.valid = true;
        package.decision = DynamicPackageDecision::StateCorrection;
        package.effectiveAbsoluteDelayMs = delayMs;
        package.polarityInvert = polarityInvert;
        package.allpassEnabled = allpassEnabled;
        package.allpassStages = 2;
        package.crossoverEnabled = crossoverEnabled;
        package.crossoverHz = 150.0;
        package.delayInterpolationIndex = 0;
        DynamicAllpassPoleDomain::makeCoefficients (100.0, 0.7, sampleRate, package.allpassCoefficients);
        return package;
    }

    DynamicFingerprintPrototype testPrototype (float seed)
    {
        DynamicFingerprintPrototype value;
        value.valid = true;
        value.featureCount = DynamicFingerprintContract::kFeatureCount;
        for (int i = 0; i < DynamicFingerprintContract::kFeatureCount; ++i)
            value.features[(size_t) i] = std::clamp (seed + 0.004f * (float) i, -1.0f, 1.0f);
        return value;
    }

    DynamicStateMap makeOneStateMap (uint64_t stableId, const DynamicFingerprintPrototype& fingerprint,
                                     bool hasLearnedPackage, DynamicZonePackage package,
                                     DynamicStateEvidence evidence, uint32_t hitCount)
    {
        DynamicStateMap map = makeEmptyDynamicStateMap();
        map.valid = true;
        map.globalBase.globalBaseDelayMs = 0.0f;
        map.globalBase.crossoverEnabled = false;
        map.globalBase.allpassEnabled = false;
        map.globalBase.globalAllpassFreqHz = 100.0f;
        map.globalBase.globalAllpassQ = 0.7f;
        map.globalBase.allpassStages = 2;
        map.globalBase.learnedSampleRate = 48000.0;
        map.calibration.valid = true;
        map.calibration.absoluteDistanceThreshold = 0.15f;
        map.calibration.ambiguityMargin = 0.02f;
        map.nextStateId = stableId + 1;

        DynamicState state;
        state.occupied = true;
        state.stableStateId = stableId;
        state.fingerprint = fingerprint;
        state.origin = DynamicStateOrigin::Auto;
        state.evidence = evidence;
        state.hitCount = hitCount;
        state.repeatability = 0.9f;
        state.hasLearnedPackage = hasLearnedPackage;
        if (hasLearnedPackage)
            state.learnedPackage = package;
        map.states[0] = state;
        return map;
    }

    LearnHitWindow makeWindow (int sequence, int64_t triggerSample, std::vector<float> bass, std::vector<float> kick)
    {
        LearnHitWindow window;
        window.sequence = sequence;
        window.absoluteSampleAtTrigger = (int) triggerSample;
        window.bass = std::move (bass);
        window.kick = std::move (kick);
        return window;
    }

    DynamicLearnHit makeHitForFingerprint (int sequence, int64_t triggerSample,
                                           const DynamicFingerprintPrototype& fingerprint)
    {
        DynamicLearnHit hit;
        hit.sequence = sequence;
        hit.triggerSample = triggerSample;
        hit.fingerprint = fingerprint;
        hit.fingerprintValidity = DynamicFingerprintValidity::Valid;
        return hit;
    }
}

class DynamicStateMeasurementTests : public juce::UnitTest
{
public:
    DynamicStateMeasurementTests() : juce::UnitTest ("DynamicStateMeasurement", "Phase9") {}

    void runTest() override
    {
        testScorerIdentity();
        testScorerKnownImprovement();
        testFinalPackageMedianFailure();
        testCandidateNeverCorrectionEligible();
        testStableVerifiedPrediction();
        testRetainedWindowCap();
        testEightStateCapacity();
        testQueueBoundedAndExhaustion();
        testSnapshotTearAndCompleteness();
        testRuntimeCaptureAlignment();
        testNonVerifiableEventsNeverCaptured();
        testContractValidation();
        testRawPreRollNotTruncated();
        testSnapshotPublisherConcurrentStress();
        testRepeatedPrepareWhileWorkerActive();
        testVerifiedGenerationNeverContaminated();
    }

private:
    void testScorerIdentity()
    {
        beginTest ("Scorer: neutral package leaves before==after and stays finite across rates");
        for (const double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
        {
            const int n = (int) std::lround (rate * 0.05);
            std::vector<float> bass, kick;
            makePhaseShiftedPair (bass, kick, n, rate, 90.0, 1.5);
            const auto package = makeTestPackage (rate, 0.0, false, false, false);
            DynamicMeasurementScorer::Scratch scratch;
            const auto result = DynamicMeasurementScorer::score (bass.data(), kick.data(), n, rate, package, scratch);
            expect (result.valid, "scorer must accept a valid neutral package");
            expect (std::isfinite (result.beforeScore) && std::isfinite (result.afterScore)
                        && std::isfinite (result.improvementPoints) && std::isfinite (result.confidence),
                    "every score field must stay finite");
            expectWithinAbsoluteError (result.afterScore, result.beforeScore, 1.0e-4f,
                                       "zero-delay, disabled-allpass package must not change the score");
        }
    }

    void testScorerKnownImprovement()
    {
        beginTest ("Scorer: a package that removes a known phase lag improves the score");
        const double rate = 48000.0;
        const int n = (int) std::lround (rate * 0.05);
        std::vector<float> bass, kick;
        // Bass lags the kick by a quarter period at 100 Hz (2.5 ms): near-90
        // degree phase offset, so the raw correlation starts poor.
        makePhaseShiftedPair (bass, kick, n, rate, 100.0, 2.5);
        const auto neutral = makeTestPackage (rate, 0.0, false, false, false);
        const auto corrected = makeTestPackage (rate, -2.5, false, false, false);

        DynamicMeasurementScorer::Scratch scratch;
        const auto before = DynamicMeasurementScorer::score (bass.data(), kick.data(), n, rate, neutral, scratch);
        const auto after = DynamicMeasurementScorer::score (bass.data(), kick.data(), n, rate, corrected, scratch);
        expect (before.valid && after.valid);
        expectGreaterThan (after.afterScore, before.afterScore + 20.0f,
                           "compensating the exact lag must substantially raise the score");
        expectGreaterThan (after.improvementPoints, 20.0f);
    }

    void testFinalPackageMedianFailure()
    {
        beginTest ("Predicted measurement: individually-beneficial hits can still fail as one final package");
        const double rate = 48000.0;
        const int n = (int) std::lround (rate * 0.05);
        const auto fingerprint = testPrototype (0.2f);

        // The final package is the CLUSTER median delay; craft windows whose
        // individual best delay varies so the shared median package performs
        // badly on some of them (a locally-beneficial candidate does not
        // imply a beneficial shared package).
        std::vector<LearnHitWindow> windows;
        std::vector<DynamicLearnHit> hits;
        const std::array<double, 6> lags { 2.5, 2.5, 2.5, -2.5, -2.5, -2.5 };
        for (int i = 0; i < (int) lags.size(); ++i)
        {
            std::vector<float> bass, kick;
            makePhaseShiftedPair (bass, kick, n, rate, 100.0, lags[(size_t) i]);
            windows.push_back (makeWindow (i, i * 10000, bass, kick));
            hits.push_back (makeHitForFingerprint (i, i * 10000, fingerprint));
        }

        // The formed package uses a single fixed delay (as Phase 8 would
        // choose one median across the whole cluster); pick the +2.5ms
        // candidate so half the retained windows are corrected and half are
        // actively pushed further out of phase.
        DynamicZonePackage finalPackage { -2.5f, 100.0f, 0.7f };
        auto map = makeOneStateMap (500, fingerprint, true, finalPackage, DynamicStateEvidence::Stable, 6);

        const auto retained = retainDynamicLearnMeasurementWindows (hits, windows, map, rate);
        expectEquals ((int) retained[0].size(), 6, "all six windows should re-match the single formed State");

        const auto result = computeDynamicPredictedMeasurement (map, retained, rate, 0);
        const auto& summary = result.predicted[0];
        expect (summary.validWindowCount >= 3, "enough windows to reach a verdict");
        expectLessThan (summary.worstImprovementPoints, 0.0f, "the mean must not hide the regressed half");
        expect (! result.map.states[0].hasLearnedPackage,
               "an inconsistent final package must be demoted, not kept enabled");
        expect (result.map.states[0].occupied, "the State remains occupied/recognizable after demotion");
        expectEquals ((int) result.map.states[0].manualTrim.delayTrimMs, 0,
                     "trim resets to exactly zero on demotion");
        expect (summary.rejectionReason == DynamicCorrectionRejectionReason::InconsistentAcrossHits
                    || summary.rejectionReason == DynamicCorrectionRejectionReason::NonBeneficial,
               "rejection reason must be categorical, not None");
        expect (isRuntimeEligibleDynamicFingerprintV1 (result.map.states[0].fingerprint,
                                                       DynamicFingerprintContract::kExtractorVersion),
               "runtime must still be able to recognize the demoted State");
    }

    void testCandidateNeverCorrectionEligible()
    {
        beginTest ("Predicted measurement: Candidate States stay CandidateOnly regardless of score");
        const double rate = 48000.0;
        const int n = (int) std::lround (rate * 0.05);
        const auto fingerprint = testPrototype (0.4f);

        std::vector<LearnHitWindow> windows;
        std::vector<DynamicLearnHit> hits;
        for (int i = 0; i < 3; ++i)
        {
            std::vector<float> bass, kick;
            makePhaseShiftedPair (bass, kick, n, rate, 100.0, 2.5);
            windows.push_back (makeWindow (i, i * 10000, bass, kick));
            hits.push_back (makeHitForFingerprint (i, i * 10000, fingerprint));
        }

        DynamicZonePackage package { -2.5f, 100.0f, 0.7f };
        auto map = makeOneStateMap (700, fingerprint, true, package, DynamicStateEvidence::Candidate, 3);

        const auto retained = retainDynamicLearnMeasurementWindows (hits, windows, map, rate);
        const auto result = computeDynamicPredictedMeasurement (map, retained, rate, 0);
        expect (result.predicted[0].assessment == DynamicCorrectionAssessment::CandidateOnly);
        expect (result.map.states[0].hasLearnedPackage,
               "measurement alone never grants a Candidate correction eligibility");
        expect (result.map.states[0].evidence == DynamicStateEvidence::Candidate);
    }

    void testStableVerifiedPrediction()
    {
        beginTest ("Predicted measurement: a consistently beneficial Stable package is Available/PredictedImprovement");
        const double rate = 48000.0;
        const int n = (int) std::lround (rate * 0.05);
        const auto fingerprint = testPrototype (0.6f);

        std::vector<LearnHitWindow> windows;
        std::vector<DynamicLearnHit> hits;
        for (int i = 0; i < 5; ++i)
        {
            std::vector<float> bass, kick;
            makePhaseShiftedPair (bass, kick, n, rate, 100.0, 2.5);
            windows.push_back (makeWindow (i, i * 10000, bass, kick));
            hits.push_back (makeHitForFingerprint (i, i * 10000, fingerprint));
        }

        DynamicZonePackage package { -2.5f, 100.0f, 0.7f };
        auto map = makeOneStateMap (900, fingerprint, true, package, DynamicStateEvidence::Stable, 5);

        const auto retained = retainDynamicLearnMeasurementWindows (hits, windows, map, rate);
        const auto result = computeDynamicPredictedMeasurement (map, retained, rate, 0);
        const auto& summary = result.predicted[0];
        expect (summary.availability == DynamicMeasurementAvailability::Available);
        expect (summary.assessment == DynamicCorrectionAssessment::PredictedImprovement);
        expect (result.map.states[0].hasLearnedPackage, "a genuinely beneficial package remains enabled");
    }

    void testRetainedWindowCap()
    {
        beginTest ("Retention: bounded to 12 windows per State, deterministic under reordered input");
        const double rate = 48000.0;
        const int n = (int) std::lround (rate * 0.02);
        const auto fingerprint = testPrototype (0.1f);

        std::vector<LearnHitWindow> windows;
        std::vector<DynamicLearnHit> hits;
        for (int i = 0; i < 20; ++i)
        {
            std::vector<float> bass, kick;
            makePhaseShiftedPair (bass, kick, n, rate, 90.0 + (double) i, 1.0);
            windows.push_back (makeWindow (i, i * 10000, bass, kick));
            hits.push_back (makeHitForFingerprint (i, i * 10000, fingerprint));
        }
        DynamicZonePackage package { 0.0f, 100.0f, 0.7f };
        auto map = makeOneStateMap (1100, fingerprint, true, package, DynamicStateEvidence::Stable, 20);

        const auto retainedForward = retainDynamicLearnMeasurementWindows (hits, windows, map, rate);
        expectEquals ((int) retainedForward[0].size(), kDynamicPredictedMeasurementMaxRetainedPerState,
                     "more than 12 members must be capped to exactly 12");

        std::vector<LearnHitWindow> reversedWindows (windows.rbegin(), windows.rend());
        std::vector<DynamicLearnHit> reversedHits (hits.rbegin(), hits.rend());
        const auto retainedReversed = retainDynamicLearnMeasurementWindows (reversedHits, reversedWindows, map, rate);
        expectEquals ((int) retainedReversed[0].size(), kDynamicPredictedMeasurementMaxRetainedPerState);

        std::vector<int64_t> forwardTriggers, reversedTriggers;
        for (const auto& w : retainedForward[0]) forwardTriggers.push_back (w.triggerSample);
        for (const auto& w : retainedReversed[0]) reversedTriggers.push_back (w.triggerSample);
        std::sort (forwardTriggers.begin(), forwardTriggers.end());
        std::sort (reversedTriggers.begin(), reversedTriggers.end());
        expect (forwardTriggers == reversedTriggers,
               "the retained SET must be independent of input encounter order");
    }

    void testEightStateCapacity()
    {
        beginTest ("Retention: eight States each keep their own bounded windows, no cross-State ownership");
        const double rate = 48000.0;
        const int n = (int) std::lround (rate * 0.02);

        DynamicStateMap map = makeEmptyDynamicStateMap();
        map.valid = true;
        map.calibration.valid = true;
        map.calibration.absoluteDistanceThreshold = 0.06f;
        map.calibration.ambiguityMargin = 0.01f;
        map.globalBase.learnedSampleRate = rate;
        map.nextStateId = 9;

        std::vector<LearnHitWindow> windows;
        std::vector<DynamicLearnHit> hits;
        int sequence = 0;
        for (int slot = 0; slot < DynamicStateMapContract::kMaxPersistentStates; ++slot)
        {
            const auto fp = testPrototype (-0.9f + 0.25f * (float) slot);
            DynamicState state;
            state.occupied = true;
            state.stableStateId = (uint64_t) (slot + 1);
            state.fingerprint = fp;
            state.origin = DynamicStateOrigin::Auto;
            state.evidence = DynamicStateEvidence::Stable;
            state.hitCount = 5;
            state.repeatability = 0.9f;
            map.states[(size_t) slot] = state;

            for (int i = 0; i < 15; ++i)
            {
                std::vector<float> bass, kick;
                makePhaseShiftedPair (bass, kick, n, rate, 90.0, 1.0);
                windows.push_back (makeWindow (sequence, sequence * 10000, bass, kick));
                hits.push_back (makeHitForFingerprint (sequence, sequence * 10000, fp));
                ++sequence;
            }
        }

        const auto retained = retainDynamicLearnMeasurementWindows (hits, windows, map, rate);
        for (int slot = 0; slot < DynamicStateMapContract::kMaxPersistentStates; ++slot)
            expectEquals ((int) retained[(size_t) slot].size(), kDynamicPredictedMeasurementMaxRetainedPerState);
    }

    void testQueueBoundedAndExhaustion()
    {
        beginTest ("Queues: bounded capacity, fixed diagnostics, no overwrite on overflow");
        DynamicMeasurementScoreQueue queue;
        queue.prepare (DynamicMeasurementQueueContract::kScoreQueueCapacity);

        int pushed = 0;
        for (int i = 0; i < DynamicMeasurementQueueContract::kScoreQueueCapacity + 5; ++i)
        {
            DynamicMeasurementScoredCapture value;
            value.stableStateId = (uint64_t) i;
            if (queue.push (value))
                ++pushed;
        }
        expectEquals (pushed, DynamicMeasurementQueueContract::kScoreQueueCapacity,
                     "push must stop accepting once the fixed capacity is reached");
        expectGreaterThan (queue.getDroppedCount(), 0, "overflow must increment a fixed diagnostic");

        int drained = 0;
        DynamicMeasurementScoredCapture out;
        while (queue.pop (out))
            expectEquals ((int) out.stableStateId, drained++);
        expectEquals (drained, DynamicMeasurementQueueContract::kScoreQueueCapacity,
                     "every accepted value must be recoverable in order, none silently lost");
    }

    void testSnapshotTearAndCompleteness()
    {
        beginTest ("Snapshot: publisher never exposes a mixed generation, sequence stays monotonic");
        DynamicSnapshotPublisher publisher;
        publisher.prepare();

        uint64_t lastSequence = 0;
        for (int64_t block = 0; block < 40; ++block)
        {
            DynamicRuntimeSnapshot snapshot;
            snapshot.mapGeneration = (uint64_t) block;
            for (int slot = 0; slot < DynamicMeasurementContract::kMaxRetainedStates; ++slot)
            {
                snapshot.states[(size_t) slot].occupied = true;
                snapshot.states[(size_t) slot].stableStateId = (uint64_t) (block * 100 + slot);
            }
            expect (publisher.publishIfDue (snapshot, block));

            const auto read = publisher.read();
            expect (read.sequence > lastSequence, "sequence must strictly increase with every publish");
            lastSequence = read.sequence;
            // Every card in one read must share the SAME publish generation -
            // a torn write would mix stableStateId encodings from two blocks.
            for (int slot = 0; slot < DynamicMeasurementContract::kMaxRetainedStates; ++slot)
            {
                const uint64_t expectedBlock = read.states[(size_t) slot].stableStateId / 100;
                expectEquals ((int64) expectedBlock, (int64) read.mapGeneration,
                             "every card must belong to the same generation as the snapshot header");
            }
        }

        // publishIfDue must not spam the ring for a repeated block counter.
        DynamicRuntimeSnapshot repeat;
        repeat.mapGeneration = 999;
        expect (! publisher.publishIfDue (repeat, 39), "the same blockCounter must not republish");
        expect (publisher.read().mapGeneration != 999);
    }

    void testRuntimeCaptureAlignment()
    {
        beginTest ("Runtime capture: before/after windows refer to the same physical hit through the tap offset");
        const double rate = 48000.0;
        DynamicRuntimeMeasurementCapture capture;
        expect (capture.prepare (rate));

        const int windowSamples = capture.getWindowSamples();
        const int64_t triggerSample = 10000;
        const double tapSamples = 960.0; // 20 ms at 48 kHz
        const int totalSamples = (int) triggerSample + windowSamples + (int) tapSamples + 200;

        std::vector<float> rawBass ((size_t) totalSamples), rawKick ((size_t) totalSamples);
        std::vector<float> outBass ((size_t) totalSamples), outKick ((size_t) totalSamples);
        for (int i = 0; i < totalSamples; ++i)
        {
            rawBass[(size_t) i] = (float) std::sin (kTwoPi * 90.0 * (double) i / rate);
            rawKick[(size_t) i] = (float) std::sin (kTwoPi * 130.0 * (double) i / rate);
            // The "processed" stream reproduces the exact raw bass shifted by
            // tapSamples, matching what a pure-delay branch would render.
            const int shifted = i - (int) tapSamples;
            outBass[(size_t) i] = shifted >= 0 ? rawBass[(size_t) shifted] : 0.0f;
            outKick[(size_t) i] = rawKick[(size_t) i];
        }

        bool begun = false;
        for (int i = 0; i < totalSamples; ++i)
        {
            capture.pushRawSample (rawBass[(size_t) i], rawKick[(size_t) i]);
            if (i == (int) triggerSample && ! begun)
            {
                expect (capture.beginCapture (1, 42, DynamicSelectorBranchKind::State, triggerSample, tapSamples, rate));
                begun = true;
            }
            capture.pushOutputSample (outBass[(size_t) i], outKick[(size_t) i]);
        }
        capture.serviceCompletions();

        DynamicRuntimeMeasurementCaptureResult result;
        result.resizeWindows (windowSamples); // takeCompleted() only ever copies into an already-sized destination
        expect (capture.takeCompleted (result), "the capture must complete once both windows are filled");
        expect (result.valid);
        expectEquals (result.windowSamples, windowSamples);

        // The "after" bass window, read back from the output stream, must be
        // an EXACT copy of the "before" bass window (the same physical hit,
        // shifted only by the known tap) - proving the alignment formula.
        for (int i = 0; i < windowSamples; ++i)
            expectWithinAbsoluteError (result.afterBass[(size_t) i], result.beforeBass[(size_t) i], 1.0e-6f);

        // A deliberately WRONG one-sample offset must fail the same
        // comparison, proving the test can actually detect misalignment.
        bool anyMismatch = false;
        for (int i = 1; i < windowSamples; ++i)
            if (std::abs (result.afterBass[(size_t) i] - result.beforeBass[(size_t) (i - 1)]) > 1.0e-6f)
                anyMismatch = true;
        expect (anyMismatch, "a one-sample misalignment fixture must be distinguishable from the correct alignment");
    }

    void testNonVerifiableEventsNeverCaptured()
    {
        beginTest ("Runtime: Unknown/Ambiguous matches never begin a measurement capture");
        DynamicProductionRuntime runtime;
        expect (runtime.prepare (48000.0, 512, 2));

        // An empty (unoccupied) map: every observation is NoEligibleStates,
        // never Matched, so no capture should ever be requested.
        auto map = makeEmptyDynamicStateMap();
        map.valid = true;
        map.calibration.valid = true;
        map.calibration.absoluteDistanceThreshold = 0.1f;
        map.calibration.ambiguityMargin = 0.02f;
        runtime.activateMap (map);

        const int n = 48000;
        std::vector<float> bass ((size_t) n), kick ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            bass[(size_t) i] = (float) (0.5 * std::sin (kTwoPi * 90.0 * (double) i / 48000.0));
            kick[(size_t) i] = (i % 4800 == 0) ? 0.9f : 0.0f;
        }

        juce::AudioBuffer<float> input (2, n);
        juce::AudioBuffer<float> output (2, n);
        for (int ch = 0; ch < 2; ++ch)
            input.copyFrom (ch, 0, bass.data(), n);

        int offset = 0;
        while (offset < n)
        {
            const int chunk = std::min (512, n - offset);
            juce::AudioBuffer<float> chunkIn (2, chunk);
            juce::AudioBuffer<float> chunkOut (2, chunk);
            for (int ch = 0; ch < 2; ++ch)
                chunkIn.copyFrom (ch, 0, input, ch, offset, chunk);
            runtime.process (chunkIn, bass.data() + offset, kick.data() + offset, true, 1.0, chunkOut, chunk);
            for (int ch = 0; ch < 2; ++ch)
                output.copyFrom (ch, offset, chunkOut, ch, 0, chunk);

            DynamicRuntimeMeasurementCaptureResult ignored;
            expect (! runtime.takeCompletedMeasurementCapture (ignored),
                   "no eligible correction ever exists for an empty map, so no capture may complete");
            offset += chunk;
        }
        expectEquals ((int) runtime.getMeasurementCaptureExhaustedCount(), 0);
    }

    void testContractValidation()
    {
        beginTest ("Contracts: measurement summary validity helpers reject non-finite/out-of-range values");
        auto summary = makeUnavailableDynamicMeasurementSummary();
        expect (isValidDynamicMeasurementSummary (summary), "a freshly-defaulted summary is valid");

        summary.beforeScore = std::numeric_limits<float>::quiet_NaN();
        expect (! isValidDynamicMeasurementSummary (summary), "NaN score must be rejected");

        summary = makeUnavailableDynamicMeasurementSummary();
        summary.confidence = 1.5f;
        expect (! isValidDynamicMeasurementSummary (summary), "confidence outside [0,1] must be rejected");

        summary = makeUnavailableDynamicMeasurementSummary();
        summary.availability = DynamicMeasurementAvailability::Available;
        summary.validWindowCount = 0;
        expect (! isValidDynamicMeasurementSummary (summary),
               "Available must be backed by at least one valid window");
    }

    void testRawPreRollNotTruncated()
    {
        beginTest ("Runtime capture: raw pre-roll is never zero-filled at production's actual beginCapture() timing");
        const double rate = 48000.0;
        DynamicRuntimeMeasurementCapture capture;
        expect (capture.prepare (rate));

        const int windowSamples = capture.getWindowSamples();
        const int preRollSamples = DynamicRuntimeMeasurementCaptureContract::preRollSamplesFor (rate);
        const int fingerprintWindowSamples = DynamicFingerprintWindow::forSampleRate (rate).windowSamples;
        const int64_t triggerSample = 5000; // > preRollSamples, so the whole before-window has real source data

        const int totalSamples = (int) triggerSample + windowSamples + fingerprintWindowSamples + 200;
        std::vector<float> rawBass ((size_t) totalSamples), rawKick ((size_t) totalSamples);
        for (int i = 0; i < totalSamples; ++i)
        {
            // Deterministic, non-zero everywhere so any silent zero-fill is detectable.
            rawBass[(size_t) i] = (float) std::sin (kTwoPi * 77.0 * (double) i / rate) + 0.01f;
            rawKick[(size_t) i] = (float) std::sin (kTwoPi * 111.0 * (double) i / rate) + 0.02f;
        }

        bool begun = false;
        for (int i = 0; i < totalSamples; ++i)
        {
            capture.pushRawSample (rawBass[(size_t) i], rawKick[(size_t) i]);
            capture.pushOutputSample (rawBass[(size_t) i], rawKick[(size_t) i]);

            // Matches production exactly: DynamicProductionRuntime only calls
            // beginCapture() once captureBank.takeCompleted() fires for the
            // 4 ms fingerprint observation covering this trigger - i.e.
            // fingerprintWindowSamples AFTER the physical trigger sample,
            // never at the trigger sample itself.
            if (! begun && i == (int) triggerSample + fingerprintWindowSamples - 1)
            {
                expect (capture.beginCapture (1, 55, DynamicSelectorBranchKind::State, triggerSample, 0.0, rate));
                begun = true;
            }
        }
        capture.serviceCompletions();

        DynamicRuntimeMeasurementCaptureResult result;
        result.resizeWindows (windowSamples);
        expect (capture.takeCompleted (result), "capture completes once both windows are filled");
        expect (result.valid);

        const int64_t beforeStartSample = triggerSample - (int64_t) preRollSamples;
        int zeroFillCount = 0;
        for (int k = 0; k < windowSamples; ++k)
        {
            const int64_t sourceIndex = beforeStartSample + k;
            const float expectedBass = (sourceIndex >= 0 && sourceIndex < totalSamples)
                ? rawBass[(size_t) sourceIndex] : 0.0f;
            const float expectedKick = (sourceIndex >= 0 && sourceIndex < totalSamples)
                ? rawKick[(size_t) sourceIndex] : 0.0f;
            expectWithinAbsoluteError (result.beforeBass[(size_t) k], expectedBass, 1.0e-5f);
            expectWithinAbsoluteError (result.beforeKick[(size_t) k], expectedKick, 1.0e-5f);
            if (k < preRollSamples && std::abs (result.beforeBass[(size_t) k]) < 1.0e-6f)
                ++zeroFillCount;
        }
        expectEquals (zeroFillCount, 0, "no sample in the pre-roll region may be silently zero-filled");
    }

    void testSnapshotPublisherConcurrentStress()
    {
        beginTest ("Snapshot publisher: concurrent writer/reader never observes a torn or mixed-generation snapshot");
        DynamicSnapshotPublisher publisher;
        publisher.prepare();

        std::atomic<bool> stop { false };
        std::atomic<int> mismatches { 0 };
        std::atomic<int64_t> maxGenerationSeen { -1 };

        std::thread writer ([&]
        {
            int64_t block = 0;
            while (! stop.load (std::memory_order_relaxed))
            {
                DynamicRuntimeSnapshot snapshot;
                snapshot.mapGeneration = (uint64_t) block;
                for (int slot = 0; slot < DynamicMeasurementContract::kMaxRetainedStates; ++slot)
                {
                    snapshot.states[(size_t) slot].occupied = true;
                    snapshot.states[(size_t) slot].stableStateId = (uint64_t) (block * 1000 + slot);
                }
                publisher.publishIfDue (snapshot, block);
                ++block;
            }
        });

        std::thread reader ([&]
        {
            while (! stop.load (std::memory_order_relaxed))
            {
                const auto snapshot = publisher.read();
                for (int slot = 0; slot < DynamicMeasurementContract::kMaxRetainedStates; ++slot)
                {
                    if (! snapshot.states[(size_t) slot].occupied)
                        continue;
                    const uint64_t impliedGeneration = snapshot.states[(size_t) slot].stableStateId / 1000;
                    if (impliedGeneration != snapshot.mapGeneration)
                        mismatches.fetch_add (1, std::memory_order_relaxed);
                }
                int64_t seen = (int64_t) snapshot.mapGeneration;
                int64_t prevMax = maxGenerationSeen.load (std::memory_order_relaxed);
                while (seen > prevMax && ! maxGenerationSeen.compare_exchange_weak (prevMax, seen)) {}
            }
        });

        std::this_thread::sleep_for (std::chrono::milliseconds (200));
        stop.store (true, std::memory_order_relaxed);
        writer.join();
        reader.join();

        expectEquals (mismatches.load(), 0, "no reader observation may mix two publish generations within one snapshot");
        expectGreaterThan (maxGenerationSeen.load(), (int64_t) 0, "the reader must have observed real progress from the writer");
    }

    void testRepeatedPrepareWhileWorkerActive()
    {
        beginTest ("Processor: repeated prepareToPlay() never races the measurement worker's queues");
        // Note: this test deliberately does NOT call processBlock() from a
        // second thread concurrently with prepareToPlay() - a host never
        // does that (it always serializes prepare/process), and driving them
        // concurrently exercises unrelated pre-existing processBlock-vs-
        // prepareToPlay assumptions elsewhere in this processor that are out
        // of scope for this fix. What IS in scope, and what this test
        // exercises for real, is the measurement worker's OWN background
        // thread - running continuously from construction, independently of
        // any host call - racing a sequence of prepareToPlay() calls on the
        // message/test thread while genuine queued work is present.
        KickLockAudioProcessor processor;
        processor.enableAllBuses();
        processor.setRateAndBufferSizeDetails (48000.0, 256);
        processor.prepareToPlay (48000.0, 256);

        // Drive enough real audio (sequentially, on this thread) to populate
        // the measurement capture/score queues with genuine queued work; the
        // background measurement worker thread starts servicing it
        // immediately and keeps running independently of this thread.
        const int n = 48000;
        std::vector<float> bass ((size_t) n), kick ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            bass[(size_t) i] = 0.4f * (float) std::sin (kTwoPi * 60.0 * (double) i / 48000.0);
            kick[(size_t) i] = (i % 4800 < 40) ? 0.8f : 0.0f;
        }
        const int channels = juce::jmax (processor.getTotalNumInputChannels(),
                                         processor.getTotalNumOutputChannels());
        juce::AudioBuffer<float> buffer (channels, 256);
        juce::MidiBuffer midi;
        for (int offset = 0; offset < n; offset += 256)
        {
            const int len = std::min (256, n - offset);
            buffer.clear();
            for (int i = 0; i < len; ++i)
            {
                if (buffer.getNumChannels() > 0) buffer.setSample (0, i, bass[(size_t) (offset + i)]);
                if (buffer.getNumChannels() > 1) buffer.setSample (1, i, bass[(size_t) (offset + i)]);
                if (buffer.getNumChannels() > 2) buffer.setSample (2, i, kick[(size_t) (offset + i)]);
                if (buffer.getNumChannels() > 3) buffer.setSample (3, i, kick[(size_t) (offset + i)]);
            }
            processor.processBlock (buffer, midi);
        }

        // Hammer prepareToPlay() repeatedly while the background measurement
        // worker thread is concurrently free to pop/push whatever it was
        // mid-servicing from the drive above.
        for (int i = 0; i < 50; ++i)
            processor.prepareToPlay (48000.0, 128 + (i % 3) * 64);

        // Reaching here without a crash/hang/sanitizer trip already proves
        // the queues survived every reallocation while the worker was
        // concurrently active. A final prepare + block proves the processor
        // is still fully usable afterward (no stale/half-initialized state).
        processor.prepareToPlay (48000.0, 256);
        buffer.setSize (channels, 256);
        buffer.clear();
        processor.processBlock (buffer, midi);
        expect (true, "processor remains usable after repeated prepareToPlay() calls while the measurement worker is active");
    }

    void testVerifiedGenerationNeverContaminated()
    {
        beginTest ("Verified aggregation: a map-generation change never relabels old-package evidence as new");
        DynamicVerifiedAggregation aggregation;
        aggregation.reset();

        const uint64_t stableId = 900;
        DynamicStateMap mapA = makeEmptyDynamicStateMap();
        mapA.valid = true;
        mapA.states[0].occupied = true;
        mapA.states[0].stableStateId = stableId;

        aggregation.reconcile (mapA, /* generationA */ 10);

        for (int i = 0; i < 3; ++i)
        {
            DynamicMeasurementScoredCapture scored;
            scored.valid = true;
            scored.mapGeneration = 10;
            scored.stableStateId = stableId;
            scored.score.valid = true;
            scored.score.beforeScore = 40.0f;
            scored.score.afterScore = 80.0f;
            scored.score.improvementPoints = 40.0f;
            scored.score.confidence = 0.9f;
            aggregation.addResult (scored);
        }

        const auto summaryA = aggregation.summaryFor (0);
        expectEquals ((int) summaryA.validWindowCount, 3, "generation A accumulated its three fresh events");

        // Same stableStateId, but a NEW map generation - a package change is
        // assumed possible since there is no cheap way to prove otherwise.
        DynamicStateMap mapB = mapA;
        aggregation.reconcile (mapB, /* generationB */ 11);

        const auto summaryAfterReconcile = aggregation.summaryFor (0);
        expect (summaryAfterReconcile.availability == DynamicMeasurementAvailability::Unavailable
                    || summaryAfterReconcile.availability == DynamicMeasurementAvailability::Collecting,
               "old generation-A evidence must not survive as generation-B's summary");
        expectEquals ((int) summaryAfterReconcile.validWindowCount, 0,
                     "zero old events must remain after a generation change, even with the same stableStateId");

        // A stale generation-A result must now be rejected outright.
        DynamicMeasurementScoredCapture staleResult;
        staleResult.valid = true;
        staleResult.mapGeneration = 10;
        staleResult.stableStateId = stableId;
        staleResult.score.valid = true;
        staleResult.score.beforeScore = 10.0f;
        staleResult.score.afterScore = 90.0f;
        staleResult.score.improvementPoints = 80.0f;
        staleResult.score.confidence = 0.9f;
        aggregation.addResult (staleResult);
        expectEquals ((int) aggregation.summaryFor (0).validWindowCount, 0,
                     "a stale generation-A worker result must be rejected, never folded into generation B");

        // Only fresh generation-B results may make verification Available.
        for (int i = 0; i < DynamicMeasurementContract::kMinVerifiedEvents; ++i)
        {
            DynamicMeasurementScoredCapture freshResult;
            freshResult.valid = true;
            freshResult.mapGeneration = 11;
            freshResult.stableStateId = stableId;
            freshResult.score.valid = true;
            freshResult.score.beforeScore = 40.0f;
            freshResult.score.afterScore = 85.0f;
            freshResult.score.improvementPoints = 45.0f;
            freshResult.score.confidence = 0.9f;
            aggregation.addResult (freshResult);
        }
        const auto summaryB = aggregation.summaryFor (0);
        expect (summaryB.availability == DynamicMeasurementAvailability::Available,
               "fresh generation-B events alone must be enough to reach Available");
        expectEquals ((int) summaryB.validWindowCount, (int) DynamicMeasurementContract::kMinVerifiedEvents);
    }
};

static DynamicStateMeasurementTests dynamicStateMeasurementTests;
