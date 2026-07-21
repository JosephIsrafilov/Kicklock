#include "TestCommon.h"
#include "PluginProcessor.h"
#include "ui/DynamicUiHelpers.h"
#include "ui/DynamicWorkspace.h"

#include <limits>
#include <type_traits>

namespace
{
    DynamicState makeState (uint64_t id, bool stable = true, bool correction = true)
    {
        DynamicState state;
        state.occupied = true;
        state.stableStateId = id;
        state.fingerprint.valid = true;
        state.fingerprint.featureCount = DynamicStateMapContract::kMaxFingerprintFeatures;
        state.origin = DynamicStateOrigin::Auto;
        state.evidence = stable ? DynamicStateEvidence::Stable : DynamicStateEvidence::Candidate;
        state.hitCount = stable ? DynamicStateMapContract::kStableAutoMinimumRepeatableHits
                                : DynamicStateMapContract::kCandidateMinimumRepeatableHits;
        state.repeatability = 0.8f;
        state.ambiguity = 0.1f;
        state.hasLearnedPackage = correction;
        state.learnedPackage = { 0.0f, 90.0f, 0.7f };
        state.hasLikelyMidi = true;
        state.likelyMidi = 36;
        state.hasLikelyPitchHz = true;
        state.likelyPitchHz = 65.4f;
        return state;
    }

    DynamicStateMap makeMap (int count = 1)
    {
        DynamicStateMap map;
        map.valid = true;
        map.globalBase.learnedSampleRate = 48000.0;
        map.calibration = { true, 0.45f, 0.08f };
        for (int i = 0; i < count; ++i)
            map.states[(size_t) i] = makeState ((uint64_t) (100 + i));
        map.nextStateId = 1000;
        return map;
    }

    DynamicMeasurementSummary available (DynamicCorrectionAssessment assessment,
                                         float improvement = 4.0f)
    {
        DynamicMeasurementSummary summary;
        summary.availability = DynamicMeasurementAvailability::Available;
        summary.assessment = assessment;
        summary.validWindowCount = 3;
        summary.beforeScore = 40.0f;
        summary.afterScore = 44.0f;
        summary.improvementPoints = improvement;
        summary.confidence = 0.8f;
        return summary;
    }

    DynamicWorkspaceViewModel runtimeModel (int stateCount = 1)
    {
        DynamicWorkspaceViewModel model;
        model.mode = CorrectionMode::Dynamic;
        model.runtime.source = DynamicMapSource::NewDynamicStateMap;
        model.runtime.mapValid = true;
        model.runtime.stateCount = stateCount;
        model.runtime.sidechainPresent = true;
        for (int i = 0; i < stateCount; ++i)
        {
            const auto state = makeState ((uint64_t) (100 + i));
            auto& card = model.runtime.states[(size_t) i];
            card.occupied = true;
            card.stableStateId = state.stableStateId;
            card.slot = i;
            card.origin = state.origin;
            card.evidence = state.evidence;
            card.enabled = state.enabled;
            card.hitCount = state.hitCount;
            card.repeatability = state.repeatability;
            card.ambiguity = state.ambiguity;
            card.hasCorrection = state.hasLearnedPackage;
            card.hasLikelyMidi = state.hasLikelyMidi;
            card.likelyMidi = state.likelyMidi;
            card.hasLikelyPitchHz = state.hasLikelyPitchHz;
            card.likelyPitchHz = state.likelyPitchHz;
            card.predicted = available (DynamicCorrectionAssessment::PredictedImprovement);
            card.verified = makeUnavailableDynamicMeasurementSummary();
            card.assessment = DynamicCorrectionAssessment::PredictedImprovement;
        }
        return model;
    }

    bool overlaps (juce::Rectangle<int> left, juce::Rectangle<int> right)
    {
        return left.getWidth() > 0 && right.getWidth() > 0 && left.intersects (right);
    }
}

class DynamicWorkspaceTests : public juce::UnitTest
{
public:
    DynamicWorkspaceTests() : juce::UnitTest ("DynamicWorkspace", "Phase10") {}

    void runTest() override
    {
        beginTest ("Source presentation keeps New, Legacy, and no-map separate");
        {
            expectEquals (dynamicMapSourceLabel (DynamicMapSource::NewDynamicStateMap), juce::String ("NEW MAP"));
            expectEquals (dynamicMapSourceLabel (DynamicMapSource::LegacyDynamicCompatibility), juce::String ("LEGACY MAP"));
            expectEquals (dynamicMapSourceLabel (DynamicMapSource::None), juce::String ("NO MAP"));

            auto legacy = runtimeModel();
            legacy.runtime.source = DynamicMapSource::LegacyDynamicCompatibility;
            expectEquals ((int) dynamicWorkspaceCards (legacy)[0].occupied, 0);
            legacy.runtime.source = DynamicMapSource::None;
            expectEquals ((int) dynamicWorkspaceCards (legacy)[0].occupied, 0);
        }

        beginTest ("Candidate, Stable, Manual, disabled, and bypassed cards retain their published roles");
        {
            auto candidate = makePreviewDynamicStateCard (makeState (17, false, true), 0,
                                                           available (DynamicCorrectionAssessment::CandidateOnly));
            expectEquals (dynamicStateEvidenceLabel (candidate.evidence), juce::String ("CANDIDATE"));
            expectEquals (dynamicCorrectionAssessmentLabel (candidate.assessment), juce::String ("Candidate Only"));
            expect (! candidate.hasCorrection);

            auto stable = makePreviewDynamicStateCard (makeState (18), 1,
                                                        available (DynamicCorrectionAssessment::PredictedImprovement));
            expect (stable.hasCorrection);
            expectEquals (dynamicCorrectionAssessmentLabel (stable.assessment), juce::String ("Predicted Improvement"));

            auto manualState = makeState (19);
            manualState.origin = DynamicStateOrigin::Manual;
            manualState.hasLearnedPackage = false;
            manualState.hasManualBasePackage = true;
            manualState.manualBasePackage = { 0.0f, 90.0f, 0.7f };
            manualState.hitCount = DynamicStateMapContract::kManualMinimumRepeatableHits;
            auto manual = makePreviewDynamicStateCard (manualState, 2,
                                                        available (DynamicCorrectionAssessment::Regressed, -6.0f));
            expectEquals (dynamicStateOriginLabel (manual.origin), juce::String ("MANUAL"));
            expectEquals (dynamicCorrectionAssessmentLabel (manual.assessment), juce::String ("Regressed"));

            stable.enabled = false;
            stable.assessment = DynamicCorrectionAssessment::Disabled;
            expectEquals ((int) dynamicWorkspaceColourRole (stable), (int) DynamicWorkspaceColourRole::Muted);
            stable.enabled = true;
            stable.bypassed = true;
            stable.assessment = DynamicCorrectionAssessment::Bypassed;
            expectEquals (dynamicCorrectionAssessmentLabel (stable.assessment), juce::String ("Bypassed"));
        }

        beginTest ("Measurements use published availability and never render unavailable scores");
        {
            const auto expected = std::array<juce::String, 5> {
                "Unavailable", "Collecting", "Available", "Insufficient Material", "Invalid"
            };
            for (int i = 0; i < (int) expected.size(); ++i)
                expectEquals (dynamicMeasurementAvailabilityLabel ((DynamicMeasurementAvailability) i), expected[(size_t) i]);

            DynamicMeasurementSummary collecting;
            collecting.availability = DynamicMeasurementAvailability::Collecting;
            collecting.validWindowCount = 1;
            expectEquals (formatDynamicMeasurementAvailability (collecting, true), juce::String ("Collecting 1/3"));
            collecting.validWindowCount = 2;
            expectEquals (formatDynamicMeasurementAvailability (collecting, true), juce::String ("Collecting 2/3"));

            DynamicMeasurementSummary unavailable;
            unavailable.beforeScore = 0.0f;
            unavailable.afterScore = 100.0f;
            expectEquals (formatDynamicBeforeAfter (unavailable), juce::String ("--"));
            expectEquals (formatDynamicImprovementPoints (-4.5f), juce::String ("-4.5 pts"));
            expectEquals (formatDynamicImprovementPoints (4.5f), juce::String ("+4.5 pts"));
            expectEquals (formatDynamicBeforeAfter (available (DynamicCorrectionAssessment::Neutral)),
                          juce::String ("40.0 -> 44.0"));
        }

        beginTest ("Measurement roles use each row's published availability and assessment");
        {
            auto predicted = available (DynamicCorrectionAssessment::PredictedImprovement);
            auto verified = makeUnavailableDynamicMeasurementSummary();
            expectEquals ((int) dynamicMeasurementColourRole (predicted),
                          (int) DynamicWorkspaceColourRole::Green);
            expectEquals ((int) dynamicMeasurementColourRole (verified),
                          (int) DynamicWorkspaceColourRole::Muted);

            verified.availability = DynamicMeasurementAvailability::Collecting;
            verified.validWindowCount = 1;
            expectEquals ((int) dynamicMeasurementColourRole (verified),
                          (int) DynamicWorkspaceColourRole::Amber);

            verified = available (DynamicCorrectionAssessment::Regressed, -5.0f);
            expectEquals ((int) dynamicMeasurementColourRole (verified),
                          (int) DynamicWorkspaceColourRole::Red);
        }

        beginTest ("Resolved Learn diagnostics remain visible when no Dynamic preview can form");
        {
            DynamicWorkspaceViewModel busy;
            busy.learnState = LearnState::Capturing;
            busy.learnStatusMessage = "Learning: captured 4, processed 2.";
            auto presentation = dynamicWorkspaceHeaderPresentation (busy);
            expectEquals (presentation.detail, busy.learnStatusMessage);

            LearnFinalizeResult insufficient;
            insufficient.message = "Need more timing-usable overlap material.";
            insufficient.diagnostics.capturedHits = 7;
            insufficient.diagnostics.rejectedPitchHits = 3;
            insufficient.diagnostics.analyzedHits = 2;

            DynamicWorkspaceViewModel model;
            model.learnState = LearnState::NotEnoughMaterial;
            model.learnStatusMessage = formatLearnFailureBody (insufficient);
            presentation = dynamicWorkspaceHeaderPresentation (model);
            expectEquals (presentation.source, juce::String ("LEARN RESULT"));
            expectEquals (presentation.status, juce::String ("NOT ENOUGH MATERIAL"));
            expect (presentation.detail.contains ("Need more timing-usable overlap material."));
            expect (presentation.detail.contains ("Captured: 7"));

            LearnFinalizeResult failed;
            failed.message = "Learn worker could not finalize the State map.";
            failed.diagnostics.capturedHits = 5;
            model.learnState = LearnState::Failed;
            model.learnStatusMessage = formatLearnFailureBody (failed);
            presentation = dynamicWorkspaceHeaderPresentation (model);
            expectEquals (presentation.status, juce::String ("LEARN FAILED"));
            expect (presentation.detail.contains ("Learn worker could not finalize the State map."));

            model.learnState = LearnState::ResultReady;
            model.learnStatusMessage = "The resolved Learn result did not contain a valid State map.";
            presentation = dynamicWorkspaceHeaderPresentation (model);
            expectEquals (presentation.status, juce::String ("NO PREVIEW"));
            expect (presentation.detail.contains ("did not contain a valid State map"));
        }

        beginTest ("Clean Scope keeps Dynamic workflow controls hidden across refreshes");
        {
            const auto visible = dynamicWorkspaceWorkflowVisibility (true, LearnState::ResultReady, false);
            expect (visible.applyLearn && visible.discard && visible.workspace
                    && visible.workspaceMapActions && visible.dynamicStrength);

            DynamicWorkspace workspace;
            workspace.setModel (runtimeModel());
            workspace.selectDetailState (100);
            for (int refresh = 0; refresh < 4; ++refresh)
            {
                const auto hidden = dynamicWorkspaceWorkflowVisibility (true, LearnState::ResultReady, true);
                expect (! hidden.applyLearn && ! hidden.discard && ! hidden.workspace
                        && ! hidden.workspaceMapActions && ! hidden.dynamicStrength);
                expectEquals ((int64) workspace.getSelectedDetailStableStateId(), (int64) 100);
            }

            const auto restored = dynamicWorkspaceWorkflowVisibility (true, LearnState::ResultReady, false);
            expect (restored.applyLearn && restored.discard && restored.workspace
                    && restored.workspaceMapActions && restored.dynamicStrength);
            expectEquals ((int64) workspace.getSelectedDetailStableStateId(), (int64) 100);
        }

        beginTest ("Stable State identity formats the full unsigned value exactly");
        {
            constexpr uint64_t highStateId = std::numeric_limits<uint64_t>::max();
            const auto identity = fullDynamicStateId (highStateId);
            expectEquals (identity, juce::String ("18446744073709551615"));
            expect (! identity.containsChar ('-'));

            const auto card = makePreviewDynamicStateCard (
                makeState (highStateId), 0, available (DynamicCorrectionAssessment::PredictedImprovement));
            expectEquals (card.stableStateId, highStateId);
            expect (! dynamicStateCardTitle (card).containsChar ('-'));
        }

        beginTest ("Preview is not applied, has no active State, and keeps Verified unavailable");
        {
            DynamicWorkspaceViewModel preview;
            preview.mode = CorrectionMode::Dynamic;
            preview.previewActive = true;
            preview.previewApplyAvailable = true;
            preview.previewMap = makeMap();
            preview.previewPredicted[0] = available (DynamicCorrectionAssessment::PredictedImprovement);
            const auto cards = dynamicWorkspaceCards (preview);
            expect (cards[0].occupied);
            expect (! cards[0].active && ! cards[0].selected);
            expectEquals ((int) cards[0].verified.availability,
                          (int) DynamicMeasurementAvailability::Unavailable);

            DynamicWorkspace workspace;
            workspace.setSize (900, 360);
            workspace.setModel (preview);
            expectEquals (workspace.getVisibleCardCount(), 1);
            expect (workspace.getName().containsIgnoreCase ("Dynamic"));
        }

        beginTest ("Pending preview is processor-owned and Clear/Revert use the New map path");
        {
            KickLockAudioProcessor processor;
            processor.prepareToPlay (48000.0, 128);
            const auto map = makeMap();

            LearnFinalizeResult pending;
            pending.valid = true;
            pending.hasDynamicStateMap = true;
            pending.dynamicMap = map;
            LearnSessionContext context;
            context.sampleRate = 48000.0;
            processor.setPendingLearnResultForTesting (pending, context);
            const auto preview = processor.getPendingDynamicLearnPreviewForUi();
            expect (preview.valid && preview.applyAvailable);
            expectEquals ((int64) preview.map.states[0].stableStateId, (int64) 100);
            expect (processor.discardLatestLearnResult());
            expect (! processor.getPendingDynamicLearnPreviewForUi().valid);

            expect (processor.publishDynamicStateMapForTesting (map));
            expect (processor.hasLearnedDynamicData());
            expect (processor.clearLearnedDynamicData());
            expect (! processor.hasLearnedDynamicData());
            expect (processor.hasRevertSnapshot());
            expect (processor.revertLatestFix());
            expect (processor.hasLearnedDynamicData());
        }

        beginTest ("Active and selected States remain distinct across branch and status presentation");
        {
            auto model = runtimeModel (2);
            model.runtime.selectedSemanticStateId = 100;
            model.runtime.activeSemanticStateId = 101;
            model.runtime.activeBranchKind = DynamicSelectorBranchKind::Service;
            auto cards = dynamicWorkspaceCards (model);
            expect (dynamicCardIsSelected (cards[0], model.runtime));
            expect (! dynamicCardIsActive (cards[0], model.runtime));
            expect (dynamicCardIsActive (cards[1], model.runtime));
            expectEquals (dynamicWorkspaceRuntimeStatus (model.runtime), juce::String ("ACTIVE SERVICE"));

            model.runtime.holdActive = true;
            expectEquals (dynamicWorkspaceRuntimeStatus (model.runtime), juce::String ("HOLD"));
            model.runtime.holdActive = false;
            model.runtime.fallbackActive = true;
            model.runtime.activeBranchKind = DynamicSelectorBranchKind::Global;
            model.runtime.activeSemanticStateId = 0;
            expect (dynamicCardIsSelected (cards[0], model.runtime));
            expect (! dynamicCardIsActive (cards[0], model.runtime));
            expectEquals (dynamicWorkspaceRuntimeStatus (model.runtime), juce::String ("GLOBAL FALLBACK"));
            model.runtime.bypassActive = true;
            expectEquals (dynamicWorkspaceRuntimeStatus (model.runtime), juce::String ("BYPASSED"));
        }

        beginTest ("Stable-ID detail selection survives slot movement and clears on removal");
        {
            DynamicWorkspace workspace;
            auto model = runtimeModel (2);
            workspace.setModel (model);
            workspace.selectDetailState (101);
            expectEquals ((int64) workspace.getSelectedDetailStableStateId(), (int64) 101);

            std::swap (model.runtime.states[0], model.runtime.states[1]);
            model.runtime.states[0].slot = 0;
            model.runtime.states[1].slot = 1;
            workspace.setModel (model);
            expectEquals ((int64) workspace.getSelectedDetailStableStateId(), (int64) 101);

            model.runtime.states[0] = {};
            model.runtime.states[1] = {};
            workspace.setModel (model);
            expectEquals ((int64) workspace.getSelectedDetailStableStateId(), (int64) 0);
        }

        beginTest ("Workspace layout keeps cards and actions within minimum, default, and wide bounds");
        {
            for (const auto size : { juce::Point<int> (900, 680), juce::Point<int> (1180, 820),
                                     juce::Point<int> (1600, 1000) })
            {
                DynamicWorkspace workspace;
                workspace.setSize (size.x, size.y);
                workspace.setModel (runtimeModel (8));
                expectEquals (workspace.getVisibleCardCount(), 8);
                expect (workspace.getCardColumnCount() >= 2 && workspace.getCardColumnCount() <= 4);
                const auto& bounds = workspace.getCardBoundsForTesting();
                for (const auto& card : bounds)
                    expect (card.getX() >= 0 && card.getY() >= 0 && card.getRight() <= size.x && card.getBottom() <= size.y);
                for (int left = 0; left < (int) bounds.size(); ++left)
                    for (int right = left + 1; right < (int) bounds.size(); ++right)
                        expect (! overlaps (bounds[(size_t) left], bounds[(size_t) right]));
            }
        }

        beginTest ("Workspace accepts stored snapshots only and pre-creates eight cards");
        {
            static_assert (! std::is_constructible_v<DynamicWorkspace, KickLockAudioProcessor&>);
            DynamicWorkspace workspace;
            // 8 State cards + Clear + Revert + the Phase 12 State Inspector
            // (a real, permanent child - not conditionally created).
            expectEquals (workspace.getNumChildComponents(), 11);
            for (int i = 0; i < 12; ++i)
            {
                workspace.setSize (900 + i * 10, 360 + i * 5);
                workspace.setModel (runtimeModel (i % 2 == 0 ? 1 : 8));
            }
            expectEquals (workspace.getNumChildComponents(), 11);
        }

        beginTest ("Editor and workspace survive Dynamic construction, resizing, and close/reopen");
        {
            KickLockAudioProcessor processor;
            processor.prepareToPlay (48000.0, 128);
            for (int i = 0; i < 3; ++i)
            {
                std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
                editor->setSize (900 + i * 140, 680 + i * 70);
                if (auto* mode = processor.apvts.getParameter ("correction_mode"))
                    mode->setValueNotifyingHost (mode->convertTo0to1 (1.0f));
                editor.reset();
            }
        }
    }
};

static DynamicWorkspaceTests dynamicWorkspaceTestsInstance;
