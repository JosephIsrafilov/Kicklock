#include "TestCommon.h"
#include "PluginEditor.h"
#include "NoteLearnAccumulator.h"
#include "util/SpectrumFifo.h"

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
            expectWithinAbsoluteError ((float) gridDivisionToMs (120.0, GridDivision::Half), 1000.0f, 1.0e-6f);
            expectWithinAbsoluteError ((float) gridDivisionToMs (120.0, GridDivision::Whole), 2000.0f, 1.0e-6f);
            expectWithinAbsoluteError ((float) gridDivisionToMs (120.0, GridDivision::Bar), 2000.0f, 1.0e-6f);
        }

        beginTest ("Grid choice indices match the parameter choice list");
        {
            expectEquals ((int) gridDivisionFromChoiceIndex (0), (int) GridDivision::Quarter);
            expectEquals ((int) gridDivisionFromChoiceIndex (1), (int) GridDivision::Half);
            expectEquals ((int) gridDivisionFromChoiceIndex (2), (int) GridDivision::Whole);
            expectEquals ((int) gridDivisionFromChoiceIndex (3), (int) GridDivision::FourBars);
            expectEquals ((int) gridDivisionFromChoiceIndex (4), (int) GridDivision::Bar);
            expectEquals ((int) gridDivisionFromChoiceIndex (5), (int) GridDivision::Milliseconds);
        }

        beginTest ("Visible scope samples follow sample rate, decimation, zoom, and grid");
        {
            expectEquals (calculateVisibleScopeSamples (8192, kSampleRate, 24, 1.0f,
                                                        GridDivision::Half, true, 120.0),
                          2000);
            expectEquals (calculateVisibleScopeSamples (8192, kSampleRate, 24, 2.0f,
                                                        GridDivision::Half, true, 120.0),
                          1000);
            expectEquals (calculateVisibleScopeSamples (8192, kSampleRate, 24, 2.0f,
                                                        GridDivision::Milliseconds, false, 0.0),
                          4096);
            expectEquals (calculateVisibleScopeSamples (8192, kSampleRate, 24, 1.0f,
                                                        GridDivision::Bar, true, 120.0),
                          4000);
            expectEquals (calculateVisibleScopeSamples (8192, kSampleRate, 24, 1.0f,
                                                        GridDivision::Milliseconds, true, 120.0),
                          8192);
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

        beginTest ("Triggered visible range keeps pre-roll context before the trigger line");
        {
            // Zoom 1 shows the whole captured window. The pre-roll remains
            // visible so the trigger line is slightly inside the left edge.
            const auto whole = computeTriggeredVisibleRange (1000, 200, 1.0f);
            expectEquals (whole.first, 0);
            expectEquals (whole.visible, 1000);

            // Zoom 2 keeps the trigger at the same fractional position it had
            // in the full capture, not glued to the left edge.
            const auto zoomed = computeTriggeredVisibleRange (1000, 200, 2.0f);
            expectEquals (zoomed.visible, 500);
            const float fraction1x = 200.0f / 999.0f;
            const float fractionZoom = (float) (200 - zoomed.first) / (float) (zoomed.visible - 1);
            expectWithinAbsoluteError (fractionZoom, fraction1x, 0.01f);

            // A trigger near the end clamps the slice to the buffer's end edge
            // rather than running past it.
            const auto nearEnd = computeTriggeredVisibleRange (1000, 999, 4.0f);
            expectEquals (nearEnd.visible, 250);
            expectEquals (nearEnd.first + nearEnd.visible, 1000);

            // Degenerate windows never produce a negative or out-of-range slice.
            const auto tiny = computeTriggeredVisibleRange (1, 0, 3.0f);
            expectEquals (tiny.first, 0);
            expect (tiny.visible <= 1);
        }

        beginTest ("Triggered kick onset moves visual zero to the first kick lobe");
        {
            std::vector<float> kick (128, 0.0f);
            kick[8] = 0.00002f;   // sub-threshold bed/noise
            kick[20] = -0.08f;
            kick[21] = -0.16f;
            kick[22] = -0.25f;
            kick[40] = 0.90f;     // later detector/body peak

            expectEquals (findTriggeredKickOnsetIndex (kick.data(), (int) kick.size(), 40), 20);

            std::fill (kick.begin(), kick.end(), 0.0f);
            expectEquals (findTriggeredKickOnsetIndex (kick.data(), (int) kick.size(), 40), 40);
        }

        beginTest ("Triggered pan moves a zoomed capture without leaving its bounds");
        {
            const int n = 1000;
            const auto range = computeTriggeredVisibleRange (n, 200, 2.0f);
            expectEquals (range.visible, 500);

            const int firstAtAnchor = computeTriggeredPannedFirst (n, range.visible, range.first, 0.0f, kSampleRate);
            expectEquals (firstAtAnchor, range.first);

            const float tenSamplesMs = (float) (10.0 * 1000.0 / kSampleRate);
            expectEquals (computeTriggeredPannedFirst (n, range.visible, range.first, tenSamplesMs, kSampleRate),
                          juce::jmax (0, range.first - 10));
            expectEquals (computeTriggeredPannedFirst (n, range.visible, range.first, -tenSamplesMs, kSampleRate),
                          range.first + 10);

            const float clampedLeft = clampTriggeredPanScrollMs (100000.0f, n, range.visible, range.first, kSampleRate);
            const float clampedRight = clampTriggeredPanScrollMs (-100000.0f, n, range.visible, range.first, kSampleRate);
            expectEquals (computeTriggeredPannedFirst (n, range.visible, range.first, clampedLeft, kSampleRate), 0);
            expectEquals (computeTriggeredPannedFirst (n, range.visible, range.first, clampedRight, kSampleRate),
                          n - range.visible);
        }

        beginTest ("Triggered frame ownership prevents stale marker reuse");
        {
            expect (triggeredMarkersBelongToFrame (2, 2, 1024, 1024));
            expect (! triggeredMarkersBelongToFrame (1, 2, 1024, 1024));
            expect (! triggeredMarkersBelongToFrame (2, 2, 512, 1024));
            expect (! triggeredMarkersBelongToFrame (0, 0, 1024, 1024));
        }

        beginTest ("Triggered reference and live frame must share trigger geometry");
        {
            expect (triggeredReferenceSharesFrameGeometry (128, 128, 1024));
            expect (! triggeredReferenceSharesFrameGeometry (128, 120, 1024));
            expect (! triggeredReferenceSharesFrameGeometry (1024, 1024, 1024));
            expect (! triggeredReferenceSharesFrameGeometry (128, 128, 1));
        }

        beginTest ("Triggered delta follows the currently displayed hit frame");
        {
            const int window = 1024;
            const int trigger = 128;
            std::vector<float> kick (window, 0.0f);
            std::vector<float> bass1 (window, 0.0f);
            std::vector<float> bass2 (window, 0.0f);

            kick[(size_t) trigger] = 1.0f;
            bass1[(size_t) (trigger + 256)] = 1.0f;
            bass2[(size_t) (trigger + 128)] = 1.0f;

            const auto markers1 = findEnvelopePeakMarkers (bass1.data(), kick.data(), window, kSampleRate, 0);
            const auto markers2 = findEnvelopePeakMarkers (bass2.data(), kick.data(), window, kSampleRate, 0);

            expect (markers1.valid);
            expect (markers2.valid);
            expectEquals (markers1.bassPeakIndex, trigger + 256);
            expectEquals (markers2.bassPeakIndex, trigger + 128);
            expectWithinAbsoluteError (markers1.deltaMs, samplesToMs (256, kSampleRate), 1.0e-5f);
            expectWithinAbsoluteError (markers2.deltaMs, samplesToMs (128, kSampleRate), 1.0e-5f);
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

        beginTest ("Persisted scope-mode indices stay compatible with existing presets");
        {
            expectEquals ((int) scopeViewModeFromChoiceIndex (0), (int) ScopeViewMode::Triggered);
            expectEquals ((int) scopeViewModeFromChoiceIndex (1), (int) ScopeViewMode::FreeRun);
            expectEquals ((int) scopeViewModeFromChoiceIndex (2), (int) ScopeViewMode::PhaseDelta);
            expectEquals ((int) scopeViewModeFromChoiceIndex (3), (int) ScopeViewMode::Spectrum);
            expectEquals ((int) scopeViewModeFromChoiceIndex (4), (int) ScopeViewMode::Separate);
            expectEquals (scopeViewModeToChoiceIndex (ScopeViewMode::Spectrum), 3);
            expectEquals (scopeViewModeToChoiceIndex (ScopeViewMode::Separate), 4);
        }

        beginTest ("Visible scope-mode menu puts Spectrum last");
        {
            expectEquals ((int) scopeViewModeFromUiIndex (0), (int) ScopeViewMode::Triggered);
            expectEquals ((int) scopeViewModeFromUiIndex (1), (int) ScopeViewMode::FreeRun);
            expectEquals ((int) scopeViewModeFromUiIndex (2), (int) ScopeViewMode::PhaseDelta);
            expectEquals ((int) scopeViewModeFromUiIndex (3), (int) ScopeViewMode::Separate);
            expectEquals ((int) scopeViewModeFromUiIndex (4), (int) ScopeViewMode::Spectrum);
            expectEquals ((int) scopeViewModeFromUiIndex (-1), (int) ScopeViewMode::Triggered);
            expectEquals ((int) scopeViewModeFromUiIndex (99), (int) ScopeViewMode::Triggered);

            for (const auto mode : { ScopeViewMode::Triggered, ScopeViewMode::FreeRun,
                                     ScopeViewMode::PhaseDelta, ScopeViewMode::Separate,
                                     ScopeViewMode::Spectrum })
                expectEquals ((int) scopeViewModeFromUiIndex (uiIndexFromScopeViewMode (mode)), (int) mode);

            expect (scopeFullModeShowsGrid (ScopeViewMode::Triggered));
            expect (scopeFullModeShowsGrid (ScopeViewMode::FreeRun));
            expect (scopeFullModeShowsGrid (ScopeViewMode::PhaseDelta));
            expect (scopeFullModeShowsGrid (ScopeViewMode::Separate));
            expect (! scopeFullModeShowsGrid (ScopeViewMode::Spectrum));
        }

        beginTest ("Generic transient markers are disabled in every scope mode");
        {
            for (const auto mode : { ScopeViewMode::Triggered, ScopeViewMode::FreeRun,
                                     ScopeViewMode::PhaseDelta, ScopeViewMode::Separate,
                                     ScopeViewMode::Spectrum })
                expect (! scopeModeShowsGenericTransientMarkers (mode));
        }

        beginTest ("Scope zoom helpers preserve direction, limits, and Spectrum gating");
        {
            expectWithinAbsoluteError (scopeZoomTargetFromWheelDelta (2.0f, 1.0f, 1.0f, 16.0f),
                                       2.6f, 1.0e-6f);
            expectWithinAbsoluteError (scopeZoomTargetFromWheelDelta (2.0f, -1.0f, 1.0f, 16.0f),
                                       1.4f, 1.0e-6f);
            expectWithinAbsoluteError (scopeZoomTargetFromWheelDelta (1.0f, -1.0f, 1.0f, 16.0f),
                                       1.0f, 1.0e-6f);
            expectWithinAbsoluteError (scopeZoomTargetFromWheelDelta (16.0f, 1.0f, 1.0f, 16.0f),
                                       16.0f, 1.0e-6f);
            expectWithinAbsoluteError (scopeZoomTargetFromMagnify (2.0f, 1.25f, 1.0f, 16.0f),
                                       2.5f, 1.0e-6f);
            expectWithinAbsoluteError (scopeZoomTargetFromMagnify (2.0f, 0.5f, 1.0f, 16.0f),
                                       1.0f, 1.0e-6f);
            expect (scopeModeAcceptsZoom (ScopeViewMode::Triggered));
            expect (! scopeModeAcceptsZoom (ScopeViewMode::Spectrum));
        }

        beginTest ("Triggered scope drag maps pixels to delay nudges");
        {
            expectWithinAbsoluteError (scopeDragPixelsToDelayDeltaMs (4.0f, false), 0.1f, 1.0e-7f);
            expectWithinAbsoluteError (scopeDragPixelsToDelayDeltaMs (-8.0f, false), -0.2f, 1.0e-7f);
            expectWithinAbsoluteError (scopeDragPixelsToDelayDeltaMs (4.0f, true), 0.01f, 1.0e-7f);
        }

        beginTest ("Free-run and Spectrum use different display semantics");
        {
            // Root cause of the old duplication was that FreeRun and Spectrum both
            // called drawSpectrumMode, so they were indistinguishable. They must now
            // differ in a real drawing decision: Free-run ignores the visual/PDC
            // offset so the waveform matches what the user is hearing *now*,
            // while Triggered aligns the display to a fixed past event.
            expect (! scopeModeSupportsVisualOffset (ScopeViewMode::FreeRun));
            expect (! scopeModeSupportsVisualOffset (ScopeViewMode::Spectrum));
            expect (! scopeModeSupportsVisualOffset (ScopeViewMode::Triggered));
            expect (scopeModeSupportsVisualOffset (ScopeViewMode::PhaseDelta));
            expect (scopeModeSupportsVisualOffset (ScopeViewMode::Separate));

            expectEquals (juce::String (scopeModeCaption (ScopeViewMode::FreeRun)),
                          juce::String ("FREE-RUN: live raw scope"));
            expectEquals (juce::String (scopeModeCaption (ScopeViewMode::Spectrum)),
                          juce::String ("SPECTRUM ANALYZER"));
            expect (juce::String (scopeModeCaption (ScopeViewMode::FreeRun))
                    != juce::String (scopeModeCaption (ScopeViewMode::Spectrum)));
        }

        beginTest ("Visual offset gating preserves the saved value outside supported modes");
        {
            expect (scopeModeSupportsVisualOffset (ScopeViewMode::Separate));
            expect (scopeModeSupportsVisualOffset (ScopeViewMode::PhaseDelta));
            expect (! scopeModeSupportsVisualOffset (ScopeViewMode::Triggered));
            expect (! scopeModeSupportsVisualOffset (ScopeViewMode::FreeRun));
            expect (! scopeModeSupportsVisualOffset (ScopeViewMode::Spectrum));
            expect (! scopeModeSupportsVisualOffset (static_cast<ScopeViewMode> (999)));

            const int savedOffset = -256;
            expectEquals (effectiveVisualOffsetSamples (ScopeViewMode::Triggered, savedOffset), 0);
            expectEquals (effectiveVisualOffsetSamples (ScopeViewMode::FreeRun, savedOffset), 0);
            expectEquals (effectiveVisualOffsetSamples (ScopeViewMode::Spectrum, savedOffset), 0);
            expectEquals (effectiveVisualOffsetSamples (ScopeViewMode::Separate, savedOffset), savedOffset);
            expectEquals (effectiveVisualOffsetSamples (ScopeViewMode::PhaseDelta, savedOffset), savedOffset);
            expectEquals (savedOffset, -256);
        }

        beginTest ("Separate cursor badges identify the selected lane only");
        {
            expectEquals (juce::String (formatScopeHoverDbBadge (ScopeViewMode::Separate, 0, -6.0f)),
                          juce::String ("BASS -6.0 dB"));
            expectEquals (juce::String (formatScopeHoverDbBadge (ScopeViewMode::Separate, 1, 3.0f)),
                          juce::String ("KICK +3.0 dB"));
            expectEquals (juce::String (formatScopeHoverDbBadge (ScopeViewMode::Separate, 0, -144.0f)),
                          juce::String ("BASS -inf dB"));
            expectEquals (juce::String (formatScopeHoverDbBadge (ScopeViewMode::Triggered, 0, -6.0f)),
                          juce::String ("-6.0 dB"));
        }

        beginTest ("Spectrum display arrays start at the analyzer floor");
        {
            std::array<float, 8> spectrumMain {};
            std::array<float, 8> spectrumSide {};
            spectrumMain.fill (SpectrumAnalysis::minimumDb);
            spectrumSide.fill (SpectrumAnalysis::minimumDb);

            for (const auto value : spectrumMain)
                expectWithinAbsoluteError (value, SpectrumAnalysis::minimumDb, 1.0e-6f);
            for (const auto value : spectrumSide)
                expectWithinAbsoluteError (value, SpectrumAnalysis::minimumDb, 1.0e-6f);
        }

        auto checkHighFrequencyBins = [this] (double sampleRate,
                                              std::array<int, 3> bins,
                                              std::array<double, 3> targetFrequencies)
        {
            for (size_t caseIndex = 0; caseIndex < bins.size(); ++caseIndex)
            {
                const int bin = bins[caseIndex];
                const double frequency = (double) bin * sampleRate / (double) SpectrumAnalysis::fftSize;
                std::array<float, SpectrumAnalysis::fftSize * 2> left {};
                std::array<float, SpectrumAnalysis::fftSize * 2> right {};
                for (int i = 0; i < SpectrumAnalysis::fftSize; ++i)
                {
                    const float phase = juce::MathConstants<float>::twoPi
                                      * (float) bin * (float) i / (float) SpectrumAnalysis::fftSize;
                    left[(size_t) i] = std::sin (phase);
                    right[(size_t) i] = left[(size_t) i];
                }

                juce::dsp::WindowingFunction<float> window (SpectrumAnalysis::fftSize,
                                                            juce::dsp::WindowingFunction<float>::hann,
                                                            false);
                std::array<float, SpectrumAnalysis::fftSize> db {};
                SpectrumAnalysis::calculatePowerSpectrumDb (left.data(), right.data(),
                                                            window, db.data(), 1.0f);

                int detectedBin = bin;
                for (int candidate = juce::jmax (0, bin - 2);
                     candidate <= juce::jmin (SpectrumAnalysis::maximumBin, bin + 2);
                     ++candidate)
                    if (db[(size_t) candidate] > db[(size_t) detectedBin])
                        detectedBin = candidate;

                expect (db[(size_t) bin] > -20.0f);
                expectWithinAbsoluteError (detectedBin, bin, 2);
                expect (bin <= SpectrumAnalysis::maximumBin);
                expectWithinAbsoluteError ((float) frequency,
                                           (float) targetFrequencies[caseIndex],
                                           (float) sampleRate / (float) SpectrumAnalysis::fftSize);
            }
        };

        beginTest ("Spectrum preserves 15-20 kHz bin-centred peaks at 48 kHz");
        {
            checkHighFrequencyBins (48000.0, { 2560, 3072, 3413 }, { 15000.0, 18000.0, 20000.0 });
        }

        beginTest ("Spectrum preserves 15-20 kHz bin-centred peaks at 96 kHz");
        {
            checkHighFrequencyBins (96000.0, { 1280, 1536, 1706 }, { 15000.0, 18000.0, 20000.0 });
        }

        beginTest ("Opposite-phase stereo high-frequency tones remain visible");
        {
            constexpr int bin = 3072;
            std::array<float, SpectrumAnalysis::fftSize * 2> left {};
            std::array<float, SpectrumAnalysis::fftSize * 2> right {};
            for (int i = 0; i < SpectrumAnalysis::fftSize; ++i)
            {
                const float phase = juce::MathConstants<float>::twoPi
                                  * (float) bin * (float) i / (float) SpectrumAnalysis::fftSize;
                left[(size_t) i] = std::sin (phase);
                right[(size_t) i] = -left[(size_t) i];
            }

            juce::dsp::WindowingFunction<float> window (SpectrumAnalysis::fftSize,
                                                        juce::dsp::WindowingFunction<float>::hann,
                                                        false);
            std::array<float, SpectrumAnalysis::fftSize> db {};
            SpectrumAnalysis::calculatePowerSpectrumDb (left.data(), right.data(),
                                                        window, db.data(), 1.0f);
            expect (db[(size_t) bin] > -20.0f);
        }

        beginTest ("Spectrum display maximum follows the Nyquist guard");
        {
            expectWithinAbsoluteError (spectrumDisplayMaximumFrequency (32000.0f), 15680.0f, 1.0e-4f);
            expectWithinAbsoluteError (spectrumDisplayMaximumFrequency (44100.0f), 20000.0f, 1.0e-4f);
            expectWithinAbsoluteError (spectrumDisplayMaximumFrequency (48000.0f), 20000.0f, 1.0e-4f);
            expectWithinAbsoluteError (spectrumDisplayMaximumFrequency (96000.0f), 20000.0f, 1.0e-4f);
        }

        beginTest ("Spectrum FIFO drops stale frames and keeps storage stable across reprepare");
        {
            SpectrumFifo fifo;
            fifo.prepare (8); // AbstractFifo stores seven usable samples.
            const auto firstGeneration = fifo.getGeneration();
            for (int i = 0; i < 16; ++i)
                fifo.pushSample ((float) i, (float) i, 0.0f, 0.0f, 1, 0);

            std::array<float, 4> mainL {}, mainR {}, sideL {}, sideR {};
            expectEquals (fifo.readLatest (mainL.data(), mainR.data(), sideL.data(), sideR.data(), 4), 4);
            expectWithinAbsoluteError (mainL[0], 3.0f, 1.0e-6f);
            expectWithinAbsoluteError (mainL[3], 6.0f, 1.0e-6f);
            expectGreaterThan (fifo.getDroppedSampleCount(), 0);

            fifo.prepare (8);
            expectGreaterThan ((int) fifo.getGeneration(), (int) firstGeneration);
        }

        beginTest ("Separate mode lane helper follows shared geometry");
        {
            const auto geometry = calculateSeparateModeGeometry (100.0f);
            expectEquals (getSeparateLaneForY (10.0f, geometry), 0);
            expectEquals (getSeparateLaneForY (90.0f, geometry), 1);
            expectEquals (getSeparateLaneForY (geometry.dividerY, geometry), 1);
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
            expect (! scopeModeUsesDelayDrag (ScopeViewMode::Spectrum));
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
            expect (! scopeWantsDelayDragGesture (ScopeViewMode::Spectrum, true));
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

        beginTest ("Auto re-lock only arms a new Triggered-mode reference");
        {
            expect (shouldAutoRelockKickReference (KickReferenceState::Locked, false, true, true, true));
            expect (shouldAutoRelockKickReference (KickReferenceState::Locked, true, true, false, true));
            // Any non-Triggered mode is represented by triggeredMode == false.
            expect (! shouldAutoRelockKickReference (KickReferenceState::Locked, false, true, true, false)); // Separate
            expect (! shouldAutoRelockKickReference (KickReferenceState::Locked, false, true, true, false)); // Free-run
            expect (! shouldAutoRelockKickReference (KickReferenceState::Locked, false, true, true, false)); // Phase Delta
            expect (! shouldAutoRelockKickReference (KickReferenceState::Locked, false, true, true, false)); // Spectrum
            expect (! shouldAutoRelockKickReference (KickReferenceState::Locked, true, true, true, true));
            expect (! shouldAutoRelockKickReference (KickReferenceState::Locked, false, false, false, true));
            // Initial Triggered state is pending, but the first sidechain edge
            // still needs the shared reset-fenced transition.
            expect (shouldAutoRelockKickReference (KickReferenceState::RelockPending, false, true, false, true));
        }

        beginTest ("Triggered mode has no live-history fallback");
        {
            expectEquals ((int) triggeredSweepSourceFor (false),
                          (int) TriggeredSweepSource::None);
            expectEquals ((int) triggeredSweepSourceFor (true),
                          (int) TriggeredSweepSource::HitCaptureSweep);
        }

        beginTest ("A relock edge is one-shot while pending");
        {
            expect (shouldAutoRelockKickReference (KickReferenceState::Locked,
                                                   false, true, true, true));
            expect (! shouldAutoRelockKickReference (KickReferenceState::RelockPending,
                                                    true, true, true, true));
            expect (! shouldAutoRelockKickReference (KickReferenceState::RelockPending,
                                                    true, true, true, true));
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
            expectWithinAbsoluteError (scopeAutoGainTargetFromPeak (2.0f), 0.44f, 1.0e-5f);
            expectWithinAbsoluteError (scopeAutoGainTargetFromPeak (0.001f), 40.0f, 1.0e-4f); // clamped ceiling
            expectWithinAbsoluteError (scopeAutoGainTargetFromPeak (0.0f), 1.0f, 1.0e-6f);    // silence stays neutral
        }

        beginTest ("Scope auto-gain stays sticky across normal hit-to-hit peak changes");
        {
            // Near-equal hits must NOT nudge the gain (that reads as visual
            // auto-zoom). Only genuinely new level ranges retarget it.
            expect (! scopeAutoGainShouldRetarget (2.0f, 2.0f));
            expect (! scopeAutoGainShouldRetarget (2.0f, 2.4f));
            expect (! scopeAutoGainShouldRetarget (2.0f, 1.5f));
            expect (scopeAutoGainShouldRetarget (2.0f, 5.2f));
            expect (scopeAutoGainShouldRetarget (2.0f, 1.2f));
            expect (scopeAutoGainShouldRetarget (0.0f, 1.0f));   // unseeded target always takes

            expectGreaterThan (scopeGlideAutoGain (1.0f, 4.0f), 1.0f);
            expectLessThan (scopeGlideAutoGain (4.0f, 1.0f), 4.0f);
        }

        beginTest ("Sweep ghosts are not drawn over the current trace");
        {
            const int count = 4;
            for (int i = 0; i < count; ++i)
                expectWithinAbsoluteError (scopeSweepGhostAlpha (i, count), 0.0f, 1.0e-6f);

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

        beginTest ("Incomplete re-lock windows cannot become a locked reference");
        {
            const float validPeak = (float) std::pow (10.0, -30.0 / 20.0);

            expect (! scopeCompletedCaptureCanLock (8159, 8160, validPeak));
            expect (! scopeCompletedCaptureCanLock (8160, 8160, 1.0e-5f));
            expect (scopeCompletedCaptureCanLock (8160, 8160, validPeak));
        }
        beginTest ("Triggered Kick source selection uses locked reference when valid");
        {
            float sweepData[1] = { 0.1f };
            float refData[1]   = { 0.9f };

            auto noRef = selectTriggeredKickSource (false, refData, 100, sweepData, 10, 50, 101);
            expect (noRef.data == sweepData);
            expectEquals (noRef.fillCount, 10);
            expectEquals ((int) noRef.hitId, 101);

            auto validRef = selectTriggeredKickSource (true, refData, 100, sweepData, 10, 50, 101);
            expect (validRef.data == refData);
            expectEquals (validRef.fillCount, 50);
            expectEquals ((int) validRef.hitId, 100);
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

class DynamicUiHelperTests : public juce::UnitTest
{
public:
    DynamicUiHelperTests() : juce::UnitTest ("DynamicUiHelpers", "UI Helpers") {}

    void runTest() override
    {
        beginTest ("Primary workflow maps Static and Dynamic states without synthetic transitions");
        {
            auto staticIdle = primaryWorkflowPresentation (false, LearnState::Idle, true, false);
            expectEquals (juce::String (staticIdle.text), juce::String ("Analyze"));
            expect (staticIdle.enabled);
            expectEquals ((int) staticIdle.action, (int) PrimaryWorkflowAction::StartAnalyze);

            // Strict ownership: unresolved Learn under Static never steals Analyze.
            for (const auto state : { LearnState::Capturing, LearnState::ResultReady,
                                      LearnState::Failed, LearnState::NotEnoughMaterial })
            {
                const auto staticOwned = primaryWorkflowPresentation (false, state, true, false);
                expectEquals (juce::String (staticOwned.text), juce::String ("Analyze"));
                expectEquals ((int) staticOwned.action, (int) PrimaryWorkflowAction::StartAnalyze);
                expect (primaryWorkflowIsLearnFamily (staticOwned) == false);
            }

            auto dynamicIdle = primaryWorkflowPresentation (true, LearnState::Idle, false, false);
            expectEquals (juce::String (dynamicIdle.text), juce::String ("Learn"));
            expect (dynamicIdle.enabled);
            expectEquals ((int) dynamicIdle.action, (int) PrimaryWorkflowAction::StartLearn);
            expect (primaryWorkflowIsLearnFamily (dynamicIdle));

            auto capturing = primaryWorkflowPresentation (true, LearnState::Capturing, false, false);
            expectEquals (primaryWorkflowText (capturing, 6), juce::String ("Stop Learn (6 hits)"));
            expect (capturing.enabled);
            expectEquals ((int) capturing.action, (int) PrimaryWorkflowAction::StopLearn);

            for (const auto state : { LearnState::Preparing, LearnState::Stopping, LearnState::Draining,
                                      LearnState::Finalizing, LearnState::Cancelling })
                expect (! primaryWorkflowPresentation (true, state, false, false).enabled);

            for (const auto state : { LearnState::ResultReady, LearnState::NotEnoughMaterial, LearnState::Failed })
            {
                const auto resolved = primaryWorkflowPresentation (true, state, false, false);
                expectEquals (juce::String (resolved.text), juce::String ("Learn Again"));
                expectEquals ((int) resolved.action, (int) PrimaryWorkflowAction::LearnAgain);
            }
        }

        beginTest ("Mode transition actions are edge-triggered and idempotent");
        {
            const auto none = modeTransitionActions (CorrectionMode::Static, CorrectionMode::Static);
            expect (! none.cancelActiveLearn && ! none.discardPendingLearn
                    && ! none.resetAnalyzePresentation && ! none.clearLearnPresentation);

            const auto toStatic = modeTransitionActions (CorrectionMode::Dynamic, CorrectionMode::Static);
            expect (toStatic.cancelActiveLearn);
            expect (toStatic.discardPendingLearn);
            expect (toStatic.clearLearnPresentation);
            expect (! toStatic.resetAnalyzePresentation);

            const auto toDynamic = modeTransitionActions (CorrectionMode::Static, CorrectionMode::Dynamic);
            expect (toDynamic.resetAnalyzePresentation);
            expect (! toDynamic.cancelActiveLearn);
            expect (! toDynamic.discardPendingLearn);

            // Construction in Static with leftover Learn only clears presentation chrome —
            // processor-owned Learn must survive editor open/close.
            const auto openStatic = initialModeActions (CorrectionMode::Static);
            expect (! openStatic.cancelActiveLearn);
            expect (! openStatic.discardPendingLearn);
            expect (openStatic.clearLearnPresentation);

            const auto openDynamic = initialModeActions (CorrectionMode::Dynamic);
            expect (openDynamic.resetAnalyzePresentation);
            expect (! openDynamic.discardPendingLearn);
        }

        beginTest ("Impossible Static/Dynamic UI combinations are rejected");
        {
            // Static + Learn family primary / Apply Learn / Discard / progress / chips
            expect (isImpossibleWorkflowCombo (CorrectionMode::Static, true, false, false, false, false));
            expect (isImpossibleWorkflowCombo (CorrectionMode::Static, false, true, false, false, false));
            expect (isImpossibleWorkflowCombo (CorrectionMode::Static, false, false, true, false, false));
            expect (isImpossibleWorkflowCombo (CorrectionMode::Static, false, false, false, true, false));
            expect (isImpossibleWorkflowCombo (CorrectionMode::Static, false, false, false, false, true));
            expect (! isImpossibleWorkflowCombo (CorrectionMode::Static, false, false, false, false, false));

            // Dynamic + Analyze primary
            expect (isImpossibleWorkflowCombo (CorrectionMode::Dynamic, false, false, false, true, false));
            expect (! isImpossibleWorkflowCombo (CorrectionMode::Dynamic, true, true, true, true, true));

            // Dynamic must not show Apply Fix as the Dynamic action
            expect (isImpossibleDynamicApplyFixAsAction (CorrectionMode::Dynamic, true, true, false));
            expect (! isImpossibleDynamicApplyFixAsAction (CorrectionMode::Dynamic, false, true, false));
            expect (! isImpossibleDynamicApplyFixAsAction (CorrectionMode::Dynamic, true, false, true));
            expect (! isImpossibleDynamicApplyFixAsAction (CorrectionMode::Static, true, true, false));
        }

        beginTest ("Workflow button labels fit top-bar widths at common OS scales");
        {
            const auto labels = workflowButtonLabelsToFit();
            for (const int scalePct : { 100, 125, 150, 200 })
            {
                const float scale = uiScaleFactor (scalePct);
                const int primaryW = scaledPx (kPrimaryWorkflowButtonWidth, scale);
                const int applyW = scaledPx (kApplyWorkflowButtonWidth, scale);
                const int discardW = scaledPx (kDiscardWorkflowButtonWidth, scale);
                // 12.5 pt approximates TextButton label font; leave 10 px padding.
                for (const auto& label : labels)
                {
                    const int textW = measureUiTextWidth (label, 12.5f * scale);
                    if (label == "Discard")
                        expect (textW + scaledPx (10, scale) <= discardW,
                                "Discard overflow at " + juce::String (scalePct) + "%: " + juce::String (textW));
                    else if (label.startsWith ("Apply"))
                        expect (textW + scaledPx (10, scale) <= applyW,
                                label + " overflow at " + juce::String (scalePct) + "%: " + juce::String (textW));
                    else
                        expect (textW + scaledPx (10, scale) <= primaryW,
                                label + " overflow at " + juce::String (scalePct) + "%: " + juce::String (textW));
                }
            }
        }

        beginTest ("Learn progress two-line metrics fit panel; chips below metrics");
        {
            expect (learnProgressChipsBeginBelowMetrics());
            expect (learnProgressChipOriginY() >= kLearnProgressMetricsBlockHeight);

            LearnProgressSnapshot progress;
            progress.capturedHits = 999;
            progress.drainedHits = 999;
            progress.pendingQueueHits = 999;
            progress.pitchAcceptedHits = 999;
            progress.rejectedPitchHits = 999;
            progress.timingUsableHits = 999;
            expect (learnProgressMetricsFitWidth (kLearnProgressMinWidthForThreeDigitCounters, progress));
            // Too narrow must fail (proves helper actually measures).
            expect (! learnProgressMetricsFitWidth (80, progress));
        }

        beginTest ("Long Learn failure body keeps reason first and fits analyzer bounds");
        {
            LearnFinalizeResult result;
            result.message = "Too few usable timing observations.";
            result.diagnostics.capturedHits = 55;
            result.diagnostics.rejectedPitchHits = 55;
            result.diagnostics.analyzedHits = 1;
            const auto body = formatLearnFailureBody (result);
            expect (body.startsWith ("Too few usable timing observations."));
            expect (body.indexOf ("Too few usable timing observations.")
                    < body.indexOf ("Captured:"));
            // Analyzer body region is typically ~200+ px tall after title/ref.
            expect (failureBodyFitsBounds (body, 280, 200, 12.5f));
            expect (! failureBodyFitsBounds (body, 280, 20, 12.5f));
        }

        beginTest ("Dynamic Failed/ResultReady -> Static presentation becomes Analyze immediately");
        {
            for (const auto state : { LearnState::Failed, LearnState::ResultReady, LearnState::NotEnoughMaterial })
            {
                const auto after = primaryWorkflowPresentation (false, state, true, false);
                expectEquals (juce::String (after.text), juce::String ("Analyze"));
                expectEquals ((int) after.action, (int) PrimaryWorkflowAction::StartAnalyze);
                expect (! primaryWorkflowIsLearnFamily (after));
                expect (! learnApplyEnabled (state, true) || state == LearnState::ResultReady);
                // Even with pending, Static presentation never offers Apply Learn.
                expect (! isImpossibleWorkflowCombo (CorrectionMode::Static, false, false, false, false, false));
            }

            // Transition actions clear pending Learn UI state.
            const auto edge = modeTransitionActions (CorrectionMode::Dynamic, CorrectionMode::Static);
            expect (edge.discardPendingLearn);
            expect (edge.cancelActiveLearn);
            expect (edge.clearLearnPresentation);
        }

        beginTest ("Static Analyze result -> Dynamic presentation becomes Learn immediately");
        {
            const auto after = primaryWorkflowPresentation (true, LearnState::Idle, true, false);
            expectEquals (juce::String (after.text), juce::String ("Learn"));
            expectEquals ((int) after.action, (int) PrimaryWorkflowAction::StartLearn);
            expect (primaryWorkflowIsLearnFamily (after));

            const auto edge = modeTransitionActions (CorrectionMode::Static, CorrectionMode::Dynamic);
            expect (edge.resetAnalyzePresentation);
            // Pending Learn discard is Dynamic->Static only; applied map is never cleared by actions.
            expect (! edge.discardPendingLearn);
            expect (! edge.cancelActiveLearn);
            expect (! edge.clearLearnPresentation);
        }

        beginTest ("Mode switch APIs cancel/discard Learn without clearing applied map");
        {
            KickLockAudioProcessor processor;
            processor.prepareToPlay (48000.0, 512);

            // No applied map by default. Mode-switch actions must not invent clearNoteMap.
            expect (! processor.hasValidNoteMap());

            // Idle cancel/discard are no-ops and must not throw or clear state.
            processor.cancelLearn();
            expectEquals ((int) processor.getLearnState(), (int) LearnState::Idle);
            expect (! processor.discardLatestLearnResult());

            // Dynamic parameter can be written by host automation without feedback loop
            // (editor listener must not write correction_mode back).
            if (auto* parameter = processor.apvts.getParameter ("correction_mode"))
            {
                parameter->beginChangeGesture();
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (1.0f));
                parameter->endChangeGesture();
                expect (processor.apvts.getRawParameterValue ("correction_mode")->load() > 0.5f);

                parameter->beginChangeGesture();
                parameter->setValueNotifyingHost (parameter->convertTo0to1 (0.0f));
                parameter->endChangeGesture();
                expect (processor.apvts.getRawParameterValue ("correction_mode")->load() < 0.5f);
            }

            // Simulated Dynamic Failed -> Static actions: cancel + discard, map untouched.
            const auto actions = modeTransitionActions (CorrectionMode::Dynamic, CorrectionMode::Static);
            if (actions.cancelActiveLearn)
                processor.cancelLearn();
            if (actions.discardPendingLearn)
                processor.discardLatestLearnResult();
            expectEquals ((int) processor.getLearnState(), (int) LearnState::Idle);
            // Still no map — proves transition does not call clearNoteMap.
            expect (! processor.hasValidNoteMap());
        }

        beginTest ("Rapid mode switching stays free of impossible combos");
        {
            CorrectionMode mode = CorrectionMode::Static;
            for (int i = 0; i < 20; ++i)
            {
                const auto next = (i % 2 == 0) ? CorrectionMode::Dynamic : CorrectionMode::Static;
                const auto actions = modeTransitionActions (mode, next);
                if (mode != next)
                    expect (actions.cancelActiveLearn == (next == CorrectionMode::Static)
                            || actions.resetAnalyzePresentation == (next == CorrectionMode::Dynamic));

                mode = next;
                const bool dynamic = mode == CorrectionMode::Dynamic;
                const auto primary = primaryWorkflowPresentation (dynamic, LearnState::Idle, true, false);
                const bool learnFamily = primaryWorkflowIsLearnFamily (primary);
                const bool applyIsLearn = dynamic; // Dynamic idle hides Apply; label would be Learn path
                const bool discardVisible = false;
                const bool progressVisible = dynamic;
                const bool chipsVisible = false;
                expect (! isImpossibleWorkflowCombo (mode, learnFamily, false, discardVisible,
                                                     progressVisible, chipsVisible));
                // Dynamic idle: Apply Fix must not be visible as Dynamic action
                expect (! isImpossibleDynamicApplyFixAsAction (mode, false, true, false));
                juce::ignoreUnused (applyIsLearn, actions);
            }

            // Idempotent: repeated same-mode transitions no-op.
            const auto again = modeTransitionActions (CorrectionMode::Dynamic, CorrectionMode::Dynamic);
            expect (! again.cancelActiveLearn && ! again.discardPendingLearn
                    && ! again.resetAnalyzePresentation && ! again.clearLearnPresentation);
        }

        beginTest ("correction_mode denormalize matches Static/Dynamic choice");
        {
            expectEquals ((int) correctionModeFromRaw (0.0f), (int) CorrectionMode::Static);
            expectEquals ((int) correctionModeFromRaw (0.4f), (int) CorrectionMode::Static);
            expectEquals ((int) correctionModeFromRaw (0.6f), (int) CorrectionMode::Dynamic);
            expectEquals ((int) correctionModeFromRaw (1.0f), (int) CorrectionMode::Dynamic);
            expect (workflowUsesLearnUi (CorrectionMode::Dynamic));
            expect (! workflowUsesLearnUi (CorrectionMode::Static));
        }

        beginTest ("Dynamic runtime status keeps the documented precedence");
        {
            expectEquals ((int) dynamicRuntimeStatus (true, true, true, true, false),
                          (int) DynamicRuntimeStatus::MapStale);
            expectEquals ((int) dynamicRuntimeStatus (true, false, false, true, true),
                          (int) DynamicRuntimeStatus::NoMap);
            expectEquals ((int) dynamicRuntimeStatus (true, false, true, false, true),
                          (int) DynamicRuntimeStatus::PhaseFilterOff);
            expectEquals ((int) dynamicRuntimeStatus (true, false, true, true, true),
                          (int) DynamicRuntimeStatus::Fallback);
            expectEquals (dynamicRuntimeStatusText (DynamicRuntimeStatus::LearnedNote, 28),
                          juce::String ("LEARNED E1"));
        }

        beginTest ("Dynamic Strength preserves its global-to-note contract");
        {
            expectEquals (formatDynamicStrength (0.0f), juce::String ("0%"));
            expectEquals (formatDynamicStrength (0.506f), juce::String ("51%"));
            expectEquals (formatDynamicStrength (1.0f), juce::String ("100%"));
            expectEquals (juce::String (dynamicStrengthTooltip()),
                          juce::String ("0% uses the global phase filter setting for every note. 100% uses each note's learned Freq/Q; Delay and Polarity stay global."));
        }

        beginTest ("Learn note chips use MIDI names and the shared readiness policy");
        {
            expectEquals (formatLearnNoteChip (28, 6), juce::String ("E1 x6"));
            expect (! learnNoteHasEnoughMaterial (NoteMap::kMinHitsPerNote - 1));
            expect (learnNoteHasEnoughMaterial (NoteMap::kMinHitsPerNote));
        }

        beginTest ("Learn progress labels are honest: PROCESSED not ANALYZED");
        {
            LearnProgressSnapshot progress;
            progress.state = LearnState::Idle;
            progress.capturedHits = 55;
            progress.drainedHits = 55;
            progress.pendingQueueHits = 0;
            progress.pitchAcceptedHits = 0;
            progress.rejectedPitchHits = 55;
            progress.timingUsableHits = 1;

            const auto line1 = formatLearnProgressSummaryLine1 (progress);
            const auto line2 = formatLearnProgressSummaryLine2 (progress);
            expect (line1.contains ("CAPTURED 55"));
            expect (line1.contains ("PROCESSED 55"));
            expect (line1.contains ("QUEUE 0"));
            expect (! line1.contains ("ANALYZED"));
            expect (! line1.contains ("HITS"));
            expect (line2.contains ("PITCH OK 0"));
            expect (line2.contains ("REJECTED 55"));
            expect (line2.contains ("TIMING OK 1"));
        }

        beginTest ("Learn listening status reports detected notes while capturing");
        {
            LearnProgressSnapshot progress;
            progress.state = LearnState::Capturing;
            progress.capturedHits = 4;
            progress.trackedNoteHitCounts[(size_t) NotePhaseMapSnapshot::indexForMidi (33)] = 2;
            progress.trackedNoteHitCounts[(size_t) NotePhaseMapSnapshot::indexForMidi (36)] = 1;
            const auto line1 = formatLearnProgressSummaryLine1 (progress);
            expect (line1.containsIgnoreCase ("Listening"));
            expect (line1.contains ("notes detected: 2"));
            const auto line2 = formatLearnProgressSummaryLine2 (progress);
            expect (line2.contains ("CAPTURED 4"));
            expect (line2.contains ("PITCH OK"));
        }

        beginTest ("Failed Learn body keeps backend message when present=false");
        {
            LearnFinalizeResult result;
            result.message = "Too few usable timing observations.";
            result.diagnostics.capturedHits = 55;
            result.diagnostics.rejectedPitchHits = 55;
            result.diagnostics.analyzedHits = 1;
            // Simulate a pitch-accepted-free hit list with one timing-usable path only.
            LearnHitAnalysis a;
            a.pitchAccepted = false;
            result.hitAnalyses.push_back (a);

            PendingLearnCandidate cand;
            cand.present = false;
            cand.result = result;

            // present gates Apply only.
            expect (! learnApplyEnabled (LearnState::NotEnoughMaterial, cand.present));
            expect (! learnApplyEnabled (LearnState::Failed, cand.present));
            expect (learnApplyEnabled (LearnState::ResultReady, true));
            expect (! learnApplyEnabled (LearnState::ResultReady, false));

            // UI still reads result even when present=false.
            const auto body = formatLearnFailureBody (cand.result);
            expect (body.startsWith ("Too few usable timing observations."));
            expect (body.contains ("Captured: 55"));
            expect (body.contains ("Pitch accepted: 0"));
            expect (body.contains ("Pitch rejected: 55"));
            expect (body.contains ("Timing usable: 1"));
            expect (! body.containsIgnoreCase ("Learn needs more usable kick and bass hits"));

            LearnFinalizeResult empty;
            const auto fallback = formatLearnFailureBody (empty);
            expect (fallback.containsIgnoreCase ("Learn needs more usable kick and bass hits"));
        }

        beginTest ("Per-note Learn outcome lines are exact for learned and fallback states");
        {
            const int a1 = NotePhaseMapSnapshot::indexForMidi (33);
            const int e2 = NotePhaseMapSnapshot::indexForMidi (40);
            const int e1 = NotePhaseMapSnapshot::indexForMidi (28);
            expect (a1 >= 0 && e2 >= 0 && e1 >= 0);

            LearnFinalizeResult result;
            NoteEntry confident;
            confident.learned = true;
            confident.fundamentalHz = 55.0f;
            confident.allpassFreqHz = 60.0f;
            confident.allpassQ = 0.5f;
            confident.confidence = 0.90f;
            confident.hitCount = 5;
            confident.rotatorHelps = true;
            expect (NoteMap::isValidNoteEntry (confident));
            if (a1 >= 0)
                result.map.notes[(size_t) a1] = confident;

            LearnNoteReport learned;
            learned.outcome = LearnNoteOutcome::Learned;
            learned.midi = 33;
            learned.acceptedHits = 5;
            if (a1 >= 0)
                result.noteReports[(size_t) a1] = learned;
            expectEquals (formatLearnNoteOutcomeLine (learned), juce::String ("A1: learned (5 hits)"));

            LearnNoteReport shortHits;
            shortHits.outcome = LearnNoteOutcome::NotEnoughOverlap;
            shortHits.midi = 40;
            shortHits.acceptedHits = 3;
            expectEquals (formatLearnNoteOutcomeLine (shortHits),
                          juce::String ("E2: not enough kick overlaps (3/4)"));

            LearnNoteReport enoughButRejected;
            enoughButRejected.outcome = LearnNoteOutcome::CorrectionNotConfident;
            enoughButRejected.midi = 33;
            enoughButRejected.acceptedHits = 6;
            expectEquals (formatLearnNoteOutcomeLine (enoughButRejected),
                          juce::String ("A1: recognized, but no confident correction (6 hits)"));

            NoteLearnAccumulator lowConfidence;
            for (int i = 0; i < NoteMap::kMinHitsPerNote; ++i)
                lowConfidence.add ({ 55.0f, 60.0f, 0.5f, 0.0f, false, 0.9f,
                                     NoteMap::kMinRuntimeConfidence - 0.01f, true });
            expect (! lowConfidence.finalizeEntry().learned);
            expectEquals (formatLearnNoteOutcomeLine (enoughButRejected),
                          juce::String ("A1: recognized, but no confident correction (6 hits)"));

            LearnHitAnalysis pitchInvalid;
            pitchInvalid.trackedFundamentalHz = 40.0f;
            pitchInvalid.pitchInvalid = true;
            LearnHitAnalysis octaveAmbiguous = pitchInvalid;
            octaveAmbiguous.pitchInvalid = false;
            octaveAmbiguous.octaveAmbiguous = true;
            expect (! pitchInvalid.pitchAccepted && ! octaveAmbiguous.pitchAccepted);

            LearnNoteReport rejectedPitch;
            rejectedPitch.outcome = LearnNoteOutcome::OutOfCorrectionWindow;
            rejectedPitch.midi = 28;
            rejectedPitch.recognizedHits = 2;
            rejectedPitch.outOfWindowHits = 2;
            expectEquals (formatLearnNoteOutcomeLine (rejectedPitch),
                          juce::String ("E1: outside correction window"));

            result.message = "Learned data did not form a valid map.";
            if (e2 >= 0) result.noteReports[(size_t) e2] = shortHits;
            if (e1 >= 0) result.noteReports[(size_t) e1] = rejectedPitch;
            const auto body = formatLearnFailureBody (result);
            expect (body.contains ("E2: not enough kick overlaps (3/4)"));
            expect (body.contains ("E1: outside correction window"));
            expect (body.contains ("Notes:"));
        }

        beginTest ("Peak delta pairs bass to the same kick hit, not a louder distant hit");
        {
            // Two hits ~400 ms apart. Hit 0: kick then bass +5 ms.
            // Hit 1: kick alone, then a much louder bass elsewhere would
            // previously produce Δ near the cross-hit distance (~400 ms).
            const int n = 24000; // 500 ms @ 48 kHz
            std::vector<float> bass ((size_t) n, 0.0f), kick ((size_t) n, 0.0f);
            const int kick0 = 2000;
            const int bass0 = kick0 + 240; // +5 ms
            const int kick1 = 2000 + 19200; // +400 ms
            const int bass1 = kick1 + 480; // +10 ms on second hit, louder
            kick[(size_t) kick0] = 0.9f;
            bass[(size_t) bass0] = 0.5f;
            kick[(size_t) kick1] = 0.6f; // quieter kick so hit0 dominates if detection works
            bass[(size_t) bass1] = 1.0f; // loudest bass overall

            const auto markers = findScopePeakMarkers (bass.data(), kick.data(), n, kSampleRate);
            expect (markers.valid);
            expectEquals (markers.kickPeakIndex, kick0);
            expectEquals (markers.bassPeakIndex, bass0);
            expectWithinAbsoluteError (markers.deltaMs, 5.0f, 1.0e-6f);
        }

        beginTest ("Mode and Strength attachments follow host restore without changing Pitch Follow");
        {
            KickLockAudioProcessor processor;
            SegmentedModeSelector modeSelector;
            juce::Slider strengthSlider;
            juce::AudioProcessorValueTreeState::ComboBoxAttachment modeAttachment (
                processor.apvts, "correction_mode", modeSelector.parameterCombo());
            juce::AudioProcessorValueTreeState::SliderAttachment strengthAttachment (
                processor.apvts, "dynamic_strength", strengthSlider);

            auto set = [&processor] (const char* id, float value)
            {
                if (auto* parameter = processor.apvts.getParameter (id))
                    parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
            };
            set ("pitch_track", 1.0f);
            set ("correction_mode", 1.0f);
            set ("dynamic_strength", 0.35f);
            expectEquals (modeSelector.parameterCombo().getSelectedItemIndex(), 1);
            expectWithinAbsoluteError ((float) strengthSlider.getValue(), 0.35f, 1.0e-6f);
            expectWithinAbsoluteError (processor.apvts.getRawParameterValue ("pitch_track")->load(), 1.0f, 1.0e-6f);

            juce::MemoryBlock state;
            processor.getStateInformation (state);
            set ("correction_mode", 0.0f);
            set ("dynamic_strength", 1.0f);
            processor.setStateInformation (state.getData(), (int) state.getSize());
            expectEquals (modeSelector.parameterCombo().getSelectedItemIndex(), 1);
            expectWithinAbsoluteError ((float) strengthSlider.getValue(), 0.35f, 1.0e-6f);
        }
    }
};

static DynamicUiHelperTests dynamicUiHelperTestsInstance;

