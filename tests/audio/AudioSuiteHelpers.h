#pragma once

#include "AudioTestHarness.h"
#include "AudioMetrics.h"

#include <vector>

// Shared helpers used by the Smoke / Acceptance / Stress suites.
namespace AudioSuite
{
    // Two renders are "identical" if their per-block state-event timelines match
    // exactly (selection, branch, decision, hold, fallback, source). This is the
    // determinism contract the spec asks for - NOT a cross-platform bit-exact WAV
    // comparison.
    inline bool timelinesIdentical (const std::vector<StateTimelineEntry>& a,
                                    const std::vector<StateTimelineEntry>& b)
    {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
        {
            const auto& x = a[i];
            const auto& y = b[i];
            if (x.selectedSemanticStateId != y.selectedSemanticStateId
                || x.activeSemanticStateId != y.activeSemanticStateId
                || x.activeBranchKind != y.activeBranchKind
                || x.matcherDecision != y.matcherDecision
                || x.selectorDiagnostic != y.selectorDiagnostic
                || x.holdActive != y.holdActive
                || x.fallbackActive != y.fallbackActive
                || x.source != y.source)
                return false;
        }
        return true;
    }

    // The worst localized transition ("click") score across every real branch /
    // selected-state change and every iteration-start (loop wrap / seek / resume)
    // boundary in a render - the artifact gate around switches, crossfades, loop
    // wrap, seek recovery and fallback transitions (spec item 6).
    inline double worstBoundaryTransitionScore (const RenderResult& r)
    {
        double worst = 0.0;
        int acc = 0;
        for (size_t i = 1; i < r.timeline.size(); ++i)
        {
            acc += r.timeline[i - 1].numSamples;
            const auto& prev = r.timeline[i - 1];
            const auto& cur = r.timeline[i];
            const bool boundary = prev.activeBranchKind != cur.activeBranchKind
                || prev.selectedSemanticStateId != cur.selectedSemanticStateId
                || cur.firstBlockOfIteration;
            if (boundary)
                worst = std::max (worst, (double) AudioMetrics::localizedTransitionScore (r.output, acc));
        }
        return worst;
    }

    // Same as worstBoundaryTransitionScore, but on the dry input reference the
    // harness spliced together to build this exact render (same read-position
    // jumps for a seek/stop-start). A seek or transport jump is INHERENTLY
    // discontinuous in the raw dry signal - that is not a processing defect and
    // is not something the plugin can or should smooth over. The meaningful
    // click-safety claim is that PROCESSING doesn't add a further, materially
    // worse discontinuity beyond what the dry splice itself already contains.
    // latencySamples accounts for the processor's own reported PDC: the dry
    // splice at input position P reappears in the OUTPUT at P + latencySamples,
    // so the dry-side score is read latencySamples earlier than the output-side
    // boundary it is being compared against.
    inline double worstBoundaryTransitionScoreAddedByProcessing (const RenderResult& r, int latencySamples)
    {
        double worstOutput = 0.0, worstDry = 0.0;
        int acc = 0;
        for (size_t i = 1; i < r.timeline.size(); ++i)
        {
            acc += r.timeline[i - 1].numSamples;
            const auto& prev = r.timeline[i - 1];
            const auto& cur = r.timeline[i];
            const bool boundary = prev.activeBranchKind != cur.activeBranchKind
                || prev.selectedSemanticStateId != cur.selectedSemanticStateId
                || cur.firstBlockOfIteration;
            if (boundary)
            {
                worstOutput = std::max (worstOutput, (double) AudioMetrics::localizedTransitionScore (r.output, acc));
                const int dryBoundary = acc - latencySamples;
                if (dryBoundary >= 0 && dryBoundary < (int) r.dryBassReference.size())
                    worstDry = std::max (worstDry, (double) AudioMetrics::localizedTransitionScore (r.dryBassReference, dryBoundary));
            }
        }
        return std::max (0.0, worstOutput - worstDry);
    }
}
