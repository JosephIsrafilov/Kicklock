#include "TestCommon.h"
#include "ui/DynamicStateInspector.h"

namespace
{
    DynamicFingerprintPrototype validFingerprint()
    {
        DynamicFingerprintPrototype fp;
        fp.valid = true;
        fp.featureCount = 4;
        fp.features[0] = 0.1f;
        fp.features[1] = -0.2f;
        fp.features[2] = 0.3f;
        fp.features[3] = -0.4f;
        return fp;
    }

    DynamicState autoState (uint64_t id, uint32_t hitCount)
    {
        DynamicState s;
        s.occupied = true;
        s.stableStateId = id;
        s.fingerprint = validFingerprint();
        s.hasLearnedPackage = true;
        s.learnedPackage = { 0.5f, 100.0f, 0.8f };
        s.hitCount = hitCount;
        s.evidence = hitCount >= 5 ? DynamicStateEvidence::Stable : DynamicStateEvidence::Candidate;
        return s;
    }

    DynamicWorkspaceViewModel modelWithAppliedMap (const DynamicState& selected)
    {
        DynamicWorkspaceViewModel model;
        model.runtime.source = DynamicMapSource::NewDynamicStateMap;
        model.selectedStableStateId = selected.stableStateId;
        model.selectedState = selected;
        model.hasSelectedState = selected.occupied;
        model.sampleRate = 48000.0;
        return model;
    }
}

class DynamicStateInspectorHelpersTests : public juce::UnitTest
{
public:
    DynamicStateInspectorHelpersTests()
        : juce::UnitTest ("DynamicStateInspectorHelpers", "Dynamic State Map") {}

    void runTest() override
    {
        beginTest ("Preview/Legacy/No-map disable controls with a specific reason each");
        {
            DynamicWorkspaceViewModel preview;
            preview.previewActive = true;
            expect (! dynamicStateControlEligibility (preview).controlsEnabled);
            expect (dynamicStateControlEligibility (preview).disabledReason.isNotEmpty());

            DynamicWorkspaceViewModel legacy;
            legacy.runtime.source = DynamicMapSource::LegacyDynamicCompatibility;
            expect (! dynamicStateControlEligibility (legacy).controlsEnabled);

            DynamicWorkspaceViewModel noMap;
            noMap.runtime.source = DynamicMapSource::None;
            expect (! dynamicStateControlEligibility (noMap).controlsEnabled);

            DynamicWorkspaceViewModel noSelection;
            noSelection.runtime.source = DynamicMapSource::NewDynamicStateMap;
            expect (! dynamicStateControlEligibility (noSelection).controlsEnabled);
        }

        beginTest ("A recognized Auto State with enough hits is promotable");
        {
            const auto model = modelWithAppliedMap (autoState (1, 5));
            const auto eligibility = dynamicStateControlEligibility (model);
            expect (eligibility.controlsEnabled);
            expect (eligibility.canPromote);
            expect (eligibility.promoteDisabledReason.isEmpty());
        }

        beginTest ("A Candidate with too few hits cannot be promoted, with a specific reason");
        {
            const auto model = modelWithAppliedMap (autoState (2, 2));
            const auto eligibility = dynamicStateControlEligibility (model);
            expect (eligibility.controlsEnabled);
            expect (! eligibility.canPromote);
            expect (eligibility.promoteDisabledReason.isNotEmpty());
        }

        beginTest ("A Manual State cannot be promoted again, but can be removed");
        {
            DynamicState manual;
            manual.occupied = true;
            manual.stableStateId = 3;
            manual.fingerprint = validFingerprint();
            manual.origin = DynamicStateOrigin::Manual;
            manual.hasManualBasePackage = true;
            manual.manualBasePackage = { 0.0f, 100.0f, 0.7f };
            manual.hitCount = 3;

            const auto model = modelWithAppliedMap (manual);
            const auto eligibility = dynamicStateControlEligibility (model);
            expect (! eligibility.canPromote);
            expect (eligibility.canRemoveManual);
        }

        beginTest ("Effective package display matches the canonical conversion, not a duplicated formula");
        {
            const auto state = autoState (4, 5);
            const auto display = dynamicEffectivePackageDisplay (state, 48000.0);
            expect (display.valid);
            expectWithinAbsoluteError (display.delayDeltaMs, 0.5f, 1.0e-5f);
            expectWithinAbsoluteError (display.frequencyHz, 100.0, 1.0e-6);
            expectWithinAbsoluteError (display.q, 0.8, 1.0e-6);
        }

        beginTest ("Correction status line names each real State condition distinctly");
        {
            auto disabled = autoState (5, 5);
            disabled.enabled = false;
            expect (dynamicStateCorrectionStatusLine (disabled).contains ("Disabled"));

            auto bypassed = autoState (6, 5);
            bypassed.bypassed = true;
            expect (dynamicStateCorrectionStatusLine (bypassed).contains ("Bypassed"));

            auto automatic = autoState (7, 5);
            expect (dynamicStateCorrectionStatusLine (automatic).contains ("Automatic"));

            DynamicState recognizedNoCorrection;
            recognizedNoCorrection.occupied = true;
            recognizedNoCorrection.stableStateId = 8;
            recognizedNoCorrection.fingerprint = validFingerprint();
            recognizedNoCorrection.hitCount = 3;
            expect (dynamicStateCorrectionStatusLine (recognizedNoCorrection).contains ("no automatic correction"));
        }

        beginTest ("Workspace summary counts recognized/correction/manual States correctly");
        {
            DynamicWorkspaceViewModel model;
            model.runtime.source = DynamicMapSource::NewDynamicStateMap;
            model.runtime.states[0].occupied = true;
            model.runtime.states[0].hasCorrection = true;
            model.runtime.states[0].origin = DynamicStateOrigin::Auto;
            model.runtime.states[1].occupied = true;
            model.runtime.states[1].hasCorrection = false;
            model.runtime.states[1].origin = DynamicStateOrigin::Manual;

            const auto summary = dynamicWorkspaceSummary (model);
            expectEquals (summary.recognizedStateCount, 2);
            expectEquals (summary.correctionCount, 1);
            expectEquals (summary.globalOnlyCount, 1);
            expectEquals (summary.manualCount, 1);
        }

        beginTest ("Inspector component applies a model and resizes without crashing (no selection and with selection)");
        {
            DynamicStateInspector inspector;
            inspector.setSize (400, 200);
            DynamicWorkspaceViewModel empty;
            inspector.setModel (empty);

            const auto model = modelWithAppliedMap (autoState (9, 5));
            inspector.setModel (model);
            inspector.setSize (500, 220);
            expect (true, "reaching here without crashing is the assertion");
        }
    }
};

static DynamicStateInspectorHelpersTests dynamicStateInspectorHelpersTests;
