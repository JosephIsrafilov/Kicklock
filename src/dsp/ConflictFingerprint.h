#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

#include "TransientDetector.h"

// A deliberately small, train/serve-identical description of one kick/bass
// conflict.  The first three values locate anti-phase overlap across the
// onset micro-window; the fourth distinguishes the kick envelope itself.  All
// values are [0, 1], so matching is a cheap L1 distance on the audio thread.
struct ConflictFingerprint
{
    static constexpr int kFeatureCount = 4;
    // Four milliseconds keeps state selection causal enough for the requested
    // 1-5 ms parameter ramp.  A longer post-hit analysis would only select a
    // correction after the conflict had already passed.
    static constexpr float kWindowMs = 4.0f;
    static constexpr float kAttackMs = 1.0f;
    static constexpr float kBodyEndMs = 3.0f;
    static constexpr uint64_t kValidMarker = 0x8000000000000000ull;

    std::array<float, kFeatureCount> values {};
    bool valid = false;

    uint64_t pack() const noexcept
    {
        if (! valid)
            return 0;

        uint64_t packed = kValidMarker;
        for (int i = 0; i < kFeatureCount; ++i)
        {
            const auto byte = (uint64_t) std::lround (
                std::clamp (std::isfinite (values[(size_t) i]) ? values[(size_t) i] : 0.0f,
                            0.0f, 1.0f) * 255.0f);
            packed |= byte << (i * 8);
        }
        return packed;
    }

    static ConflictFingerprint unpack (uint64_t packed) noexcept
    {
        ConflictFingerprint result;
        result.valid = (packed & kValidMarker) != 0;
        if (! result.valid)
            return result;

        for (int i = 0; i < kFeatureCount; ++i)
            result.values[(size_t) i] = (float) ((packed >> (i * 8)) & 0xffu) / 255.0f;
        return result;
    }

    static float distance (uint64_t a, uint64_t b) noexcept
    {
        const auto left = unpack (a);
        const auto right = unpack (b);
        if (! left.valid || ! right.valid)
            return std::numeric_limits<float>::infinity();

        float total = 0.0f;
        for (int i = 0; i < kFeatureCount; ++i)
            total += std::abs (left.values[(size_t) i] - right.values[(size_t) i]);
        return total / (float) kFeatureCount;
    }
};

// Shared streaming primitive: worker-side Learn feeds a captured hit into it;
// the audio thread feeds exactly the same bass/kick samples after a production
// transient trigger.  It has fixed storage and no allocation or locking.
class ConflictFingerprintAccumulator
{
public:
    void prepare (double newSampleRate) noexcept
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        windowSamples = std::max (1, (int) std::lround (sampleRate * ConflictFingerprint::kWindowMs / 1000.0));
        attackSamples = std::max (1, (int) std::lround (sampleRate * ConflictFingerprint::kAttackMs / 1000.0));
        bodyEndSamples = std::max (attackSamples + 1,
                                   (int) std::lround (sampleRate * ConflictFingerprint::kBodyEndMs / 1000.0));
        reset();
    }

    void reset() noexcept
    {
        samples = 0;
        kickEnergy = {};
        bassEnergy = {};
        cross = {};
    }

    void pushSample (float bass, float kick) noexcept
    {
        if (samples >= windowSamples || ! std::isfinite (bass) || ! std::isfinite (kick))
            return;

        const int region = samples < attackSamples ? 0 : (samples < bodyEndSamples ? 1 : 2);
        const float bb = bass * bass;
        const float kk = kick * kick;
        bassEnergy[(size_t) region] += bb;
        kickEnergy[(size_t) region] += kk;
        cross[(size_t) region] += bass * kick;
        ++samples;
    }

    bool complete() const noexcept { return samples >= windowSamples; }

    ConflictFingerprint finish() const noexcept
    {
        ConflictFingerprint result;
        if (! complete())
            return result;

        for (int i = 0; i < 3; ++i)
        {
            const float norm = std::sqrt (std::max (0.0f, bassEnergy[(size_t) i] * kickEnergy[(size_t) i]));
            result.values[(size_t) i] = norm > 1.0e-12f
                ? std::clamp (-cross[(size_t) i] / norm, 0.0f, 1.0f) : 0.0f;
        }

        const float totalKick = kickEnergy[0] + kickEnergy[1] + kickEnergy[2];
        result.values[3] = totalKick > 1.0e-12f
            ? std::clamp (kickEnergy[0] / totalKick, 0.0f, 1.0f) : 0.0f;
        result.valid = totalKick > 1.0e-12f;
        return result;
    }

private:
    double sampleRate = 44100.0;
    int windowSamples = 1;
    int attackSamples = 1;
    int bodyEndSamples = 2;
    int samples = 0;
    std::array<float, 3> kickEnergy {};
    std::array<float, 3> bassEnergy {};
    std::array<float, 3> cross {};
};

inline ConflictFingerprint makeConflictFingerprint (const float* bass, const float* kick,
                                                     int numSamples, double sampleRate,
                                                     int onsetSample) noexcept
{
    ConflictFingerprintAccumulator accumulator;
    accumulator.prepare (sampleRate);
    if (bass == nullptr || kick == nullptr || onsetSample < 0 || onsetSample >= numSamples)
        return {};

    for (int i = onsetSample; i < numSamples && ! accumulator.complete(); ++i)
        accumulator.pushSample (bass[i], kick[i]);
    return accumulator.finish();
}

// Runtime onset capture using the exact detector configuration used by Learn.
class RuntimeConflictFingerprintCapture
{
public:
    void prepare (double newSampleRate) noexcept
    {
        detector.prepare (newSampleRate);
        detector.setThreshold (1.0e-7f);
        detector.setMinimumEnergyGate (1.0e-8f);
        detector.setAttackReleaseMs (2.0f, 60.0f);
        detector.setTriggerRatio (1.35f);
        detector.setHoldoffMs (90.0f);
        accumulator.prepare (newSampleRate);
        reset();
    }

    void reset() noexcept
    {
        detector.reset();
        accumulator.reset();
        capturing = false;
        ready = false;
        latest = {};
    }

    void pushSample (float bass, float kick) noexcept
    {
        if (detector.processSample (kick))
        {
            accumulator.reset();
            capturing = true;
        }

        if (! capturing)
            return;

        accumulator.pushSample (bass, kick);
        if (accumulator.complete())
        {
            latest = accumulator.finish();
            ready = latest.valid;
            capturing = false;
        }
    }

    bool takeCompleted (ConflictFingerprint& out) noexcept
    {
        if (! ready)
            return false;
        out = latest;
        ready = false;
        return true;
    }

private:
    TransientDetector detector;
    ConflictFingerprintAccumulator accumulator;
    ConflictFingerprint latest;
    bool capturing = false;
    bool ready = false;
};
