#pragma once

#include <juce_core/juce_core.h>

#include "MultiBandCorrelation.h"
#include "AudioTestHarness.h"
#include "AudioFixtureManifest.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

// =============================================================================
// AudioMetrics: per-event and global acceptance metrics for the real-audio
// harness, computed from the baseline (dry) bass, the corrected processor
// output, the kick reference, the ground-truth event list, and the recorded
// state timeline. Correlation/low-band figures reuse the production
// MultiBandCorrelation ruler (the same analysis the plugin's own Phase Match
// meter reads), so a metric here means the same thing the product means.
//
// The threshold evaluator enforces the spec's dual rule: never accept on
// average improvement alone - a correction-capable fixture must clear BOTH a
// minimum median low-band improvement AND a bounded worst-case per-event
// degradation.
// =============================================================================

namespace AudioMetrics
{
    // ---- shared small helpers ------------------------------------------------

    inline bool allFinite (const std::vector<float>& s)
    {
        for (const float v : s) if (! std::isfinite (v)) return false;
        return true;
    }

    inline float peakAbs (const std::vector<float>& s)
    {
        float p = 0.0f; for (const float v : s) p = std::max (p, std::abs (v)); return p;
    }

    // One-pole low-pass (sub/low emphasis) sum-of-squares energy over a window.
    inline double subEnergy (const float* x, int n, double sampleRate, double cutoffHz = 120.0)
    {
        if (n <= 0) return 0.0;
        const double a = std::exp (-2.0 * 3.14159265358979323846 * cutoffHz / sampleRate);
        double y = 0.0, energy = 0.0;
        for (int i = 0; i < n; ++i)
        {
            y = (1.0 - a) * (double) x[i] + a * y;
            energy += y * y;
        }
        return energy;
    }

    // Peak-cross-correlation lag (in samples, signed) of a low-passed signal
    // against a low-passed kick, searched over +/- maxLagSamples. Used as a
    // transient-displacement proxy.
    inline int transientLagSamples (const float* sig, const float* kick, int n,
                                    double sampleRate, int maxLagSamples)
    {
        if (n <= 4) return 0;
        const double a = std::exp (-2.0 * 3.14159265358979323846 * 120.0 / sampleRate);
        std::vector<double> ls ((size_t) n), lk ((size_t) n);
        double ys = 0.0, yk = 0.0;
        for (int i = 0; i < n; ++i)
        {
            ys = (1.0 - a) * (double) sig[i] + a * ys;  ls[(size_t) i] = ys;
            yk = (1.0 - a) * (double) kick[i] + a * yk;  lk[(size_t) i] = yk;
        }
        double best = -1.0e30; int bestLag = 0;
        for (int lag = -maxLagSamples; lag <= maxLagSamples; ++lag)
        {
            double acc = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const int j = i + lag;
                if (j < 0 || j >= n) continue;
                acc += ls[(size_t) i] * lk[(size_t) j];
            }
            if (acc > best) { best = acc; bestLag = lag; }
        }
        return bestLag;
    }

    // Localized transition score - the same click metric the release-gate and
    // E2E tests use (window 24), so a "click" here means what it means there.
    inline float localizedTransitionScore (const std::vector<float>& samples, int boundary)
    {
        constexpr int W = 24;
        constexpr float silenceEps = 1.0e-4f;
        if (boundary < W || boundary + W >= (int) samples.size())
            return 0.0f;
        double dEnergy = 0.0, sEnergy = 0.0; int count = 0;
        for (int i = boundary - W + 1; i < boundary; ++i)
        {
            const float d = samples[(size_t) i] - samples[(size_t) (i - 1)];
            dEnergy += (double) d * d; sEnergy += (double) samples[(size_t) i] * samples[(size_t) i]; ++count;
        }
        for (int i = boundary + 2; i < boundary + W; ++i)
        {
            const float d = samples[(size_t) i] - samples[(size_t) (i - 1)];
            dEnergy += (double) d * d; sEnergy += (double) samples[(size_t) i] * samples[(size_t) i]; ++count;
        }
        const float dRms = (float) std::sqrt (dEnergy / (double) std::max (1, count));
        const float sRms = (float) std::sqrt (sEnergy / (double) std::max (1, count));
        const float scale = std::max ({ silenceEps, dRms * 2.0f, sRms * 0.01f });
        return std::abs (samples[(size_t) boundary] - samples[(size_t) (boundary - 1)]) / scale;
    }

    // Short-window mean difference across a boundary - a DC-jump detector.
    inline float dcJump (const std::vector<float>& s, int boundary, int win = 64)
    {
        if (boundary < win || boundary + win >= (int) s.size()) return 0.0f;
        double before = 0.0, after = 0.0;
        for (int i = 0; i < win; ++i) { before += s[(size_t) (boundary - 1 - i)]; after += s[(size_t) (boundary + i)]; }
        return (float) std::abs (after / win - before / win);
    }

    // ---- timeline analysis (selection / loop-wrap continuity) ---------------

    inline int nonGlobalBlockCount (const std::vector<StateTimelineEntry>& tl)
    {
        int n = 0;
        for (const auto& e : tl)
            if (e.activeBranchKind != 0 /*Global*/ || e.selectedSemanticStateId != 0) ++n;
        return n;
    }

    inline int matchedPersistentBlockCount (const std::vector<StateTimelineEntry>& tl)
    {
        int n = 0;
        for (const auto& e : tl)
            if (e.activeBranchKind == 1 /*State*/) ++n;
        return n;
    }

    // The internal monotonic runtime timeline must never step backwards - a
    // confirmed loop wrap preserves it (does not reset it). Returns the number
    // of backward steps (0 => strictly non-decreasing).
    inline int internalTimestampBackwardSteps (const std::vector<StateTimelineEntry>& tl)
    {
        int back = 0;
        for (size_t i = 1; i < tl.size(); ++i)
            if (tl[i].internalTimestampSample < tl[i - 1].internalTimestampSample) ++back;
        return back;
    }

    // ---- metric result types -------------------------------------------------

    struct EventMetric
    {
        int sample = 0;
        int label = -1;
        bool correctionCapable = false;

        double baselineLowEndMatch = 0.0;   // 0..100
        double correctedLowEndMatch = 0.0;
        double improvement = 0.0;           // corrected - baseline
        double baselineSignedLowCorr = 0.0; // -1..1
        double correctedSignedLowCorr = 0.0;
        double phaseConflictBaseline = 0.0; // (1 - corr)/2, 0..1
        double phaseConflictCorrected = 0.0;
        double subEnergyLossPercent = 0.0;  // >= 0 means the low end lost energy
        double transientDisplacementBaselineMs = 0.0;
        double transientDisplacementCorrectedMs = 0.0;

        int decision = 0;      // DynamicMatchDecision sampled during the event body
        int branchKind = 0;    // DynamicSelectorBranchKind
        bool hold = false;
        bool global = true;
        uint64_t selectedStateId = 0;
    };

    struct GlobalMetrics
    {
        int eventCount = 0;
        int occupiedStateCount = 0;

        double medianImprovement = 0.0;
        double meanImprovement = 0.0;
        double worstCaseDegradation = 0.0;  // min improvement (negative = degradation)
        double maxConsistencyStdDev = 0.0;

        double unknownRate = 0.0;
        double ambiguousRate = 0.0;
        double holdRate = 0.0;
        double globalRate = 0.0;
        double stateSelectionAccuracy = -1.0; // -1 => no labeled ground truth

        float peak = 0.0f;
        bool outputFinite = true;
        double worstTransitionScore = 0.0;
        float worstDcJump = 0.0f;

        std::vector<EventMetric> events;
    };

    // Maps an output-sample index to the timeline block covering it (linear
    // render: blocks are contiguous appended output). Returns the last block if
    // out of range.
    inline const StateTimelineEntry* timelineAtOutputSample (const std::vector<StateTimelineEntry>& tl, int outSample)
    {
        int acc = 0;
        for (const auto& e : tl)
        {
            if (outSample < acc + e.numSamples)
                return &e;
            acc += e.numSamples;
        }
        return tl.empty() ? nullptr : &tl.back();
    }

    // Computes the full metric set for a LINEAR render (planLinear).
    inline GlobalMetrics compute (const ResolvedFixture& fx, const RenderResult& r,
                                  int latencySamples, int occupiedStateCount, double sampleRate)
    {
        GlobalMetrics g;
        g.occupiedStateCount = occupiedStateCount;
        g.outputFinite = allFinite (r.output);
        g.peak = peakAbs (r.output);

        const int total = fx.totalSamples();
        const int win = std::min ((int) (sampleRate * 0.16), std::max (256, total / 4));
        const int maxLag = (int) (sampleRate * 0.006);

        std::vector<double> improvements;
        std::map<int, std::vector<double>> byLabelCorrected;
        std::map<int, std::map<uint64_t, int>> byLabelSelectedVotes;
        int unknownCount = 0, ambiguousCount = 0, holdCount = 0, globalCount = 0, sampledEvents = 0;

        for (const auto& ev : fx.events)
        {
            EventMetric m;
            m.sample = ev.sample;
            m.label = ev.groundTruthLabel;
            m.correctionCapable = ev.correctionCapable;

            const int s = ev.sample;
            const int inEnd = std::min (total, s + win);
            const int outStart = s + latencySamples;
            const int outEnd = std::min ((int) r.output.size(), outStart + win);
            const int len = std::min (inEnd - s, outEnd - outStart);
            if (len > 128)
            {
                const auto baseline = MultiBandCorrelation::analyze (
                    fx.bass.data() + s, fx.kick.data() + s, len, sampleRate);
                const auto corrected = MultiBandCorrelation::analyze (
                    r.output.data() + outStart, fx.kick.data() + s, len, sampleRate);

                m.baselineLowEndMatch = baseline.lowEndMatchPercent;
                m.correctedLowEndMatch = corrected.lowEndMatchPercent;
                m.improvement = m.correctedLowEndMatch - m.baselineLowEndMatch;
                m.baselineSignedLowCorr = baseline.lowEndMatchPercent / 50.0 - 1.0;
                m.correctedSignedLowCorr = corrected.lowEndMatchPercent / 50.0 - 1.0;
                m.phaseConflictBaseline = std::clamp ((1.0 - m.baselineSignedLowCorr) / 2.0, 0.0, 1.0);
                m.phaseConflictCorrected = std::clamp ((1.0 - m.correctedSignedLowCorr) / 2.0, 0.0, 1.0);

                const double baseSub = subEnergy (fx.bass.data() + s, len, sampleRate);
                const double corrSub = subEnergy (r.output.data() + outStart, len, sampleRate);
                m.subEnergyLossPercent = baseSub > 1.0e-9 ? std::max (0.0, (baseSub - corrSub) / baseSub) * 100.0 : 0.0;

                m.transientDisplacementBaselineMs =
                    1000.0 * transientLagSamples (fx.bass.data() + s, fx.kick.data() + s, len, sampleRate, maxLag) / sampleRate;
                m.transientDisplacementCorrectedMs =
                    1000.0 * transientLagSamples (r.output.data() + outStart, fx.kick.data() + s, len, sampleRate, maxLag) / sampleRate;

                if (ev.correctionCapable)
                    improvements.push_back (m.improvement);
            }

            // Sample the runtime decision mid-body (after the fingerprint window
            // has resolved), at ~60 ms past the kick, in output-time.
            const int sampleAt = s + latencySamples + (int) (sampleRate * 0.06);
            if (const auto* tlEntry = timelineAtOutputSample (r.timeline, sampleAt))
            {
                m.decision = tlEntry->matcherDecision;
                m.branchKind = tlEntry->activeBranchKind;
                m.hold = tlEntry->holdActive;
                m.global = tlEntry->fallbackActive;
                m.selectedStateId = tlEntry->selectedSemanticStateId;

                ++sampledEvents;
                if (m.decision == 2) ++unknownCount;      // DynamicMatchDecision::Unknown
                if (m.decision == 1) ++ambiguousCount;     // Ambiguous
                if (m.hold) ++holdCount;
                if (m.global) ++globalCount;

                if (m.label >= 0)
                {
                    byLabelCorrected[m.label].push_back (m.correctedLowEndMatch);
                    if (m.selectedStateId != 0 && ! m.global)
                        byLabelSelectedVotes[m.label][m.selectedStateId]++;
                }
            }

            g.events.push_back (m);
        }

        g.eventCount = (int) fx.events.size();

        if (! improvements.empty())
        {
            auto sorted = improvements;
            std::sort (sorted.begin(), sorted.end());
            g.medianImprovement = sorted[sorted.size() / 2];
            g.worstCaseDegradation = sorted.front();
            double sum = 0.0; for (double v : sorted) sum += v;
            g.meanImprovement = sum / (double) sorted.size();
        }

        // Consistency: max stddev of corrected low-end match within a label.
        for (const auto& kv : byLabelCorrected)
        {
            const auto& vals = kv.second;
            if (vals.size() < 2) continue;
            double mean = 0.0; for (double v : vals) mean += v; mean /= (double) vals.size();
            double var = 0.0; for (double v : vals) var += (v - mean) * (v - mean); var /= (double) vals.size();
            g.maxConsistencyStdDev = std::max (g.maxConsistencyStdDev, std::sqrt (var));
        }

        if (sampledEvents > 0)
        {
            g.unknownRate = (double) unknownCount / (double) sampledEvents;
            g.ambiguousRate = (double) ambiguousCount / (double) sampledEvents;
            g.holdRate = (double) holdCount / (double) sampledEvents;
            g.globalRate = (double) globalCount / (double) sampledEvents;
        }

        // State-selection accuracy: each label's plurality selected state id must
        // be non-zero and distinct across labels; accuracy = fraction of labeled
        // events selecting their label's plurality id.
        int labeledEvents = 0, correctEvents = 0;
        std::map<int, uint64_t> pluralityForLabel;
        for (const auto& kv : byLabelSelectedVotes)
        {
            uint64_t best = 0; int bestVotes = -1;
            for (const auto& v : kv.second)
                if (v.second > bestVotes) { bestVotes = v.second; best = v.first; }
            pluralityForLabel[kv.first] = best;
        }
        // Detect collapsed families (two labels sharing a plurality id).
        std::map<uint64_t, int> idOwners;
        for (const auto& kv : pluralityForLabel) idOwners[kv.second]++;
        for (const auto& ev : g.events)
        {
            if (ev.label < 0) continue;
            ++labeledEvents;
            const auto it = pluralityForLabel.find (ev.label);
            if (it == pluralityForLabel.end() || it->second == 0) continue;
            if (idOwners[it->second] > 1) continue; // collapsed -> not a correct distinct selection
            if (ev.selectedStateId == it->second && ! ev.global) ++correctEvents;
        }
        if (labeledEvents > 0)
            g.stateSelectionAccuracy = (double) correctEvents / (double) labeledEvents;

        // Artifact scan at transition boundaries (branch/state changes) and at
        // every block boundary flagged as an iteration start (loop wrap / seek /
        // stop-start resume).
        int acc = 0;
        for (size_t i = 1; i < r.timeline.size(); ++i)
        {
            acc += r.timeline[i - 1].numSamples;
            const auto& prev = r.timeline[i - 1];
            const auto& cur = r.timeline[i];
            const bool changed = prev.activeBranchKind != cur.activeBranchKind
                || prev.selectedSemanticStateId != cur.selectedSemanticStateId
                || cur.firstBlockOfIteration;
            if (changed)
            {
                g.worstTransitionScore = std::max (g.worstTransitionScore, (double) localizedTransitionScore (r.output, acc));
                g.worstDcJump = std::max (g.worstDcJump, dcJump (r.output, acc));
            }
        }

        return g;
    }

    // ---- serialization -------------------------------------------------------

    inline juce::var toJson (const juce::String& fixtureName, const GlobalMetrics& g,
                             const RenderResult& r, bool fromRealOverride)
    {
        auto* root = new juce::DynamicObject();
        root->setProperty ("fixture", fixtureName);
        root->setProperty ("fromRealOverride", fromRealOverride);
        root->setProperty ("eventCount", g.eventCount);
        root->setProperty ("occupiedStateCount", g.occupiedStateCount);
        root->setProperty ("medianImprovement", g.medianImprovement);
        root->setProperty ("meanImprovement", g.meanImprovement);
        root->setProperty ("worstCaseDegradation", g.worstCaseDegradation);
        root->setProperty ("maxConsistencyStdDev", g.maxConsistencyStdDev);
        root->setProperty ("unknownRate", g.unknownRate);
        root->setProperty ("ambiguousRate", g.ambiguousRate);
        root->setProperty ("holdRate", g.holdRate);
        root->setProperty ("globalRate", g.globalRate);
        root->setProperty ("stateSelectionAccuracy", g.stateSelectionAccuracy);
        root->setProperty ("peak", g.peak);
        root->setProperty ("outputFinite", g.outputFinite);
        root->setProperty ("worstTransitionScore", g.worstTransitionScore);
        root->setProperty ("worstDcJump", g.worstDcJump);
        root->setProperty ("processedSeconds", r.processedSeconds);
        root->setProperty ("wallSeconds", r.wallSeconds);
        root->setProperty ("realtimeFactor", r.realtimeFactor());

        juce::Array<juce::var> events;
        for (const auto& e : g.events)
        {
            auto* eo = new juce::DynamicObject();
            eo->setProperty ("sample", e.sample);
            eo->setProperty ("label", e.label);
            eo->setProperty ("correctionCapable", e.correctionCapable);
            eo->setProperty ("baselineLowEndMatch", e.baselineLowEndMatch);
            eo->setProperty ("correctedLowEndMatch", e.correctedLowEndMatch);
            eo->setProperty ("improvement", e.improvement);
            eo->setProperty ("phaseConflictBaseline", e.phaseConflictBaseline);
            eo->setProperty ("phaseConflictCorrected", e.phaseConflictCorrected);
            eo->setProperty ("subEnergyLossPercent", e.subEnergyLossPercent);
            eo->setProperty ("transientDisplacementBaselineMs", e.transientDisplacementBaselineMs);
            eo->setProperty ("transientDisplacementCorrectedMs", e.transientDisplacementCorrectedMs);
            eo->setProperty ("decision", e.decision);
            eo->setProperty ("branchKind", e.branchKind);
            eo->setProperty ("hold", e.hold);
            eo->setProperty ("global", e.global);
            eo->setProperty ("selectedStateId", juce::String (e.selectedStateId));
            events.add (juce::var (eo));
        }
        root->setProperty ("events", events);
        return juce::var (root);
    }

    inline juce::String stateTimelineCsv (const RenderResult& r)
    {
        juce::String csv = "blockIndex,hostTimeInSamples,internalTimestampSample,loopIteration,firstBlockOfIteration,"
                           "numSamples,source,mapValid,selectedStateId,activeStateId,branchKind,matcherDecision,"
                           "selectorDiagnostic,hold,fallback\n";
        for (const auto& e : r.timeline)
        {
            csv << e.blockIndex << "," << e.hostTimeInSamples << "," << e.internalTimestampSample << ","
                << e.loopIteration << "," << (int) e.firstBlockOfIteration << "," << e.numSamples << ","
                << e.source << "," << (int) e.mapValid << "," << juce::String (e.selectedSemanticStateId) << ","
                << juce::String (e.activeSemanticStateId) << "," << e.activeBranchKind << "," << e.matcherDecision << ","
                << e.selectorDiagnostic << "," << (int) e.holdActive << "," << (int) e.fallbackActive << "\n";
        }
        return csv;
    }

    inline juce::String transportTimelineCsv (const RenderResult& r)
    {
        juce::String csv = "blockIndex,hostTimeInSamples,loopIteration,firstBlockOfIteration,numSamples\n";
        for (const auto& e : r.timeline)
            csv << e.blockIndex << "," << e.hostTimeInSamples << "," << e.loopIteration << ","
                << (int) e.firstBlockOfIteration << "," << e.numSamples << "\n";
        return csv;
    }

    // Evaluates a fixture's thresholds, returning the list of failed threshold
    // descriptions (empty => all pass). Enforces the dual improvement rule.
    inline juce::StringArray evaluateThresholds (const FixtureManifestEntry& entry, const GlobalMetrics& g)
    {
        juce::StringArray failed;
        const auto& t = entry.thresholds;

        if (! g.outputFinite)
            failed.add ("output contains NaN/Inf");
        if (g.peak > t.maxPeak)
            failed.add ("peak " + juce::String (g.peak, 3) + " > maxPeak " + juce::String (t.maxPeak, 3));
        if (g.worstTransitionScore > t.maxLocalizedTransitionScore)
            failed.add ("worstTransitionScore " + juce::String (g.worstTransitionScore, 3)
                        + " > max " + juce::String (t.maxLocalizedTransitionScore, 3));
        if (g.worstDcJump > t.maxDcJump)
            failed.add ("worstDcJump " + juce::String (g.worstDcJump, 3) + " > max " + juce::String (t.maxDcJump, 3));

        // Dual acceptance rule (spec item 5): assert BOTH the median-not-degraded
        // gate and the bounded worst-case-degradation gate. Only skipped for the
        // explicitly flagged extreme-timbre fixtures (which are safety+diagnosis
        // only; their degradation is recorded/logged, never silently accepted).
        if (t.gateDegradation && g.eventCount > 0)
        {
            if (g.medianImprovement < t.minMedianLowBandImprovementPercent)
                failed.add ("medianImprovement " + juce::String (g.medianImprovement, 3)
                            + " < min " + juce::String (t.minMedianLowBandImprovementPercent, 3));
            if (g.worstCaseDegradation < -t.maxWorstCaseDegradationPercent)
                failed.add ("worstCaseDegradation " + juce::String (g.worstCaseDegradation, 3)
                            + " < -" + juce::String (t.maxWorstCaseDegradationPercent, 3));
        }

        if (g.unknownRate > t.maxUnknownRate)
            failed.add ("unknownRate " + juce::String (g.unknownRate, 3) + " > max " + juce::String (t.maxUnknownRate, 3));
        if (g.holdRate > t.maxHoldRate)
            failed.add ("holdRate " + juce::String (g.holdRate, 3) + " > max " + juce::String (t.maxHoldRate, 3));
        if (g.globalRate > t.maxGlobalRate)
            failed.add ("globalRate " + juce::String (g.globalRate, 3) + " > max " + juce::String (t.maxGlobalRate, 3));

        return failed;
    }
}
