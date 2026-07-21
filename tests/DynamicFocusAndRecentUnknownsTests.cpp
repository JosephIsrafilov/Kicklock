#include "TestCommon.h"
#include "ui/DynamicFocusStatus.h"
#include "ui/DynamicRecentUnknownsPanel.h"

class DynamicFocusAndRecentUnknownsTests : public juce::UnitTest
{
public:
    DynamicFocusAndRecentUnknownsTests()
        : juce::UnitTest ("DynamicFocusAndRecentUnknowns", "Dynamic State Map") {}

    void runTest() override
    {
        beginTest ("Focus status text distinguishes off/unavailable/waiting/detected/collecting/verified");
        {
            DynamicWorkspaceViewModel off;
            expect (dynamicFocusStatusText (off).isEmpty());

            DynamicWorkspaceViewModel previewCase;
            previewCase.focusEnabled = true;
            previewCase.previewActive = true;
            expect (dynamicFocusStatusText (previewCase).contains ("unavailable"));

            DynamicWorkspaceViewModel noSelection;
            noSelection.focusEnabled = true;
            noSelection.runtime.source = DynamicMapSource::NewDynamicStateMap;
            expect (dynamicFocusStatusText (noSelection).contains ("Select a State"));

            DynamicWorkspaceViewModel notDetected;
            notDetected.focusEnabled = true;
            notDetected.runtime.source = DynamicMapSource::NewDynamicStateMap;
            notDetected.focusedStableStateId = 42;
            expect (dynamicFocusStatusText (notDetected).contains ("not detected recently"));

            DynamicWorkspaceViewModel disabledCase;
            disabledCase.focusEnabled = true;
            disabledCase.runtime.source = DynamicMapSource::NewDynamicStateMap;
            disabledCase.focusedStableStateId = 7;
            disabledCase.runtime.states[0].occupied = true;
            disabledCase.runtime.states[0].stableStateId = 7;
            disabledCase.runtime.states[0].enabled = false;
            expect (dynamicFocusStatusText (disabledCase).contains ("disabled"));

            DynamicWorkspaceViewModel waiting;
            waiting.focusEnabled = true;
            waiting.runtime.source = DynamicMapSource::NewDynamicStateMap;
            waiting.focusedStableStateId = 9;
            waiting.runtime.states[0].occupied = true;
            waiting.runtime.states[0].stableStateId = 9;
            waiting.runtime.states[0].active = false;
            expect (dynamicFocusStatusText (waiting).contains ("Waiting"));

            DynamicWorkspaceViewModel collecting;
            collecting.focusEnabled = true;
            collecting.runtime.source = DynamicMapSource::NewDynamicStateMap;
            collecting.focusedStableStateId = 11;
            collecting.runtime.activeSemanticStateId = 11;
            collecting.runtime.states[0].occupied = true;
            collecting.runtime.states[0].stableStateId = 11;
            collecting.runtime.states[0].active = true;
            collecting.runtime.states[0].verified.availability = DynamicMeasurementAvailability::Collecting;
            collecting.runtime.states[0].verified.validWindowCount = 1;
            expect (dynamicFocusStatusText (collecting).contains ("Collecting"));

            DynamicWorkspaceViewModel verified = collecting;
            verified.runtime.states[0].verified.availability = DynamicMeasurementAvailability::Available;
            expect (dynamicFocusStatusText (verified).contains ("Verified"));
        }

        beginTest ("DynamicFocusStatus applies a model and resizes without crashing, with and without a trace");
        {
            DynamicFocusStatus status;
            status.setSize (400, 120);
            DynamicWorkspaceViewModel model;
            model.focusEnabled = true;
            model.runtime.source = DynamicMapSource::NewDynamicStateMap;
            model.focusedStableStateId = 5;
            model.hasSelectedState = true;
            status.setModel (model);

            model.focusedTrace.available = true;
            model.focusedTrace.stableStateId = 5;
            model.focusedTrace.beforeKick = { 0.1f, 0.2f, -0.1f };
            model.focusedTrace.beforeBass = { 0.05f, 0.1f };
            model.focusedTrace.afterBass = { 0.02f, 0.04f };
            status.setModel (model);
            status.setSize (500, 130);
            expect (true, "reaching here without crashing is the assertion");
        }

        beginTest ("DynamicRecentUnknownsPanel applies a model with clusters without crashing");
        {
            DynamicRecentUnknownsPanel panel;
            panel.setSize (400, 200);
            DynamicWorkspaceViewModel model;
            DynamicRecentUnknownCluster cluster;
            cluster.eventId = 1;
            cluster.repeatCount = 3;
            cluster.bestDistance = 0.4f;
            model.recentUnknownClusters.push_back (cluster);
            panel.setModel (model);
            panel.setSize (450, 210);
            expect (true, "reaching here without crashing is the assertion");
        }
    }
};

static DynamicFocusAndRecentUnknownsTests dynamicFocusAndRecentUnknownsTests;
