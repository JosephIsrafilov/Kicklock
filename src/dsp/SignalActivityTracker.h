#pragma once

#include <algorithm>
#include <cmath>

// Musically-aware activity tracker for one signal (kick or bass).
//
// Instant per-block RMS is the wrong primary signal for "is there a kick?":
// a normal beat is mostly silence between transients, so a per-block gate
// reads "no signal" in every gap and the status flickers to SIGNAL TOO LOW
// between hits. This tracker instead HOLDS an "active" verdict for a
// configurable window (~300-800 ms) after the last time the level crossed a
// small threshold, so a steady loop stays "active" continuously.
//
// Header-only, no JUCE, so it can be unit-tested directly. Updated once per
// processed block from the message/audio-adjacent context with that block's
// RMS and duration; never allocates.
class SignalActivityTracker
{
public:
    void prepare (double newSampleRate, float holdMs = 500.0f, float activationRms = 1.0e-3f) noexcept
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        setHoldMs (holdMs);
        activationThreshold = std::max (0.0f, activationRms);
        reset();
    }

    void reset() noexcept
    {
        heldSamplesRemaining = 0;
        lastRms = 0.0f;
        heldPeakRms = 0.0f;
        everActive = false;
    }

    void setHoldMs (float holdMs) noexcept
    {
        holdSamples = std::max (1, (int) std::llround (sampleRate * (double) std::max (1.0f, holdMs) / 1000.0));
    }

    void setActivationRms (float rms) noexcept
    {
        activationThreshold = std::max (0.0f, rms);
    }

    // Feed one processed block. rms is that block's RMS for this signal,
    // blockSamples its length. A block whose RMS crosses the activation
    // threshold refreshes the hold window; otherwise the window counts down.
    void pushBlock (float rms, int blockSamples) noexcept
    {
        lastRms = rms;

        if (rms >= activationThreshold)
        {
            heldSamplesRemaining = holdSamples;
            heldPeakRms = std::max (heldPeakRms, rms);
            everActive = true;
        }
        else
        {
            heldSamplesRemaining = std::max (0, heldSamplesRemaining - std::max (0, blockSamples));
            if (heldSamplesRemaining <= 0)
                heldPeakRms = 0.0f;
        }
    }

    // True while inside the hold window after the most recent activation.
    bool isActive() const noexcept { return heldSamplesRemaining > 0; }

    // True if the signal has ever crossed the threshold since the last reset.
    bool hasEverBeenActive() const noexcept { return everActive; }

    float getLastRms() const noexcept { return lastRms; }

    // Loudest block RMS seen within the current hold window (0 once it lapses).
    // Used to tell "present but far too quiet to phase-read" (SIGNAL TOO LOW)
    // apart from "genuinely playing", without flickering between hits.
    float getHeldPeakRms() const noexcept { return heldPeakRms; }

    // True when the recent held peak clears a usable floor for a phase read.
    bool isUsable (float usableFloorRms) const noexcept
    {
        return isActive() && heldPeakRms >= usableFloorRms;
    }

private:
    double sampleRate = 44100.0;
    int holdSamples = 1;
    float activationThreshold = 1.0e-3f;
    int heldSamplesRemaining = 0;
    float lastRms = 0.0f;
    float heldPeakRms = 0.0f;
    bool everActive = false;
};
