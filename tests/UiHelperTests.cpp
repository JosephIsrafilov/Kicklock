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

        beginTest ("Timing verdict speaks producer language");
        {
            expectEquals (juce::String (timingVerdictText (5.2f)), juce::String ("bass late"));
            expectEquals (juce::String (timingVerdictText (-3.1f)), juce::String ("bass early"));
            expectEquals (juce::String (timingVerdictText (0.1f)), juce::String ("in time"));
            expectEquals (juce::String (timingVerdictText (-0.2f)), juce::String ("in time"));
        }

        beginTest ("Polarity hint shows on sustained cancellation with hysteresis");
        {
            // Comes on below 25%, stays on until above 32%, never during silence.
            expect (! shouldShowPolarityHint (false, 40.0f, true));
            expect (! shouldShowPolarityHint (false, 28.0f, true));   // below on-threshold? no (25)
            expect (shouldShowPolarityHint (false, 20.0f, true));
            expect (shouldShowPolarityHint (true, 28.0f, true));      // hysteresis holds it on
            expect (! shouldShowPolarityHint (true, 35.0f, true));    // clears above 32
            expect (! shouldShowPolarityHint (true, 10.0f, false));   // silence gates it off
        }

        beginTest ("Min/max columns capture every peak and split ranges exactly");
        {
            // 12 samples into 3 columns of 4: each column reports its true
            // extremes, including peaks a point-sampler would skip.
            const float src[12] = { 0.0f, 0.9f, -0.2f, 0.1f,
                                    -0.8f, 0.0f, 0.3f, 0.05f,
                                    0.2f, -0.1f, 0.6f, -0.5f };
            float lo[3] = {}, hi[3] = {};
            buildMinMaxColumns (src, 0, 12, 3, lo, hi);

            expectWithinAbsoluteError (hi[0], 0.9f, 1.0e-6f);
            expectWithinAbsoluteError (lo[0], -0.2f, 1.0e-6f);
            expectWithinAbsoluteError (hi[1], 0.3f, 1.0e-6f);
            expectWithinAbsoluteError (lo[1], -0.8f, 1.0e-6f);
            expectWithinAbsoluteError (hi[2], 0.6f, 1.0e-6f);
            expectWithinAbsoluteError (lo[2], -0.5f, 1.0e-6f);

            // The `first` offset shifts the analysed range.
            float lo1[1] = {}, hi1[1] = {};
            buildMinMaxColumns (src, 4, 4, 1, lo1, hi1);
            expectWithinAbsoluteError (hi1[0], 0.3f, 1.0e-6f);
            expectWithinAbsoluteError (lo1[0], -0.8f, 1.0e-6f);

            // Band rendering only engages once several samples share a pixel.
            expect (! scopeShouldRenderMinMaxBand (1000, 1000));
            expect (! scopeShouldRenderMinMaxBand (3000, 1000));
            expect (scopeShouldRenderMinMaxBand (3001, 1000));
        }

        beginTest ("Anchored zoom keeps the time under the cursor fixed");
        {
            // Point at fraction f from the left has age scroll + (1-f)*window.
            // Halving a 100 ms window anchored at mid-view (f = 0.5) must add
            // 25 ms of scroll to keep that point's age constant.
            expectWithinAbsoluteError (scopeAnchoredZoomScrollMs (10.0f, 100.0f, 50.0f, 0.5f),
                                       35.0f, 1.0e-5f);
            // Right-edge anchor (live view) is a no-op.
            expectWithinAbsoluteError (scopeAnchoredZoomScrollMs (10.0f, 100.0f, 50.0f, 1.0f),
                                       10.0f, 1.0e-5f);
            // Left-edge anchor absorbs the whole window change.
            expectWithinAbsoluteError (scopeAnchoredZoomScrollMs (10.0f, 100.0f, 50.0f, 0.0f),
                                       60.0f, 1.0e-5f);
            // Zooming back out reverses the compensation symmetrically.
            expectWithinAbsoluteError (scopeAnchoredZoomScrollMs (35.0f, 50.0f, 100.0f, 0.5f),
                                       10.0f, 1.0e-5f);
        }

        beginTest ("Triggered visible range starts at the trigger line");
        {
            // Zoom 1 shows the post-trigger part of the captured window. The
            // pre-roll is still captured, but no longer displayed on the left.
            const auto whole = computeTriggeredVisibleRange (1000, 200, 1.0f);
            expectEquals (whole.first, 200);
            expectEquals (whole.visible, 800);

            // Zoom 2 keeps the trigger pinned to the left edge and narrows the
            // post-trigger window.
            const auto zoomed = computeTriggeredVisibleRange (1000, 200, 2.0f);
            expectEquals (zoomed.first, 200);
            expectEquals (zoomed.visible, 400);

            // A trigger near the end clamps to the remaining post-trigger span.
            const auto nearEnd = computeTriggeredVisibleRange (1000, 999, 4.0f);
            expectEquals (nearEnd.first, 999);
            expectEquals (nearEnd.visible, 1);
            expectEquals (nearEnd.first + nearEnd.visible, 1000);

            // Degenerate windows never produce a negative or out-of-range slice.
            const auto tiny = computeTriggeredVisibleRange (1, 0, 3.0f);
            expectEquals (tiny.first, 0);
            expect (tiny.visible <= 1);
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

        beginTest ("Triggered scope is the default first view mode");
        {
            expectEquals ((int) scopeViewModeFromChoiceIndex (0), (int) ScopeViewMode::Triggered);
            expectEquals ((int) scopeViewModeFromChoiceIndex (1), (int) ScopeViewMode::FreeRun);
            expectEquals ((int) scopeViewModeFromChoiceIndex (2), (int) ScopeViewMode::PhaseDelta);
            expectEquals ((int) scopeViewModeFromChoiceIndex (3), (int) ScopeViewMode::Overlay);
            expectEquals ((int) scopeViewModeFromChoiceIndex (4), (int) ScopeViewMode::Separate);
        }

        beginTest ("Triggered scope drag maps pixels to delay nudges");
        {
            expectWithinAbsoluteError (scopeDragPixelsToDelayDeltaMs (4.0f, false), 0.1f, 1.0e-7f);
            expectWithinAbsoluteError (scopeDragPixelsToDelayDeltaMs (-8.0f, false), -0.2f, 1.0e-7f);
            expectWithinAbsoluteError (scopeDragPixelsToDelayDeltaMs (4.0f, true), 0.01f, 1.0e-7f);
        }

        beginTest ("Free-run and Overlay use different display semantics");
        {
            // Root cause of the old duplication was that FreeRun and Overlay both
            // called drawOverlayMode, so they were indistinguishable. They must now
            // differ in a real drawing decision: Free-run ignores the visual/PDC
            // offset (raw signal) while Overlay applies it (aligned comparison).
            expect (! scopeModeAppliesVisualOffset (ScopeViewMode::FreeRun));
            expect (scopeModeAppliesVisualOffset (ScopeViewMode::Overlay));
            expect (scopeModeAppliesVisualOffset (ScopeViewMode::PhaseDelta));
            expect (scopeModeAppliesVisualOffset (ScopeViewMode::Separate));

            expectEquals (juce::String (scopeModeCaption (ScopeViewMode::FreeRun)),
                          juce::String ("FREE-RUN: live raw scope"));
            expectEquals (juce::String (scopeModeCaption (ScopeViewMode::Overlay)),
                          juce::String ("OVERLAY: aligned bass/kick comparison"));
            expect (juce::String (scopeModeCaption (ScopeViewMode::FreeRun))
                    != juce::String (scopeModeCaption (ScopeViewMode::Overlay)));
        }

        beginTest ("Scope scroll clamps to the valid history range");
        {
            const int history = 8192, visible = 2048, dec = 4;
            const float maxMs = samplesToMs ((history - visible) * dec, kSampleRate);

            // 0 ms is the live edge — negative scroll is clamped up to it.
            expectWithinAbsoluteError (clampScopeScrollMs (-50.0f, history, visible, kSampleRate, dec),
                                       0.0f, 1.0e-6f);
            // Beyond the oldest sample is clamped down to history-minus-window.
            expectWithinAbsoluteError (clampScopeScrollMs (maxMs + 500.0f, history, visible, kSampleRate, dec),
                                       maxMs, 1.0e-3f);
            // In-range values pass through untouched.
            expectWithinAbsoluteError (clampScopeScrollMs (maxMs * 0.5f, history, visible, kSampleRate, dec),
                                       maxMs * 0.5f, 1.0e-3f);
            // When the visible window covers all history there is no room to scroll.
            expectWithinAbsoluteError (clampScopeScrollMs (10.0f, history, history, kSampleRate, dec),
                                       0.0f, 1.0e-6f);
        }

        beginTest ("Scope drag maps to scroll deterministically and in a stable direction");
        {
            const float msPerPixel = 0.5f;
            const float start = 100.0f;

            // Dragging right (positive pixels) scrolls further back in time.
            expectWithinAbsoluteError (scopeDragToScrollMs (start, 40.0f, msPerPixel), 120.0f, 1.0e-5f);
            // Dragging left moves back toward the live edge.
            expectWithinAbsoluteError (scopeDragToScrollMs (start, -40.0f, msPerPixel), 80.0f, 1.0e-5f);
            // Zero movement holds position (no drift).
            expectWithinAbsoluteError (scopeDragToScrollMs (start, 0.0f, msPerPixel), start, 1.0e-6f);
            // Deterministic: identical inputs give identical output.
            expectWithinAbsoluteError (scopeDragToScrollMs (start, 12.5f, msPerPixel),
                                       scopeDragToScrollMs (start, 12.5f, msPerPixel), 0.0f);
        }

        beginTest ("Display hold combines manual freeze and temporary inspection hold");
        {
            // Manual freeze holds the display whether or not a mouse hold is active.
            expect (scopeDisplayHeld (true, false));
            expect (scopeDisplayHeld (true, true));
            // A temporary hold freezes only while active.
            expect (scopeDisplayHeld (false, true));
            // Released with no manual freeze -> display resumes live.
            expect (! scopeDisplayHeld (false, false));
        }

        beginTest ("Only triggered mode maps horizontal drag to the delay parameter");
        {
            // Preserves delay-drag in Triggered (documented in the scope tooltip)
            // while every scrolling mode uses drag to inspect/pan instead.
            expect (scopeModeUsesDelayDrag (ScopeViewMode::Triggered));
            expect (! scopeModeUsesDelayDrag (ScopeViewMode::FreeRun));
            expect (! scopeModeUsesDelayDrag (ScopeViewMode::PhaseDelta));
            expect (! scopeModeUsesDelayDrag (ScopeViewMode::Overlay));
            expect (! scopeModeUsesDelayDrag (ScopeViewMode::Separate));
        }

        beginTest ("Delay-drag gesture requires Shift even in Triggered mode");
        {
            // Root cause of the accidental-Delay bug: Triggered mode used to enter
            // a delay-drag on ANY ordinary mouseDown, so simply inspecting the
            // waveform silently moved Delay. The gesture must now require an
            // explicit Shift modifier, and every other mode must never start it
            // regardless of modifiers.
            expect (! scopeWantsDelayDragGesture (ScopeViewMode::Triggered, false));
            expect (scopeWantsDelayDragGesture (ScopeViewMode::Triggered, true));

            expect (! scopeWantsDelayDragGesture (ScopeViewMode::FreeRun, true));
            expect (! scopeWantsDelayDragGesture (ScopeViewMode::PhaseDelta, true));
            expect (! scopeWantsDelayDragGesture (ScopeViewMode::Overlay, true));
            expect (! scopeWantsDelayDragGesture (ScopeViewMode::Separate, true));
        }

        beginTest ("Relock enters a visible pending kick-reference state");
        {
            const auto state = kickReferenceStateAfterRelock();

            expectEquals ((int) state, (int) KickReferenceState::RelockPending);
            expect (kickReferenceShouldReplaceOnCapture (state));
            expectEquals (juce::String (triggeredScopeEmptyText (state)),
                          juce::String ("WAITING FOR NEW KICK REF"));
        }

        beginTest ("Next valid triggered capture locks the new kick reference");
        {
            expect (kickReferenceShouldReplaceOnCapture (KickReferenceState::NoReference));
            expect (kickReferenceShouldReplaceOnCapture (KickReferenceState::RelockPending));
            expect (! kickReferenceShouldReplaceOnCapture (KickReferenceState::Locked));

            expectEquals ((int) kickReferenceStateAfterCapture (KickReferenceState::NoReference),
                          (int) KickReferenceState::Locked);
            expectEquals ((int) kickReferenceStateAfterCapture (KickReferenceState::RelockPending),
                          (int) KickReferenceState::Locked);
            expectEquals ((int) kickReferenceStateAfterCapture (KickReferenceState::Locked),
                          (int) KickReferenceState::Locked);
            expectEquals (juce::String (triggeredScopeEmptyText (KickReferenceState::NoReference)),
                          juce::String ("WAITING FOR KICK"));
        }

        beginTest ("Triggered rendering is capped to screen-resolution points");
        {
            expectEquals (calculateTriggeredRenderPointCount (0, 800), 0);
            expectEquals (calculateTriggeredRenderPointCount (1, 800), 1);
            expectEquals (calculateTriggeredRenderPointCount (400, 800), 400);
            expectEquals (calculateTriggeredRenderPointCount (96000, 800), 1600);
            expectEquals (triggeredRenderSampleIndex (0, 1600, 96000), 0);
            expectEquals (triggeredRenderSampleIndex (1599, 1600, 96000), 95999);
        }

        beginTest ("Sweep auto-gain fills the lane from the hit peak, clamped");
        {
            expectWithinAbsoluteError (scopeAutoGainTargetFromPeak (0.88f), 1.0f, 1.0e-5f);
            expectWithinAbsoluteError (scopeAutoGainTargetFromPeak (0.44f), 2.0f, 1.0e-5f);
            expectWithinAbsoluteError (scopeAutoGainTargetFromPeak (2.0f), 1.0f, 1.0e-5f);   // never attenuates below unity
            expectWithinAbsoluteError (scopeAutoGainTargetFromPeak (0.001f), 40.0f, 1.0e-4f); // clamped ceiling
            expectWithinAbsoluteError (scopeAutoGainTargetFromPeak (0.0f), 1.0f, 1.0e-6f);    // silence stays neutral
        }

        beginTest ("Sweep auto-gain only retargets past the hysteresis band");
        {
            // Near-equal hits must NOT nudge the gain (that reads as flicker);
            // genuinely louder or quieter material must.
            expect (! scopeAutoGainShouldRetarget (2.0f, 2.0f));
            expect (! scopeAutoGainShouldRetarget (2.0f, 2.1f));
            expect (! scopeAutoGainShouldRetarget (2.0f, 1.85f));
            expect (scopeAutoGainShouldRetarget (2.0f, 2.5f));
            expect (scopeAutoGainShouldRetarget (2.0f, 1.5f));
            expect (scopeAutoGainShouldRetarget (0.0f, 1.0f));   // unseeded target always takes
        }

        beginTest ("Sweep ghosts fade monotonically, newest brightest");
        {
            const int count = 4;
            for (int i = 1; i < count; ++i)
                expect (scopeSweepGhostAlpha (i, count) < scopeSweepGhostAlpha (i - 1, count));

            expect (scopeSweepGhostAlpha (0, count) > 0.0f);
            expect (scopeSweepGhostAlpha (count - 1, count) > 0.0f);
            expectWithinAbsoluteError (scopeSweepGhostAlpha (count, count), 0.0f, 1.0e-6f);
            expectWithinAbsoluteError (scopeSweepGhostAlpha (-1, count), 0.0f, 1.0e-6f);
            expectWithinAbsoluteError (scopeSweepGhostAlpha (0, 0), 0.0f, 1.0e-6f);
        }

        beginTest ("Only completed sweeps become ghosts");
        {
            const int window = 8160;
            expect (! scopeSweepWorthKeepingAsGhost (0, window));
            expect (! scopeSweepWorthKeepingAsGhost (959, window));
            expect (! scopeSweepWorthKeepingAsGhost (window - 1, window));
            expect (scopeSweepWorthKeepingAsGhost (window, window));
            expect (! scopeSweepWorthKeepingAsGhost (1, 0));
        }

        beginTest ("Kick reference replacement requires a real completed kick window");
        {
            expect (! scopeKickReferenceCaptureIsValid (0.0f));
            expect (! scopeKickReferenceCaptureIsValid (1.0e-5f));
            expect (scopeKickReferenceCaptureIsValid ((float) std::pow (10.0, -30.0 / 20.0)));
            expect (scopeKickReferenceCaptureIsValid (0.7f));
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

        beginTest ("Top-bar analysis status maps to each required label bucket");
        {
            // No sidechain -> NO SIDECHAIN.
            expectEquals ((int) classifyAnalysisMaterialStatus (false, false, false, false, false),
                          (int) AnalysisMaterialStatus::WaitingForSidechain);
            // Sidechain but no kick yet -> WAITING FOR KICK.
            expectEquals ((int) classifyAnalysisMaterialStatus (true, false, true, false, false),
                          (int) AnalysisMaterialStatus::WaitingForKick);
            // Kick but no bass -> WAITING FOR BASS.
            expectEquals ((int) classifyAnalysisMaterialStatus (true, true, false, false, false),
                          (int) AnalysisMaterialStatus::WaitingForBass);
            // Both present but too quiet -> SIGNAL TOO LOW.
            expectEquals ((int) classifyAnalysisMaterialStatus (true, true, true, false, false),
                          (int) AnalysisMaterialStatus::SignalTooLow);
            // Banked material trumps momentary activity: once enough capture
            // exists, the status is ReadyToAnalyze even mid-gap / after stop.
            expectEquals ((int) classifyAnalysisMaterialStatus (true, false, false, false, true),
                          (int) AnalysisMaterialStatus::ReadyToAnalyze);
            // Usable but still gathering -> CAPTURING (shown as SIDECHAIN ACTIVE).
            expectEquals ((int) classifyAnalysisMaterialStatus (true, true, true, true, false),
                          (int) AnalysisMaterialStatus::CapturingMaterial);
            // Ready.
            expectEquals ((int) classifyAnalysisMaterialStatus (true, true, true, true, true),
                          (int) AnalysisMaterialStatus::ReadyToAnalyze);
        }

        beginTest ("Analyze can start only once material is ready");
        {
            expect (! analysisStatusCanStartAnalyze (AnalysisMaterialStatus::WaitingForSidechain));
            expect (! analysisStatusCanStartAnalyze (AnalysisMaterialStatus::WaitingForKick));
            expect (! analysisStatusCanStartAnalyze (AnalysisMaterialStatus::WaitingForBass));
            expect (! analysisStatusCanStartAnalyze (AnalysisMaterialStatus::SignalTooLow));
            expect (! analysisStatusCanStartAnalyze (AnalysisMaterialStatus::CapturingMaterial));
            expect (analysisStatusCanStartAnalyze (AnalysisMaterialStatus::ReadyToAnalyze));
        }

        beginTest ("Analyze button text explains disabled material states");
        {
            expectEquals (juce::String (analyzeButtonTextForStatus (AnalysisMaterialStatus::WaitingForSidechain)),
                          juce::String ("Analyze - no sidechain"));
            expectEquals (juce::String (analyzeButtonTextForStatus (AnalysisMaterialStatus::WaitingForKick)),
                          juce::String ("Analyze - waiting for kick"));
            expectEquals (juce::String (analyzeButtonTextForStatus (AnalysisMaterialStatus::WaitingForBass)),
                          juce::String ("Analyze - waiting for bass"));
            expectEquals (juce::String (analyzeButtonTextForStatus (AnalysisMaterialStatus::SignalTooLow)),
                          juce::String ("Analyze - signal too low"));
            expectEquals (juce::String (analyzeButtonTextForStatus (AnalysisMaterialStatus::CapturingMaterial)),
                          juce::String ("Analyze - capturing"));
            expectEquals (juce::String (analyzeButtonTextForStatus (AnalysisMaterialStatus::ReadyToAnalyze)),
                          juce::String ("Analyze"));
        }
    }
};

static StatusHelperTests statusHelperTestsInstance;

