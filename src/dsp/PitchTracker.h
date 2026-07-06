#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>

// Real-time bass fundamental tracker for the dynamic phase-filter follow mode.
//
// Design notes (why zero-crossing, not YIN):
//   The input is the bass AFTER the crossover low-pass (<= 500 Hz, typically
//   150 Hz), i.e. a near-sinusoidal sub/low fundamental. On that signal a
//   Schmitt-triggered zero-crossing period measurement is as accurate as a
//   lag-domain search (at 48 kHz a 60 Hz period is 800 samples, so +-1 sample
//   of crossing jitter is +-0.075 Hz) while costing O(1) per sample with ZERO
//   per-block spikes — YIN's O(window x maxLag) difference function would
//   either spike the audio thread when a window completes or need a worker
//   thread + FIFO for no accuracy gain on this band-limited material.
//
// Robustness layers, in order:
//   1. Envelope gate — no crossings are trusted below a small floor, and a
//      sustained silence publishes "not tracking" (0 Hz).
//   2. Schmitt hysteresis scaled by the envelope — noise riding the zero line
//      cannot chatter the detector.
//   3. Deep-swing requirement — a rising crossing only counts if the signal
//      actually swung properly negative since the previous one, so shallow
//      harmonic ripple mid-cycle is ignored rather than halving the period.
//   4. Median-of-5 with a stability gate — the published frequency only moves
//      when at least 4 of the last 5 periods agree with their median within
//      ~12%, so one bad crossing never detunes the phase filter. While the
//      pitch is unstable (slides, fills) the last stable value is held, which
//      is the musical behaviour for a filter that is following notes.
//
// pushSample() runs on the audio thread: no allocation, no locks, no
// transcendentals. The published frequency is an atomic so the UI (and the
// audio thread itself) can read it lock-free. Header-only and JUCE-free so it
// unit-tests directly like the other DSP helpers.
class PitchTracker
{
public:
    void prepare (double newSampleRate, float minHz = 25.0f, float maxHz = 300.0f) noexcept
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        const float loHz = std::max (1.0f, std::min (minHz, maxHz));
        const float hiHz = std::max (loHz + 1.0f, std::max (minHz, maxHz));

        minPeriodSamples = std::max (2, (int) (sampleRate / (double) hiHz));
        maxPeriodSamples = std::max (minPeriodSamples + 1, (int) (sampleRate / (double) loHz));

        envelopeAttackCoeff = timeMsToCoeff (5.0f);
        envelopeReleaseCoeff = timeMsToCoeff (150.0f);
        silenceTimeoutSamples = std::max (1, (int) (sampleRate * 0.3));

        reset();
    }

    void reset() noexcept
    {
        envelope = 0.0f;
        schmittLow = false;
        samplesSinceCrossing = 0;
        minSinceCrossing = 0.0f;
        silentSamples = silenceTimeoutSamples;
        validPeriods = 0;
        periodWriteIndex = 0;
        periods.fill (0);
        publishedHz.store (0.0f, std::memory_order_relaxed);
    }

    // Audio thread. Feed the LOW-PASSED mono bass (post-crossover), one sample
    // at a time. O(1), never allocates, locks, or throws.
    void pushSample (float x) noexcept
    {
        const float ax = std::abs (x);
        const float envCoeff = ax > envelope ? envelopeAttackCoeff : envelopeReleaseCoeff;
        envelope = envCoeff * envelope + (1.0f - envCoeff) * ax;

        if (samplesSinceCrossing < maxPeriodSamples * 4)
            ++samplesSinceCrossing;

        minSinceCrossing = std::min (minSinceCrossing, x);

        // Silence detection runs on the RAW magnitude, not the envelope: the
        // slow release means the envelope outlives the note by most of a
        // second, which would delay the "note over" verdict. A playing note
        // only dips below the floor for a few samples around each zero
        // crossing, so the counter can never accumulate to the timeout while
        // material is actually present.
        if (ax < silenceFloor)
        {
            if (silentSamples < silenceTimeoutSamples && ++silentSamples >= silenceTimeoutSamples)
            {
                // Note over: report "not tracking" and demand a fresh stable
                // run of periods before publishing again.
                publishedHz.store (0.0f, std::memory_order_relaxed);
                validPeriods = 0;
                schmittLow = false;
            }
        }
        else
        {
            silentSamples = 0;
        }

        if (envelope < silenceFloor)
            return;

        const float hysteresis = std::max (envelope * 0.25f, 1.0e-4f);

        if (x < -hysteresis)
        {
            schmittLow = true;
        }
        else if (schmittLow && x > hysteresis)
        {
            schmittLow = false;

            const int period = samplesSinceCrossing;
            const bool deepSwing = minSinceCrossing < -0.4f * envelope;
            samplesSinceCrossing = 0;
            minSinceCrossing = 0.0f;

            if (deepSwing && period >= minPeriodSamples && period <= maxPeriodSamples)
                acceptPeriod (period);
        }
    }

    // 0 when not tracking (silence / nothing stable yet). Lock-free.
    float getFrequencyHz() const noexcept
    {
        return publishedHz.load (std::memory_order_relaxed);
    }

    bool isTracking() const noexcept { return getFrequencyHz() > 0.0f; }

private:
    void acceptPeriod (int period) noexcept
    {
        periods[(size_t) periodWriteIndex] = period;
        periodWriteIndex = (periodWriteIndex + 1) % (int) periods.size();
        if (validPeriods < (int) periods.size())
            ++validPeriods;

        if (validPeriods < (int) periods.size())
            return;

        // Median of 5 on the stack; publish only when at least 4 of 5 agree
        // with the median within ~12% (stable, sustained note).
        auto sorted = periods;
        std::nth_element (sorted.begin(), sorted.begin() + 2, sorted.end());
        const int median = sorted[2];
        if (median <= 0)
            return;

        int agreeing = 0;
        for (const int p : periods)
            if (std::abs (p - median) <= median / 8)
                ++agreeing;

        if (agreeing >= 4)
            publishedHz.store ((float) (sampleRate / (double) median), std::memory_order_relaxed);
    }

    float timeMsToCoeff (float ms) const noexcept
    {
        const double samples = std::max (1.0, sampleRate * (double) ms / 1000.0);
        return (float) std::exp (-1.0 / samples);
    }

    static constexpr float silenceFloor = 1.0e-3f;

    double sampleRate = 44100.0;
    int minPeriodSamples = 2;
    int maxPeriodSamples = 4096;
    float envelopeAttackCoeff = 0.0f;
    float envelopeReleaseCoeff = 0.0f;
    int silenceTimeoutSamples = 1;

    float envelope = 0.0f;
    bool schmittLow = false;
    int samplesSinceCrossing = 0;
    float minSinceCrossing = 0.0f;
    int silentSamples = 0;

    std::array<int, 5> periods {};
    int periodWriteIndex = 0;
    int validPeriods = 0;

    std::atomic<float> publishedHz { 0.0f };
};
