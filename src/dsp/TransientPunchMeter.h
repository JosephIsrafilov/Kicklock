#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <cmath>

// Kick-punch transient integrity meter. Gated to the kick's own transient
// flag, it opens a short window on each hit and compares the peak of the
// summed kick+bass low end against the kick-alone low end:
//
//     PUNCH_dB = 20 * log10( (sumPeak + eps) / (kickPeak + eps) )
//
// > 0  -> bass reinforces the kick's low end.
// < 0  -> bass cancels it (the "hollow" low end).
// ~ 0  -> neutral.
//
// Readings smooth hit-to-hit with a light EMA so the number doesn't jitter,
// and go invalid after ~1.5 s of no kick so the UI can show a placeholder
// rather than a stale value. Kick peaks below a small floor are ignored so
// silence never produces garbage. If a new transient arrives before the
// current window closes (very fast patterns) the open window is finalized
// early so nothing double-counts.
//
// pushSample() runs on the audio thread and must not allocate, lock, or throw.
// The published readings are stored in atomics so the message-thread getters
// can read them without a lock.
class TransientPunchMeter
{
public:
    void prepare (double sampleRate, float windowMs = 40.0f) noexcept
    {
        const double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
        windowSamples = juce::jmax (1, (int) std::round (sr * (double) windowMs / 1000.0));
        validityTimeoutSamples = juce::jmax (1, (int) std::round (sr * 1.5));
        reset();
    }

    void reset() noexcept
    {
        windowActive = false;
        windowRemaining = 0;
        kickPeak = 0.0f;
        sumPeak = 0.0f;
        smoothedPunchDb = 0.0f;
        hasReading = false;
        samplesSinceValidHit = validityTimeoutSamples;

        publishedPunchDb.store (0.0f, std::memory_order_relaxed);
        publishedKickPeak.store (0.0f, std::memory_order_relaxed);
        publishedSumPeak.store (0.0f, std::memory_order_relaxed);
        publishedValid.store (false, std::memory_order_relaxed);
    }

    void pushSample (float kickLow, float bassLow, bool transientDetected) noexcept
    {
        // A fresh transient finalizes any window still open (early finalize on
        // fast patterns) and starts a new measurement window.
        if (transientDetected)
        {
            if (windowActive)
                finalizeWindow();

            windowActive = true;
            windowRemaining = windowSamples;
            kickPeak = 0.0f;
            sumPeak = 0.0f;
        }

        if (windowActive)
        {
            kickPeak = juce::jmax (kickPeak, std::abs (kickLow));
            sumPeak  = juce::jmax (sumPeak,  std::abs (kickLow + bassLow));

            if (--windowRemaining <= 0)
                finalizeWindow();
        }

        // Age the last valid reading; previously this dropped validity after 1.5s.
        // The user requested the reading to stay constantly without disappearing.
        if (samplesSinceValidHit < validityTimeoutSamples)
        {
            ++samplesSinceValidHit;
        }
    }

    // Smoothed signed punch in dB (positive = reinforcing, negative = cancelling).
    float getPunchDb() const noexcept  { return publishedPunchDb.load (std::memory_order_relaxed); }
    // Last finalized kick-alone / summed low-end peaks (linear).
    float getKickPunch() const noexcept { return publishedKickPeak.load (std::memory_order_relaxed); }
    float getSumPunch() const noexcept  { return publishedSumPeak.load (std::memory_order_relaxed); }
    bool  isValid() const noexcept      { return publishedValid.load (std::memory_order_relaxed); }

private:
    void finalizeWindow() noexcept
    {
        windowActive = false;

        // Ignore windows whose kick never cleared the floor (silence / noise).
        if (kickPeak < kickFloor)
            return;

        const float punchDb = juce::jlimit (-24.0f, 24.0f,
                                             20.0f * std::log10 ((sumPeak + eps) / (kickPeak + eps)));

        if (hasReading)
            smoothedPunchDb += emaAlpha * (punchDb - smoothedPunchDb);
        else
            smoothedPunchDb = punchDb;

        hasReading = true;
        samplesSinceValidHit = 0;

        publishedPunchDb.store (smoothedPunchDb, std::memory_order_relaxed);
        publishedKickPeak.store (kickPeak, std::memory_order_relaxed);
        publishedSumPeak.store (sumPeak, std::memory_order_relaxed);
        publishedValid.store (true, std::memory_order_relaxed);
    }

    static constexpr float emaAlpha = 0.35f;
    static constexpr float kickFloor = 1.0e-4f; // ~ -80 dBFS
    static constexpr float eps = 1.0e-9f;

    int windowSamples = 1;
    int validityTimeoutSamples = 1;

    bool windowActive = false;
    int windowRemaining = 0;
    float kickPeak = 0.0f;
    float sumPeak = 0.0f;
    float smoothedPunchDb = 0.0f;
    bool hasReading = false;
    int samplesSinceValidHit = 1;

    std::atomic<float> publishedPunchDb { 0.0f };
    std::atomic<float> publishedKickPeak { 0.0f };
    std::atomic<float> publishedSumPeak { 0.0f };
    std::atomic<bool> publishedValid { false };
};
