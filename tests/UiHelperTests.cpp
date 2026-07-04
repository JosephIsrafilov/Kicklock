#include "TestCommon.h"

class ScopeVisualTests : public juce::UnitTest
{
public:
    ScopeVisualTests() : juce::UnitTest ("ScopeVisuals", "UI Helpers") {}

    void runTest() override
    {
        beginTest ("Phase relation classifies constructive, destructive, and silent buckets");
        {
            expectEquals ((int) classifyPhaseRelation (0.4f, 0.2f), (int) PhaseRelation::Constructive);
            expectEquals ((int) classifyPhaseRelation (-0.5f, 0.1f), (int) PhaseRelation::Destructive);
            expectEquals ((int) classifyPhaseRelation (1.0e-7f, 0.2f), (int) PhaseRelation::Silent);
        }

        beginTest ("Peak delta is positive when bass transient is later than kick");
        {
            std::vector<float> bass (128, 0.0f), kick (128, 0.0f);
            kick[20] = 1.0f;
            bass[60] = 1.0f;

            const float deltaMs = calculatePeakDeltaMs (bass.data(), kick.data(), (int) bass.size(), kSampleRate);
            expectWithinAbsoluteError (deltaMs, (float) (40.0 / kSampleRate * 1000.0), 0.001f);
        }

        beginTest ("Peak delta is negative when bass transient is earlier than kick");
        {
            std::vector<float> bass (128, 0.0f), kick (128, 0.0f);
            bass[18] = 1.0f;
            kick[58] = 1.0f;

            const float deltaMs = calculatePeakDeltaMs (bass.data(), kick.data(), (int) bass.size(), kSampleRate);
            expectWithinAbsoluteError (deltaMs, (float) (-40.0 / kSampleRate * 1000.0), 0.001f);
        }

        beginTest ("Sample and millisecond conversion stay consistent");
        {
            expectWithinAbsoluteError (samplesToMs (480, kSampleRate), 10.0f, 1.0e-6f);
            expectEquals (msToSamples (10.0f, kSampleRate), 480);
        }

        beginTest ("BPM grid conversion maps note values to milliseconds");
        {
            expectWithinAbsoluteError ((float) bpmToQuarterMs (120.0), 500.0f, 1.0e-6f);
            expectWithinAbsoluteError ((float) gridDivisionToMs (120.0, GridDivision::Quarter), 500.0f, 1.0e-6f);
            expectWithinAbsoluteError ((float) gridDivisionToMs (120.0, GridDivision::Eighth), 250.0f, 1.0e-6f);
            expectWithinAbsoluteError ((float) gridDivisionToMs (120.0, GridDivision::Sixteenth), 125.0f, 1.0e-6f);
            expectWithinAbsoluteError ((float) gridDivisionToMs (120.0, GridDivision::Bar), 2000.0f, 1.0e-6f);
        }

        beginTest ("Visible scope samples follow sample rate, decimation, zoom, and grid");
        {
            expectEquals (calculateVisibleScopeSamples (8192, kSampleRate, 24, 1.0f,
                                                        GridDivision::Sixteenth, true, 120.0),
                          250);
            expectEquals (calculateVisibleScopeSamples (8192, kSampleRate, 24, 2.0f,
                                                        GridDivision::Sixteenth, true, 120.0),
                          125);
            expectEquals (calculateVisibleScopeSamples (8192, kSampleRate, 24, 2.0f,
                                                        GridDivision::Milliseconds, false, 0.0),
                          4096);
        }

        beginTest ("Visual offset shifts the display index by decimated samples");
        {
            const int unshifted = resolveDisplayHistoryIndex (100, 2048, 1800, 12, 0, 4);
            const int shifted = resolveDisplayHistoryIndex (100, 2048, 1800, 12, 24, 4);
            const int expected = wrapHistoryIndex (unshifted + 6, 2048);

            expectEquals (visualOffsetToDisplaySamples (24, 4), 6);
            expectEquals (shifted, expected);
        }

        beginTest ("Visual offset shifts bass relative to kick only");
        {
            const auto unshifted = resolveRelativeDisplayHistoryIndices (100, 2048, 1800, 12, 0, 4);
            const auto shifted = resolveRelativeDisplayHistoryIndices (100, 2048, 1800, 12, 24, 4);

            expectEquals (shifted.kickIndex, unshifted.kickIndex);
            expectEquals (shifted.bassIndex, wrapHistoryIndex (unshifted.bassIndex + 6, 2048));
            expectEquals (shifted.bassIndex - shifted.kickIndex, 6);
        }

        beginTest ("Visual offset wraps bass index without moving kick index");
        {
            const auto unshifted = resolveRelativeDisplayHistoryIndices (2038, 2048, 0, 8, 0, 2);
            const auto shifted = resolveRelativeDisplayHistoryIndices (2038, 2048, 0, 8, 12, 2);

            expectEquals (shifted.kickIndex, unshifted.kickIndex);
            expectEquals (shifted.bassIndex, wrapHistoryIndex (unshifted.bassIndex + 6, 2048));
        }
    }
};

static ScopeVisualTests scopeVisualTestsInstance;

class StatusHelperTests : public juce::UnitTest
{
public:
    StatusHelperTests() : juce::UnitTest ("StatusHelpers", "UI Helpers") {}

    void runTest() override
    {
        beginTest ("Sidechain status distinguishes missing, quiet, and active states");
        {
            expectEquals ((int) classifySidechainSignalStatus (false, 0.5f, 0.5f),
                          (int) SidechainSignalStatus::WaitingForSidechain);
            expectEquals ((int) classifySidechainSignalStatus (true, 1.0e-6f, 0.5f),
                          (int) SidechainSignalStatus::SignalTooLow);
            expectEquals ((int) classifySidechainSignalStatus (true, 0.2f, 0.15f),
                          (int) SidechainSignalStatus::SidechainActive);
        }

        beginTest ("Workflow status distinguishes monitoring, fix ready, and processing");
        {
            expectEquals ((int) classifyProcessingWorkflowStatus (true, false),
                          (int) ProcessingWorkflowStatus::MonitoringDryInput);
            expectEquals ((int) classifyProcessingWorkflowStatus (true, true),
                          (int) ProcessingWorkflowStatus::FixReady);
            expectEquals ((int) classifyProcessingWorkflowStatus (false, true),
                          (int) ProcessingWorkflowStatus::ProcessingBass);
        }
    }
};

static StatusHelperTests statusHelperTestsInstance;

