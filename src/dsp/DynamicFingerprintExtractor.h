#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "DynamicStateMap.h"

// =============================================================================
// Dynamic fingerprint extractor v1 (Phase 3).
//
// This is the single train/serve-identical descriptor for the frozen Dynamic
// architecture. Learn and the audio thread must both build fingerprints from
// this exact contract: one fixed analysis front end, one trigger sample, one
// 4 ms window, one set of accumulators, one set of feature formulas, one set of
// validity gates and one extractor version.
//
// It deliberately does NOT touch ConflictFingerprint (the legacy packed uint64
// descriptor) or the legacy Dynamic selector. Those remain compatibility-only.
// This header is not wired into PluginProcessor, Learn, transport or UI yet.
// =============================================================================

namespace DynamicFingerprintContract
{
    // Extractor version 1 produces exactly eight active features. This mirrors
    // the persistent DynamicStateMap contract and must stay in lockstep with it.
    inline constexpr int kFeatureCount = 8;
    static_assert (kFeatureCount == DynamicStateMapContract::kMaxFingerprintFeatures,
                   "v1 fingerprint fills the full DynamicStateMap feature capacity");

    inline constexpr uint32_t kExtractorVersion = DynamicStateMapContract::kExtractorVersion;

    // Fixed analysis front end (independent of the user crossover): a second
    // order Butterworth high-pass at 30 Hz followed by a second order
    // Butterworth low-pass at 220 Hz, both at Q = 1 / sqrt(2).
    inline constexpr double kHighPassHz = 30.0;
    inline constexpr double kLowPassHz = 220.0;
    inline constexpr double kButterworthQ = 0.70710678118654752440; // 1 / sqrt(2)

    // Fingerprint duration and temporal regions (milliseconds from the trigger).
    inline constexpr double kWindowMs = 4.0;
    inline constexpr double kAttackEndMs = 1.0;
    inline constexpr double kBodyEndMs = 3.0;

    // One centralized positive epsilon. It is small enough that it cannot
    // influence any signal above the RMS validity gate, but it keeps the log
    // ratio and share features finite when a region is empty.
    inline constexpr double kEpsilon = 1.0e-12;

    // Absolute-level validity gate (filtered-signal RMS over the full window).
    inline constexpr double kMinKickRms = 1.0e-5;
    inline constexpr double kMinBassRms = 1.0e-5;

    // Bounded concurrent runtime capture slots. Four overlapping triggers may be
    // in flight at once; a fifth is explicitly rejected rather than silently
    // dropping or overwriting an existing capture.
    inline constexpr int kMaxConcurrentDynamicFingerprintCaptures = 4;

    // A sample rate must be finite and high enough that the 220 Hz low-pass sits
    // safely below Nyquist. All supported host rates (44.1 - 192 kHz) qualify.
    inline bool isSupportedSampleRate (double sampleRate) noexcept
    {
        return std::isfinite (sampleRate) && sampleRate > 0.0
            && kLowPassHz < 0.45 * sampleRate;
    }
}

// Categorical validity reason for one completed fingerprint. A valid fingerprint
// is never fabricated from silence, NaN or an incomplete window.
enum class DynamicFingerprintValidity : uint8_t
{
    Valid = 0,
    Incomplete,
    InvalidSampleRate,
    NonFiniteInput,
    InsufficientKick,
    InsufficientBass
};

// One extracted v1 fingerprint. `features` are always finite and, when Valid,
// inside [-1, +1]. This is the runtime-side result; `toPrototype()` converts it
// to the persistent DynamicStateMap storage form.
struct DynamicRuntimeFingerprint
{
    DynamicFingerprintValidity validity = DynamicFingerprintValidity::Incomplete;
    uint32_t extractorVersion = DynamicFingerprintContract::kExtractorVersion;
    uint8_t featureCount = 0;
    std::array<float, DynamicFingerprintContract::kFeatureCount> features {};

    bool isValid() const noexcept { return validity == DynamicFingerprintValidity::Valid; }

    DynamicFingerprintPrototype toPrototype() const noexcept
    {
        DynamicFingerprintPrototype prototype;
        prototype.valid = isValid();
        prototype.featureCount = prototype.valid
            ? (uint8_t) DynamicFingerprintContract::kFeatureCount : (uint8_t) 0;
        for (int i = 0; i < DynamicFingerprintContract::kFeatureCount; ++i)
            prototype.features[(size_t) i] = prototype.valid ? features[(size_t) i] : 0.0f;
        return prototype;
    }
};

// Runtime eligibility gate for extractor version 1. A prototype is only usable
// as a v1 fingerprint when it is valid, carries exactly eight features, every
// feature is finite and in [-1, +1], and the owning extractor version is 1.
inline bool isRuntimeEligibleDynamicFingerprintV1 (const DynamicFingerprintPrototype& prototype,
                                                   uint32_t extractorVersion) noexcept
{
    if (extractorVersion != DynamicFingerprintContract::kExtractorVersion)
        return false;
    if (! prototype.valid || prototype.featureCount != DynamicFingerprintContract::kFeatureCount)
        return false;
    for (int i = 0; i < DynamicFingerprintContract::kFeatureCount; ++i)
    {
        const float value = prototype.features[(size_t) i];
        if (! std::isfinite (value) || value < -1.0f || value > 1.0f)
            return false;
    }
    return true;
}

// Allocation-free v1 distance. Equal feature weighting, normalized to [0, 1].
// Identity is exactly 0 where the floating representation permits, the metric is
// symmetric, and mismatched or invalid prototypes return +infinity.
inline float dynamicFingerprintDistanceV1 (const DynamicFingerprintPrototype& a,
                                           const DynamicFingerprintPrototype& b) noexcept
{
    if (! isRuntimeEligibleDynamicFingerprintV1 (a, DynamicFingerprintContract::kExtractorVersion)
        || ! isRuntimeEligibleDynamicFingerprintV1 (b, DynamicFingerprintContract::kExtractorVersion))
        return std::numeric_limits<float>::infinity();

    float total = 0.0f;
    for (int i = 0; i < DynamicFingerprintContract::kFeatureCount; ++i)
        total += std::abs (a.features[(size_t) i] - b.features[(size_t) i]);
    return total / (float) (2 * DynamicFingerprintContract::kFeatureCount);
}

// -----------------------------------------------------------------------------
// Fixed analysis front end.
//
// One shared, continuously running high-pass -> low-pass chain. Bass and kick
// share identical coefficients and topology but keep independent state. State is
// continuous across ordinary samples and only cleared at prepare()/reset(). No
// allocation, locks or JUCE reference-counted coefficient objects appear in
// processSample(); coefficients are computed once in prepare().
// -----------------------------------------------------------------------------
namespace DynamicFingerprintDetail
{
    inline double flushDenormal (double x) noexcept
    {
        // abs(NaN) > tiny is false, so this also sanitizes any stray non-finite
        // state back to zero, keeping the shared front end bounded forever.
        return (std::abs (x) > 1.0e-300) ? x : 0.0;
    }

    // A single fixed-coefficient biquad section, transposed direct form II, with
    // denormal/NaN flushing on the state. Coefficients are already normalized by
    // a0. This is a value type: no allocation, no locks, no JUCE coefficients.
    struct Biquad
    {
        enum class Kind { highPass, lowPass };

        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
        double z1 = 0.0, z2 = 0.0;

        void resetState() noexcept { z1 = 0.0; z2 = 0.0; }

        void setCoefficients (Kind kind, double frequencyHz, double q, double fs) noexcept
        {
            b0 = 1.0; b1 = 0.0; b2 = 0.0; a1 = 0.0; a2 = 0.0;
            if (! (fs > 0.0) || ! (frequencyHz > 0.0) || ! (q > 0.0))
                return;

            const double w0 = 2.0 * 3.14159265358979323846 * frequencyHz / fs;
            const double cosw0 = std::cos (w0);
            const double sinw0 = std::sin (w0);
            const double alpha = sinw0 / (2.0 * q);
            const double a0 = 1.0 + alpha;
            if (! (std::abs (a0) > 0.0))
                return;

            if (kind == Kind::lowPass)
            {
                const double b = (1.0 - cosw0);
                b0 = (b * 0.5) / a0;
                b1 = b / a0;
                b2 = (b * 0.5) / a0;
            }
            else
            {
                const double b = (1.0 + cosw0);
                b0 = (b * 0.5) / a0;
                b1 = -(b) / a0;
                b2 = (b * 0.5) / a0;
            }
            a1 = (-2.0 * cosw0) / a0;
            a2 = (1.0 - alpha) / a0;
        }

        double process (double x) noexcept
        {
            const double y = b0 * x + z1;
            z1 = flushDenormal (b1 * x - a1 * y + z2);
            z2 = flushDenormal (b2 * x - a2 * y);
            return y;
        }
    };
}

class DynamicFingerprintFrontEnd
{
public:
    void prepare (double newSampleRate) noexcept
    {
        supported = DynamicFingerprintContract::isSupportedSampleRate (newSampleRate);
        sampleRate = supported ? newSampleRate : 0.0;
        for (auto* section : { &bassHigh, &kickHigh })
            section->setCoefficients (Biquad::Kind::highPass, DynamicFingerprintContract::kHighPassHz,
                                      DynamicFingerprintContract::kButterworthQ, sampleRate);
        for (auto* section : { &bassLow, &kickLow })
            section->setCoefficients (Biquad::Kind::lowPass, DynamicFingerprintContract::kLowPassHz,
                                      DynamicFingerprintContract::kButterworthQ, sampleRate);
        reset();
    }

    void reset() noexcept
    {
        bassHigh.resetState();
        bassLow.resetState();
        kickHigh.resetState();
        kickLow.resetState();
    }

    bool isReady() const noexcept { return supported; }

    // Filters one raw stereo-mono sample pair. Non-finite input is substituted
    // with 0 so the shared state can never be poisoned; callers observe the
    // non-finite condition separately at the capture layer.
    void processSample (float rawBass, float rawKick, double& filteredBass, double& filteredKick) noexcept
    {
        const double bassIn = std::isfinite (rawBass) ? (double) rawBass : 0.0;
        const double kickIn = std::isfinite (rawKick) ? (double) rawKick : 0.0;
        filteredBass = bassLow.process (bassHigh.process (bassIn));
        filteredKick = kickLow.process (kickHigh.process (kickIn));
    }

private:
    using Biquad = DynamicFingerprintDetail::Biquad;

    double sampleRate = 0.0;
    bool supported = false;
    Biquad bassHigh, bassLow, kickHigh, kickLow;
};

// -----------------------------------------------------------------------------
// Streaming accumulator for one capture window. Holds only fixed-size double
// accumulators; no allocation, no vector. Fed with FILTERED bass/kick samples.
// -----------------------------------------------------------------------------
struct DynamicFingerprintWindow
{
    int windowSamples = 1;
    int attackEnd = 1; // first Body index
    int bodyEnd = 2;   // first Tail index

    static DynamicFingerprintWindow forSampleRate (double sampleRate) noexcept
    {
        DynamicFingerprintWindow w;
        const int total = std::max (3, (int) std::lround (
            sampleRate * DynamicFingerprintContract::kWindowMs / 1000.0));
        int attack = (int) std::lround (sampleRate * DynamicFingerprintContract::kAttackEndMs / 1000.0);
        int body = (int) std::lround (sampleRate * DynamicFingerprintContract::kBodyEndMs / 1000.0);

        // Clamp rounded boundaries so all three regions are ordered and
        // non-empty regardless of sample rate.
        attack = std::clamp (attack, 1, total - 2);
        body = std::clamp (body, attack + 1, total - 1);

        w.windowSamples = total;
        w.attackEnd = attack;
        w.bodyEnd = body;
        return w;
    }
};

class DynamicFingerprintAccumulator
{
public:
    void configure (const DynamicFingerprintWindow& windowLayout, bool sampleRateSupported) noexcept
    {
        window = windowLayout;
        supported = sampleRateSupported;
        reset();
    }

    void reset() noexcept
    {
        samples = 0;
        sawNonFinite = false;
        bassEnergy = {};
        kickEnergy = {};
        cross = {};
        bassMoment = 0.0;
        kickMoment = 0.0;
    }

    bool complete() const noexcept { return samples >= window.windowSamples; }

    // `rawWasFinite` reports whether the pre-filter sample pair was finite; a
    // single non-finite raw sample marks the whole window as non-finite.
    void pushFiltered (double filteredBass, double filteredKick, bool rawWasFinite) noexcept
    {
        if (samples >= window.windowSamples)
            return;
        if (! rawWasFinite || ! std::isfinite (filteredBass) || ! std::isfinite (filteredKick))
            sawNonFinite = true;

        const int region = samples < window.attackEnd ? 0 : (samples < window.bodyEnd ? 1 : 2);
        const double bb = filteredBass * filteredBass;
        const double kk = filteredKick * filteredKick;
        bassEnergy[(size_t) region] += bb;
        kickEnergy[(size_t) region] += kk;
        cross[(size_t) region] += filteredBass * filteredKick;
        bassMoment += (double) samples * bb;
        kickMoment += (double) samples * kk;
        ++samples;
    }

    DynamicRuntimeFingerprint finish() const noexcept
    {
        using namespace DynamicFingerprintContract;
        DynamicRuntimeFingerprint result;
        result.extractorVersion = kExtractorVersion;
        result.featureCount = 0;

        if (! supported)
        {
            result.validity = DynamicFingerprintValidity::InvalidSampleRate;
            return result;
        }
        if (! complete())
        {
            result.validity = DynamicFingerprintValidity::Incomplete;
            return result;
        }
        if (sawNonFinite)
        {
            result.validity = DynamicFingerprintValidity::NonFiniteInput;
            return result;
        }

        const double totalBass = bassEnergy[0] + bassEnergy[1] + bassEnergy[2];
        const double totalKick = kickEnergy[0] + kickEnergy[1] + kickEnergy[2];
        const double totalCross = cross[0] + cross[1] + cross[2];
        const double n = (double) window.windowSamples;
        const double kickRms = std::sqrt (totalKick / n);
        const double bassRms = std::sqrt (totalBass / n);

        if (kickRms < kMinKickRms)
        {
            result.validity = DynamicFingerprintValidity::InsufficientKick;
            return result;
        }
        if (bassRms < kMinBassRms)
        {
            result.validity = DynamicFingerprintValidity::InsufficientBass;
            return result;
        }

        std::array<float, kFeatureCount> f {};
        for (int r = 0; r < 3; ++r)
            f[(size_t) r] = regionalCorrelation (bassEnergy[(size_t) r], kickEnergy[(size_t) r],
                                                 cross[(size_t) r]);
        f[3] = regionalCorrelation (totalBass, totalKick, totalCross);
        f[4] = (float) std::clamp (std::log2 ((totalBass + kEpsilon) / (totalKick + kEpsilon)) / 6.0,
                                   -1.0, 1.0);
        f[5] = energyShare (kickEnergy[0], totalKick);
        f[6] = energyShare (bassEnergy[0], totalBass);

        const double bassCentroid = totalBass > kEpsilon
            ? (bassMoment / totalBass) / std::max (1.0, n - 1.0) : 0.0;
        const double kickCentroid = totalKick > kEpsilon
            ? (kickMoment / totalKick) / std::max (1.0, n - 1.0) : 0.0;
        f[7] = (float) std::clamp (bassCentroid - kickCentroid, -1.0, 1.0);

        for (int i = 0; i < kFeatureCount; ++i)
        {
            if (! std::isfinite (f[(size_t) i]))
            {
                result.validity = DynamicFingerprintValidity::NonFiniteInput;
                return result;
            }
            f[(size_t) i] = std::clamp (f[(size_t) i], -1.0f, 1.0f);
        }

        result.features = f;
        result.featureCount = (uint8_t) kFeatureCount;
        result.validity = DynamicFingerprintValidity::Valid;
        return result;
    }

private:
    static float regionalCorrelation (double bassEnergyR, double kickEnergyR, double crossR) noexcept
    {
        const double norm = std::sqrt (std::max (0.0, bassEnergyR * kickEnergyR));
        if (norm <= DynamicFingerprintContract::kEpsilon)
            return 0.0f;
        return (float) std::clamp (crossR / norm, -1.0, 1.0);
    }

    static float energyShare (double attackEnergy, double totalEnergy) noexcept
    {
        const double share = 2.0 * attackEnergy / (totalEnergy + DynamicFingerprintContract::kEpsilon) - 1.0;
        return (float) std::clamp (share, -1.0, 1.0);
    }

    DynamicFingerprintWindow window;
    bool supported = false;
    int samples = 0;
    bool sawNonFinite = false;
    std::array<double, 3> bassEnergy {};
    std::array<double, 3> kickEnergy {};
    std::array<double, 3> cross {};
    double bassMoment = 0.0;
    double kickMoment = 0.0;
};

// One completed capture: its absolute timestamps plus the extracted fingerprint.
struct DynamicFingerprintObservation
{
    int64_t triggerSample = 0;
    int64_t readySample = 0; // logical: triggerSample + windowSamples
    DynamicRuntimeFingerprint fingerprint;
};

// Result of requesting a new capture slot.
enum class DynamicCaptureRequest : uint8_t
{
    Accepted = 0,
    Exhausted,     // all four slots are already active
    Rejected       // invalid sample rate, trigger in the past, or would overflow
};

// -----------------------------------------------------------------------------
// Streaming capture bank.
//
// One shared, continuously running front end feeds exactly four fixed capture
// slots. No vector, no allocation, no mutex. An active capture is never
// overwritten; a fifth overlapping trigger is explicitly Exhausted. Absolute
// sample arithmetic is int64 and overflow-checked.
// -----------------------------------------------------------------------------
class DynamicFingerprintCaptureBank
{
public:
    void prepare (double newSampleRate) noexcept
    {
        supported = DynamicFingerprintContract::isSupportedSampleRate (newSampleRate);
        sampleRate = supported ? newSampleRate : 0.0;
        frontEnd.prepare (newSampleRate);
        window = DynamicFingerprintWindow::forSampleRate (supported ? sampleRate : 48000.0);
        reset();
    }

    void reset() noexcept
    {
        frontEnd.reset();
        currentSample = 0;
        for (auto& slot : slots)
        {
            slot.active = false;
            slot.triggerSample = 0;
            slot.readySample = 0;
            slot.accumulator.configure (window, supported);
        }
        completedCount = 0;
        completedHead = 0;
    }

    int64_t nextSample() const noexcept { return currentSample; }
    const DynamicFingerprintWindow& windowLayout() const noexcept { return window; }
    int activeCaptureCount() const noexcept
    {
        int count = 0;
        for (const auto& slot : slots)
            count += slot.active ? 1 : 0;
        return count;
    }

    // Requests a capture whose first sample is the sample at `triggerSample`.
    // The trigger must be at or after the next streamed sample; a trigger in the
    // past cannot be captured without replaying the stream from reset.
    DynamicCaptureRequest requestCapture (int64_t triggerSample) noexcept
    {
        if (! supported)
            return DynamicCaptureRequest::Rejected;
        if (triggerSample < currentSample)
            return DynamicCaptureRequest::Rejected;
        if (triggerSample > std::numeric_limits<int64_t>::max() - (int64_t) window.windowSamples)
            return DynamicCaptureRequest::Rejected;

        for (auto& slot : slots)
        {
            if (slot.active)
                continue;
            slot.active = true;
            slot.triggerSample = triggerSample;
            slot.readySample = triggerSample + (int64_t) window.windowSamples;
            slot.accumulator.configure (window, supported);
            return DynamicCaptureRequest::Accepted;
        }
        return DynamicCaptureRequest::Exhausted;
    }

    // Streams one raw sample pair through the shared front end and distributes
    // the filtered result to every active capture that covers this sample.
    void pushSample (float rawBass, float rawKick) noexcept
    {
        double filteredBass = 0.0;
        double filteredKick = 0.0;
        const bool rawFinite = std::isfinite (rawBass) && std::isfinite (rawKick);
        frontEnd.processSample (rawBass, rawKick, filteredBass, filteredKick);

        for (auto& slot : slots)
        {
            if (! slot.active)
                continue;
            if (currentSample < slot.triggerSample || currentSample >= slot.readySample)
                continue;

            slot.accumulator.pushFiltered (filteredBass, filteredKick, rawFinite);
            if (slot.accumulator.complete())
            {
                DynamicFingerprintObservation observation;
                observation.triggerSample = slot.triggerSample;
                observation.readySample = slot.readySample;
                observation.fingerprint = slot.accumulator.finish();
                pushCompleted (observation);
                slot.active = false;
            }
        }

        if (currentSample < std::numeric_limits<int64_t>::max())
            ++currentSample;
    }

    // Pops the oldest completed observation in deterministic completion order.
    bool takeCompleted (DynamicFingerprintObservation& out) noexcept
    {
        if (completedCount == 0)
            return false;
        out = completed[(size_t) completedHead];
        completedHead = (completedHead + 1) % kCapacity;
        --completedCount;
        return true;
    }

    bool hasCompleted() const noexcept { return completedCount > 0; }

private:
    static constexpr int kCapacity = DynamicFingerprintContract::kMaxConcurrentDynamicFingerprintCaptures;

    struct Slot
    {
        bool active = false;
        int64_t triggerSample = 0;
        int64_t readySample = 0;
        DynamicFingerprintAccumulator accumulator;
    };

    void pushCompleted (const DynamicFingerprintObservation& observation) noexcept
    {
        if (completedCount >= kCapacity)
            return; // capacity bounded by concurrent captures; never fabricate loss
        const int tail = (completedHead + completedCount) % kCapacity;
        completed[(size_t) tail] = observation;
        ++completedCount;
    }

    double sampleRate = 0.0;
    bool supported = false;
    DynamicFingerprintFrontEnd frontEnd;
    DynamicFingerprintWindow window;
    std::array<Slot, kCapacity> slots;
    std::array<DynamicFingerprintObservation, kCapacity> completed {};
    int completedCount = 0;
    int completedHead = 0;
    int64_t currentSample = 0;
};

// -----------------------------------------------------------------------------
// Offline / test helper. Replays the entire buffer through the SAME capture bank
// from a single reset point, so it is train/serve identical to streaming by
// construction. Returns whether a completed observation was produced.
// -----------------------------------------------------------------------------
inline bool extractDynamicFingerprintOffline (const float* bass, const float* kick,
                                              int numSamples, double sampleRate,
                                              int64_t triggerSample,
                                              DynamicFingerprintObservation& out) noexcept
{
    DynamicFingerprintCaptureBank bank;
    bank.prepare (sampleRate);
    if (bass == nullptr || kick == nullptr || numSamples <= 0)
        return false;
    if (bank.requestCapture (triggerSample) != DynamicCaptureRequest::Accepted)
        return false;

    for (int i = 0; i < numSamples; ++i)
        bank.pushSample (bass[i], kick[i]);
    return bank.takeCompleted (out);
}
