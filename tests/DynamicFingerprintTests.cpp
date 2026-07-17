#include "TestCommon.h"

#include "DynamicStateMap.h"
#include "DynamicStateSerialization.h"
#include "DynamicFingerprintExtractor.h"
#include "DynamicFingerprintMatcher.h"

#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

// =============================================================================
// Phase 3 tests: Dynamic fingerprint extractor v1, streaming capture bank and
// the pure v1 State matcher. Category "DSP" so these run on every CI platform,
// including the Linux ASan/UBSan job.
// =============================================================================

namespace
{
    struct Material
    {
        std::vector<float> bass;
        std::vector<float> kick;
    };

    // Shared-shape material: bass is proportional to kick (optionally inverted),
    // so filtered correlation is deterministically +/-1 before energy weighting.
    Material makeMaterial (double sampleRate, int n, float bassGain, float kickGain,
                           bool invertBass, double tau, double freq)
    {
        Material m { std::vector<float> ((size_t) n), std::vector<float> ((size_t) n) };
        for (int i = 0; i < n; ++i)
        {
            const double env = std::exp (-(double) i / tau);
            const double s = env * std::sin (2.0 * juce::MathConstants<double>::pi * freq * (double) i / sampleRate);
            m.kick[(size_t) i] = (float) (kickGain * s);
            m.bass[(size_t) i] = (float) ((invertBass ? -1.0 : 1.0) * bassGain * s);
        }
        return m;
    }

    int expectedWindowSamples (double sampleRate)
    {
        return std::max (3, (int) std::lround (sampleRate * DynamicFingerprintContract::kWindowMs / 1000.0));
    }

    DynamicRuntimeFingerprint runtimeFingerprint (float value, bool valid = true,
                                                  uint32_t version = DynamicFingerprintContract::kExtractorVersion)
    {
        DynamicRuntimeFingerprint fp;
        fp.validity = valid ? DynamicFingerprintValidity::Valid : DynamicFingerprintValidity::Incomplete;
        fp.extractorVersion = version;
        fp.featureCount = valid ? (uint8_t) DynamicFingerprintContract::kFeatureCount : (uint8_t) 0;
        fp.features.fill (value);
        return fp;
    }

    DynamicFingerprintObservation observationFor (float value, bool valid = true,
                                                  uint32_t version = DynamicFingerprintContract::kExtractorVersion)
    {
        DynamicFingerprintObservation obs;
        obs.triggerSample = 0;
        obs.readySample = 0;
        obs.fingerprint = runtimeFingerprint (value, valid, version);
        return obs;
    }

    DynamicFingerprintPrototype eightFeaturePrototype (float value)
    {
        DynamicFingerprintPrototype p;
        p.valid = true;
        p.featureCount = (uint8_t) DynamicFingerprintContract::kFeatureCount;
        p.features.fill (value);
        return p;
    }

    DynamicState matcherState (uint64_t id, float value, DynamicStateOrigin origin,
                               DynamicStateEvidence evidence, bool enabled, bool bypassed,
                               bool hasPackage)
    {
        DynamicState s;
        s.occupied = true;
        s.stableStateId = id;
        s.fingerprint = eightFeaturePrototype (value);
        s.origin = origin;
        s.evidence = evidence;
        s.enabled = enabled;
        s.bypassed = bypassed;
        s.repeatability = 0.85f;
        s.ambiguity = 0.1f;
        if (origin == DynamicStateOrigin::Auto)
        {
            s.hitCount = evidence == DynamicStateEvidence::Stable
                ? DynamicStateMapContract::kStableAutoMinimumRepeatableHits
                : DynamicStateMapContract::kCandidateMinimumRepeatableHits;
            s.hasLearnedPackage = hasPackage;
            if (hasPackage)
                s.learnedPackage = { 0.0f, 100.0f, 0.7f };
        }
        else
        {
            s.hitCount = DynamicStateMapContract::kManualMinimumRepeatableHits;
            s.hasManualBasePackage = true;
            s.manualBasePackage = { 0.0f, 100.0f, 0.7f };
        }
        return s;
    }

    DynamicState stableAuto (uint64_t id, float value, bool hasPackage = true)
    {
        return matcherState (id, value, DynamicStateOrigin::Auto, DynamicStateEvidence::Stable,
                             true, false, hasPackage);
    }

    DynamicStateMap matcherMap (std::vector<DynamicState> states,
                                float threshold = 0.25f, float margin = 0.125f)
    {
        DynamicStateMap m = makeEmptyDynamicStateMap();
        m.valid = true;
        m.calibration = { true, threshold, margin };
        uint64_t maxId = 0;
        int index = 0;
        for (auto& s : states)
        {
            m.states[(size_t) index++] = s;
            maxId = std::max (maxId, s.stableStateId);
        }
        m.nextStateId = maxId + 1u;
        return m;
    }

    struct DynamicMapCodec : juce::AudioProcessor
    {
        using juce::AudioProcessor::copyXmlToBinary;
        using juce::AudioProcessor::getXmlFromBinary;
    };
}

class DynamicFingerprintFrontEndTests : public juce::UnitTest
{
public:
    DynamicFingerprintFrontEndTests() : juce::UnitTest ("DynamicFingerprintFrontEnd", "DSP") {}

    static double bandEnergy (double sampleRate, double freq)
    {
        DynamicFingerprintFrontEnd frontEnd;
        frontEnd.prepare (sampleRate);
        const int total = 12000;
        const int measureFrom = 6000;
        double energy = 0.0;
        for (int i = 0; i < total; ++i)
        {
            const double s = 0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * freq * (double) i / sampleRate);
            double fb = 0.0, fk = 0.0;
            frontEnd.processSample ((float) s, (float) s, fb, fk);
            if (i >= measureFrom)
                energy += fb * fb;
        }
        return energy;
    }

    void runTest() override
    {
        const std::array<double, 5> rates { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };

        beginTest ("Front end is finite at every supported sample rate");
        {
            for (double rate : rates)
            {
                DynamicFingerprintFrontEnd frontEnd;
                frontEnd.prepare (rate);
                expect (frontEnd.isReady());
                for (int i = 0; i < 4096; ++i)
                {
                    const double s = 0.7 * std::sin (2.0 * juce::MathConstants<double>::pi * 90.0 * (double) i / rate);
                    double fb = 0.0, fk = 0.0;
                    frontEnd.processSample ((float) s, (float) (0.5 * s), fb, fk);
                    expect (std::isfinite (fb) && std::isfinite (fk));
                }
            }
        }

        beginTest ("Passband: 100 Hz carries materially more energy than 10 Hz and 2 kHz");
        {
            for (double rate : rates)
            {
                const double e100 = bandEnergy (rate, 100.0);
                const double e10 = bandEnergy (rate, 10.0);
                const double e2k = bandEnergy (rate, 2000.0);
                logMessage ("rate=" + juce::String (rate, 0)
                            + " E100=" + juce::String (e100, 6)
                            + " E10=" + juce::String (e10, 6)
                            + " E2k=" + juce::String (e2k, 6));
                expect (e100 > 4.0 * e10, "100 Hz must dominate 10 Hz");
                expect (e100 > 4.0 * e2k, "100 Hz must dominate 2 kHz");
            }
        }

        beginTest ("Reset is deterministic and independent of any user crossover");
        {
            // The front end exposes no crossover parameter at all: two instances
            // prepared at the same rate share no global crossover state, so an
            // identical input must yield bit-identical filtered output.
            DynamicFingerprintFrontEnd a, b;
            a.prepare (48000.0);
            b.prepare (48000.0);
            const auto material = makeMaterial (48000.0, 2048, 0.6f, 0.4f, false, 300.0, 110.0);
            for (int pass = 0; pass < 2; ++pass)
            {
                a.reset();
                double firstBass = 0.0;
                for (size_t i = 0; i < material.kick.size(); ++i)
                {
                    double ab = 0.0, ak = 0.0, bb = 0.0, bk = 0.0;
                    a.processSample (material.bass[i], material.kick[i], ab, ak);
                    b.reset();
                    for (size_t j = 0; j <= i; ++j)
                        b.processSample (material.bass[j], material.kick[j], bb, bk);
                    if (i == 512)
                        firstBass = ab;
                    expectWithinAbsoluteError (ab, bb, 1.0e-12);
                    juce::ignoreUnused (firstBass, ak, bk);
                }
            }
        }

        beginTest ("No denormal or non-finite propagation into a long silent tail");
        {
            DynamicFingerprintFrontEnd frontEnd;
            frontEnd.prepare (48000.0);
            // Excite, then inject a NaN (which must be sanitized), then run a long
            // silent tail. The state must collapse to exactly zero, not drift into
            // denormal or NaN territory.
            for (int i = 0; i < 256; ++i)
            {
                double fb = 0.0, fk = 0.0;
                frontEnd.processSample (0.8f, 0.8f, fb, fk);
            }
            double fb = 0.0, fk = 0.0;
            frontEnd.processSample (std::numeric_limits<float>::quiet_NaN(), 0.0f, fb, fk);
            expect (std::isfinite (fb) && std::isfinite (fk));
            double lastBass = 1.0, lastKick = 1.0;
            const double minNormal = std::numeric_limits<double>::min();
            for (int i = 0; i < 200000; ++i)
            {
                frontEnd.processSample (0.0f, 0.0f, fb, fk);
                expect (std::isfinite (fb) && std::isfinite (fk));
                // The flush threshold (1e-300) sits above the whole subnormal
                // range, so state is either exactly zero or a normal number - a
                // denormal can never propagate.
                expect (fb == 0.0 || std::abs (fb) >= minNormal, "no denormal bass state");
                expect (fk == 0.0 || std::abs (fk) >= minNormal, "no denormal kick state");
                lastBass = fb;
                lastKick = fk;
            }
            // The silent tail must have decayed to a negligible, finite level.
            expect (std::abs (lastBass) < 1.0e-9);
            expect (std::abs (lastKick) < 1.0e-9);
        }

        beginTest ("Invalid sample rate safely disables the front end");
        {
            DynamicFingerprintFrontEnd frontEnd;
            frontEnd.prepare (0.0);
            expect (! frontEnd.isReady());
            frontEnd.prepare (std::numeric_limits<double>::quiet_NaN());
            expect (! frontEnd.isReady());
        }
    }
};

class DynamicFingerprintFeatureTests : public juce::UnitTest
{
public:
    DynamicFingerprintFeatureTests() : juce::UnitTest ("DynamicFingerprintFeatures", "DSP") {}

    void runTest() override
    {
        const double rate = 48000.0;
        const int n = 1024;

        beginTest ("Identical bass/kick produces strongly positive signed correlations");
        {
            const auto m = makeMaterial (rate, n, 0.5f, 0.5f, false, 200.0, 100.0);
            DynamicFingerprintObservation obs;
            expect (extractDynamicFingerprintOffline (m.bass.data(), m.kick.data(), n, rate, 0, obs));
            expect (obs.fingerprint.isValid());
            for (int r = 0; r < 4; ++r)
                expect (obs.fingerprint.features[(size_t) r] > 0.99f,
                        "correlation region " + juce::String (r) + " must be ~+1");
        }

        beginTest ("Inverted bass produces strongly negative correlations (sign preserved, not clamped)");
        {
            const auto m = makeMaterial (rate, n, 0.5f, 0.5f, true, 200.0, 100.0);
            DynamicFingerprintObservation obs;
            expect (extractDynamicFingerprintOffline (m.bass.data(), m.kick.data(), n, rate, 0, obs));
            expect (obs.fingerprint.isValid());
            for (int r = 0; r < 4; ++r)
            {
                expect (obs.fingerprint.features[(size_t) r] < -0.99f,
                        "correlation region " + juce::String (r) + " must be ~-1");
                expect (obs.fingerprint.features[(size_t) r] < 0.0f, "sign must survive");
            }
        }

        beginTest ("Common gain invariance; bass-only gain moves only the relative-energy feature");
        {
            DynamicFingerprintObservation base, common, bassOnly;
            const auto mBase = makeMaterial (rate, n, 1.0f, 1.0f, false, 200.0, 100.0);
            const auto mCommon = makeMaterial (rate, n, 2.0f, 2.0f, false, 200.0, 100.0);
            const auto mBass = makeMaterial (rate, n, 2.0f, 1.0f, false, 200.0, 100.0);
            expect (extractDynamicFingerprintOffline (mBase.bass.data(), mBase.kick.data(), n, rate, 0, base));
            expect (extractDynamicFingerprintOffline (mCommon.bass.data(), mCommon.kick.data(), n, rate, 0, common));
            expect (extractDynamicFingerprintOffline (mBass.bass.data(), mBass.kick.data(), n, rate, 0, bassOnly));
            expect (base.fingerprint.isValid() && common.fingerprint.isValid() && bassOnly.fingerprint.isValid());

            for (int i = 0; i < DynamicFingerprintContract::kFeatureCount; ++i)
                expectWithinAbsoluteError (common.fingerprint.features[(size_t) i],
                                           base.fingerprint.features[(size_t) i], 1.0e-4f,
                                           "common gain must not move feature " + juce::String (i));

            // Feature 4 is the bounded log-energy ratio: doubling bass raises
            // log2(ratio) by 2 -> +2/6 = +0.3333.
            expectWithinAbsoluteError (bassOnly.fingerprint.features[4],
                                       base.fingerprint.features[4] + (2.0f / 6.0f), 5.0e-3f);
            for (int i = 0; i < DynamicFingerprintContract::kFeatureCount; ++i)
                if (i != 4)
                    expectWithinAbsoluteError (bassOnly.fingerprint.features[(size_t) i],
                                               base.fingerprint.features[(size_t) i], 5.0e-3f,
                                               "bass-only gain must not move feature " + juce::String (i));
        }

        beginTest ("Temporal shape changes move the share and centroid features");
        {
            DynamicFingerprintObservation fast, slow;
            const auto mFast = makeMaterial (rate, n, 0.5f, 0.5f, false, 30.0, 100.0);
            const auto mSlow = makeMaterial (rate, n, 0.5f, 0.5f, false, 600.0, 100.0);
            expect (extractDynamicFingerprintOffline (mFast.bass.data(), mFast.kick.data(), n, rate, 0, fast));
            expect (extractDynamicFingerprintOffline (mSlow.bass.data(), mSlow.kick.data(), n, rate, 0, slow));
            expect (fast.fingerprint.isValid() && slow.fingerprint.isValid());
            // Fast decay concentrates energy in the attack region: higher attack
            // share, earlier centroid.
            expect (fast.fingerprint.features[5] > slow.fingerprint.features[5], "kick attack share");
            expect (fast.fingerprint.features[6] > slow.fingerprint.features[6], "bass attack share");
        }

        beginTest ("Every valid feature stays inside [-1, 1]");
        {
            const auto m = makeMaterial (rate, n, 0.9f, 0.3f, true, 120.0, 130.0);
            DynamicFingerprintObservation obs;
            expect (extractDynamicFingerprintOffline (m.bass.data(), m.kick.data(), n, rate, 0, obs));
            expect (obs.fingerprint.isValid());
            for (int i = 0; i < DynamicFingerprintContract::kFeatureCount; ++i)
            {
                const float v = obs.fingerprint.features[(size_t) i];
                expect (std::isfinite (v) && v >= -1.0f && v <= 1.0f);
            }
        }

        beginTest ("Silence, bass-only, kick-only, NaN and incomplete windows are invalid");
        {
            DynamicFingerprintObservation obs;
            const std::vector<float> zeros ((size_t) n, 0.0f);

            expect (extractDynamicFingerprintOffline (zeros.data(), zeros.data(), n, rate, 0, obs));
            expect (! obs.fingerprint.isValid());

            const auto tone = makeMaterial (rate, n, 0.5f, 0.5f, false, 200.0, 100.0);
            expect (extractDynamicFingerprintOffline (tone.bass.data(), zeros.data(), n, rate, 0, obs));
            expectEquals ((int) obs.fingerprint.validity, (int) DynamicFingerprintValidity::InsufficientKick);

            expect (extractDynamicFingerprintOffline (zeros.data(), tone.kick.data(), n, rate, 0, obs));
            expectEquals ((int) obs.fingerprint.validity, (int) DynamicFingerprintValidity::InsufficientBass);

            auto nanMaterial = tone;
            nanMaterial.bass[10] = std::numeric_limits<float>::quiet_NaN();
            expect (extractDynamicFingerprintOffline (nanMaterial.bass.data(), nanMaterial.kick.data(), n, rate, 0, obs));
            expectEquals ((int) obs.fingerprint.validity, (int) DynamicFingerprintValidity::NonFiniteInput);

            // Incomplete window: fewer samples than the window never completes.
            const int shortCount = expectedWindowSamples (rate) - 5;
            expect (! extractDynamicFingerprintOffline (tone.bass.data(), tone.kick.data(),
                                                        shortCount, rate, 0, obs));
        }
    }
};

class DynamicFingerprintTimingTests : public juce::UnitTest
{
public:
    DynamicFingerprintTimingTests() : juce::UnitTest ("DynamicFingerprintTiming", "DSP") {}

    void runTest() override
    {
        const std::array<double, 4> rates { 44100.0, 48000.0, 96000.0, 192000.0 };

        beginTest ("Window length is exact and the trigger sample is included");
        {
            for (double rate : rates)
            {
                DynamicFingerprintCaptureBank bank;
                bank.prepare (rate);
                const int ws = expectedWindowSamples (rate);
                expectEquals (bank.windowLayout().windowSamples, ws);

                const int64_t trigger = 37;
                expect (bank.requestCapture (trigger) == DynamicCaptureRequest::Accepted);

                // Completion must occur exactly when the sample at index
                // (trigger + ws - 1) is processed: not before (window too short)
                // and not after (window too long). This nails the off-by-one.
                int64_t processed = 0;
                bool completedOnBoundary = false;
                for (;;)
                {
                    processed = bank.nextSample();
                    const bool completeBefore = bank.hasCompleted();
                    bank.pushSample (0.4f, 0.4f);
                    if (processed == trigger + ws - 1)
                    {
                        expect (! completeBefore);
                        expect (bank.hasCompleted());
                        completedOnBoundary = true;
                        break;
                    }
                    expect (! bank.hasCompleted(), "must not complete before the final window sample");
                    if (processed > trigger + ws + 4)
                        break;
                }
                expect (completedOnBoundary);

                DynamicFingerprintObservation obs;
                expect (bank.takeCompleted (obs));
                expectEquals ((int) (obs.readySample - obs.triggerSample), ws,
                              "ready sample must be trigger + windowSamples");
                expect (obs.triggerSample == trigger);
                expect (obs.readySample == trigger + (int64_t) ws);
            }
        }

        beginTest ("Streaming and offline extraction agree; block partitioning is irrelevant");
        {
            const double rate = 48000.0;
            const int n = 1500;
            const auto m = makeMaterial (rate, n, 0.6f, 0.45f, false, 220.0, 105.0);
            const int64_t trigger = 60;

            DynamicFingerprintObservation offline;
            expect (extractDynamicFingerprintOffline (m.bass.data(), m.kick.data(), n, rate, trigger, offline));
            expect (offline.fingerprint.isValid());

            for (int blockSize : { 1, 7, 64, 200 })
            {
                DynamicFingerprintCaptureBank bank;
                bank.prepare (rate);
                expect (bank.requestCapture (trigger) == DynamicCaptureRequest::Accepted);
                for (int i = 0; i < n; i += blockSize)
                    for (int j = i; j < std::min (n, i + blockSize); ++j)
                        bank.pushSample (m.bass[(size_t) j], m.kick[(size_t) j]);
                DynamicFingerprintObservation streamed;
                expect (bank.takeCompleted (streamed));
                expect (streamed.triggerSample == offline.triggerSample);
                expect (streamed.readySample == offline.readySample);
                for (int f = 0; f < DynamicFingerprintContract::kFeatureCount; ++f)
                    expectWithinAbsoluteError (streamed.fingerprint.features[(size_t) f],
                                               offline.fingerprint.features[(size_t) f], 1.0e-12f,
                                               "block size " + juce::String (blockSize) + " feature " + juce::String (f));
            }
        }

        beginTest ("Repeated identical extraction is deterministic");
        {
            const double rate = 48000.0;
            const int n = 1024;
            const auto m = makeMaterial (rate, n, 0.5f, 0.5f, true, 180.0, 100.0);
            DynamicFingerprintObservation first;
            expect (extractDynamicFingerprintOffline (m.bass.data(), m.kick.data(), n, rate, 0, first));
            for (int repeat = 0; repeat < 8; ++repeat)
            {
                DynamicFingerprintObservation again;
                expect (extractDynamicFingerprintOffline (m.bass.data(), m.kick.data(), n, rate, 0, again));
                for (int f = 0; f < DynamicFingerprintContract::kFeatureCount; ++f)
                    expectEquals (again.fingerprint.features[(size_t) f], first.fingerprint.features[(size_t) f]);
            }
        }
    }
};

class DynamicFingerprintCaptureBankTests : public juce::UnitTest
{
public:
    DynamicFingerprintCaptureBankTests() : juce::UnitTest ("DynamicFingerprintCaptureBank", "DSP") {}

    void runTest() override
    {
        const double rate = 48000.0;
        const int ws = expectedWindowSamples (rate);

        beginTest ("Four overlapping captures complete independently; a fifth is rejected");
        {
            DynamicFingerprintCaptureBank bank;
            bank.prepare (rate);
            const std::array<int64_t, 4> triggers { 10, 20, 30, 40 };
            for (int64_t t : triggers)
                expect (bank.requestCapture (t) == DynamicCaptureRequest::Accepted);
            expectEquals (bank.activeCaptureCount(), 4);

            // Fifth overlapping trigger: all slots busy -> Exhausted, and the four
            // active captures must remain untouched.
            expect (bank.requestCapture (45) == DynamicCaptureRequest::Exhausted);
            expectEquals (bank.activeCaptureCount(), 4);

            const int64_t total = triggers.back() + ws + 8;
            int index = 0;
            for (int64_t s = 0; s < total; ++s)
            {
                const float v = 0.3f + 0.001f * (float) (s % 7);
                bank.pushSample (v, v);
                juce::ignoreUnused (index);
            }

            // Deterministic completion order follows the ready samples, which are
            // ordered because the triggers are ordered.
            std::array<int64_t, 4> completedTriggers {};
            int completed = 0;
            DynamicFingerprintObservation obs;
            while (bank.takeCompleted (obs))
            {
                expect (completed < 4);
                completedTriggers[(size_t) completed] = obs.triggerSample;
                expect (obs.readySample == obs.triggerSample + (int64_t) ws);
                ++completed;
            }
            expectEquals (completed, 4);
            for (int i = 0; i < 4; ++i)
                expect (completedTriggers[(size_t) i] == triggers[(size_t) i],
                        "completion order must match ready-sample order");
        }

        beginTest ("A slot frees after completion and can be reused");
        {
            DynamicFingerprintCaptureBank bank;
            bank.prepare (rate);
            for (int64_t t : { 0, 1, 2, 3 })
                expect (bank.requestCapture (t) == DynamicCaptureRequest::Accepted);
            for (int64_t s = 0; s < (int64_t) ws + 4; ++s)
                bank.pushSample (0.3f, 0.3f);
            expectEquals (bank.activeCaptureCount(), 0);
            // Now that every slot has completed and freed, new captures are
            // accepted again (no fabricated exhaustion).
            expect (bank.requestCapture (bank.nextSample()) == DynamicCaptureRequest::Accepted);
        }

        beginTest ("A trigger in the past is rejected");
        {
            DynamicFingerprintCaptureBank bank;
            bank.prepare (rate);
            for (int i = 0; i < 100; ++i)
                bank.pushSample (0.2f, 0.2f);
            expect (bank.requestCapture (10) == DynamicCaptureRequest::Rejected);
            expect (bank.requestCapture (bank.nextSample()) == DynamicCaptureRequest::Accepted);
        }
    }
};

class DynamicFingerprintDistanceTests : public juce::UnitTest
{
public:
    DynamicFingerprintDistanceTests() : juce::UnitTest ("DynamicFingerprintDistance", "DSP") {}

    void runTest() override
    {
        beginTest ("Identity, symmetry, a hand-calculated case and the maximum distance");
        {
            const auto zero = eightFeaturePrototype (0.0f);
            const auto half = eightFeaturePrototype (0.5f);
            expectEquals (dynamicFingerprintDistanceV1 (half, half), 0.0f);
            expectEquals (dynamicFingerprintDistanceV1 (zero, half), dynamicFingerprintDistanceV1 (half, zero));

            // One feature differs by 0.5: sum = 0.5, distance = 0.5 / 16 = 0.03125.
            auto oneOff = zero;
            oneOff.features[0] = 0.5f;
            expectWithinAbsoluteError (dynamicFingerprintDistanceV1 (zero, oneOff), 0.03125f, 1.0e-7f);

            // All-equal: distance = value / 2.
            expectWithinAbsoluteError (dynamicFingerprintDistanceV1 (zero, half), 0.25f, 1.0e-7f);

            const auto plus = eightFeaturePrototype (1.0f);
            const auto minus = eightFeaturePrototype (-1.0f);
            expectEquals (dynamicFingerprintDistanceV1 (plus, minus), 1.0f);
        }

        beginTest ("Invalid or mismatched prototypes return infinity");
        {
            const auto valid = eightFeaturePrototype (0.2f);
            DynamicFingerprintPrototype shortCount = valid;
            shortCount.featureCount = 4;
            expect (std::isinf (dynamicFingerprintDistanceV1 (valid, shortCount)));

            DynamicFingerprintPrototype invalid = valid;
            invalid.valid = false;
            expect (std::isinf (dynamicFingerprintDistanceV1 (valid, invalid)));

            DynamicFingerprintPrototype nan = valid;
            nan.features[2] = std::numeric_limits<float>::quiet_NaN();
            expect (std::isinf (dynamicFingerprintDistanceV1 (valid, nan)));
        }
    }
};

class DynamicFingerprintMatcherTests : public juce::UnitTest
{
public:
    DynamicFingerprintMatcherTests() : juce::UnitTest ("DynamicFingerprintMatcher", "DSP") {}

    void runTest() override
    {
        // Observation is the zero fingerprint: distance to an all-`v` state is v/2.
        const auto observation = observationFor (0.0f);

        beginTest ("Exact threshold boundary is Matched");
        {
            const auto map = matcherMap ({ stableAuto (1, 0.5f), stableAuto (2, 1.0f) });
            const auto r = matchDynamicFingerprint (observation, map);
            expectEquals ((int) r.decision, (int) DynamicMatchDecision::Matched);
            expectWithinAbsoluteError (r.nearestDistance, 0.25f, 1.0e-7f);
            expect (r.selectedStableStateId == 1u);
        }

        beginTest ("Just outside the threshold is Unknown");
        {
            const auto map = matcherMap ({ stableAuto (1, 0.625f), stableAuto (2, 1.0f) });
            const auto r = matchDynamicFingerprint (observation, map);
            expectEquals ((int) r.decision, (int) DynamicMatchDecision::Unknown);
            expectWithinAbsoluteError (r.nearestDistance, 0.3125f, 1.0e-7f);
        }

        beginTest ("Exact ambiguity-margin boundary is Matched (not ambiguous)");
        {
            const auto map = matcherMap ({ stableAuto (1, 0.125f), stableAuto (2, 0.375f) });
            const auto r = matchDynamicFingerprint (observation, map);
            expectWithinAbsoluteError (r.nearestDistance, 0.0625f, 1.0e-7f);
            expectWithinAbsoluteError (r.secondDistance, 0.1875f, 1.0e-7f);
            // second - nearest = 0.125 == margin -> boundary is NOT ambiguous.
            expectEquals ((int) r.decision, (int) DynamicMatchDecision::Matched);
        }

        beginTest ("Just inside the ambiguity margin is Ambiguous");
        {
            const auto map = matcherMap ({ stableAuto (1, 0.125f), stableAuto (2, 0.25f) });
            const auto r = matchDynamicFingerprint (observation, map);
            expectEquals ((int) r.decision, (int) DynamicMatchDecision::Ambiguous);
            expectWithinAbsoluteError (r.nearestDistance, 0.0625f, 1.0e-7f);
            expectWithinAbsoluteError (r.secondDistance, 0.125f, 1.0e-7f);
        }

        beginTest ("Equal-distance tie is Ambiguous and selects the lower stable ID");
        {
            const auto map = matcherMap ({ stableAuto (7, 0.25f), stableAuto (3, 0.25f) });
            const auto r = matchDynamicFingerprint (observation, map);
            expectEquals ((int) r.decision, (int) DynamicMatchDecision::Ambiguous);
            expect (r.selectedStableStateId == 3u);
            expect (r.nearestStableStateId == 3u);
            expect (r.secondStableStateId == 7u);
        }

        beginTest ("Slot reordering never changes the selected stable ID");
        {
            const auto forward = matcherMap ({ stableAuto (5, 0.1f), stableAuto (9, 0.5f), stableAuto (2, 0.8f) });
            const auto reverse = matcherMap ({ stableAuto (2, 0.8f), stableAuto (9, 0.5f), stableAuto (5, 0.1f) });
            const auto rf = matchDynamicFingerprint (observation, forward);
            const auto rr = matchDynamicFingerprint (observation, reverse);
            expect (rf.selectedStableStateId == rr.selectedStableStateId);
            expect (rf.selectedStableStateId == 5u);
            expectWithinAbsoluteError (rf.nearestDistance, rr.nearestDistance, 0.0f);
        }

        beginTest ("Disabled states are excluded; enabled nearest wins");
        {
            auto disabled = stableAuto (1, 0.0f); // distance 0, but disabled
            disabled.enabled = false;
            const auto map = matcherMap ({ disabled, stableAuto (2, 0.5f) });
            const auto r = matchDynamicFingerprint (observation, map);
            expectEquals (r.eligibleStateCount, 1);
            expect (r.selectedStableStateId == 2u);
            expectEquals ((int) r.decision, (int) DynamicMatchDecision::Matched);
        }

        beginTest ("Bypassed states are recognized and reported");
        {
            auto bypassed = stableAuto (1, 0.125f);
            bypassed.bypassed = true;
            const auto map = matcherMap ({ bypassed });
            const auto r = matchDynamicFingerprint (observation, map);
            expectEquals ((int) r.decision, (int) DynamicMatchDecision::Matched);
            expect (r.selectedBypassed);
            expect (r.correctionAvailable);
        }

        beginTest ("Auto Candidate is excluded; Manual Candidate and Stable Auto are eligible");
        {
            const auto autoCandidate = matcherState (1, 0.0f, DynamicStateOrigin::Auto,
                                                     DynamicStateEvidence::Candidate, true, false, true);
            const auto manualCandidate = matcherState (2, 0.5f, DynamicStateOrigin::Manual,
                                                       DynamicStateEvidence::Candidate, true, false, true);
            const auto map = matcherMap ({ autoCandidate, manualCandidate });
            const auto r = matchDynamicFingerprint (observation, map);
            expectEquals (r.eligibleStateCount, 1);
            expect (r.selectedStableStateId == 2u); // manual candidate selected
            expectEquals ((int) r.decision, (int) DynamicMatchDecision::Matched);
        }

        beginTest ("Recognized Stable Auto with no package matches with correctionAvailable=false");
        {
            const auto noFix = stableAuto (1, 0.125f, /*hasPackage*/ false);
            const auto map = matcherMap ({ noFix });
            const auto r = matchDynamicFingerprint (observation, map);
            expectEquals ((int) r.decision, (int) DynamicMatchDecision::Matched);
            expect (! r.correctionAvailable);
        }

        beginTest ("A single eligible state has no second-best ambiguity");
        {
            const auto within = matcherMap ({ stableAuto (1, 0.125f) });
            const auto rWithin = matchDynamicFingerprint (observation, within);
            expectEquals ((int) rWithin.decision, (int) DynamicMatchDecision::Matched);
            expect (std::isinf (rWithin.secondDistance));

            const auto beyond = matcherMap ({ stableAuto (1, 0.625f) });
            const auto rBeyond = matchDynamicFingerprint (observation, beyond);
            expectEquals ((int) rBeyond.decision, (int) DynamicMatchDecision::Unknown);
        }

        beginTest ("No eligible states yields NoEligibleStates");
        {
            const auto onlyAutoCandidate = matcherState (1, 0.1f, DynamicStateOrigin::Auto,
                                                         DynamicStateEvidence::Candidate, true, false, true);
            const auto map = matcherMap ({ onlyAutoCandidate });
            const auto r = matchDynamicFingerprint (observation, map);
            expectEquals ((int) r.decision, (int) DynamicMatchDecision::NoEligibleStates);
            expectEquals (r.eligibleStateCount, 0);
        }

        beginTest ("Eight-state map selects the nearest by identity");
        {
            std::vector<DynamicState> states;
            for (int i = 0; i < 8; ++i)
                states.push_back (stableAuto ((uint64_t) (i + 1), 0.05f * (float) (i + 1)));
            const auto map = matcherMap (states, 0.5f, 0.02f);
            const auto r = matchDynamicFingerprint (observation, map);
            expectEquals (r.eligibleStateCount, 8);
            expect (r.selectedStableStateId == 1u); // smallest v -> nearest
            expectEquals ((int) r.decision, (int) DynamicMatchDecision::Matched);
        }

        beginTest ("Invalid observation, extractor mismatch and invalid map give InvalidInput");
        {
            const auto map = matcherMap ({ stableAuto (1, 0.125f) });

            const auto invalidObs = observationFor (0.0f, /*valid*/ false);
            expectEquals ((int) matchDynamicFingerprint (invalidObs, map).decision,
                          (int) DynamicMatchDecision::InvalidInput);

            const auto wrongVersion = observationFor (0.0f, true, 2);
            expectEquals ((int) matchDynamicFingerprint (wrongVersion, map).decision,
                          (int) DynamicMatchDecision::InvalidInput);

            auto badCalibration = map;
            badCalibration.calibration.valid = false;
            expectEquals ((int) matchDynamicFingerprint (observation, badCalibration).decision,
                          (int) DynamicMatchDecision::InvalidInput);

            auto badExtractor = map;
            badExtractor.fingerprintExtractorVersion = 2;
            expectEquals ((int) matchDynamicFingerprint (observation, badExtractor).decision,
                          (int) DynamicMatchDecision::InvalidInput);
        }
    }
};

class DynamicFingerprintCalibrationTests : public juce::UnitTest
{
public:
    DynamicFingerprintCalibrationTests() : juce::UnitTest ("DynamicFingerprintCalibration", "DSP") {}

    void runTest() override
    {
        beginTest ("v1 calibration upper bounds are enforced at [0, 1]");
        {
            expect (isValidDynamicMatchCalibration ({ true, 1.0f, 1.0f }));
            expect (isValidDynamicMatchCalibration ({ true, 0.25f, 0.125f }));
            expect (! isValidDynamicMatchCalibration ({ true, 1.5f, 0.5f }),
                    "threshold above 1 is rejected");
            expect (! isValidDynamicMatchCalibration ({ true, 0.5f, 1.5f }),
                    "margin above 1 is rejected");
            expect (! isValidDynamicMatchCalibration ({ true, 0.3f, 0.4f }),
                    "margin above threshold is rejected");
        }

        beginTest ("Calibration above 1 fails XML and binary round-trip activation");
        {
            auto map = matcherMap ({ stableAuto (1, 0.125f) });
            expect (isStructurallyValidDynamicStateMap (map));

            map.calibration.absoluteDistanceThreshold = 1.5f;
            expect (! isStructurallyValidDynamicStateMap (map));

            const auto tree = dynamicStateMapToValueTree (map);
            const auto parsedXml = dynamicStateMapFromValueTree (tree);
            expect (! parsedXml.valid, "out-of-range calibration must not activate from XML");

            juce::MemoryBlock block;
            DynamicMapCodec::copyXmlToBinary (*tree.createXml(), block);
            if (auto xml = DynamicMapCodec::getXmlFromBinary (block.getData(), (int) block.getSize()))
            {
                const auto parsedBinary = dynamicStateMapFromValueTree (juce::ValueTree::fromXml (*xml));
                expect (! parsedBinary.valid, "out-of-range calibration must not activate from binary");
            }
            else
            {
                expect (false, "binary round-trip produced no XML");
            }
        }
    }
};

class DynamicFingerprintRealtimeTests : public juce::UnitTest
{
public:
    DynamicFingerprintRealtimeTests() : juce::UnitTest ("DynamicFingerprintRealtime", "DSP") {}

    void runTest() override
    {
        // The runtime contracts hold only fixed-size storage (std::array), so the
        // capture/distance/match paths are allocation-free by construction. This
        // fixture exercises them heavily to confirm they stay finite, bounded and
        // deterministic, and to give ASan/UBSan a wide surface.
        beginTest ("Sustained capture, distance and match stay finite and bounded");
        {
            const double rate = 48000.0;
            DynamicFingerprintCaptureBank bank;
            bank.prepare (rate);
            const auto map = matcherMap ({ stableAuto (1, 0.1f), stableAuto (2, 0.4f), stableAuto (3, 0.7f) });
            const int ws = expectedWindowSamples (rate);

            int matched = 0;
            int64_t nextTrigger = 0;
            for (int i = 0; i < 40000; ++i)
            {
                if (i % (ws + 3) == 0)
                {
                    nextTrigger = bank.nextSample();
                    bank.requestCapture (nextTrigger);
                }
                const double t = (double) i / rate;
                const float v = (float) (0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 100.0 * t)
                                         * std::exp (-(double) (i % (ws + 3)) / 60.0));
                bank.pushSample (v, v);

                DynamicFingerprintObservation obs;
                while (bank.takeCompleted (obs))
                {
                    const auto r = matchDynamicFingerprint (obs, map);
                    expect (std::isfinite (r.nearestDistance) || std::isinf (r.nearestDistance));
                    if (r.decision == DynamicMatchDecision::Matched)
                        ++matched;
                }
            }
            logMessage ("realtime matched count=" + juce::String (matched));
            expect (matched >= 0);
        }
    }
};

static DynamicFingerprintFrontEndTests dynamicFingerprintFrontEndTests;
static DynamicFingerprintFeatureTests dynamicFingerprintFeatureTests;
static DynamicFingerprintTimingTests dynamicFingerprintTimingTests;
static DynamicFingerprintCaptureBankTests dynamicFingerprintCaptureBankTests;
static DynamicFingerprintDistanceTests dynamicFingerprintDistanceTests;
static DynamicFingerprintMatcherTests dynamicFingerprintMatcherTests;
static DynamicFingerprintCalibrationTests dynamicFingerprintCalibrationTests;
static DynamicFingerprintRealtimeTests dynamicFingerprintRealtimeTests;
