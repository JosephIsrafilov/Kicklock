#pragma once

#include <algorithm>
#include <cmath>

// Relative transient detector.
//
// The old detector compared a single envelope follower against an absolute
// threshold and armed a rising-edge flag that only cleared once the envelope
// fell back below that threshold. On long 808s / dense or noisy material the
// energy between hits never dropped below the threshold, so the flag stayed
// latched and every hit after the first was ignored until the transport
// stopped and the signal hit absolute zero.
//
// This version tracks two envelopes:
//   * a FAST follower that snaps to transient peaks, and
//   * a SLOW follower that tracks the signal "body" / floor.
// A hit is a rising edge where the fast envelope pulls meaningfully ahead of
// the slow one. The slow envelope catches up within tens of ms after a
// transient, so the edge flag re-arms on its own regardless of the absolute
// level between hits — no return to silence required. The absolute floors
// (threshold + minimumEnergyGate) remain only as safeguards against triggering
// on the microscopic digital noise floor; they no longer drive the edge.
class TransientDetector
{
public:
    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        updateCoefficients();
        reset();
    }

    void reset() noexcept
    {
        fastEnvelope = 0.0f;
        slowEnvelope = 0.0f;
        wasTriggering = false;
        holdoffSamplesRemaining = 0;
    }

    void setThreshold (float newThreshold) noexcept
    {
        threshold = std::clamp (newThreshold, 0.0f, 1.0f);
    }

    void setMinimumEnergyGate (float newMinimumEnergy) noexcept
    {
        minimumEnergyGate = std::max (0.0f, newMinimumEnergy);
    }

    // Sets the FAST follower's attack/release (transient tracking). The slow
    // "body" follower is derived from these so callers that only tune the fast
    // envelope still get a sensible relative comparison.
    void setAttackReleaseMs (float attackMs, float releaseMs) noexcept
    {
        attackTimeMs = std::max (0.1f, attackMs);
        releaseTimeMs = std::max (attackTimeMs, releaseMs);
        updateCoefficients();
    }

    // Optional: tune the slow "body" follower directly. Defaults are derived
    // from the fast release in setAttackReleaseMs, so this is rarely needed.
    void setSlowAttackReleaseMs (float attackMs, float releaseMs) noexcept
    {
        slowAttackTimeMs = std::max (attackTimeMs, attackMs);
        slowReleaseTimeMs = std::max (slowAttackTimeMs, releaseMs);
        slowTimesExplicit = true;
        updateCoefficients();
    }

    // How far the fast envelope must exceed the slow envelope (energy ratio)
    // for a new attack to register. 1.0 = no headroom required.
    void setTriggerRatio (float newRatio) noexcept
    {
        triggerRatio = std::max (1.0f, newRatio);
    }

    void setHoldoffMs (float newHoldoffMs) noexcept
    {
        holdoffMs = std::max (1.0f, newHoldoffMs);
        updateHoldoffSamples();
    }

    bool processSample (float kickSample) noexcept
    {
        const float energy = kickSample * kickSample;

        const float fastCoeff = energy > fastEnvelope ? fastAttackCoeff : fastReleaseCoeff;
        fastEnvelope = fastCoeff * fastEnvelope + (1.0f - fastCoeff) * energy;

        const float slowCoeff = energy > slowEnvelope ? slowAttackCoeff : slowReleaseCoeff;
        slowEnvelope = slowCoeff * slowEnvelope + (1.0f - slowCoeff) * energy;

        if (holdoffSamplesRemaining > 0)
            --holdoffSamplesRemaining;

        // Safeguard floors: never react to the digital noise floor.
        const bool aboveFloor = fastEnvelope >= minimumEnergyGate
                             && fastEnvelope >= threshold;

        // Relative attack: the fast peak follower has pulled ahead of the slow
        // body follower. This condition falls back to false on its own as the
        // slow envelope catches up, so it re-arms without needing silence.
        const bool triggering = aboveFloor
                             && fastEnvelope > slowEnvelope * triggerRatio;

        const bool detected = triggering && ! wasTriggering && holdoffSamplesRemaining <= 0;

        wasTriggering = triggering;

        if (detected)
            holdoffSamplesRemaining = holdoffSamples;

        return detected;
    }

    // Reports the fast (peak) envelope, matching the previous single-envelope
    // semantics used by any UI/metering callers.
    float getEnvelope() const noexcept { return fastEnvelope; }

private:
    void updateCoefficients() noexcept
    {
        fastAttackCoeff = timeMsToCoeff (attackTimeMs);
        fastReleaseCoeff = timeMsToCoeff (releaseTimeMs);

        // Derive the slow "body" follower from the fast release unless the
        // caller pinned it explicitly. A slow attack lets the body lag behind
        // a fresh transient; a long release keeps the reference steady between
        // closely spaced hits.
        if (! slowTimesExplicit)
        {
            slowAttackTimeMs = std::max (attackTimeMs, releaseTimeMs * 4.0f);
            slowReleaseTimeMs = std::max (slowAttackTimeMs, releaseTimeMs * 12.5f);
        }

        slowAttackCoeff = timeMsToCoeff (slowAttackTimeMs);
        slowReleaseCoeff = timeMsToCoeff (slowReleaseTimeMs);

        updateHoldoffSamples();
    }

    void updateHoldoffSamples() noexcept
    {
        holdoffSamples = std::max (1, (int) std::round (sampleRate * holdoffMs / 1000.0));
    }

    float timeMsToCoeff (float ms) const noexcept
    {
        const double samples = std::max (1.0, sampleRate * (double) ms / 1000.0);
        return (float) std::exp (-1.0 / samples);
    }

    double sampleRate = 44100.0;
    float threshold = 1.0e-7f;
    float minimumEnergyGate = 1.0e-8f;
    float attackTimeMs = 2.0f;
    float releaseTimeMs = 60.0f;
    float slowAttackTimeMs = 240.0f;
    float slowReleaseTimeMs = 750.0f;
    bool slowTimesExplicit = false;
    float triggerRatio = 1.35f;
    float holdoffMs = 90.0f;
    float fastAttackCoeff = 0.0f;
    float fastReleaseCoeff = 0.0f;
    float slowAttackCoeff = 0.0f;
    float slowReleaseCoeff = 0.0f;
    float fastEnvelope = 0.0f;
    float slowEnvelope = 0.0f;
    bool wasTriggering = false;
    int holdoffSamples = 1;
    int holdoffSamplesRemaining = 0;
};
