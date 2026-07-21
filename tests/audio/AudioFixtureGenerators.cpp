#include "AudioFixtureGenerators.h"

#include "../DynamicReleaseFixture.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    // Deterministic low-level dither, matching the DynamicReleaseFixture idea so
    // Learn's clustering sees the same kind of tiny signed noise floor it was
    // tuned against (a perfectly silent noise floor is not representative).
    struct Dither
    {
        uint32_t state;
        explicit Dither (uint32_t seed) : state (seed) {}
        float next() noexcept
        {
            state = state * 1664525u + 1013904223u;
            return ((float) ((state >> 8) & 0xffffu) / 32767.5f - 1.0f) * 0.0025f;
        }
    };

    // One kick transient: a fast-decaying tone with a little 2nd/3rd content,
    // soft-clipped. Mirrors DynamicReleaseFixture's kick synthesis so triggers
    // and fingerprints behave the way the proven fixtures already do.
    void addKick (std::vector<float>& kick, int at, double sampleRate,
                  double hz, double decay, double amp, Dither& dither)
    {
        const int tail = (int) std::lround (0.180 * sampleRate);
        for (int local = 0; local < tail; ++local)
        {
            const int index = at + local;
            if (index < 0 || index >= (int) kick.size())
                continue;
            const double t = (double) local / sampleRate;
            const double env = std::exp (-t * decay);
            const double v = amp * env * (std::sin (2.0 * kPi * hz * t)
                + 0.22 * std::sin (2.0 * kPi * hz * 2.6 * t));
            kick[(size_t) index] += (float) std::tanh (v * 1.03) + dither.next();
        }
    }

    // One bass note: fundamental + harmonic, exponential decay, optional extra
    // drive (soft saturation) and optional detune stack for unison. bassOffset
    // shifts the note's start relative to the kick (the phase-conflict timing).
    void addBassNote (std::vector<float>& bass, int at, double sampleRate,
                      double hz, double phase, double decay, double harmonic,
                      double amp, double drive, int bassOffsetSamples, Dither& dither,
                      int unisonVoices = 1, double detuneHz = 0.0, double vibratoHz = 0.0)
    {
        const int tail = (int) std::lround (0.180 * sampleRate);
        for (int local = 0; local < tail; ++local)
        {
            const int index = at + local;
            if (index < 0 || index >= (int) bass.size())
                continue;
            const double t = (double) (local - bassOffsetSamples) / sampleRate;
            const double seconds = (double) local / sampleRate;
            const double env = std::exp (-seconds * decay);
            const double vib = vibratoHz > 0.0 ? (1.0 + 0.01 * std::sin (2.0 * kPi * vibratoHz * seconds)) : 1.0;
            double v = 0.0;
            for (int voice = 0; voice < std::max (1, unisonVoices); ++voice)
            {
                const double voiceHz = (hz + detuneHz * (double) (voice - (unisonVoices - 1) / 2)) * vib;
                v += std::sin (2.0 * kPi * voiceHz * t + phase)
                   + harmonic * std::sin (2.0 * kPi * voiceHz * 2.0 * t + 0.3);
            }
            v = v / (double) std::max (1, unisonVoices);
            v *= amp * env;
            if (drive > 1.0)
                v = std::tanh (v * drive) / std::tanh (drive);
            else
                v = std::tanh (v * 1.08);
            bass[(size_t) index] += (float) v + dither.next();
        }
    }

    struct Layout
    {
        int start;
        int spacing;
        int tail;
        int total;
    };

    Layout layoutFor (double sampleRate, int eventCount, double spacingSeconds = 0.260)
    {
        Layout l;
        l.start = (int) std::lround (0.090 * sampleRate);
        l.spacing = (int) std::lround (spacingSeconds * sampleRate);
        l.tail = (int) std::lround (0.180 * sampleRate);
        l.total = l.start + eventCount * l.spacing + l.tail;
        return l;
    }

    // A shared "family" of (bassHz, kickHz, phase, timingMs, harmonic), matching
    // the well-clustered families in DynamicReleaseFixture.
    struct FamilyShape { double bassHz, kickHz, phase, timingMs, harmonic; };

    FamilyShape families[] = {
        { 46.0, 57.0, 0.15, -1.6, 0.18 },
        { 61.0, 69.0, 1.10,  0.8, 0.34 },
        { 78.0, 52.0, -1.35, 2.1, 0.49 },
        { 96.0, 83.0, 2.25, -0.5, 0.27 },
    };

    // ---- individual case generators --------------------------------------

    // Ground-truth label + correction-capability for a release-fixture family.
    void labelForReleaseFamily (DynamicReleaseFixtureFamily fam, int& label, bool& capable)
    {
        switch (fam)
        {
            case DynamicReleaseFixtureFamily::FamilyA: label = 0; capable = true; break;
            case DynamicReleaseFixtureFamily::FamilyB: label = 1; capable = true; break;
            case DynamicReleaseFixtureFamily::FamilyC: label = 2; capable = true; break;
            case DynamicReleaseFixtureFamily::FamilyD: label = 3; capable = true; break;
            case DynamicReleaseFixtureFamily::RecognizedGlobalOnly: label = 4; capable = false; break;
            default: label = -1; capable = false; break; // Foreign / Ambiguous
        }
    }

    // Anchored on the proven makeDynamicReleaseFixture material: five interleaved
    // families (incl. RecognizedGlobalOnly) plus foreign/ambiguous tail. This is
    // the same generator the release-gate and E2E tests use, so its Learn'd map
    // is known to form runtime-selectable States.
    GeneratedAudioFixture genMultiNote (double sampleRate)
    {
        const auto release = makeDynamicReleaseFixture (sampleRate);
        GeneratedAudioFixture f;
        f.sampleRate = sampleRate;
        f.bpm = 120.0;
        f.bass = release.bass;
        f.kick = release.kick;
        for (const auto& e : release.events)
        {
            int label = -1; bool capable = false;
            labelForReleaseFamily (e.family, label, capable);
            f.events.push_back ({ e.sample, label, capable });
        }
        return f;
    }

    GeneratedAudioFixture genSameNotePhaseFamilies (double sampleRate, int repeats)
    {
        GeneratedAudioFixture f;
        f.sampleRate = sampleRate;
        f.bpm = 120.0;
        // One bass note (55 Hz), three distinct kick-relative phase offsets that
        // repeat -> three repeatable phase families sharing the same pitch.
        const double phaseFamPhase[3] = { 0.10, 1.85, -1.65 };
        const double phaseFamTimingMs[3] = { -1.8, 1.4, 2.6 };
        const int eventCount = repeats * 3;
        const auto L = layoutFor (sampleRate, eventCount + 1);
        f.bass.assign ((size_t) L.total, 0.0f);
        f.kick.assign ((size_t) L.total, 0.0f);
        Dither d (0x7c3311u);
        for (int i = 0; i < eventCount; ++i)
        {
            const int fam = i % 3;
            const int at = L.start + i * L.spacing;
            const int bassOff = (int) std::lround (phaseFamTimingMs[fam] * sampleRate / 1000.0);
            addKick (f.kick, at, sampleRate, 60.0, 33.0, 0.78, d);
            addBassNote (f.bass, at, sampleRate, 55.0, phaseFamPhase[fam], 13.0, 0.30, 0.44, 1.08, bassOff, d);
            f.events.push_back ({ at, fam, true });
        }
        return f;
    }

    GeneratedAudioFixture genFreeRunningPhase (double sampleRate, int events)
    {
        GeneratedAudioFixture f;
        f.sampleRate = sampleRate;
        f.bpm = 123.0;
        const auto L = layoutFor (sampleRate, events + 1);
        f.bass.assign ((size_t) L.total, 0.0f);
        f.kick.assign ((size_t) L.total, 0.0f);
        Dither d (0x1177aau);
        // Continuous, never-retriggered bass oscillator: every kick lands on a
        // different, non-repeatable phase -> no stable family should form.
        for (int i = 0; i < L.total; ++i)
        {
            const double t = (double) i / sampleRate;
            const double v = 0.40 * std::sin (2.0 * kPi * 47.3 * t + 0.7)
                + 0.14 * std::sin (2.0 * kPi * 94.6 * t + 0.2);
            f.bass[(size_t) i] = (float) std::tanh (v * 1.08) + d.next();
        }
        int at = L.start;
        for (int i = 0; i < events; ++i)
        {
            // Irregular spacing so the kick phase against the free-running bass
            // keeps sliding - no two kicks land on the same bass phase.
            const int clamped = std::min (at, L.total - L.tail - 1);
            addKick (f.kick, clamped, sampleRate, 58.0, 33.0, 0.78, d);
            f.events.push_back ({ clamped, -1, false });
            const int jitter = (int) std::lround (0.03 * std::sin ((double) i * 1.7) * sampleRate);
            at += L.spacing + jitter;
        }
        return f;
    }

    GeneratedAudioFixture genDistortedBass (double sampleRate, int repeats)
    {
        GeneratedAudioFixture f;
        f.sampleRate = sampleRate;
        f.bpm = 120.0;
        const auto L = layoutFor (sampleRate, repeats + 1);
        f.bass.assign ((size_t) L.total, 0.0f);
        f.kick.assign ((size_t) L.total, 0.0f);
        Dither d (0x2299ffu);
        for (int i = 0; i < repeats; ++i)
        {
            const int at = L.start + i * L.spacing;
            const int bassOff = (int) std::lround (-1.2 * sampleRate / 1000.0);
            addKick (f.kick, at, sampleRate, 61.0, 33.0, 0.78, d);
            // Driven: repeatable but harmonically rich / lightly clipped.
            addBassNote (f.bass, at, sampleRate, 52.0, 0.35, 12.0, 0.55, 0.6, 2.5, bassOff, d);
            f.events.push_back ({ at, 0, true });
        }
        return f;
    }

    GeneratedAudioFixture genGlide (double sampleRate, int repeats)
    {
        GeneratedAudioFixture f;
        f.sampleRate = sampleRate;
        f.bpm = 120.0;
        const auto L = layoutFor (sampleRate, repeats + 1);
        f.bass.assign ((size_t) L.total, 0.0f);
        f.kick.assign ((size_t) L.total, 0.0f);
        Dither d (0x33aa11u);
        for (int i = 0; i < repeats; ++i)
        {
            const int at = L.start + i * L.spacing;
            addKick (f.kick, at, sampleRate, 60.0, 33.0, 0.78, d);
            // Pitch glides within each note from ~48 to ~64 Hz.
            for (int local = 0; local < L.tail; ++local)
            {
                const int index = at + local;
                if (index >= (int) f.bass.size()) break;
                const double seconds = (double) local / sampleRate;
                const double hz = 48.0 + 16.0 * std::min (1.0, seconds / 0.12);
                const double env = std::exp (-seconds * 12.0);
                const double v = 0.42 * env * std::sin (2.0 * kPi * hz * seconds + 0.2);
                f.bass[(size_t) index] += (float) std::tanh (v * 1.08) + d.next();
            }
            f.events.push_back ({ at, -1, false });
        }
        return f;
    }

    GeneratedAudioFixture genUnisonModulated (double sampleRate, int repeats)
    {
        GeneratedAudioFixture f;
        f.sampleRate = sampleRate;
        f.bpm = 120.0;
        const auto L = layoutFor (sampleRate, repeats + 1);
        f.bass.assign ((size_t) L.total, 0.0f);
        f.kick.assign ((size_t) L.total, 0.0f);
        Dither d (0x44bb22u);
        for (int i = 0; i < repeats; ++i)
        {
            const int at = L.start + i * L.spacing;
            const int bassOff = (int) std::lround (0.6 * sampleRate / 1000.0);
            addKick (f.kick, at, sampleRate, 69.0, 33.0, 0.78, d);
            addBassNote (f.bass, at, sampleRate, 58.0, 0.5, 12.5, 0.30, 0.42, 1.08, bassOff, d,
                         2 /*voices*/, 0.25 /*detuneHz*/, 2.5 /*vibratoHz*/);
            f.events.push_back ({ at, 0, true });
        }
        return f;
    }

    GeneratedAudioFixture genWeakOrMissingKick (double sampleRate, int events)
    {
        GeneratedAudioFixture f;
        f.sampleRate = sampleRate;
        f.bpm = 120.0;
        const auto L = layoutFor (sampleRate, events + 1);
        f.bass.assign ((size_t) L.total, 0.0f);
        f.kick.assign ((size_t) L.total, 0.0f);
        Dither d (0x55cc33u);
        for (int i = 0; i < events; ++i)
        {
            const int at = L.start + i * L.spacing;
            // Bass is present and repeatable, but the kick is either far too weak
            // to trigger (even events) or entirely absent (odd events).
            const double kickAmp = (i % 2 == 0) ? 0.02 : 0.0;
            if (kickAmp > 0.0)
                addKick (f.kick, at, sampleRate, 60.0, 33.0, kickAmp, d);
            addBassNote (f.bass, at, sampleRate, 55.0, 0.2, 13.0, 0.3, 0.44, 1.08,
                         (int) std::lround (-1.0 * sampleRate / 1000.0), d);
            f.events.push_back ({ at, -1, false });
        }
        return f;
    }

    GeneratedAudioFixture genGlobalOnlyCandidate (double sampleRate, int repeats)
    {
        GeneratedAudioFixture f;
        f.sampleRate = sampleRate;
        f.bpm = 120.0;
        const auto L = layoutFor (sampleRate, repeats + 1);
        f.bass.assign ((size_t) L.total, 0.0f);
        f.kick.assign ((size_t) L.total, 0.0f);
        Dither d (0x66dd44u);
        // A single repeatable family that is already close to aligned (small
        // timing offset), so it should be recognized but carry little/no
        // beneficial correction -> recognized-global / candidate territory.
        for (int i = 0; i < repeats; ++i)
        {
            const int at = L.start + i * L.spacing;
            addKick (f.kick, at, sampleRate, 61.0, 33.0, 0.78, d);
            addBassNote (f.bass, at, sampleRate, 112.0, -2.35, 13.5, 0.42, 0.44, 1.08,
                         (int) std::lround (0.2 * sampleRate / 1000.0), d);
            f.events.push_back ({ at, 0, false });
        }
        return f;
    }

    GeneratedAudioFixture genAmbiguous (double sampleRate, int repeats)
    {
        GeneratedAudioFixture f;
        f.sampleRate = sampleRate;
        f.bpm = 120.0;
        const int eventCount = repeats * 2;
        const auto L = layoutFor (sampleRate, eventCount + 1);
        f.bass.assign ((size_t) L.total, 0.0f);
        f.kick.assign ((size_t) L.total, 0.0f);
        Dither d (0x77ee55u);
        // Two families whose bass/kick shapes are nearly identical: repeatable
        // enough to form states, but close enough to trigger ambiguity/Hold.
        for (int i = 0; i < eventCount; ++i)
        {
            const int fam = i % 2;
            const int at = L.start + i * L.spacing;
            const double phase = fam == 0 ? 0.60 : 0.66;
            addKick (f.kick, at, sampleRate, 64.0, 33.0, 0.78, d);
            addBassNote (f.bass, at, sampleRate, 69.0, phase, 13.0, 0.39, 0.42, 1.08,
                         (int) std::lround (0.1 * sampleRate / 1000.0), d);
            f.events.push_back ({ at, fam, true });
        }
        return f;
    }

    GeneratedAudioFixture genLoopWrapFirstKick (double sampleRate, int eventsPerLoop)
    {
        GeneratedAudioFixture f;
        f.sampleRate = sampleRate;
        f.bpm = 120.0;
        // Tight loop whose FIRST event sits right at the loop start, so a wrap
        // puts a kick immediately after the boundary. loop = whole buffer.
        const auto L = layoutFor (sampleRate, eventsPerLoop);
        f.bass.assign ((size_t) L.total, 0.0f);
        f.kick.assign ((size_t) L.total, 0.0f);
        Dither d (0x88ff66u);
        for (int i = 0; i < eventsPerLoop; ++i)
        {
            const int fam = i % 4;
            const auto& s = families[fam];
            const int at = L.start + i * L.spacing;
            const int bassOff = (int) std::lround (s.timingMs * sampleRate / 1000.0);
            addKick (f.kick, at, sampleRate, s.kickHz, 33.0, 0.78, d);
            addBassNote (f.bass, at, sampleRate, s.bassHz, s.phase, 13.5, s.harmonic, 0.44, 1.08, bassOff, d);
            f.events.push_back ({ at, fam, true });
        }
        f.loopStartSample = 0;
        f.loopEndSample = L.total;
        return f;
    }

    GeneratedAudioFixture genRapidTransitions (double sampleRate, int repeats)
    {
        GeneratedAudioFixture f;
        f.sampleRate = sampleRate;
        f.bpm = 140.0;
        const int eventCount = repeats * 4;
        // Tighter-than-default spacing so states change quickly, back to back,
        // without the note tails overlapping enough to corrupt per-event metrics.
        const auto L = layoutFor (sampleRate, eventCount + 1, 0.170);
        f.bass.assign ((size_t) L.total, 0.0f);
        f.kick.assign ((size_t) L.total, 0.0f);
        Dither d (0x9a0b77u);
        for (int i = 0; i < eventCount; ++i)
        {
            const int fam = i % 4;
            const auto& s = families[fam];
            const int at = L.start + i * L.spacing;
            const int bassOff = (int) std::lround (s.timingMs * sampleRate / 1000.0);
            addKick (f.kick, at, sampleRate, s.kickHz, 36.0, 0.78, d);
            addBassNote (f.bass, at, sampleRate, s.bassHz, s.phase, 16.0, s.harmonic, 0.44, 1.08, bassOff, d);
            f.events.push_back ({ at, fam, true });
        }
        return f;
    }
}

std::vector<std::string> listAudioFixtureNames()
{
    return {
        "repeatable_multi_note",
        "same_note_phase_families",
        "free_running_phase",
        "distorted_bass",
        "glide",
        "unison_modulated",
        "weak_or_missing_kick",
        "global_only_candidate",
        "ambiguous_events",
        "loop_wrap_first_kick",
        "rapid_transitions",
    };
}

GeneratedAudioFixture generateAudioFixture (const std::string& name, double sampleRate)
{
    if (name == "repeatable_multi_note")     return genMultiNote (sampleRate);
    if (name == "same_note_phase_families")  return genSameNotePhaseFamilies (sampleRate, 5);
    if (name == "free_running_phase")        return genFreeRunningPhase (sampleRate, 14);
    if (name == "distorted_bass")            return genDistortedBass (sampleRate, 8);
    if (name == "glide")                     return genGlide (sampleRate, 8);
    if (name == "unison_modulated")          return genUnisonModulated (sampleRate, 8);
    if (name == "weak_or_missing_kick")      return genWeakOrMissingKick (sampleRate, 10);
    if (name == "global_only_candidate")     return genGlobalOnlyCandidate (sampleRate, 8);
    if (name == "ambiguous_events")          return genAmbiguous (sampleRate, 6);
    if (name == "loop_wrap_first_kick")      return genLoopWrapFirstKick (sampleRate, 4);
    if (name == "rapid_transitions")         return genRapidTransitions (sampleRate, 6);
    return {};
}
