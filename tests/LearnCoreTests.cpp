#include "TestCommon.h"

#include "FixedTimingRotatorSearch.h"
#include "ConstrainedDtwRefiner.h"
#include "FrozenCrossoverPhaseSimulation.h"
#include "LearnHitQueue.h"
#include "LearnPipelineCore.h"
#include "NoteLearnAccumulator.h"
#include "NotePhaseMap.h"
#include "NoteQuantizer.h"

// =============================================================================
// Phase 3 tests: the offline, worker-side Learn core (plan v1.2).
//   * Pass A recovers ONE global Delay + Polarity (rotator disabled);
//   * FixedTimingRotatorSearch searches allpass F/Q/stages with timing FIXED;
//   * Pass B builds a per-note map with deterministic aggregation and the
//     section-5 acceptance limits;
//   * empty / invalid datasets yield an invalid map + diagnostics, never stale
//     or partial data.
// Synthetic material is exercised at 44.1 / 48 / 96 kHz.
// =============================================================================

namespace
{
    float sampleLinear (const std::vector<float>& x, double index) noexcept
    {
        if (index < 0.0 || index >= (double) (x.size() - 1))
            return 0.0f;
        const int i = (int) std::floor (index);
        const float frac = (float) (index - (double) i);
        return x[(size_t) i] + frac * (x[(size_t) i + 1] - x[(size_t) i]);
    }

    // One synthetic kick+bass hit window. A shared decaying low tone at `bassHz`
    // (fundamental + a 2nd-harmonic so polarity is unambiguous) is carried by both
    // signals: the bass is that tone delayed by `delayMs`; the kick is that tone
    // at the reference timing, polarity-inverted when `inverted`, plus a short
    // high-frequency click. The click sits above the 500 Hz analysis cap, so it
    // only anchors transient detection and never perturbs the low-band
    // correlation. Because both signals share the SAME low fundamental, the
    // analysis engine recovers the true signed delay and polarity (no
    // frequency-mismatch offset), while the offline estimator - which only sees
    // the bass - still reports `bassHz`.
    LearnHitWindow makeHit (double sampleRate,
                            double bassHz,
                            float delayMs,
                            bool inverted,
                            int sequence,
                            double durSec = 0.2,
                            float bassAmp = 0.8f,
                            float kickAmp = 0.8f,
                            std::vector<std::pair<double, double>> bassPartials = {},
                            float trackedHz = -1.0f)
    {
        LearnHitWindow w;
        w.sequence = sequence;
        w.trackedHzAtTrigger = trackedHz >= 0.0f ? trackedHz : (float) bassHz;

        const int n = (int) std::lround (sampleRate * durSec);
        const int start = (int) std::lround (sampleRate * 0.02);
        const float delaySamples = delayMs * (float) sampleRate / 1000.0f;

        if (bassPartials.empty())
            bassPartials = { { bassHz, 1.0 }, { 2.0 * bassHz, 0.4 } };

        // Shared low tone (unit amplitude) at the reference timing.
        std::vector<float> low ((size_t) n, 0.0f);
        for (int i = 0; i < n; ++i)
        {
            const int local = i - start;
            if (local < 0)
                continue;

            const double t = (double) local / sampleRate;
            const double toneEnv = std::exp (-t * 9.0);
            double tone = 0.0;
            for (const auto& p : bassPartials)
                tone += p.second * std::sin (kTwoPi * p.first * t);
            low[(size_t) i] = (float) (toneEnv * tone);
        }

        w.bass.assign ((size_t) n, 0.0f);
        w.kick.assign ((size_t) n, 0.0f);

        const float kickSign = inverted ? -1.0f : 1.0f;
        for (int i = 0; i < n; ++i)
        {
            // Bass: the shared tone, delayed.
            w.bass[(size_t) i] = bassAmp * sampleLinear (low, (double) i - (double) delaySamples);

            // Kick: the shared tone (maybe inverted) at reference timing, plus a
            // short HF click well above the 500 Hz analysis band.
            float click = 0.0f;
            const int local = i - start;
            if (local >= 0)
            {
                const double t = (double) local / sampleRate;
                const double clickEnv = std::exp (-t * 400.0);
                click = 0.5f * kickAmp * (float) (clickEnv * std::sin (kTwoPi * 2000.0 * t));
            }
            w.kick[(size_t) i] = kickSign * kickAmp * low[(size_t) i] + click;
        }

        return w;
    }

    LearnPipelineConfig testConfig (double sampleRate)
    {
        LearnPipelineConfig c;
        c.sampleRate = sampleRate;
        c.delayInterpolation = InterpolationType::Linear;
        c.crossoverEnabled = false;
        // Small rotator grid keeps the offline search cheap in tests; production
        // uses the full FixedTimingRotatorSearch defaults.
        c.rotatorFreqs = { 60.0f, 90.0f, 140.0f };
        c.rotatorQs = { 0.7f, 1.5f };
        c.rotatorStages = { 2, 3 };
        return c;
    }

    // The kick is an exact production allpass rendering of the bass.  This
    // gives the fixed-timing search a known positive target instead of relying
    // on an accidental phase relationship in the generic hit fixture.
    LearnHitWindow makeKnownRotatorHit (double sampleRate, double bassHz,
                                        float allpassHz, float q, int stages,
                                        int sequence)
    {
        auto hit = makeHit (sampleRate, bassHz, 0.0f, false, sequence);
        hit.kick = hit.bass;
        PhaseFixRenderSettings settings;
        settings.phaseFilterEnabled = true;
        settings.phaseFilterFreqHz = allpassHz;
        settings.phaseFilterQ = q;
        settings.phaseFilterStages = stages;
        settings.delayInterpolation = InterpolationType::Linear;
        PhaseFixEngine::renderCandidate (hit.bass.data(), (int) hit.bass.size(),
                                         sampleRate, settings, hit.kick);
        return hit;
    }

    int midiFor (double hz) { return NoteQuantizer::hzToMidi ((float) hz); }

    void addDeterministicNoise (LearnHitWindow& hit, float amount)
    {
        for (int i = 0; i < (int) hit.kick.size(); ++i)
            hit.kick[(size_t) i] += amount * std::sin ((float) i * 0.173f + 0.37f);
    }
}

//============================================================================//
// Learn-only constrained DTW timing refinement
//============================================================================//
class ConstrainedDtwRefinerTests : public juce::UnitTest
{
public:
    ConstrainedDtwRefinerTests() : juce::UnitTest ("ConstrainedDtwRefiner", "Phase3") {}

    void runTest() override
    {
        beginTest ("FFT-coarse Learn refinement recovers signed fractional offsets with noise");
        {
            const double sr = 48000.0;
            ConstrainedDtwRefiner::Scratch scratch;
            for (const float delayMs : { 2.35f, -2.65f })
            {
                auto hit = makeHit (sr, 55.0, delayMs, false, 0);
                addDeterministicNoise (hit, 0.005f);
                const auto coarse = PhaseFixEngine::analyze (
                    hit.bass.data(), hit.kick.data(), (int) hit.bass.size(), sr,
                    20.0f, InterpolationType::Linear, false);
                expect (coarse.valid && coarse.enoughSignal);
                const auto refined = ConstrainedDtwRefiner::refine (
                    hit.bass.data(), hit.kick.data(), (int) hit.bass.size(), sr,
                    coarse.bassDelayMs * (float) sr / 1000.0f,
                    coarse.bassPolarityInvert, scratch);
                expect (refined.valid && ! refined.ambiguous,
                        "delay=" + juce::String (delayMs, 2));
                expectWithinAbsoluteError (refined.delaySamples * 1000.0f / (float) sr,
                                           -delayMs, 0.20f,
                                           "delay=" + juce::String (delayMs, 2));

                const auto repeated = ConstrainedDtwRefiner::refine (
                    hit.bass.data(), hit.kick.data(), (int) hit.bass.size(), sr,
                    coarse.bassDelayMs * (float) sr / 1000.0f,
                    coarse.bassPolarityInvert, scratch);
                expectWithinAbsoluteError (repeated.delaySamples, refined.delaySamples, 0.0f,
                                           "reuses fixed scratch deterministically");
            }
        }

        beginTest ("Periodic alternatives inside the narrow band are rejected as ambiguous");
        {
            constexpr int n = 1200;
            std::vector<float> bass ((size_t) n), kick ((size_t) n);
            for (int i = 0; i < n; ++i)
                bass[(size_t) i] = kick[(size_t) i]
                    = (float) std::sin (kTwoPi * 4000.0 * (double) i / 48000.0);
            ConstrainedDtwRefiner::Scratch scratch;
            const auto result = ConstrainedDtwRefiner::refine (
                bass.data(), kick.data(), n, 48000.0, 0.0f, false, scratch);
            expect (! result.valid && result.ambiguous);
        }

        beginTest ("Cancellation is observed at bounded DTW work units");
        {
            auto hit = makeHit (48000.0, 55.0, 2.25f, false, 0);
            ConstrainedDtwRefiner::Scratch scratch;
            std::atomic<int> checks { 0 };
            const auto result = ConstrainedDtwRefiner::refine (
                hit.bass.data(), hit.kick.data(), (int) hit.bass.size(), 48000.0,
                2.25f * 48.0f, false, scratch,
                [&checks] { return checks.fetch_add (1, std::memory_order_relaxed) >= 2; });
            expect (result.cancelled && ! result.valid);
        }

        beginTest ("Multiple notes retain one refined scalar timing and offsets beyond 20 ms fail");
        {
            const double sr = 48000.0;
            std::vector<LearnHitWindow> multiNote;
            int sequence = 0;
            for (double hz : { 55.0, 82.41 })
                for (int i = 0; i < 5; ++i)
                    multiNote.push_back (makeHit (sr, hz, 2.35f, false, sequence++));
            const auto refined = LearnPipelineCore::finalize (multiNote, testConfig (sr));
            expect (refined.valid);
            expectWithinAbsoluteError (refined.map.base.delayMs, -2.35f, 0.10f);

            std::vector<LearnHitWindow> outsideBudget;
            for (int i = 0; i < 5; ++i)
                outsideBudget.push_back (makeHit (sr, 55.0, 25.25f, false, i));
            const auto rejected = LearnPipelineCore::finalize (outsideBudget, testConfig (sr));
            expect (! rejected.valid);
            expect (rejected.message.containsIgnoreCase ("budget"));
        }
    }
};

//============================================================================//
// Pass A - global timing recovery
//============================================================================//
class LearnPassATests : public juce::UnitTest
{
public:
    LearnPassATests() : juce::UnitTest ("LearnPassA", "Phase3") {}

    void runTest() override
    {
        const double rates[] = { 44100.0, 48000.0, 96000.0 };

        beginTest ("Signed global delay is recovered and distinguishes +/- across rates");
        {
            for (double sr : rates)
            {
                const double dur = sr > 90000.0 ? 0.16 : 0.2;
                const double bassHz = 55.0;   // A1 -> MIDI 33, in range

                auto build = [&] (float delayMs)
                {
                    std::vector<LearnHitWindow> hits;
                    for (int i = 0; i < 5; ++i)
                        hits.push_back (makeHit (sr, bassHz, delayMs, false, i, dur));
                    return LearnPipelineCore::finalize (hits, testConfig (sr));
                };

                const auto pos = build (+3.0f);
                const auto neg = build (-3.0f);

                expect (pos.valid, "positive-delay map valid sr=" + juce::String (sr, 0));
                expect (neg.valid, "negative-delay map valid sr=" + juce::String (sr, 0));

                // Opposite signs and clearly separated (half-period at 55 Hz is
                // ~9 ms, so a 3 ms delay never wraps into the other polarity).
                expect (pos.map.base.delayMs * neg.map.base.delayMs < 0.0f,
                        "opposite signs sr=" + juce::String (sr, 0)
                        + " pos=" + juce::String (pos.map.base.delayMs, 2)
                        + " neg=" + juce::String (neg.map.base.delayMs, 2));
                expectGreaterThan (std::abs (pos.map.base.delayMs - neg.map.base.delayMs), 3.0f,
                                   "separation sr=" + juce::String (sr, 0));

                // Global fix is a pure timing solution: Pass A never enables the
                // rotator (searchRotator = false).
                expect (! pos.globalFix.phaseFilterEnabled, "Pass A rotator disabled (pos)");
                expect (! neg.globalFix.phaseFilterEnabled, "Pass A rotator disabled (neg)");
            }
        }

        beginTest ("Polarity inversion is recovered; non-inverted stays non-inverted");
        {
            for (double sr : rates)
            {
                const double dur = sr > 90000.0 ? 0.16 : 0.2;

                std::vector<LearnHitWindow> inv, normal;
                for (int i = 0; i < 5; ++i)
                {
                    inv.push_back (makeHit (sr, 60.0, 0.0f, true, i, dur));
                    normal.push_back (makeHit (sr, 60.0, 0.0f, false, i, dur));
                }

                const auto invRes = LearnPipelineCore::finalize (inv, testConfig (sr));
                const auto normRes = LearnPipelineCore::finalize (normal, testConfig (sr));

                expect (invRes.valid && normRes.valid, "both valid sr=" + juce::String (sr, 0));
                expect (invRes.map.base.polarityInvert, "inverted recovered sr=" + juce::String (sr, 0));
                expect (! normRes.map.base.polarityInvert, "non-inverted recovered sr=" + juce::String (sr, 0));
                expectLessThan (std::abs (invRes.map.base.delayMs), 4.0f, "inverted delay ~0");
                expectLessThan (std::abs (normRes.map.base.delayMs), 4.0f, "normal delay ~0");
            }
        }

        beginTest ("The single global timing is shared by every learned note");
        {
            const double sr = 48000.0;
            // Two notes needing potentially different allpass corrections but the
            // SAME global timing. Per-note search must not move the base timing.
            std::vector<LearnHitWindow> hits;
            int seq = 0;
            for (int i = 0; i < 5; ++i) hits.push_back (makeHit (sr, 55.0, 2.0f, false, seq++));
            for (int i = 0; i < 5; ++i) hits.push_back (makeHit (sr, 82.41, 2.0f, false, seq++));

            const auto res = LearnPipelineCore::finalize (hits, testConfig (sr));
            expect (res.valid);

            const int mA = midiFor (55.0);
            const int mB = midiFor (82.41);
            const auto* a = res.map.lookup (mA);
            const auto* b = res.map.lookup (mB);
            expect (a != nullptr && b != nullptr);
            expect (a != nullptr && b != nullptr, "both note slots exist");

            // The map carries exactly one base delay/polarity/stage count.
            expect (res.map.base.delayMs == res.globalFix.bassDelayMs, "base delay == global fix delay");
            expect (res.map.base.polarityInvert == res.globalFix.bassPolarityInvert, "base polarity == global fix");
            expectGreaterOrEqual (res.map.base.allpassStages, 2);
            expectLessOrEqual (res.map.base.allpassStages, 4);
        }

        beginTest ("Invalid tracked pitch remains in Pass A but is global-only");
        {
            const double sr = 48000.0;
            std::vector<LearnHitWindow> hits;
            for (int i = 0; i < 5; ++i)
                hits.push_back (makeHit (sr, 55.0, 2.0f, false, i, 0.2, 0.8f, 0.8f, {}, 0.0f));
            const auto res = LearnPipelineCore::finalize (hits, testConfig (sr));
            expect (res.valid, "timing consensus is retained");
            expectEquals (res.diagnostics.analyzedHits, 5);
            expectEquals (res.diagnostics.pitchInvalidHits, 5);
            expectEquals (res.diagnostics.globalOnlyHits, 5);
            expect (res.hitAnalyses[0].timingUsable && ! res.hitAnalyses[0].pitchAccepted);
        }

        beginTest ("Fewer than three timing observations are rejected");
        {
            const double sr = 48000.0;
            std::vector<LearnHitWindow> hits;
            for (int i = 0; i < 2; ++i) hits.push_back (makeHit (sr, 55.0, 1.0f, false, i));
            const auto res = LearnPipelineCore::finalize (hits, testConfig (sr));
            expect (! res.valid);
            expect (res.message.containsIgnoreCase ("few"));
        }
    }
};

//============================================================================//
// Pass B - per-note map, quantization, aggregation, rejection
//============================================================================//
class LearnPassBTests : public juce::UnitTest
{
public:
    LearnPassBTests() : juce::UnitTest ("LearnPassB", "Phase3") {}

    void runTest() override
    {
        beginTest ("Pitch-agreeing multi-note dataset reaches its tracked MIDI buckets");
        {
            const double sr = 48000.0;
            const double notes[] = { 41.20, 55.00, 82.41 };   // MIDI 28, 33, 40
            std::vector<LearnHitWindow> hits;
            int seq = 0;
            for (double f : notes)
                for (int i = 0; i < 5; ++i)
                    hits.push_back (makeHit (sr, f, 1.5f, false, seq++));

            const auto res = LearnPipelineCore::finalize (hits, testConfig (sr));
            expect (res.valid);
            expectEquals (res.diagnostics.analyzedHits, 15);

            for (double f : notes)
            {
                const auto* e = res.map.lookup (midiFor (f));
                expect (e != nullptr, "tracked MIDI slot f=" + juce::String (f, 2));
            }
        }

        beginTest ("Tracked/offline disagreement is global-only");
        {
            const double sr = 48000.0;
            std::vector<LearnHitWindow> hits;
            // Deliberately wrong tracked pitch (999 Hz) on every hit.
            for (int i = 0; i < 5; ++i)
                hits.push_back (makeHit (sr, 55.0, 1.5f, false, i, 0.2, 0.8f, 0.8f, {}, 999.0f));

            const auto res = LearnPipelineCore::finalize (hits, testConfig (sr));
            expect (res.valid, "timing still produces a global fallback");
            const auto* e = res.map.lookup (midiFor (55.0));
            expect (e != nullptr && ! e->learned, "disagreement must not learn offline MIDI");
            expectEquals (res.diagnostics.pitchDisagreementHits, 5);
        }

        beginTest ("A dominant second harmonic still passes tracked/offline pitch validation");
        {
            const double sr = 48000.0;
            const double f0 = 55.0;
            std::vector<LearnHitWindow> hits;
            for (int i = 0; i < 5; ++i)
                hits.push_back (makeHit (sr, f0, 1.5f, false, i, 0.2, 0.8f, 0.8f,
                                         { { f0, 0.35 }, { 2.0 * f0, 1.0 }, { 3.0 * f0, 0.2 } }));

            const auto res = LearnPipelineCore::finalize (hits, testConfig (sr));
            expect (res.valid);
            expectEquals (res.diagnostics.rejectedPitchHits, 0);
            expect (res.hitAnalyses[0].pitchAccepted, "fundamental agreement accepted");
        }

        beginTest ("Detuned tracked pitch quantizes to the nearest semitone after agreement");
        {
            const double sr = 48000.0;
            const double detuned = 440.0 * std::pow (2.0, (33.0 + 0.2 - 69.0) / 12.0); // ~20 cents sharp of A1
            std::vector<LearnHitWindow> hits;
            for (int i = 0; i < 5; ++i)
                hits.push_back (makeHit (sr, detuned, 1.5f, false, i));

            const auto res = LearnPipelineCore::finalize (hits, testConfig (sr));
            expect (res.valid);
            expect (res.hitAnalyses[0].pitchAccepted, "detuned agreement accepted");
            expectEquals (NoteQuantizer::hzToMidi (res.hitAnalyses[0].trackedFundamentalHz), 33,
                          "tracked pitch quantizes to MIDI 33");
        }

        beginTest ("Octave-ambiguous material is global-only");
        {
            const double sr = 48000.0;
            const double f0 = 55.0;
            // Only even harmonics -> true period is 1/(2*f0); estimator reports 2*f0.
            auto build = [&]
            {
                std::vector<LearnHitWindow> hits;
                for (int i = 0; i < 5; ++i)
                    hits.push_back (makeHit (sr, f0, 1.5f, false, i, 0.2, 0.8f, 0.8f,
                                             { { 2.0 * f0, 0.7 }, { 4.0 * f0, 0.4 } }));
                return LearnPipelineCore::finalize (hits, testConfig (sr));
            };
            const auto a = build();
            const auto b = build();
            expect (a.valid && b.valid);
            // Deterministic: same learned-note set and same base timing.
            expect (a.map.base.delayMs == b.map.base.delayMs, "deterministic delay");
            expectEquals (a.diagnostics.octaveAmbiguousHits, 5);
            const auto* na = a.map.lookup (midiFor (2.0 * f0));
            expect (na != nullptr && ! na->learned, "octave ambiguity is not auto-corrected");
        }

        beginTest ("Fewer than the minimum hits: note falls back to global, map still valid");
        {
            const double sr = 48000.0;
            std::vector<LearnHitWindow> hits;
            for (int i = 0; i < 3; ++i)   // below kMinHitsPerNote (4)
                hits.push_back (makeHit (sr, 55.0, 1.5f, false, i));

            const auto res = LearnPipelineCore::finalize (hits, testConfig (sr));
            expect (res.valid, "map valid via global fallback");
            const auto* e = res.map.lookup (midiFor (55.0));
            expect (e != nullptr && ! e->learned, "under-min note not learned");
            expectEquals (res.diagnostics.globalOnlyHits, 3);
            expect (NoteMap::isValidGlobalEntry (res.map.global), "global fallback valid");
        }

        beginTest ("Global fallback is populated and valid even when a note is learned");
        {
            const double sr = 48000.0;
            std::vector<LearnHitWindow> hits;
            for (int i = 0; i < 6; ++i)
                hits.push_back (makeHit (sr, 55.0, 1.5f, false, i));

            const auto res = LearnPipelineCore::finalize (hits, testConfig (sr));
            expect (res.valid);
            expect (NoteMap::isValidGlobalEntry (res.map.global), "global valid");
            expect (res.map.global.allpassFreqHz > 0.0f && res.map.global.allpassQ > 0.0f);
            // An in-range note that was never learned would use the global fallback.
            const auto* unlearned = res.map.lookup (20);
            expect (unlearned != nullptr && ! unlearned->learned, "unlearned note present, uses global at runtime");
        }

    }
};

//============================================================================//
// Fixed-timing guarantees + global stage selection
//============================================================================//
class LearnFixedTimingTests : public juce::UnitTest
{
public:
    LearnFixedTimingTests() : juce::UnitTest ("LearnFixedTiming", "Phase3") {}

    void runTest() override
    {
        beginTest ("Rotator search never moves the fixed delay/polarity");
        {
            const double sr = 48000.0;
            auto hit = makeHit (sr, 55.0, 2.0f, false, 0);
            const int n = (int) hit.bass.size();

            const float fixedDelay = 2.5f;
            const bool fixedPol = true;

            const auto r = FixedTimingRotatorSearch::search (
                hit.bass.data(), hit.kick.data(), n, sr, fixedDelay, fixedPol,
                InterpolationType::Linear, { 2, 3 }, { 60.0f, 90.0f, 140.0f }, { 0.7f, 1.5f });

            expect (r.valid);
            // Every reported candidate keeps the SAME stage list and only varies
            // frequency/Q; the search API has no way to alter delay/polarity, and
            // the result carries none - the timing stays exactly what we passed.
            expect (! r.perStage.empty());
            for (const auto& c : r.perStage)
            {
                expect (c.allpassFreqHz >= 60.0f && c.allpassFreqHz <= 140.0f, "freq within grid");
                expect (c.allpassQ >= 0.7f && c.allpassQ <= 1.5f, "Q within grid");
                expect (c.stages == 2 || c.stages == 3, "stage within grid");
                expect (std::isfinite (c.matchPercent) && std::isfinite (c.gainPoints), "finite");
            }
        }

        beginTest ("rotatorHelps requires a real match-point gain");
        {
            const double sr = 48000.0;
            // Already well-aligned material: a rotator should not claim to help.
            auto hit = makeHit (sr, 60.0, 0.0f, false, 0);
            const int n = (int) hit.bass.size();

            const auto r = FixedTimingRotatorSearch::search (
                hit.bass.data(), hit.kick.data(), n, sr, 0.0f, false,
                InterpolationType::Linear, { 2 }, { 60.0f, 90.0f, 140.0f }, { 0.7f, 1.5f });

            expect (r.valid);
            for (const auto& c : r.perStage)
                if (c.helps)
                    expectGreaterOrEqual (c.gainPoints, FixedTimingRotatorSearch::kMinGainPoints,
                                          "helps => gain >= threshold");
        }

        beginTest ("One global stage count is chosen and lies in [2, 4]");
        {
            const double sr = 48000.0;
            std::vector<LearnHitWindow> hits;
            for (int i = 0; i < 6; ++i)
                hits.push_back (makeHit (sr, 55.0, 1.5f, false, i));

            auto cfg = testConfig (sr);
            const auto a = LearnPipelineCore::finalize (hits, cfg);
            const auto b = LearnPipelineCore::finalize (hits, cfg);

            expect (a.valid);
            expectGreaterOrEqual (a.map.base.allpassStages, 2);
            expectLessOrEqual (a.map.base.allpassStages, 4);
            expectEquals (a.map.base.allpassStages, b.map.base.allpassStages, "stage choice deterministic");
        }

        beginTest ("Clean, aligned material defaults to the fewest stages");
        {
            const double sr = 48000.0;
            std::vector<LearnHitWindow> hits;
            for (int i = 0; i < 6; ++i)
                hits.push_back (makeHit (sr, 60.0, 0.0f, false, i));

            LearnPipelineConfig cfg = testConfig (sr);
            cfg.rotatorStages = { 2, 3, 4 };
            const auto res = LearnPipelineCore::finalize (hits, cfg);
            expect (res.valid);
            // No stage helps meaningfully -> tie-break to the fewest stages.
            expectEquals (res.map.base.allpassStages, 2, "fewest stages when nothing helps");
        }

        beginTest ("Stage 4 and an invalid forced stage fail safely");
        {
            const double sr = 48000.0;
            auto hit = makeHit (sr, 60.0, 0.0f, false, 0);
            const auto stage4 = FixedTimingRotatorSearch::search (hit.bass.data(), hit.kick.data(), (int) hit.bass.size(), sr,
                0.0f, false, InterpolationType::Linear, { 4 }, { 60.0f }, { 0.7f });
            const auto forced4 = FixedTimingRotatorSearch::search (hit.bass.data(), hit.kick.data(), (int) hit.bass.size(), sr,
                0.0f, false, InterpolationType::Linear, { 2, 3 }, { 60.0f }, { 0.7f }, 4);
            expect (! stage4.valid && ! forced4.valid);
        }

        beginTest ("No-help global search preserves frozen manual F/Q");
        {
            const double sr = 48000.0;
            std::vector<LearnHitWindow> hits;
            for (int i = 0; i < 5; ++i) hits.push_back (makeHit (sr, 60.0, 0.0f, false, i));
            auto cfg = testConfig (sr);
            cfg.preLearnAllpassFreqHz = 177.0f;
            cfg.preLearnAllpassQ = 1.3f;
            const auto res = LearnPipelineCore::finalize (hits, cfg);
            expect (res.valid);
            expect (! res.map.global.rotatorHelps);
            expectWithinAbsoluteError (res.map.global.allpassFreqHz, 177.0f, 1.0e-6f);
            expectWithinAbsoluteError (res.map.global.allpassQ, 1.3f, 1.0e-6f);
            expect (! res.globalFix.phaseFilterEnabled);
        }

        beginTest ("Known production-grid allpass produces a helping fixed-timing candidate");
        {
            const double sr = 48000.0;
            const auto hit = makeKnownRotatorHit (sr, 55.0, 90.0f, 0.7f, 2, 0);
            const auto result = FixedTimingRotatorSearch::search (
                hit.bass.data(), hit.kick.data(), (int) hit.bass.size(), sr,
                0.0f, false, InterpolationType::Linear, { 2, 3 },
                { 60.0f, 90.0f, 140.0f }, { 0.7f, 1.5f });
            const auto candidate = FixedTimingRotatorSearch::candidateForStage (result, 2);
            expect (result.valid && candidate.valid && candidate.helps);
            expectGreaterOrEqual (candidate.gainPoints, FixedTimingRotatorSearch::kMinGainPoints);
            expectWithinAbsoluteError (candidate.allpassFreqHz, 90.0f, 0.1f);
            expectWithinAbsoluteError (candidate.allpassQ, 0.7f, 0.01f);
        }

        beginTest ("Forced stages constrain the positive search to stage 2 or 3");
        {
            const double sr = 48000.0;
            const auto hit = makeKnownRotatorHit (sr, 55.0, 90.0f, 0.7f, 2, 0);
            const auto two = FixedTimingRotatorSearch::search (hit.bass.data(), hit.kick.data(), (int) hit.bass.size(), sr,
                0.0f, false, InterpolationType::Linear, { 2, 3 }, { 90.0f }, { 0.7f }, 2);
            const auto three = FixedTimingRotatorSearch::search (hit.bass.data(), hit.kick.data(), (int) hit.bass.size(), sr,
                0.0f, false, InterpolationType::Linear, { 2, 3 }, { 90.0f }, { 0.7f }, 3);
            expect (two.valid && two.perStage.size() == 1 && two.perStage[0].stages == 2);
            expect (three.valid && three.perStage.size() == 1 && three.perStage[0].stages == 3);
        }
    }
};

//============================================================================//
// NoteLearnAccumulator - aggregation and acceptance limits (unit level)
//============================================================================//
class FrozenCrossoverTests : public juce::UnitTest
{
public:
    FrozenCrossoverTests() : juce::UnitTest ("FrozenCrossover", "Phase3") {}
    void runTest() override
    {
        beginTest ("Frozen crossover phase simulation is deterministic and frequency-specific");
        std::vector<float> raw (1024);
        for (int i = 0; i < (int) raw.size(); ++i)
            raw[(size_t) i] = std::sin ((float) i * 0.031f) + 0.2f * std::sin ((float) i * 0.19f);
        auto a = raw, b = raw, differentFrequency = raw;
        expect (FrozenCrossoverPhaseSimulation::apply (a, 48000.0, 90.0f));
        expect (FrozenCrossoverPhaseSimulation::apply (b, 48000.0, 90.0f));
        expect (FrozenCrossoverPhaseSimulation::apply (differentFrequency, 48000.0, 220.0f));
        expect (a == b, "same frozen context is bit deterministic");
        expect (a != raw, "enabled crossover applies the Static allpass phase path");
        expect (a != differentFrequency, "only the frozen frequency controls the result");
    }
};

class NoteLearnAccumulatorTests : public juce::UnitTest
{
public:
    NoteLearnAccumulatorTests() : juce::UnitTest ("NoteLearnAccumulator", "Phase3") {}

    void runTest() override
    {
        beginTest ("Median aggregation of fundamentals and F/Q, majority rotatorHelps");
        {
            NoteLearnAccumulator acc;
            acc.add ({ 54.0f, 90.0f, 1.0f, 0.8f, true });
            acc.add ({ 55.0f, 100.0f, 1.0f, 0.8f, true });
            acc.add ({ 56.0f, 110.0f, 1.2f, 0.8f, false });
            acc.add ({ 55.0f, 100.0f, 1.0f, 0.8f, true });

            const auto e = acc.finalizeEntry();
            expect (e.learned, "accepted");
            expectWithinAbsoluteError (e.fundamentalHz, 55.0f, 0.6f, "median fundamental");
            expectWithinAbsoluteError (e.allpassFreqHz, 100.0f, 6.0f, "median allpass freq");
            expectEquals (e.hitCount, 4);
            expect (e.rotatorHelps, "majority helps");
            expect (e.confidence > 0.5f, "confidence carried");
            expect (std::isfinite (e.fundamentalSpreadRatio) && e.fundamentalSpreadRatio >= 0.0f);
        }

        beginTest ("Below-minimum hit count is rejected");
        {
            NoteLearnAccumulator acc;
            for (int i = 0; i < NoteMap::kMinHitsPerNote - 1; ++i)
                acc.add ({ 55.0f, 100.0f, 1.0f, 0.9f, true });
            expect (! acc.finalizeEntry().learned, "too few hits -> rejected");
        }

        beginTest ("Excessive fundamental spread is rejected");
        {
            NoteLearnAccumulator acc;
            // Wide spread around ~55 Hz -> CV well above kNoteSpreadMaxRatio (0.15).
            acc.add ({ 40.0f, 100.0f, 1.0f, 0.9f, true });
            acc.add ({ 55.0f, 100.0f, 1.0f, 0.9f, true });
            acc.add ({ 70.0f, 100.0f, 1.0f, 0.9f, true });
            acc.add ({ 85.0f, 100.0f, 1.0f, 0.9f, true });

            const auto raw = acc.aggregate();
            expectGreaterThan (raw.fundamentalSpreadRatio, NoteMap::kNoteSpreadMaxRatio, "high spread computed");
            expect (! acc.finalizeEntry().learned, "high spread -> rejected");
        }

        beginTest ("Low confidence is rejected");
        {
            NoteLearnAccumulator acc;
            for (int i = 0; i < 5; ++i)
                acc.add ({ 55.0f, 100.0f, 1.0f, 0.10f, true });   // below kMinRuntimeConfidence
            expect (! acc.finalizeEntry().learned, "low confidence -> rejected");
        }

        beginTest ("Out-of-range fundamental is rejected, values stay finite");
        {
            NoteLearnAccumulator acc;
            for (int i = 0; i < 5; ++i)
                acc.add ({ 10.0f, 100.0f, 1.0f, 0.9f, true });    // < kFundamentalMinHz
            const auto e = acc.finalizeEntry();
            expect (! e.learned, "out-of-range fundamental -> rejected");
            expect (std::isfinite (e.fundamentalHz), "no NaN/Inf");
        }

        beginTest ("Non-finite inputs are skipped defensively");
        {
            NoteLearnAccumulator acc;
            acc.add ({ std::numeric_limits<float>::quiet_NaN(), 100.0f, 1.0f, 0.9f, true });
            acc.add ({ 55.0f, std::numeric_limits<float>::infinity(), 1.0f, 0.9f, true });
            acc.add ({ 55.0f, 100.0f, 1.0f, 0.9f, true });
            acc.add ({ 55.0f, 100.0f, 1.0f, 0.9f, true });
            acc.add ({ 55.0f, 100.0f, 1.0f, 0.9f, true });
            acc.add ({ 55.0f, 100.0f, 1.0f, 0.9f, true });
            const auto e = acc.finalizeEntry();
            expect (e.learned, "finite hits still accepted");
            expectEquals (e.hitCount, 4, "non-finite hits skipped");
        }

        beginTest ("F/Q outliers are removed before strict-majority acceptance");
        {
            NoteLearnAccumulator acc;
            acc.add ({ 55.0f, 100.0f, 1.0f, 0.9f, true });
            acc.add ({ 55.0f, 100.0f, 1.0f, 0.9f, true });
            acc.add ({ 55.0f, 100.0f, 1.0f, 0.9f, false });
            acc.add ({ 55.0f, 400.0f, 8.0f, 0.9f, true });
            const auto e = acc.finalizeEntry();
            expect (! e.learned, "outlier removal leaves fewer than four hits");
        }

        beginTest ("Tie and false majority never learn a note");
        {
            NoteLearnAccumulator tie, falseMajority;
            for (int i = 0; i < 4; ++i)
            {
                tie.add ({ 55.0f, 100.0f, 1.0f, 0.9f, i < 2 });
                falseMajority.add ({ 55.0f, 100.0f, 1.0f, 0.9f, i == 0 });
            }
            expect (! tie.finalizeEntry().learned && ! falseMajority.finalizeEntry().learned);
        }
    }
};

//============================================================================//
// Determinism + invalid-dataset behaviour
//============================================================================//
class LearnDeterminismTests : public juce::UnitTest
{
public:
    LearnDeterminismTests() : juce::UnitTest ("LearnDeterminism", "Phase3") {}

    static void expectEntryEqual (juce::UnitTest& t, const NoteEntry& a, const NoteEntry& b, const juce::String& tag)
    {
        t.expect (a.learned == b.learned, tag + " learned");
        t.expect (a.fundamentalHz == b.fundamentalHz, tag + " fundamental");
        t.expect (a.allpassFreqHz == b.allpassFreqHz, tag + " allpassFreq");
        t.expect (a.allpassQ == b.allpassQ, tag + " q");
        t.expect (a.hitCount == b.hitCount, tag + " hits");
        t.expect (a.rotatorHelps == b.rotatorHelps, tag + " helps");
    }

    void runTest() override
    {
        beginTest ("Identical input yields a bit-identical map");
        {
            const double sr = 44100.0;
            std::vector<LearnHitWindow> hits;
            int seq = 0;
            for (double f : { 55.0, 82.41 })
                for (int i = 0; i < 5; ++i)
                    hits.push_back (makeHit (sr, f, 2.0f, false, seq++));

            const auto a = LearnPipelineCore::finalize (hits, testConfig (sr));
            const auto b = LearnPipelineCore::finalize (hits, testConfig (sr));

            expect (a.valid && b.valid);
            expect (a.map.base.delayMs == b.map.base.delayMs, "delay");
            expect (a.map.base.polarityInvert == b.map.base.polarityInvert, "polarity");
            expect (a.map.base.allpassStages == b.map.base.allpassStages, "stages");
            for (int idx = 0; idx < NotePhaseMapSnapshot::size; ++idx)
                expectEntryEqual (*this, a.map.notes[(size_t) idx], b.map.notes[(size_t) idx],
                                  "note idx=" + juce::String (idx));
            expectEntryEqual (*this, a.map.global, b.map.global, "global");
        }

        beginTest ("Empty dataset returns an invalid, empty map with diagnostics");
        {
            const auto res = LearnPipelineCore::finalize ({}, testConfig (48000.0));
            expect (! res.valid, "invalid");
            expect (! NoteMap::isValidNoteMap (res.map), "map invalid");
            expect (! res.map.valid);
            expectEquals (res.diagnostics.capturedHits, 0);
            expect (res.diagnostics.warning.isNotEmpty(), "warning set");
        }

        beginTest ("All-silent dataset returns an invalid map (no stale data)");
        {
            const double sr = 48000.0;
            std::vector<LearnHitWindow> hits;
            for (int i = 0; i < 5; ++i)
            {
                LearnHitWindow w;
                w.sequence = i;
                w.bass.assign ((size_t) (sr * 0.2), 0.0f);
                w.kick.assign ((size_t) (sr * 0.2), 0.0f);
                hits.push_back (std::move (w));
            }
            const auto res = LearnPipelineCore::finalize (hits, testConfig (sr));
            expect (! res.valid, "invalid");
            expect (! res.map.valid, "no partial/stale map");
            expectGreaterThan (res.diagnostics.capturedHits, 0);
            expectEquals (res.diagnostics.analyzedHits, 0, "nothing analyzable");
        }

        beginTest ("Low-confidence pitch is global-only when timing remains usable");
        {
            const double sr = 48000.0;
            std::vector<LearnHitWindow> hits;
            for (int i = 0; i < 5; ++i)
            {
                LearnHitWindow w;
                w.sequence = i;
                const int n = (int) (sr * 0.2);
                w.bass.assign ((size_t) n, 0.0f);
                w.kick.assign ((size_t) n, 0.0f);
                // Deterministic broadband noise: no clear fundamental.
                for (int k = 0; k < n; ++k)
                {
                    const float s = std::sin ((float) (k * (i + 1)) * 12.9898f) * 43758.5453f;
                    w.bass[(size_t) k] = 0.5f * (2.0f * (s - std::floor (s)) - 1.0f);
                    w.kick[(size_t) k] = w.bass[(size_t) k];
                }
                hits.push_back (std::move (w));
            }
            const auto res = LearnPipelineCore::finalize (hits, testConfig (sr));
            expect (res.valid, "timing-only hits keep the global fallback valid");
            expectGreaterThan (res.diagnostics.rejectedPitchHits, 0, "pitch rejects counted");
        }
    }
};

static LearnPassATests learnPassATests;
static ConstrainedDtwRefinerTests constrainedDtwRefinerTests;
static LearnPassBTests learnPassBTests;
static LearnFixedTimingTests learnFixedTimingTests;
static FrozenCrossoverTests frozenCrossoverTests;
static NoteLearnAccumulatorTests noteLearnAccumulatorTests;
static LearnDeterminismTests learnDeterminismTests;
