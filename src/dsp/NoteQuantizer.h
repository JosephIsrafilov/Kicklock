#pragma once

#include <cmath>

// Pure, allocation-free MIDI <-> Hz helpers plus a hysteresis-aware live note
// selector. This is the single home for the note-frequency formula; no other
// file should re-derive midi<->Hz. It has no dependency on JUCE, the processor,
// the note map, or the live PitchTracker, and never allocates, locks, throws,
// logs, or touches parameters, so it is safe to call from any thread.
namespace NoteQuantizer
{
    inline constexpr float kA4Hz   = 440.0f;
    inline constexpr int   kA4Midi = 69;

    // Default runtime hysteresis half-width in cents (v1.2 kHysteresisCents).
    inline constexpr float kDefaultHysteresisCents = 60.0f;

    // Equal-tempered MIDI note -> Hz. Finite for any int input.
    inline float midiToHz (int midi) noexcept
    {
        return kA4Hz * std::pow (2.0f, (float) (midi - kA4Midi) / 12.0f);
    }

    // Continuous MIDI value for a frequency. Returns a negative sentinel
    // (-1.0f) for non-finite, zero, or negative input so callers never have to
    // reason about NaN/Inf. Realistic bass frequencies (>= ~9 Hz) never map to
    // the sentinel, so it is unambiguous in practice.
    inline float hzToMidiFloat (float hz) noexcept
    {
        if (! std::isfinite (hz) || hz <= 0.0f)
            return -1.0f;

        return (float) kA4Midi + 12.0f * std::log2 (hz / kA4Hz);
    }

    // Nearest MIDI semitone, or -1 for invalid input.
    inline int hzToMidi (float hz) noexcept
    {
        if (! std::isfinite (hz) || hz <= 0.0f)
            return -1;

        return (int) std::lround (hzToMidiFloat (hz));
    }

    // Live note selection with hysteresis.
    //
    //  - invalid/non-finite/<= 0 Hz resets lastMidi to -1 and returns -1;
    //  - the first valid pitch snaps to the nearest semitone;
    //  - while the pitch stays within +/- `cents` of the currently held note
    //    centre, the held note is retained (boundary rule: exactly +/- `cents`
    //    is RETAINED, i.e. the comparison is <=);
    //  - crossing that boundary requantises to the nearest semitone.
    //
    // `lastMidi` is caller-owned state (audio-thread-only at runtime). This
    // helper does not clamp to any map range; out-of-range notes are handled by
    // the map lookup, which falls back safely.
    inline int updateWithHysteresis (float trackedHz,
                                     int& lastMidi,
                                     float cents = kDefaultHysteresisCents) noexcept
    {
        if (! std::isfinite (trackedHz) || trackedHz <= 0.0f)
        {
            lastMidi = -1;
            return -1;
        }

        const float midiFloat = hzToMidiFloat (trackedHz);

        if (lastMidi >= 0)
        {
            const float centsFromHeldCentre = (midiFloat - (float) lastMidi) * 100.0f;
            if (std::abs (centsFromHeldCentre) <= cents)
                return lastMidi;
        }

        lastMidi = (int) std::lround (midiFloat);
        return lastMidi;
    }
}
