#include "TestCommon.h"

#include "DynamicReleaseFixture.h"
#include "ui/DynamicWorkspaceHelpers.h"

#include <chrono>

// =============================================================================
// Phase 12: the critical end-to-end acceptance path, through the REAL
// PluginProcessor with a REAL, fully async Learn (not a pre-formed map) and a
// REAL multi-family audio fixture:
//
//   Learn -> Apply -> select a recognized Global-only State -> see the exact
//   reason -> Promote to Manual -> edit Delay/Frequency/Q -> safe Service-
//   mediated audition becomes audible -> fresh Verified collection begins ->
//   save/reload -> Revert.
//
// Reuses the exact same shared fixture (makeDynamicReleaseFixture(), five
// interleaved families incl. RecognizedGlobalOnly) and real-Learn technique
// DynamicReleaseGateTests.cpp's own "Actual Learn..." tests already use and
// have already proven reliable - this is deliberate: the interleaved
// families give the targeted State's own branch genuine "not currently
// selected" gaps whenever one of the other four families is selected
// instead, which the safe-retune design's commit path depends on (see
// DynamicProductionRuntime.h's configurePackagesIfNeeded() - a State that is
// matched with literally zero gap, ever, may defer an edit until a gap
// occurs; this is a documented limitation, not exercised by this realistic,
// multi-note fixture). Host-transport loop-wrap is not re-simulated here -
// see DynamicReleaseGateTests.cpp's transport-boundary test and
// DynamicStateEditAuditionTests.cpp's loop-wrap-during-edit test.
// =============================================================================

namespace
{
    // Same localized click metric DynamicReleaseGateTests.cpp uses for its own
    // "real production transition continuity" tests: a naive global
    // max-adjacent-sample-delta is meaningless against a real percussive
    // fixture (a legitimate kick/bass transient attack alone can exceed 0.3
    // in adjacent-sample delta with zero processing defect). This scores a
    // specific boundary sample against the RMS of its own local derivative
    // and signal energy, so it only fires on a discontinuity that stands out
    // from the surrounding, already-transient-laden material.
    constexpr int kContinuityWindowSamples = 24;
    constexpr float kContinuitySilenceEpsilon = 1.0e-4f;
    constexpr float kMaximumLocalizedTransitionScore = 4.0f;

    float localizedTransitionScore (const std::vector<float>& samples, int boundary)
    {
        if (boundary < kContinuityWindowSamples || boundary + kContinuityWindowSamples >= (int) samples.size())
            return 0.0f; // too close to an edge to score; not this test's concern

        double derivativeEnergy = 0.0;
        double signalEnergy = 0.0;
        int derivativeCount = 0;
        for (int i = boundary - kContinuityWindowSamples + 1; i < boundary; ++i)
        {
            const float derivative = samples[(size_t) i] - samples[(size_t) (i - 1)];
            derivativeEnergy += (double) derivative * derivative;
            signalEnergy += (double) samples[(size_t) i] * samples[(size_t) i];
            ++derivativeCount;
        }
        for (int i = boundary + 2; i < boundary + kContinuityWindowSamples; ++i)
        {
            const float derivative = samples[(size_t) i] - samples[(size_t) (i - 1)];
            derivativeEnergy += (double) derivative * derivative;
            signalEnergy += (double) samples[(size_t) i] * samples[(size_t) i];
            ++derivativeCount;
        }

        const float derivativeRms = (float) std::sqrt (derivativeEnergy / (double) std::max (1, derivativeCount));
        const float signalRms = (float) std::sqrt (signalEnergy / (double) std::max (1, derivativeCount));
        const float scale = std::max ({ kContinuitySilenceEpsilon, derivativeRms * 2.0f, signalRms * 0.01f });
        return std::abs (samples[(size_t) boundary] - samples[(size_t) (boundary - 1)]) / scale;
    }

    void setParameter (KickLockAudioProcessor& processor, const char* id, float value)
    {
        if (auto* parameter = processor.apvts.getParameter (id))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    }

    void prepare (KickLockAudioProcessor& processor, double sampleRate, int blockSize)
    {
        processor.enableAllBuses();
        processor.setRateAndBufferSizeDetails (sampleRate, blockSize);
        processor.prepareToPlay (sampleRate, blockSize);
    }

    juce::String learnDiagnostics (KickLockAudioProcessor& processor)
    {
        const auto progress = processor.getLearnProgress();
        const auto pending = processor.getPendingLearnResult();
        return "state=" + juce::String ((int) processor.getLearnState())
             + " captured=" + juce::String (progress.capturedHits)
             + " drained=" + juce::String (progress.drainedHits)
             + " message=" + pending.message;
    }

    bool waitForLearnCapturing (KickLockAudioProcessor& processor, int blockSize)
    {
        const int channels = juce::jmax (processor.getTotalNumInputChannels(), processor.getTotalNumOutputChannels());
        juce::AudioBuffer<float> silence (channels, blockSize);
        juce::MidiBuffer midi;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (processor.getLearnState() == LearnState::Capturing)
                return true;
            if (! learnStateIsBusy (processor.getLearnState()))
                return false;
            silence.clear();
            processor.processBlock (silence, midi);
            juce::Thread::sleep (2);
        }
        return processor.getLearnState() == LearnState::Capturing;
    }

    bool waitForLearnResolution (KickLockAudioProcessor& processor)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (! learnStateIsBusy (processor.getLearnState()))
                return true;
            juce::Thread::sleep (2);
        }
        return ! learnStateIsBusy (processor.getLearnState());
    }

    void driveProcessor (KickLockAudioProcessor& processor, const DynamicReleaseFixture& fixture, int blockSize,
                         std::vector<float>* output = nullptr, std::vector<DynamicRuntimeSnapshot>* snapshots = nullptr)
    {
        const int channels = juce::jmax (processor.getTotalNumInputChannels(), processor.getTotalNumOutputChannels());
        juce::AudioBuffer<float> buffer (channels, blockSize);
        juce::MidiBuffer midi;
        for (int offset = 0; offset < (int) fixture.bass.size(); offset += blockSize)
        {
            const int n = std::min (blockSize, (int) fixture.bass.size() - offset);
            buffer.clear();
            for (int i = 0; i < n; ++i)
            {
                const auto b = fixture.bass[(size_t) (offset + i)];
                const auto k = fixture.kick[(size_t) (offset + i)];
                buffer.setSample (0, i, b);
                if (channels > 1) buffer.setSample (1, i, b);
                if (channels > 2) buffer.setSample (2, i, k);
                if (channels > 3) buffer.setSample (3, i, k);
            }
            processor.processBlock (buffer, midi);
            if (output != nullptr)
                for (int i = 0; i < n; ++i)
                    output->push_back (buffer.getSample (0, i));
            if (snapshots != nullptr)
                snapshots->push_back (processor.getDynamicRuntimeSnapshotForTesting());
        }
    }

    bool allFiniteAndBounded (const std::vector<float>& samples)
    {
        for (const float sample : samples)
            if (! std::isfinite (sample) || std::abs (sample) > 4.0f)
                return false;
        return true;
    }
}

class DynamicManualWorkflowE2ETests : public juce::UnitTest
{
public:
    DynamicManualWorkflowE2ETests()
        : juce::UnitTest ("DynamicManualWorkflowE2E", "Dynamic State Map") {}

    void runTest() override
    {
        beginTest ("Learn -> Apply -> select Global-only -> Promote -> edit -> audible via safe audition -> Verified -> persist -> Revert");
        {
            const int blockSize = 256;
            const auto learnFixture = makeDynamicReleaseFixture (48000.0);
            const auto runtimeFixture = makeDynamicReleaseFixture (48000.0);

            KickLockAudioProcessor processor;
            prepare (processor, 48000.0, blockSize);
            setParameter (processor, "correction_mode", 1.0f);
            setParameter (processor, "dynamic_strength", 1.0f);

            // ---- Learn ----
            expect (processor.beginLearn());
            expect (waitForLearnCapturing (processor, blockSize), learnDiagnostics (processor));
            driveProcessor (processor, learnFixture, blockSize);
            expect (processor.stopLearn());
            expect (waitForLearnResolution (processor), learnDiagnostics (processor));
            expect (processor.getLearnState() == LearnState::ResultReady, learnDiagnostics (processor));

            const auto pending = processor.getPendingLearnResult();
            expect (pending.hasDynamicStateMap && isStructurallyValidDynamicStateMap (pending.dynamicMap));

            // ---- Apply ----
            expect (processor.applyLatestLearnResult(), processor.getLearnApplyBlockedReason());
            driveProcessor (processor, runtimeFixture, blockSize);
            expect (processor.getActiveDynamicMapSourceForTesting() == DynamicMapSource::NewDynamicStateMap);

            // ---- select recognized Global-only State; see the exact reason ----
            const auto applied = processor.getMessageOwnedDynamicStateMapForTesting();
            uint64_t targetId = 0;
            for (const auto& state : applied.states)
                if (state.occupied && state.origin == DynamicStateOrigin::Auto && ! state.hasLearnedPackage)
                {
                    targetId = state.stableStateId;
                    break;
                }
            expect (targetId != 0, "the RecognizedGlobalOnly family forms a recognized State with no learned package");
            const auto beforePromotion = processor.getDynamicStateForUi (targetId);
            expect (beforePromotion.occupied);
            expect (dynamicStateCorrectionStatusLine (beforePromotion).contains ("no automatic correction"),
                    "the producer-facing reason names exactly this condition");

            // ---- Promote to Manual ----
            KickLockAudioProcessor::DynamicStateEditRequest promote;
            promote.kind = KickLockAudioProcessor::DynamicStateEditKind::PromoteToManual;
            promote.stableStateId = targetId;
            const auto promoteOutcome = processor.applyDynamicStateEdit (promote);
            expect (promoteOutcome.success, "promotion succeeds for a recognized State with enough hits");
            expect (processor.getDynamicStateForUi (targetId).origin == DynamicStateOrigin::Manual);
            driveProcessor (processor, runtimeFixture, blockSize);

            // ---- edit Delay/Frequency/Q ----
            KickLockAudioProcessor::DynamicStateEditRequest edit;
            edit.kind = KickLockAudioProcessor::DynamicStateEditKind::SetManualTrim;
            edit.stableStateId = targetId;
            edit.trim = { -1.0f, 2.0f, -0.2f };
            expect (processor.applyDynamicStateEdit (edit).success, "the manual edit is accepted");

            // ---- safe Service-mediated audition becomes audible; no click ----
            // The fixture interleaves five families, so this State's own
            // branch gets genuine gaps whenever another family is selected -
            // exactly the condition the safe commit path depends on. Replay
            // the fixture a few times to give it several such gaps. Click
            // safety is scored with the same localized transition metric
            // DynamicReleaseGateTests.cpp uses for its own real-audio
            // transition tests, not a raw global max-adjacent-delta - this
            // fixture's own legitimate kick transients regularly exceed 0.3
            // in adjacent-sample delta with zero processing defect, so a
            // naive global max is not a meaningful click detector here.
            std::vector<float> output;
            std::vector<DynamicRuntimeSnapshot> snapshots;
            for (int pass = 0; pass < 4; ++pass)
                driveProcessor (processor, runtimeFixture, blockSize, &output, &snapshots);

            expect (allFiniteAndBounded (output), "no NaN/Inf/unbounded excursion anywhere in the audible output");

            bool everSelected = false;
            float worstTransitionScore = 0.0f;
            const int samplesPerSnapshot = blockSize;
            for (size_t i = 0; i < snapshots.size(); ++i)
            {
                if (snapshots[i].selectedSemanticStateId == targetId)
                    everSelected = true;
                if (i == 0)
                    continue;
                const bool touchesTarget = snapshots[i - 1].selectedSemanticStateId == targetId
                    || snapshots[i].selectedSemanticStateId == targetId;
                if (touchesTarget && snapshots[i - 1].activeBranchKind != snapshots[i].activeBranchKind)
                {
                    const int boundary = (int) i * samplesPerSnapshot;
                    worstTransitionScore = std::max (worstTransitionScore, localizedTransitionScore (output, boundary));
                }
            }
            expectLessThan (worstTransitionScore, kMaximumLocalizedTransitionScore,
                            "every transition into/out of the edited State's branch stays below the localized click threshold");
            expect (everSelected, "the edited State is confidently recognized and selected at some point, not permanently fallen back");

            // ---- fresh Verified collection begins ----
            {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (10);
                bool collecting = false;
                while (std::chrono::steady_clock::now() < deadline && ! collecting)
                {
                    driveProcessor (processor, runtimeFixture, blockSize);
                    const auto verified = processor.getDynamicVerifiedMeasurementForTesting (targetId);
                    if (verified.availability == DynamicMeasurementAvailability::Collecting
                        || verified.availability == DynamicMeasurementAvailability::Available)
                        collecting = true;
                }
                expect (collecting, "Verified evidence starts accumulating for the newly-edited package");
            }

            // ---- save/reload ----
            juce::MemoryBlock savedState;
            processor.getStateInformation (savedState);

            KickLockAudioProcessor restored;
            prepare (restored, 48000.0, blockSize);
            restored.setStateInformation (savedState.getData(), (int) savedState.getSize());
            driveProcessor (restored, runtimeFixture, blockSize);

            const auto reloadedState = restored.getDynamicStateForUi (targetId);
            expect (reloadedState.occupied);
            expect (reloadedState.origin == DynamicStateOrigin::Manual);
            expectWithinAbsoluteError (reloadedState.manualTrim.delayTrimMs, -1.0f, 1.0e-5f);
            expect (restored.getDynamicVerifiedMeasurementForTesting (targetId).availability
                    == DynamicMeasurementAvailability::Unavailable,
                    "Verified restarts fresh after reload - it is never persisted");

            // ---- Revert (on the original processor, not the reloaded one) ----
            // ensureRevertBundleCaptured() is idempotent per session - Apply
            // itself already established the one revert checkpoint (back to
            // the pre-Apply, no-map state), so Promote/Edit never get their
            // own separate checkpoint on top of it. Revert therefore undoes
            // the *entire* session back to before this Apply, exactly as
            // DynamicReleaseGateTests.cpp's own Apply/Revert tests already
            // establish - not merely the last edit. This is confirmed
            // pre-existing, documented behavior (see DYNAMIC_DESIGN_FREEZE.md
            // "Revert bundle"), not a Phase 12 regression.
            expect (processor.revertLatestFix());
            // getDynamicStateForUi()/getActiveDynamicMapSourceForTesting()
            // gate on activeDynamicMapSource, an audio-thread atomic that
            // only updates once a block has actually been processed after
            // the revert.
            {
                const int channels = juce::jmax (processor.getTotalNumInputChannels(), processor.getTotalNumOutputChannels());
                juce::AudioBuffer<float> silence (channels, blockSize);
                silence.clear();
                juce::MidiBuffer midi;
                processor.processBlock (silence, midi);
            }
            expect (processor.getActiveDynamicMapSourceForTesting() != DynamicMapSource::NewDynamicStateMap,
                    "Revert undoes the whole session's Apply, not just the latest edit");
            const auto afterRevertMessageOwned = processor.getMessageOwnedDynamicStateMapForTesting();
            bool targetStillOccupied = false;
            for (const auto& state : afterRevertMessageOwned.states)
                if (state.occupied && state.stableStateId == targetId)
                    targetStillOccupied = true;
            expect (! targetStillOccupied, "the promoted/edited State no longer exists once the whole Apply is reverted");
        }
    }
};

static DynamicManualWorkflowE2ETests dynamicManualWorkflowE2ETests;
