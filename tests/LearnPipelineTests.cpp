#include "TestCommon.h"

#include "NotePhaseMap.h"
#include "NoteQuantizer.h"
#include "OfflineFundamentalEstimator.h"

// =============================================================================
// Phase 1 tests: pure data models and pitch helpers only (plan v1.2).
//   T3  - NoteQuantizer
//   T4  - OfflineFundamentalEstimator
//   T9  - Note-map data-model subset (no accumulator / Pass A / Pass B)
//   T16 - Pure ValueTree serialization subset (no APVTS / Apply / Revert)
// =============================================================================

namespace
{
    // Frequency for a fractional MIDI value; the inverse of hzToMidiFloat used to
    // place a pitch a precise number of cents away from a note centre.
    double hzForMidiFloat (double midi) noexcept
    {
        return 440.0 * std::pow (2.0, (midi - 69.0) / 12.0);
    }

    // Deterministic pseudo-noise in [-1, 1] (no RNG, so tests stay reproducible).
    float hashNoise (int i) noexcept
    {
        const float s = std::sin ((float) i * 12.9898f) * 43758.5453f;
        return 2.0f * (s - std::floor (s)) - 1.0f;
    }

    std::vector<float> makeSignal (double sampleRate,
                                   int numSamples,
                                   const std::vector<std::pair<double, double>>& partials,
                                   double noiseAmp = 0.0)
    {
        std::vector<float> buf ((size_t) numSamples, 0.0f);
        for (int i = 0; i < numSamples; ++i)
        {
            const double t = (double) i / sampleRate;
            double v = 0.0;
            for (const auto& p : partials)
                v += p.second * std::sin (kTwoPi * p.first * t);
            if (noiseAmp > 0.0)
                v += noiseAmp * (double) hashNoise (i);
            buf[(size_t) i] = (float) v;
        }
        return buf;
    }

    NoteEntry makeValidNote (float fundamentalHz, float allpassFreqHz, float q = 1.0f)
    {
        NoteEntry e;
        e.learned = true;
        e.fundamentalHz = fundamentalHz;
        e.allpassFreqHz = allpassFreqHz;
        e.allpassQ = q;
        e.confidence = 0.8f;
        e.fundamentalSpreadRatio = 0.04f;
        e.hitCount = 6;
        e.rotatorHelps = true;
        return e;
    }
}

//============================================================================//
// T3 - NoteQuantizer
//============================================================================//
class NoteQuantizerTests : public juce::UnitTest
{
public:
    NoteQuantizerTests() : juce::UnitTest ("NoteQuantizer", "Phase1") {}

    void runTest() override
    {
        beginTest ("Standard MIDI <-> Hz conversions");
        {
            expectWithinAbsoluteError (NoteQuantizer::midiToHz (69), 440.0f, 1.0e-2f);
            expectWithinAbsoluteError (NoteQuantizer::midiToHz (57), 220.0f, 1.0e-2f);
            expectWithinAbsoluteError (NoteQuantizer::midiToHz (81), 880.0f, 1.0e-2f);
            expectEquals (NoteQuantizer::hzToMidi (440.0f), 69);
            expectEquals (NoteQuantizer::hzToMidi (220.0f), 57);
            expectWithinAbsoluteError (NoteQuantizer::hzToMidiFloat (440.0f), 69.0f, 1.0e-4f);
        }

        beginTest ("Round-trip across the map range is exact to the nearest semitone");
        {
            for (int midi = NotePhaseMapSnapshot::minMidi; midi <= NotePhaseMapSnapshot::maxMidi; ++midi)
                expectEquals (NoteQuantizer::hzToMidi (NoteQuantizer::midiToHz (midi)), midi,
                              "midi=" + juce::String (midi));
        }

        beginTest ("Invalid and non-finite Hz return -1");
        {
            expectEquals (NoteQuantizer::hzToMidi (0.0f), -1);
            expectEquals (NoteQuantizer::hzToMidi (-55.0f), -1);
            expectEquals (NoteQuantizer::hzToMidi (std::numeric_limits<float>::quiet_NaN()), -1);
            expectEquals (NoteQuantizer::hzToMidi (std::numeric_limits<float>::infinity()), -1);
            expectWithinAbsoluteError (NoteQuantizer::hzToMidiFloat (0.0f), -1.0f, 1.0e-6f);
            expectWithinAbsoluteError (NoteQuantizer::hzToMidiFloat (-1.0f), -1.0f, 1.0e-6f);
        }

        beginTest ("Map range boundaries are finite");
        {
            expect (std::isfinite (NoteQuantizer::midiToHz (NotePhaseMapSnapshot::minMidi)));
            expect (std::isfinite (NoteQuantizer::midiToHz (NotePhaseMapSnapshot::maxMidi)));
            expectGreaterThan (NoteQuantizer::midiToHz (NotePhaseMapSnapshot::minMidi), 0.0f);
        }

        beginTest ("First valid pitch snaps to the nearest semitone");
        {
            int last = -1;
            const int m = NoteQuantizer::updateWithHysteresis ((float) hzForMidiFloat (40.1), last);
            expectEquals (m, 40);
            expectEquals (last, 40);
        }

        // Boundary rule: a distance <= 60 cents from the held note centre RETAINS
        // the held note; > 60 cents requantises. Tested at +/- 0.1 cents around
        // the boundary so float error (~1e-3 cents) cannot flip the result.
        beginTest ("Hysteresis retains within +/-60 cents and requantises past it");
        {
            int last = -1;
            NoteQuantizer::updateWithHysteresis ((float) hzForMidiFloat (40.0), last); // hold 40
            expectEquals (last, 40);

            // 30 cents sharp -> retained.
            expectEquals (NoteQuantizer::updateWithHysteresis ((float) hzForMidiFloat (40.30), last), 40);
            // 59.9 cents sharp -> just inside -> retained.
            expectEquals (NoteQuantizer::updateWithHysteresis ((float) hzForMidiFloat (40.599), last), 40);
            // 59.9 cents flat -> retained.
            expectEquals (NoteQuantizer::updateWithHysteresis ((float) hzForMidiFloat (39.401), last), 40);
            // 60.1 cents sharp -> just outside -> requantise to nearest (41).
            expectEquals (NoteQuantizer::updateWithHysteresis ((float) hzForMidiFloat (40.601), last), 41);
            expectEquals (last, 41);
        }

        beginTest ("Exact semitone jump requantises");
        {
            int last = -1;
            NoteQuantizer::updateWithHysteresis (NoteQuantizer::midiToHz (40), last);
            expectEquals (NoteQuantizer::updateWithHysteresis (NoteQuantizer::midiToHz (43), last), 43);
        }

        beginTest ("Multi-semitone glide steps monotonically and ends on the target");
        {
            int last = -1;
            int previous = -1;
            const double startMidi = 40.0;
            const double endMidi = 45.0;
            const int steps = 500;
            for (int s = 0; s <= steps; ++s)
            {
                const double m = startMidi + (endMidi - startMidi) * (double) s / (double) steps;
                const int note = NoteQuantizer::updateWithHysteresis ((float) hzForMidiFloat (m), last);
                expectGreaterOrEqual (note, previous); // never steps backwards on a rising glide
                previous = note;
            }
            expectEquals (last, 45);
        }

        beginTest ("Invalid pitch and silence reset the held note");
        {
            int last = -1;
            NoteQuantizer::updateWithHysteresis (NoteQuantizer::midiToHz (40), last);
            expectEquals (last, 40);

            expectEquals (NoteQuantizer::updateWithHysteresis (0.0f, last), -1);
            expectEquals (last, -1);

            NoteQuantizer::updateWithHysteresis (NoteQuantizer::midiToHz (40), last);
            expectEquals (NoteQuantizer::updateWithHysteresis (std::numeric_limits<float>::quiet_NaN(), last), -1);
            expectEquals (last, -1);

            // No stale note after reset: the next valid pitch snaps fresh.
            expectEquals (NoteQuantizer::updateWithHysteresis ((float) hzForMidiFloat (50.1), last), 50);
        }
    }
};

//============================================================================//
// T4 - OfflineFundamentalEstimator
//============================================================================//
class OfflineFundamentalEstimatorTests : public juce::UnitTest
{
public:
    OfflineFundamentalEstimatorTests() : juce::UnitTest ("OfflineFundamentalEstimator", "Phase1") {}

    void expectFrequency (const std::vector<float>& buf, double sr, double trueHz, const juce::String& tag)
    {
        const auto e = OfflineFundamentalEstimator::estimate (buf.data(), (int) buf.size(), sr);
        expect (e.valid, tag + ": expected a valid estimate");
        if (e.valid)
        {
            const double tol = std::max (1.0, trueHz * 0.03);
            expectWithinAbsoluteError ((double) e.frequencyHz, trueHz, tol, tag);
            expect (std::isfinite (e.frequencyHz) && std::isfinite (e.confidence), tag + ": finite");
        }
    }

    void runTest() override
    {
        const double rates[] = { 44100.0, 48000.0, 96000.0 };
        const double bassNotes[] = { 41.20, 55.00, 73.42, 82.41, 110.00 };

        beginTest ("Clean sine fundamentals at 44.1 / 48 / 96 kHz");
        {
            for (double sr : rates)
                for (double f0 : bassNotes)
                {
                    const int n = (int) std::round (sr * 0.3);
                    const auto buf = makeSignal (sr, n, { { f0, 0.7 } });
                    expectFrequency (buf, sr, f0, "sr=" + juce::String (sr, 0) + " f0=" + juce::String (f0, 2));
                }
        }

        beginTest ("Harmonic-rich bass resolves to the fundamental");
        {
            const double sr = 48000.0;
            const int n = (int) std::round (sr * 0.3);
            for (double f0 : { 55.0, 82.41 })
            {
                const auto buf = makeSignal (sr, n, { { f0, 0.7 }, { 2.0 * f0, 0.45 }, { 3.0 * f0, 0.3 }, { 4.0 * f0, 0.2 } });
                expectFrequency (buf, sr, f0, "harmonic f0=" + juce::String (f0, 2));
            }
        }

        beginTest ("A dominant second harmonic still prefers the fundamental");
        {
            const double sr = 48000.0;
            const int n = (int) std::round (sr * 0.3);
            const double f0 = 55.0;
            // Second harmonic is clearly stronger than the fundamental, but the
            // fundamental carries sufficient evidence to be preferred.
            const auto buf = makeSignal (sr, n, { { f0, 0.35 }, { 2.0 * f0, 1.0 }, { 3.0 * f0, 0.2 } });
            expectFrequency (buf, sr, f0, "dominant-2nd f0=" + juce::String (f0, 2));
        }

        beginTest ("Detuned note is estimated at its detuned frequency");
        {
            const double sr = 48000.0;
            const int n = (int) std::round (sr * 0.3);
            const double detuned = hzForMidiFloat (33.0 + 0.15); // ~15 cents sharp of A1 (55 Hz)
            const auto buf = makeSignal (sr, n, { { detuned, 0.7 }, { 2.0 * detuned, 0.3 } });
            expectFrequency (buf, sr, detuned, "detuned=" + juce::String (detuned, 3));
        }

        beginTest ("Octave-ambiguous signal is estimated at its true period, deterministically");
        {
            const double sr = 48000.0;
            const int n = (int) std::round (sr * 0.3);
            const double f0 = 55.0;
            // Only even harmonics -> the true period is 1/(2*f0); the estimator
            // reports that (it does not invent the missing fundamental).
            const auto buf = makeSignal (sr, n, { { 2.0 * f0, 0.7 }, { 4.0 * f0, 0.4 } });
            const auto a = OfflineFundamentalEstimator::estimate (buf.data(), (int) buf.size(), sr);
            const auto b = OfflineFundamentalEstimator::estimate (buf.data(), (int) buf.size(), sr);
            expect (a.valid);
            expectWithinAbsoluteError (a.frequencyHz, b.frequencyHz, 1.0e-6f, "deterministic");
            if (a.valid)
                expectWithinAbsoluteError ((double) a.frequencyHz, 2.0 * f0, std::max (1.0, 2.0 * f0 * 0.03),
                                           "true period = 2*f0");
        }

        beginTest ("Moderate noise on a clear tone still resolves");
        {
            const double sr = 48000.0;
            const int n = (int) std::round (sr * 0.3);
            const auto buf = makeSignal (sr, n, { { 55.0, 0.7 }, { 110.0, 0.3 } }, 0.06);
            expectFrequency (buf, sr, 55.0, "moderate-noise");
        }

        beginTest ("Silence, very low amplitude, short and non-finite input are rejected");
        {
            const double sr = 48000.0;
            const int n = (int) std::round (sr * 0.3);

            std::vector<float> silence ((size_t) n, 0.0f);
            expect (! OfflineFundamentalEstimator::estimate (silence.data(), n, sr).valid, "silence");

            const auto tiny = makeSignal (sr, n, { { 55.0, 1.0e-5 } });
            expect (! OfflineFundamentalEstimator::estimate (tiny.data(), n, sr).valid, "very low amplitude");

            const auto shortBuf = makeSignal (sr, 100, { { 55.0, 0.7 } });
            expect (! OfflineFundamentalEstimator::estimate (shortBuf.data(), 100, sr).valid, "too short");

            auto nanBuf = makeSignal (sr, n, { { 55.0, 0.7 } });
            nanBuf[(size_t) (n / 2)] = std::numeric_limits<float>::quiet_NaN();
            expect (! OfflineFundamentalEstimator::estimate (nanBuf.data(), n, sr).valid, "NaN");

            auto infBuf = makeSignal (sr, n, { { 55.0, 0.7 } });
            infBuf[(size_t) (n / 3)] = std::numeric_limits<float>::infinity();
            expect (! OfflineFundamentalEstimator::estimate (infBuf.data(), n, sr).valid, "Inf");

            expect (! OfflineFundamentalEstimator::estimate (nullptr, n, sr).valid, "null");
            expect (! OfflineFundamentalEstimator::estimate (silence.data(), n, 0.0).valid, "bad sample rate");
        }

        beginTest ("Out-of-range fundamentals never yield an out-of-range estimate");
        {
            const double sr = 48000.0;
            const int n = (int) std::round (sr * 0.3);
            for (double f : { 10.0, 500.0, 800.0 })
            {
                const auto buf = makeSignal (sr, n, { { f, 0.7 } });
                const auto e = OfflineFundamentalEstimator::estimate (buf.data(), n, sr);
                if (e.valid)
                {
                    expectGreaterOrEqual (e.frequencyHz, 25.0f, "f=" + juce::String (f, 0));
                    expectLessOrEqual (e.frequencyHz, 300.0f, "f=" + juce::String (f, 0));
                }
            }
        }

        beginTest ("Repeated calls on identical input are deterministic");
        {
            const double sr = 44100.0;
            const int n = (int) std::round (sr * 0.3);
            const auto buf = makeSignal (sr, n, { { 73.42, 0.7 }, { 146.84, 0.3 } });
            const auto a = OfflineFundamentalEstimator::estimate (buf.data(), n, sr);
            const auto b = OfflineFundamentalEstimator::estimate (buf.data(), n, sr);
            expect (a.valid && b.valid);
            expectWithinAbsoluteError (a.frequencyHz, b.frequencyHz, 0.0f);
            expectWithinAbsoluteError (a.confidence, b.confidence, 0.0f);
        }
    }
};

//============================================================================//
// T9 - Note-map data-model subset
//============================================================================//
class NoteMapModelTests : public juce::UnitTest
{
public:
    NoteMapModelTests() : juce::UnitTest ("NoteMapModel", "Phase1") {}

    void runTest() override
    {
        beginTest ("fundamentalHz and allpassFreqHz stay independent");
        {
            NoteEntry e = makeValidNote (55.0f, 110.0f);
            expectWithinAbsoluteError (e.fundamentalHz, 55.0f, 1.0e-6f);
            expectWithinAbsoluteError (e.allpassFreqHz, 110.0f, 1.0e-6f);
            expectGreaterThan (std::abs (e.allpassFreqHz - e.fundamentalHz), 1.0f); // distinct fields
            e.allpassFreqHz = 140.0f;                    // changing one must not move the other
            expectWithinAbsoluteError (e.fundamentalHz, 55.0f, 1.0e-6f);
            expectWithinAbsoluteError (e.allpassFreqHz, 140.0f, 1.0e-6f);
        }

        beginTest ("Index / range helpers are safe at and beyond the boundaries");
        {
            expect (NotePhaseMapSnapshot::containsMidi (16));
            expect (NotePhaseMapSnapshot::containsMidi (67));
            expect (! NotePhaseMapSnapshot::containsMidi (15));
            expect (! NotePhaseMapSnapshot::containsMidi (68));

            expectEquals (NotePhaseMapSnapshot::indexForMidi (16), 0);
            expectEquals (NotePhaseMapSnapshot::indexForMidi (67), NotePhaseMapSnapshot::size - 1);
            expectEquals (NotePhaseMapSnapshot::indexForMidi (15), -1);
            expectEquals (NotePhaseMapSnapshot::indexForMidi (68), -1);

            expectEquals (NotePhaseMapSnapshot::midiForIndex (0), 16);
            expectEquals (NotePhaseMapSnapshot::midiForIndex (NotePhaseMapSnapshot::size - 1), 67);
            expectEquals (NotePhaseMapSnapshot::midiForIndex (-1), -1);
            expectEquals (NotePhaseMapSnapshot::midiForIndex (NotePhaseMapSnapshot::size), -1);

            NotePhaseMapSnapshot m = NoteMap::makeEmptyNoteMap();
            m.notes[0] = makeValidNote (55.0f, 110.0f);
            expect (m.lookup (15) == nullptr);
            expect (m.lookup (68) == nullptr);
            expect (m.lookup (16) == &m.notes[0]);
        }

        beginTest ("Entry validation enforces the section 4.1 invariants");
        {
            expect (NoteMap::isValidNoteEntry (makeValidNote (55.0f, 110.0f)));

            NoteEntry unlearned = makeValidNote (55.0f, 110.0f);
            unlearned.learned = false;
            expect (! NoteMap::isValidNoteEntry (unlearned));

            NoteEntry lowFund = makeValidNote (20.0f, 110.0f);   // < 25 Hz
            expect (! NoteMap::isValidNoteEntry (lowFund));
            NoteEntry highFund = makeValidNote (350.0f, 110.0f);  // > 300 Hz
            expect (! NoteMap::isValidNoteEntry (highFund));

            NoteEntry lowAp = makeValidNote (55.0f, 10.0f);       // allpass < 20 Hz
            expect (! NoteMap::isValidNoteEntry (lowAp));
            NoteEntry highAp = makeValidNote (55.0f, 600.0f);     // allpass > 500 Hz
            expect (! NoteMap::isValidNoteEntry (highAp));

            NoteEntry nanEntry = makeValidNote (55.0f, 110.0f);
            nanEntry.confidence = std::numeric_limits<float>::quiet_NaN();
            expect (! NoteMap::isValidNoteEntry (nanEntry));
        }

        beginTest ("Minimum-hit, spread and Q invariants");
        {
            NoteEntry e = makeValidNote (55.0f, 110.0f);

            e.hitCount = NoteMap::kMinHitsPerNote - 1;
            expect (! NoteMap::isValidNoteEntry (e), "hitCount below minimum");
            e.hitCount = NoteMap::kMinHitsPerNote;
            expect (NoteMap::isValidNoteEntry (e), "hitCount at minimum");

            e = makeValidNote (55.0f, 110.0f);
            e.fundamentalSpreadRatio = NoteMap::kNoteSpreadMaxRatio;
            expect (NoteMap::isValidNoteEntry (e), "spread at maximum");
            e.fundamentalSpreadRatio = NoteMap::kNoteSpreadMaxRatio + 0.01f;
            expect (! NoteMap::isValidNoteEntry (e), "spread above maximum");

            e = makeValidNote (55.0f, 110.0f, NoteMap::kAllpassQMin);
            expect (NoteMap::isValidNoteEntry (e), "Q at minimum");
            e = makeValidNote (55.0f, 110.0f, NoteMap::kAllpassQMax);
            expect (NoteMap::isValidNoteEntry (e), "Q at maximum");
            e = makeValidNote (55.0f, 110.0f, NoteMap::kAllpassQMin - 0.01f);
            expect (! NoteMap::isValidNoteEntry (e), "Q below minimum");
            e = makeValidNote (55.0f, 110.0f, NoteMap::kAllpassQMax + 0.5f);
            expect (! NoteMap::isValidNoteEntry (e), "Q above maximum");
        }

        beginTest ("Global fallback and map validity invariants");
        {
            NoteEntry g;
            g.allpassFreqHz = 90.0f;
            g.allpassQ = 1.0f;
            expect (NoteMap::isValidGlobalEntry (g), "finite positive F/Q is a valid fallback even if unlearned");

            g.allpassFreqHz = 0.0f;
            expect (! NoteMap::isValidGlobalEntry (g), "zero F invalid");
            g.allpassFreqHz = std::numeric_limits<float>::quiet_NaN();
            g.allpassQ = 1.0f;
            expect (! NoteMap::isValidGlobalEntry (g), "NaN F invalid");

            expect (! NoteMap::isValidNoteMap (NoteMap::makeEmptyNoteMap()), "empty map invalid");

            NotePhaseMapSnapshot m = NoteMap::makeEmptyNoteMap();
            m.valid = true;
            m.global.allpassFreqHz = 90.0f;
            m.global.allpassQ = 1.0f;
            m.base.learnedSampleRate = 48000.0;
            expect (NoteMap::isValidNoteMap (m), "valid flag + good global + good base");

            m.global.allpassFreqHz = 0.0f;   // malformed global fallback
            expect (! NoteMap::isValidNoteMap (m), "malformed global invalidates the map");
        }

        beginTest ("Sanitisation removes non-finite fields deterministically");
        {
            NoteEntry e = makeValidNote (55.0f, 110.0f);
            e.allpassQ = std::numeric_limits<float>::quiet_NaN();
            e.confidence = std::numeric_limits<float>::infinity();
            e.fundamentalSpreadRatio = -std::numeric_limits<float>::infinity();
            const NoteEntry s = NoteMap::sanitizeNoteEntry (e);
            expect (std::isfinite (s.allpassQ) && s.allpassQ >= NoteMap::kAllpassQMin && s.allpassQ <= NoteMap::kAllpassQMax);
            expect (std::isfinite (s.confidence) && s.confidence >= 0.0f && s.confidence <= 1.0f);
            expect (std::isfinite (s.fundamentalSpreadRatio) && s.fundamentalSpreadRatio >= 0.0f);
        }
    }
};

//============================================================================//
// T16 - Pure serialization subset
//============================================================================//
class NoteMapSerializationTests : public juce::UnitTest
{
public:
    NoteMapSerializationTests() : juce::UnitTest ("NoteMapSerialization", "Phase1") {}

    static NotePhaseMapSnapshot makePopulatedMap()
    {
        NotePhaseMapSnapshot m = NoteMap::makeEmptyNoteMap();
        m.valid = true;
        m.base.delayMs = -2.3f;
        m.base.polarityInvert = true;
        m.base.crossoverEnabled = true;
        m.base.crossoverHz = 140.0f;
        m.base.allpassStages = 3;
        m.base.delayInterpolationIndex = 1;
        m.base.learnedSampleRate = 48000.0;

        m.global = makeGlobal();

        m.notes[(size_t) NotePhaseMapSnapshot::indexForMidi (28)] = makeNote (28, 41.20f, 60.0f, 1.2f);
        m.notes[(size_t) NotePhaseMapSnapshot::indexForMidi (33)] = makeNote (33, 55.00f, 80.0f, 1.0f);
        m.notes[(size_t) NotePhaseMapSnapshot::indexForMidi (40)] = makeNote (40, 82.41f, 120.0f, 0.9f);
        return m;
    }

    static NoteEntry makeGlobal()
    {
        NoteEntry g;
        g.learned = true;
        g.fundamentalHz = 55.0f;
        g.allpassFreqHz = 90.0f;
        g.allpassQ = 1.1f;
        g.confidence = 0.7f;
        g.fundamentalSpreadRatio = 0.05f;
        g.hitCount = 24;
        g.rotatorHelps = true;
        return g;
    }

    static NoteEntry makeNote (int /*midi*/, float fundamentalHz, float allpassFreqHz, float q)
    {
        NoteEntry e;
        e.learned = true;
        e.fundamentalHz = fundamentalHz;
        e.allpassFreqHz = allpassFreqHz;
        e.allpassQ = q;
        e.confidence = 0.82f;
        e.fundamentalSpreadRatio = 0.04f;
        e.hitCount = 6;
        e.rotatorHelps = true;
        return e;
    }

    void expectEntryEquals (const NoteEntry& a, const NoteEntry& b, const juce::String& tag)
    {
        expect (a.learned == b.learned, tag + " learned");
        expectWithinAbsoluteError (a.fundamentalHz, b.fundamentalHz, 1.0e-3f, tag + " fundamentalHz");
        expectWithinAbsoluteError (a.allpassFreqHz, b.allpassFreqHz, 1.0e-3f, tag + " allpassFreqHz");
        expectWithinAbsoluteError (a.allpassQ, b.allpassQ, 1.0e-3f, tag + " q");
        expectWithinAbsoluteError (a.confidence, b.confidence, 1.0e-3f, tag + " confidence");
        expectWithinAbsoluteError (a.fundamentalSpreadRatio, b.fundamentalSpreadRatio, 1.0e-3f, tag + " spread");
        expectEquals (a.hitCount, b.hitCount, tag + " hits");
        expect (a.rotatorHelps == b.rotatorHelps, tag + " helps");
    }

    void runTest() override
    {
        beginTest ("Complete valid map round-trips");
        {
            const auto original = makePopulatedMap();
            const auto parsed = noteMapFromValueTree (noteMapToValueTree (original));

            expect (parsed.valid);
            expectEquals ((int) parsed.schemaVersion, (int) NoteMap::kSchemaVersion);
            expectWithinAbsoluteError (parsed.base.delayMs, original.base.delayMs, 1.0e-3f);
            expect (parsed.base.polarityInvert == original.base.polarityInvert);
            expect (parsed.base.crossoverEnabled == original.base.crossoverEnabled);
            expectWithinAbsoluteError (parsed.base.crossoverHz, original.base.crossoverHz, 1.0e-3f);
            expectEquals (parsed.base.allpassStages, original.base.allpassStages);
            expectEquals (parsed.base.delayInterpolationIndex, original.base.delayInterpolationIndex);
            expectWithinAbsoluteError (parsed.base.learnedSampleRate, original.base.learnedSampleRate, 1.0e-6);

            expectEntryEquals (parsed.global, original.global, "global");

            for (int midi : { 28, 33, 40 })
            {
                const int idx = NotePhaseMapSnapshot::indexForMidi (midi);
                expectEntryEquals (parsed.notes[(size_t) idx], original.notes[(size_t) idx],
                                   "note " + juce::String (midi));
            }
        }

        beginTest ("Unlearned notes stay unlearned and absent from the tree");
        {
            const auto original = makePopulatedMap();
            const auto tree = noteMapToValueTree (original);
            expectEquals (tree.getNumChildren(), 3);   // only the three learned notes

            const auto parsed = noteMapFromValueTree (tree);
            const int idx = NotePhaseMapSnapshot::indexForMidi (50);
            expect (! parsed.notes[(size_t) idx].learned);
        }

        beginTest ("Empty map round-trips to an invalid map");
        {
            const auto parsed = noteMapFromValueTree (noteMapToValueTree (NoteMap::makeEmptyNoteMap()));
            expect (! parsed.valid);
            expect (! NoteMap::isValidNoteMap (parsed));
        }

        beginTest ("Missing / foreign tree and old state without a map parse to invalid");
        {
            expect (! noteMapFromValueTree (juce::ValueTree()).valid, "invalid tree");
            expect (! noteMapFromValueTree (juce::ValueTree ("SomeOtherState")).valid, "foreign tree");
        }

        beginTest ("Schema mismatch yields an invalid empty map");
        {
            auto tree = noteMapToValueTree (makePopulatedMap());
            tree.setProperty (juce::Identifier (NoteMapKeys::schemaVersion), 99, nullptr);
            const auto parsed = noteMapFromValueTree (tree);
            expect (! parsed.valid);
            expect (! parsed.notes[(size_t) NotePhaseMapSnapshot::indexForMidi (33)].learned);
        }

        beginTest ("Corrupted float values cannot produce NaN/Inf and reject the note");
        {
            auto tree = noteMapToValueTree (makePopulatedMap());
            // Corrupt the fundamental of the note at MIDI 33.
            for (int c = 0; c < tree.getNumChildren(); ++c)
            {
                auto child = tree.getChild (c);
                if ((int) child.getProperty (juce::Identifier (NoteMapKeys::noteMidi), -1) == 33)
                    child.setProperty (juce::Identifier (NoteMapKeys::noteFundamentalHz),
                                       juce::var (std::numeric_limits<double>::quiet_NaN()), nullptr);
            }
            const auto parsed = noteMapFromValueTree (tree);
            const int idx = NotePhaseMapSnapshot::indexForMidi (33);
            expect (! parsed.notes[(size_t) idx].learned, "corrupt note rejected");
            expect (std::isfinite (parsed.notes[(size_t) idx].fundamentalHz), "no NaN leaks through");
            // Other notes still parse.
            expect (parsed.notes[(size_t) NotePhaseMapSnapshot::indexForMidi (28)].learned);
        }

        beginTest ("Out-of-range MIDI note children are ignored");
        {
            auto tree = noteMapToValueTree (makePopulatedMap());
            juce::ValueTree bad { juce::Identifier (NoteMapKeys::note) };
            bad.setProperty (juce::Identifier (NoteMapKeys::noteMidi), 200, nullptr);
            bad.setProperty (juce::Identifier (NoteMapKeys::noteFundamentalHz), 60.0, nullptr);
            bad.setProperty (juce::Identifier (NoteMapKeys::noteAllpassFreqHz), 90.0, nullptr);
            bad.setProperty (juce::Identifier (NoteMapKeys::noteQ), 1.0, nullptr);
            bad.setProperty (juce::Identifier (NoteMapKeys::noteHits), 8, nullptr);
            tree.appendChild (bad, nullptr);
            const auto parsed = noteMapFromValueTree (tree);
            expect (parsed.valid, "map still valid; bad-MIDI child ignored");
        }

        beginTest ("Duplicate MIDI children: the last valid child wins");
        {
            auto tree = noteMapToValueTree (makePopulatedMap());
            juce::ValueTree dup { juce::Identifier (NoteMapKeys::note) };
            dup.setProperty (juce::Identifier (NoteMapKeys::noteMidi), 33, nullptr);
            dup.setProperty (juce::Identifier (NoteMapKeys::noteFundamentalHz), 55.0, nullptr);
            dup.setProperty (juce::Identifier (NoteMapKeys::noteAllpassFreqHz), 200.0, nullptr); // different
            dup.setProperty (juce::Identifier (NoteMapKeys::noteQ), 2.5, nullptr);
            dup.setProperty (juce::Identifier (NoteMapKeys::noteConfidence), 0.9, nullptr);
            dup.setProperty (juce::Identifier (NoteMapKeys::noteSpread), 0.03, nullptr);
            dup.setProperty (juce::Identifier (NoteMapKeys::noteHits), 9, nullptr);
            dup.setProperty (juce::Identifier (NoteMapKeys::noteHelps), true, nullptr);
            tree.appendChild (dup, nullptr);   // appended after the original MIDI-33 child

            const auto parsed = noteMapFromValueTree (tree);
            const int idx = NotePhaseMapSnapshot::indexForMidi (33);
            expectWithinAbsoluteError (parsed.notes[(size_t) idx].allpassFreqHz, 200.0f, 1.0e-3f, "last child wins");
            expectWithinAbsoluteError (parsed.notes[(size_t) idx].allpassQ, 2.5f, 1.0e-3f);
        }

        beginTest ("Note missing only optional fields still loads");
        {
            auto tree = noteMapToValueTree (NoteMap::makeEmptyNoteMap());
            // A syntactically minimal but valid learned note (no confidence /
            // spread / helps attributes).
            juce::ValueTree n { juce::Identifier (NoteMapKeys::note) };
            n.setProperty (juce::Identifier (NoteMapKeys::noteMidi), 36, nullptr);
            n.setProperty (juce::Identifier (NoteMapKeys::noteFundamentalHz), 65.41, nullptr);
            n.setProperty (juce::Identifier (NoteMapKeys::noteAllpassFreqHz), 80.0, nullptr);
            n.setProperty (juce::Identifier (NoteMapKeys::noteQ), 1.0, nullptr);
            n.setProperty (juce::Identifier (NoteMapKeys::noteHits), 6, nullptr);
            tree.appendChild (n, nullptr);

            const auto parsed = noteMapFromValueTree (tree);
            const int idx = NotePhaseMapSnapshot::indexForMidi (36);
            expect (parsed.notes[(size_t) idx].learned, "minimal note loads");
            expectWithinAbsoluteError (parsed.notes[(size_t) idx].fundamentalSpreadRatio, 0.0f, 1.0e-6f);
        }

        beginTest ("Parsing is deterministic");
        {
            const auto tree = noteMapToValueTree (makePopulatedMap());
            const auto a = noteMapFromValueTree (tree);
            const auto b = noteMapFromValueTree (tree);
            expect (a.valid == b.valid);
            const int idx = NotePhaseMapSnapshot::indexForMidi (33);
            expectWithinAbsoluteError (a.notes[(size_t) idx].allpassFreqHz,
                                       b.notes[(size_t) idx].allpassFreqHz, 0.0f);
        }
    }
};

static NoteQuantizerTests noteQuantizerTests;
static OfflineFundamentalEstimatorTests offlineFundamentalEstimatorTests;
static NoteMapModelTests noteMapModelTests;
static NoteMapSerializationTests noteMapSerializationTests;
