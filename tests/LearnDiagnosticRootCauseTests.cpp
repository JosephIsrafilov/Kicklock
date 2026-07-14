#include "TestCommon.h"

#include "LearnHitQueue.h"
#include "LearnPipelineCore.h"
#include "NotePhaseMap.h"
#include "NoteQuantizer.h"
#include "OfflineFundamentalEstimator.h"
#include "PitchTracker.h"
#include "TransientDetector.h"
#include "ui/DynamicUiHelpers.h"

#include <map>
#include <sstream>
#include <string>
#include <vector>

// =============================================================================
// Real-chain Dynamic Learn regression coverage (Commit 2).
//
// PitchTracker -> TransientDetector -> LearnHitQueue -> LearnPipelineCore
// -> NotePhaseMap
//
// Does NOT inject trackedHzAtTrigger = expectedBassHz.
// Does NOT change production thresholds, post-roll, or delay budget.
// Locks CURRENT production behavior (including known defects) before Commit 3.
// =============================================================================

namespace
{
    struct HitDiag
    {
        int sequence = -1;
        int expectedMidi = -1;
        float bassOnsetMs = 0.0f;
        float trackedHzAtTrigger = 0.0f;
        float offlineHz = 0.0f;
        float offlineConf = 0.0f;
        float cents = 0.0f;
        int quantizedMidi = -1;
        bool pitchAccepted = false;
        bool pitchInvalid = false;
        bool pitchDisagrees = false;
        bool octaveAmbiguous = false;
        bool timingUsable = false;
        const char* pitchClass = "none";
    };

    struct ScenarioResult
    {
        juce::String name;
        double sampleRate = 0.0;
        int musicalKicks = 0;
        int detections = 0;
        int completedCaptures = 0;
        int drained = 0;
        int ignoredOverlaps = 0;
        int droppedHits = 0;
        int pitchAccepted = 0;
        int pitchRejected = 0;
        int timingUsable = 0;
        int learnedNotes = 0;
        bool finalizeValid = false;
        juce::String finalizeMessage;
        std::array<int, NotePhaseMapSnapshot::size> noteHitCounts {};
        std::vector<HitDiag> hits;
        std::vector<int> triggerSamples;
    };

    void configureProductionLearnDetector (TransientDetector& d, double sr)
    {
        d.prepare (sr);
        d.setThreshold (1.0e-7f);
        d.setMinimumEnergyGate (1.0e-8f);
        d.setAttackReleaseMs (2.0f, 60.0f);
        d.setTriggerRatio (1.35f);
        d.setHoldoffMs (90.0f);
    }

    LearnPipelineConfig reducedConfig (double sampleRate)
    {
        // Reduced F/Q grid: tests target pitch/capture behavior, not full rotator search.
        LearnPipelineConfig c;
        c.sampleRate = sampleRate;
        c.delayInterpolation = InterpolationType::Linear;
        c.crossoverEnabled = false;
        c.maxDelayMs = PhaseFixEngine::defaultAutoFixMaxDelayMs;
        c.rotatorFreqs = { 60.0f, 90.0f, 140.0f };
        c.rotatorQs = { 0.7f, 1.5f };
        c.rotatorStages = { 2 };
        return c;
    }

    float isolatedKickSample (int local, double sr) noexcept
    {
        if (local < 0)
            return 0.0f;
        const double t = (double) local / sr;
        const double body = std::exp (-t * 35.0) * std::sin (kTwoPi * 55.0 * t);
        const double click = 0.55 * std::exp (-t * 450.0) * std::sin (kTwoPi * 2200.0 * t);
        return (float) (0.95 * body + click);
    }

    float deepKickSample (int local, double sr) noexcept
    {
        if (local < 0)
            return 0.0f;
        const double t = (double) local / sr;
        const double click = (t < 0.008)
            ? 0.9 * std::exp (-t * 500.0) * std::sin (kTwoPi * 1800.0 * t) : 0.0;
        const double body = 0.7 * std::exp (-t * 6.0) * std::sin (kTwoPi * 50.0 * t);
        // Secondary energy bump ~180 ms after attack (after 90 ms holdoff).
        const double bump = (t > 0.17 && t < 0.22)
            ? 0.35 * std::exp (-(t - 0.18) * 80.0) * std::sin (kTwoPi * 70.0 * t) : 0.0;
        return (float) (click + body + bump);
    }

    float bassTone (double t, double hz, float amp) noexcept
    {
        if (t < 0.0)
            return 0.0f;
        const double env = 1.0 - std::exp (-t * 80.0);
        const double tone = std::sin (kTwoPi * hz * t)
                          + 0.35 * std::sin (kTwoPi * 2.0 * hz * t);
        return amp * (float) (env * tone);
    }

    const char* classifyPitch (const LearnHitAnalysis& a)
    {
        if (a.pitchAccepted)
            return "accepted";
        if (a.pitchInvalid)
            return "pitchInvalid";
        if (a.octaveAmbiguous)
            return "octaveAmbiguous";
        if (a.pitchDisagrees)
            return "pitchDisagrees";
        return "unknown";
    }

    juce::String hitTable (const ScenarioResult& s)
    {
        std::ostringstream oss;
        oss << s.name << " @ " << s.sampleRate << " Hz\n"
            << "detections=" << s.detections
            << " completed=" << s.completedCaptures
            << " drained=" << s.drained
            << " pitchOk=" << s.pitchAccepted
            << " pitchRej=" << s.pitchRejected
            << " timingOk=" << s.timingUsable
            << " learned=" << s.learnedNotes
            << " valid=" << (s.finalizeValid ? "yes" : "no")
            << " msg=\"" << s.finalizeMessage << "\"\n";
        for (const auto& h : s.hits)
        {
            oss << "  seq=" << h.sequence
                << " expMidi=" << h.expectedMidi
                << " onsetMs=" << h.bassOnsetMs
                << " tracked=" << h.trackedHzAtTrigger
                << " offline=" << h.offlineHz
                << " conf=" << h.offlineConf
                << " cents=" << h.cents
                << " qMidi=" << h.quantizedMidi
                << " timing=" << (h.timingUsable ? 1 : 0)
                << " class=" << h.pitchClass << "\n";
        }
        return juce::String (oss.str());
    }

    ScenarioResult runRealChain (const juce::String& name,
                                 double sr,
                                 const std::vector<float>& bass,
                                 const std::vector<float>& kick,
                                 const std::vector<int>& expectedMidiAtSample,
                                 float bassOnsetMs,
                                 int musicalKicks)
    {
        jassert (bass.size() == kick.size());

        TransientDetector det;
        configureProductionLearnDetector (det, sr);

        PitchTracker tracker;
        tracker.prepare (sr, NoteMap::kFundamentalMinHz, NoteMap::kFundamentalMaxHz);

        LearnHitQueue q;
        q.prepare (sr, 24, 20.0f, 150.0f);

        ScenarioResult s;
        s.name = name;
        s.sampleRate = sr;
        s.musicalKicks = musicalKicks;

        for (int i = 0; i < (int) bass.size(); ++i)
        {
            tracker.pushSample (bass[(size_t) i]);
            const bool trig = det.processSample (kick[(size_t) i]);
            if (trig)
            {
                ++s.detections;
                s.triggerSamples.push_back (i);
            }
            q.pushSample (bass[(size_t) i], kick[(size_t) i], trig,
                          tracker.getFrequencyHz(), true);
        }

        // Finish any in-progress capture without new triggers.
        const int flush = q.getWindowSamples() + 8;
        for (int i = 0; i < flush; ++i)
            q.pushSample (0.0f, 0.0f, false, tracker.getFrequencyHz(), false);

        s.completedCaptures = q.getAcceptedHitCount();
        s.ignoredOverlaps = q.getIgnoredOverlapCount();
        s.droppedHits = q.getDroppedHitCount();

        std::vector<LearnHitWindow> windows;
        LearnHitWindow w;
        while (q.pop (w))
            windows.push_back (std::move (w));
        s.drained = (int) windows.size();

        const auto result = LearnPipelineCore::finalize (windows, reducedConfig (sr));
        s.finalizeValid = result.valid;
        s.finalizeMessage = result.message;
        s.timingUsable = result.diagnostics.analyzedHits;
        s.pitchRejected = result.diagnostics.rejectedPitchHits;

        for (const auto& e : result.map.notes)
            if (NoteMap::isValidNoteEntry (e))
                ++s.learnedNotes;

        for (const auto& a : result.hitAnalyses)
        {
            HitDiag d;
            d.sequence = a.sequence;
            d.trackedHzAtTrigger = a.trackedFundamentalHz;
            d.offlineHz = a.offlineFundamentalHz;
            d.offlineConf = a.offlinePitchConfidence;
            d.cents = a.pitchCentsDifference;
            d.quantizedMidi = NoteQuantizer::hzToMidi (a.trackedFundamentalHz);
            d.pitchAccepted = a.pitchAccepted;
            d.pitchInvalid = a.pitchInvalid;
            d.pitchDisagrees = a.pitchDisagrees;
            d.octaveAmbiguous = a.octaveAmbiguous;
            d.timingUsable = a.timingUsable;
            d.pitchClass = classifyPitch (a);
            d.bassOnsetMs = bassOnsetMs;

            if (d.sequence >= 0 && d.sequence < (int) s.triggerSamples.size())
            {
                const int ts = s.triggerSamples[(size_t) d.sequence];
                if (ts >= 0 && ts < (int) expectedMidiAtSample.size())
                    d.expectedMidi = expectedMidiAtSample[(size_t) ts];
            }

            if (a.pitchAccepted)
            {
                ++s.pitchAccepted;
                const int idx = NotePhaseMapSnapshot::indexForMidi (d.quantizedMidi);
                if (idx >= 0)
                    ++s.noteHitCounts[(size_t) idx];
            }
            s.hits.push_back (d);
        }

        return s;
    }

    // Isolated short kick + pitched bass with fixed onset relative to each kick.
    void buildIsolatedKickBass (double sr,
                                const std::vector<double>& noteHz,
                                int repsPerNote,
                                float kickPeriodMs,
                                float bassOnsetMs,
                                float bassDurationMs,
                                bool longSilenceBetweenNotes,
                                std::vector<float>& bass,
                                std::vector<float>& kick,
                                std::vector<int>& expectedMidiAtSample,
                                int& musicalKicks)
    {
        const int period = juce::jmax (1, (int) std::lround (sr * (double) kickPeriodMs / 1000.0));
        const int silencePad = longSilenceBetweenNotes
            ? (int) std::lround (sr * 0.45)   // > PitchTracker 300 ms silence timeout
            : 0;
        musicalKicks = (int) noteHz.size() * repsPerNote;
        const int total = period * musicalKicks
                        + silencePad * juce::jmax (0, (int) noteHz.size() - 1)
                        + (int) std::lround (sr * 0.25);
        bass.assign ((size_t) total, 0.0f);
        kick.assign ((size_t) total, 0.0f);
        expectedMidiAtSample.assign ((size_t) total, -1);

        int hit = 0;
        int cursor = (int) std::lround (sr * 0.05);
        for (size_t n = 0; n < noteHz.size(); ++n)
        {
            const double hz = noteHz[n];
            const int midi = NoteQuantizer::hzToMidi ((float) hz);
            for (int r = 0; r < repsPerNote; ++r, ++hit)
            {
                const int kickAt = cursor + r * period;
                for (int i = 0; i < (int) std::lround (sr * 0.08); ++i)
                {
                    const int idx = kickAt + i;
                    if (idx >= 0 && idx < total)
                        kick[(size_t) idx] += isolatedKickSample (i, sr);
                }

                const int bassAt = kickAt + (int) std::lround (sr * (double) bassOnsetMs / 1000.0);
                const int bassEnd = bassAt + (int) std::lround (sr * (double) bassDurationMs / 1000.0);
                for (int i = bassAt; i < bassEnd && i < total; ++i)
                {
                    if (i < 0)
                        continue;
                    const double t = (double) (i - bassAt) / sr;
                    bass[(size_t) i] += bassTone (t, hz, 0.55f);
                    expectedMidiAtSample[(size_t) i] = midi;
                }
                for (int i = juce::jmax (0, kickAt - 2); i < juce::jmin (total, kickAt + 2); ++i)
                    expectedMidiAtSample[(size_t) i] = midi;
            }
            cursor += repsPerNote * period;
            if (n + 1 < noteHz.size())
                cursor += silencePad;
        }
    }

    void buildPreActiveBass (double sr,
                             double hz,
                             int reps,
                             float kickPeriodMs,
                             std::vector<float>& bass,
                             std::vector<float>& kick,
                             std::vector<int>& expectedMidiAtSample,
                             int& musicalKicks)
    {
        // Bass already sounding before first kick and continuous through all kicks.
        musicalKicks = reps;
        const int period = juce::jmax (1, (int) std::lround (sr * (double) kickPeriodMs / 1000.0));
        const int pre = (int) std::lround (sr * 0.40);
        const int total = pre + period * reps + (int) std::lround (sr * 0.20);
        bass.assign ((size_t) total, 0.0f);
        kick.assign ((size_t) total, 0.0f);
        expectedMidiAtSample.assign ((size_t) total, NoteQuantizer::hzToMidi ((float) hz));

        for (int i = 0; i < total; ++i)
            bass[(size_t) i] = bassTone ((double) i / sr, hz, 0.55f);

        for (int h = 0; h < reps; ++h)
        {
            const int kickAt = pre + h * period;
            for (int i = 0; i < (int) std::lround (sr * 0.08); ++i)
                if (kickAt + i < total)
                    kick[(size_t) (kickAt + i)] += isolatedKickSample (i, sr);
        }
    }

    int distinctAcceptedNotes (const ScenarioResult& s)
    {
        int n = 0;
        for (int c : s.noteHitCounts)
            if (c > 0)
                ++n;
        return n;
    }

    int firstTransitionHoldCount (const ScenarioResult& s)
    {
        // Count hits where expected MIDI differs from quantized tracked MIDI
        // and pitch was rejected as disagreement (classic previous-note hold).
        int count = 0;
        for (const auto& h : s.hits)
            if (h.expectedMidi >= 0 && h.quantizedMidi >= 0
                && h.expectedMidi != h.quantizedMidi
                && h.pitchDisagrees)
                ++count;
        return count;
    }
}

class LearnRealChainRegressionTests : public juce::UnitTest
{
public:
    LearnRealChainRegressionTests()
        : juce::UnitTest ("LearnRealChainRegression", "Learn") {}

    void runTest() override
    {
        const double rates[] = { 44100.0, 48000.0, 96000.0 };

        // ==============================================================
        beginTest ("A. Correct-routing onset lattice at 44.1/48/96 kHz");
        {
            for (const double sr : rates)
            {
                // Bass already active before kick.
                {
                    std::vector<float> bass, kick;
                    std::vector<int> midi;
                    int musical = 0;
                    buildPreActiveBass (sr, 55.0, 6, 480.0f, bass, kick, midi, musical);
                    auto s = runRealChain ("pre_active_bass", sr, bass, kick, midi, -400.0f, musical);
                    expectEquals (s.completedCaptures, s.drained);
                    expect (s.detections >= musical - 1 && s.detections <= musical + 2,
                            hitTable (s));
                    // Pre-active bass should produce some pitch-accepted hits at mid register.
                    // Do not require finalizeValid: reduced rotator grid / timing may still fail.
                    expect (s.pitchAccepted >= 3, hitTable (s));
                }

                for (const float onsetMs : { 0.0f, 10.0f, 30.0f, 100.0f, 150.0f, 160.0f })
                {
                    std::vector<float> bass, kick;
                    std::vector<int> midi;
                    int musical = 0;
                    buildIsolatedKickBass (sr, { 55.0 }, 6, 480.0f, onsetMs, 280.0f, false,
                                           bass, kick, midi, musical);
                    auto s = runRealChain ("onset_" + juce::String (onsetMs, 0) + "ms",
                                           sr, bass, kick, midi, onsetMs, musical);

                    expectEquals (s.completedCaptures, s.drained, hitTable (s));
                    expect (s.detections >= musical - 1 && s.detections <= musical + 2, hitTable (s));
                    expect (s.droppedHits == 0, hitTable (s));

                    if (onsetMs <= 30.0f)
                    {
                        // Early onset: pitch path can accept after tracker stabilizes.
                        expect (s.pitchAccepted >= 3, hitTable (s));
                    }
                    else if (onsetMs >= 150.0f)
                    {
                        // Outside Learn 150 ms post-roll / correction domain.
                        // Must NOT silently become a valid phase correction map.
                        expect (! s.finalizeValid, hitTable (s));
                        expect (s.learnedNotes == 0, hitTable (s));
                        expect (s.pitchAccepted == 0, hitTable (s));
                        expect (s.finalizeMessage.isNotEmpty(), hitTable (s));
                        // Specific backend reason, not empty / not generic-only silence.
                        expect (s.finalizeMessage.containsIgnoreCase ("timing")
                                || s.finalizeMessage.containsIgnoreCase ("budget")
                                || s.finalizeMessage.containsIgnoreCase ("observation")
                                || s.finalizeMessage.containsIgnoreCase ("No hits"),
                                hitTable (s));
                    }
                    else if (onsetMs >= 100.0f)
                    {
                        // Near edge: pitch usually fails; must not invent a learned map.
                        expect (s.learnedNotes == 0, hitTable (s));
                        expect (s.pitchAccepted == 0, hitTable (s));
                    }
                }
            }
        }

        // ==============================================================
        beginTest ("B. Three-note Learn exposes stale trigger pitch at note transitions");
        {
            const std::vector<double> notes = { 55.0, 65.41, 82.41 }; // A1 C2 E2
            expectEquals (NoteQuantizer::hzToMidi (55.0f), 33);
            expectEquals (NoteQuantizer::hzToMidi (65.41f), 36);
            expectEquals (NoteQuantizer::hzToMidi (82.41f), 40);

            for (const double sr : rates)
            {
                // B1: >=5 reps/note — lock current pitch-acceptance behavior.
                {
                    std::vector<float> bass, kick;
                    std::vector<int> midi;
                    int musical = 0;
                    buildIsolatedKickBass (sr, notes, 5, 480.0f, 0.0f, 300.0f, false,
                                           bass, kick, midi, musical);
                    auto s = runRealChain ("three_note_5reps", sr, bass, kick, midi, 0.0f, musical);

                    expectEquals (s.completedCaptures, s.drained, hitTable (s));
                    // First hit of each new note often holds previous pitch.
                    expect (firstTransitionHoldCount (s) >= 1, hitTable (s));
                    // With 5 reps and real tracker, current production typically
                    // pitch-accepts all three buckets (after burning transition hits).
                    // Lock that we get AT LEAST two distinct accepted notes; third may
                    // still fail elsewhere (timing/rotator) — pitch layer is the focus.
                    expect (distinctAcceptedNotes (s) >= 2, hitTable (s));
                    // UI chip limit is 6 — not the loss mechanism for 3 notes.
                    expect (distinctAcceptedNotes (s) <= 6);
                }

                // B2: third note only 4 musical kicks, but first transition burns one
                // accepted hit → third note often ends below kMinHitsPerNote after
                // pitch filtering. Build unequal sequence manually.
                {
                    std::vector<double> seq;
                    for (int i = 0; i < 5; ++i) seq.push_back (55.0);
                    for (int i = 0; i < 5; ++i) seq.push_back (65.41);
                    for (int i = 0; i < 4; ++i) seq.push_back (82.41); // exactly four

                    const int period = (int) std::lround (sr * 0.48);
                    const int total = period * (int) seq.size() + (int) (sr * 0.2);
                    std::vector<float> bass ((size_t) total, 0.0f), kick ((size_t) total, 0.0f);
                    std::vector<int> midi ((size_t) total, -1);
                    for (int h = 0; h < (int) seq.size(); ++h)
                    {
                        const int kickAt = (int) (sr * 0.05) + h * period;
                        for (int i = 0; i < (int) (sr * 0.08); ++i)
                            if (kickAt + i < total)
                                kick[(size_t) (kickAt + i)] += isolatedKickSample (i, sr);
                        for (int i = 0; i < (int) (sr * 0.30) && kickAt + i < total; ++i)
                        {
                            bass[(size_t) (kickAt + i)] = bassTone ((double) i / sr, seq[(size_t) h], 0.5f);
                            midi[(size_t) (kickAt + i)] = NoteQuantizer::hzToMidi ((float) seq[(size_t) h]);
                        }
                    }
                    auto s = runRealChain ("three_note_third_has_4", sr, bass, kick, midi, 0.0f,
                                           (int) seq.size());

                    const int midi40 = NotePhaseMapSnapshot::indexForMidi (40);
                    const int acceptedThird = midi40 >= 0 ? s.noteHitCounts[(size_t) midi40] : 0;
                    // Transition hold on first E2 hit means acceptedThird is typically
                    // < kMinHitsPerNote (4). Document: loss is min-hit / pitch path,
                    // not the six-chip UI limit.
                    expect (distinctAcceptedNotes (s) <= 3, hitTable (s));
                    expect (acceptedThird <= 4, hitTable (s));
                    if (acceptedThird > 0 && acceptedThird < NoteMap::kMinHitsPerNote)
                    {
                        // Explicit regression lock: third note dies at aggregation
                        // threshold, not because MIDI merge collapsed it.
                        expectEquals (NoteQuantizer::hzToMidi (82.41f), 40);
                    }
                    // Prove NoteQuantizer does not merge A1/C2/E2.
                    expectEquals (NoteQuantizer::hzToMidi (55.0f), 33);
                    expectEquals (NoteQuantizer::hzToMidi (65.41f), 36);
                    expectEquals (NoteQuantizer::hzToMidi (82.41f), 40);
                }

                // B3: transitions exactly on kicks (continuous bass, no gap).
                {
                    const int reps = 5;
                    const int period = (int) std::lround (sr * 0.5);
                    const int totalHits = 3 * reps;
                    const int pre = (int) (sr * 0.4);
                    const int total = pre + period * totalHits + (int) (sr * 0.2);
                    std::vector<float> bass ((size_t) total, 0.0f), kick ((size_t) total, 0.0f);
                    std::vector<int> midi ((size_t) total, -1);

                    for (int i = 0; i < pre; ++i)
                        bass[(size_t) i] = bassTone ((double) i / sr, notes[0], 0.5f);

                    for (int hit = 0; hit < totalHits; ++hit)
                    {
                        const int ni = hit % 3;
                        const int kickAt = pre + hit * period;
                        for (int i = 0; i < (int) (sr * 0.08); ++i)
                            if (kickAt + i < total)
                                kick[(size_t) (kickAt + i)] += isolatedKickSample (i, sr);
                        for (int i = 0; i < period && kickAt + i < total; ++i)
                        {
                            bass[(size_t) (kickAt + i)] = bassTone ((double) i / sr, notes[(size_t) ni], 0.5f);
                            midi[(size_t) (kickAt + i)] = NoteQuantizer::hzToMidi ((float) notes[(size_t) ni]);
                        }
                    }

                    auto s = runRealChain ("three_note_transition_on_kick", sr, bass, kick, midi,
                                           0.0f, totalHits);
                    // Document defect: many hits hold previous note at trigger.
                    expect (firstTransitionHoldCount (s) >= 2, hitTable (s));
                }

                // B4: short silence (< 300 ms tracker timeout) between notes.
                {
                    std::vector<float> bass, kick;
                    std::vector<int> midi;
                    int musical = 0;
                    // kick period 200 ms + no long pad → silence between note groups short
                    buildIsolatedKickBass (sr, notes, 3, 200.0f, 0.0f, 120.0f, false,
                                           bass, kick, midi, musical);
                    auto s = runRealChain ("three_note_short_silence", sr, bass, kick, midi,
                                           0.0f, musical);
                    // Tracker may still hold previous note across short gaps.
                    expect (s.completedCaptures == s.drained, hitTable (s));
                }

                // B5: long silence clears tracker (> 300 ms).
                {
                    std::vector<float> bass, kick;
                    std::vector<int> midi;
                    int musical = 0;
                    buildIsolatedKickBass (sr, notes, 3, 480.0f, 0.0f, 200.0f, true,
                                           bass, kick, midi, musical);
                    auto s = runRealChain ("three_note_long_silence_clear", sr, bass, kick, midi,
                                           0.0f, musical);
                    // After long silence, first hit of a new group often has tracked=0
                    // (not previous note) until re-lock.
                    int zeroTrackedFirsts = 0;
                    for (const auto& h : s.hits)
                        if (h.sequence >= 0 && ! (h.trackedHzAtTrigger > 0.0f))
                            ++zeroTrackedFirsts;
                    expect (zeroTrackedFirsts >= 1, hitTable (s));
                }
            }
        }

        // ==============================================================
        beginTest ("C. Long-tail deep kick secondary trigger bound");
        {
            for (const double sr : rates)
            {
                const int musical = 8;
                const int period = (int) std::lround (sr * 0.55);
                const int total = period * musical + (int) (sr * 0.3);
                std::vector<float> bass ((size_t) total, 0.0f), kick ((size_t) total, 0.0f);
                std::vector<int> midi ((size_t) total, NoteQuantizer::hzToMidi (55.0f));

                for (int h = 0; h < musical; ++h)
                {
                    const int kickAt = (int) (sr * 0.05) + h * period;
                    for (int i = 0; i < (int) (sr * 0.35) && kickAt + i < total; ++i)
                        kick[(size_t) (kickAt + i)] += deepKickSample (i, sr);
                    // On-time bass so zero notes cannot be blamed on late onset.
                    for (int i = 0; i < (int) (sr * 0.30) && kickAt + i < total; ++i)
                        bass[(size_t) (kickAt + i)] = bassTone ((double) i / sr, 55.0, 0.55f);
                }

                auto s = runRealChain ("long_tail_kick", sr, bass, kick, midi, 0.0f, musical);
                // Bounded: secondary bump may add a few extra triggers, not spam.
                expect (s.detections >= musical, hitTable (s));
                expect (s.detections <= musical + 4, hitTable (s));
                // Overlaps possible if secondary fires during capture.
                expect (s.ignoredOverlaps >= 0, hitTable (s));
                // Long tail alone does not force zero pitch-accepted notes when bass is on-time.
                // (May still fail finalize for timing/rotator reasons with reduced grid.)
                expect (s.pitchAccepted >= 1 || s.timingUsable >= 1, hitTable (s));
            }
        }

        // ==============================================================
        beginTest ("D. Counter semantics: captured/drained/pitch/timing");
        {
            for (const double sr : rates)
            {
                std::vector<float> bass, kick;
                std::vector<int> midi;
                int musical = 0;
                buildIsolatedKickBass (sr, { 55.0 }, 6, 480.0f, 0.0f, 280.0f, false,
                                       bass, kick, midi, musical);
                auto s = runRealChain ("counter_semantics", sr, bass, kick, midi, 0.0f, musical);

                // capturedHits == completed LearnHitQueue windows
                expectEquals (s.completedCaptures, s.drained);
                // After drain, queue empty
                expect (s.drained == s.completedCaptures);
                // pitchAcceptedHits == pitchAccepted observations
                expectEquals (s.pitchAccepted, (int) std::count_if (
                    s.hits.begin(), s.hits.end(),
                    [] (const HitDiag& h) { return h.pitchAccepted; }));
                // timing usable matches hit flags
                int timingFlags = 0;
                for (const auto& h : s.hits)
                    if (h.timingUsable)
                        ++timingFlags;
                expectEquals (s.timingUsable, timingFlags);
                // drained is NOT "successful analysis"
                expect (s.drained >= s.timingUsable);
                expect (s.pitchRejected >= 0);
                int classifiedRejects = 0;
                for (const auto& h : s.hits)
                    if (h.pitchInvalid || h.pitchDisagrees || h.octaveAmbiguous)
                        ++classifiedRejects;
                expectEquals (s.pitchRejected, classifiedRejects, hitTable (s));
            }

            // Explicit: late path has drained >> timing usable / pitch ok
            {
                const double sr = 48000.0;
                std::vector<float> bass, kick;
                std::vector<int> midi;
                int musical = 0;
                buildIsolatedKickBass (sr, { 55.0 }, 6, 480.0f, 160.0f, 280.0f, false,
                                       bass, kick, midi, musical);
                auto s = runRealChain ("counter_late_not_success", sr, bass, kick, midi, 160.0f, musical);
                expect (s.drained >= 5, hitTable (s));
                expect (s.pitchAccepted == 0, hitTable (s));
                expect (s.timingUsable < s.drained, hitTable (s));
            }
        }

        // ==============================================================
        beginTest ("E. Failed result preserves backend message; Apply stays off");
        {
            for (const double sr : rates)
            {
                std::vector<float> bass, kick;
                std::vector<int> midi;
                int musical = 0;
                buildIsolatedKickBass (sr, { 55.0 }, 6, 480.0f, 160.0f, 280.0f, false,
                                       bass, kick, midi, musical);
                auto s = runRealChain ("failed_result_message", sr, bass, kick, midi, 160.0f, musical);

                expect (! s.finalizeValid, hitTable (s));
                expect (s.finalizeMessage.isNotEmpty(), hitTable (s));
                expect (! s.finalizeMessage.containsIgnoreCase ("Learn needs more usable kick and bass hits"),
                        hitTable (s));

                // Simulate present=false candidate with this finalize payload.
                LearnFinalizeResult result;
                result.valid = false;
                result.message = s.finalizeMessage;
                result.diagnostics.capturedHits = s.completedCaptures;
                result.diagnostics.rejectedPitchHits = s.pitchRejected;
                result.diagnostics.analyzedHits = s.timingUsable;
                for (const auto& h : s.hits)
                {
                    LearnHitAnalysis a;
                    a.pitchAccepted = h.pitchAccepted;
                    result.hitAnalyses.push_back (a);
                }

                PendingLearnCandidate cand;
                cand.present = false;
                cand.result = result;

                expect (! learnApplyEnabled (LearnState::NotEnoughMaterial, cand.present));
                expect (! learnApplyEnabled (LearnState::Failed, cand.present));

                const auto body = formatLearnFailureBody (cand.result);
                expect (body.startsWith (s.finalizeMessage), body);
                expect (body.contains ("Captured:"));
                expect (body.contains ("Pitch accepted:"));
                expect (body.contains ("Pitch rejected:"));
                expect (body.contains ("Timing usable:"));
            }
        }

        // ==============================================================
        beginTest ("F. Capture window sizing vs offline min length");
        {
            for (const double sr : rates)
            {
                LearnHitQueue q;
                q.prepare (sr, 24, 20.0f, 150.0f);
                expect (q.getPreRollSamples() > 0);
                expect (q.getPostRollSamples() == (int) std::lround (150.0 * sr / 1000.0)
                        || std::abs (q.getPostRollSamples() - (int) std::lround (150.0 * sr / 1000.0)) <= 1);
                // +156 ms onset is outside Learn post-roll.
                expect (q.getPostRollSamples() < (int) std::lround (sr * 0.156));
            }
        }
    }
};

static LearnRealChainRegressionTests learnRealChainRegressionTestsInstance;
