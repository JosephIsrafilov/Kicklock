#include "TestCommon.h"

namespace
{
    struct MaterialCase
    {
        const char* name = "";
        float bpm = 120.0f;
        float kickGain = 0.8f;
        float bassGain = 0.65f;
        float bassLagMs = -2.0f;
        bool longKick = false;
        bool distortedBass = false;
        bool long808 = false;
        bool sustained = false;
        bool moving = false;
        bool noisyKick = false;
        bool expectStrong = true;
    };

    float sampleLinear (const std::vector<float>& x, double index) noexcept
    {
        if (index < 0.0 || index >= (double) (x.size() - 1))
            return 0.0f;

        const int i = (int) std::floor (index);
        const float frac = (float) (index - (double) i);
        return x[(size_t) i] + frac * (x[(size_t) i + 1] - x[(size_t) i]);
    }

    void makeMaterial (const MaterialCase& c,
                       double sampleRate,
                       std::vector<float>& bass,
                       std::vector<float>& kick)
    {
        const int hitCount = 4;
        const int spacing = (int) std::round (sampleRate * 60.0 / (double) c.bpm);
        const int start = (int) std::round (sampleRate * 0.120);
        const int tail = (int) std::round (sampleRate * 0.500);
        const int total = start + spacing * hitCount + tail;
        const float lagSamples = c.bassLagMs * (float) sampleRate / 1000.0f;

        bass.assign ((size_t) total, 0.0f);
        kick.assign ((size_t) total, 0.0f);
        juce::Random rng (0x7057);

        std::vector<float> cleanBass ((size_t) total, 0.0f);

        for (int hit = 0; hit < hitCount; ++hit)
        {
            const int base = start + hit * spacing;
            const double noteHz = c.moving ? (hit % 4 == 0 ? 45.0 : hit % 4 == 1 ? 55.0 : hit % 4 == 2 ? 67.0 : 82.0)
                                           : (c.long808 ? 48.0 : 55.0);

            for (int i = base; i < std::min (total, base + tail); ++i)
            {
                const int local = i - base;
                const double t = (double) local / sampleRate;

                const double kickDecay = c.longKick ? 9.0 : 34.0;
                const double kickBody = std::sin (kTwoPi * 68.0 * t) * std::exp (-kickDecay * t);
                const double kickClick = 0.18 * std::sin (kTwoPi * 2500.0 * t) * std::exp (-160.0 * t);
                float k = c.kickGain * (float) (kickBody + kickClick);
                if (c.noisyKick)
                    k += c.kickGain * 0.035f * (float) (rng.nextDouble() * 2.0 - 1.0) * (float) std::exp (-18.0 * t);
                kick[(size_t) i] += k;

                double bassFreq = noteHz;
                if (c.long808)
                    bassFreq = noteHz + 22.0 * std::exp (-18.0 * t);

                const double bassDecay = c.sustained ? 0.45 : (c.long808 ? 3.2 : 8.0);
                double b = std::sin (kTwoPi * bassFreq * t) * std::exp (-bassDecay * t);
                b += 0.12 * std::sin (kTwoPi * bassFreq * 2.0 * t + 0.2) * std::exp (-bassDecay * t);
                float bf = c.bassGain * (float) b;
                if (c.distortedBass)
                    bf = std::tanh (2.6f * bf) / std::tanh (2.6f);
                cleanBass[(size_t) i] += bf;
            }
        }

        for (int i = 0; i < total; ++i)
            bass[(size_t) i] = sampleLinear (cleanBass, (double) i - (double) lagSamples);
    }

    float earlyPeakDbDelta (const std::vector<float>& before,
                            const std::vector<float>& after,
                            double sampleRate)
    {
        const int n = std::min ({ (int) before.size(), (int) after.size(),
                                  (int) std::round (sampleRate * 0.030) });
        float pre = 0.0f;
        float post = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            pre = std::max (pre, std::abs (before[(size_t) i]));
            post = std::max (post, std::abs (after[(size_t) i]));
        }

        return 20.0f * std::log10 (std::max (post, 1.0e-9f) / std::max (pre, 1.0e-9f));
    }

    std::pair<int, int> firstHitWindow (int numSamples, double sampleRate)
    {
        const int firstHit = (int) std::round (sampleRate * 0.120);
        const int pre = (int) std::round (sampleRate * PhaseFixEngine::hitPreRollMs / 1000.0);
        const int post = (int) std::round (sampleRate * PhaseFixEngine::hitPostRollMs / 1000.0);
        const int start = juce::jlimit (0, juce::jmax (0, numSamples - 1), firstHit - pre);
        const int end = juce::jlimit (start + 1, numSamples, firstHit + post);
        return { start, end - start };
    }
}

class SyntheticMaterialValidationTests : public juce::UnitTest
{
public:
    SyntheticMaterialValidationTests()
        : juce::UnitTest ("SyntheticMaterialValidation", "DSP") {}

    void runTest() override
    {
        const MaterialCase cases[] =
        {
            { "kick + clean sine sub", 120.0f },
            { "kick + long 808", 100.0f, 0.8f, 0.65f, -2.4f, false, false, true, false, false, false, false },
            { "kick + distorted 808", 140.0f, 0.8f, 0.65f, -1.8f, false, true, true, false, false, false, false },
            { "kick + sustained bassline", 124.0f, 0.8f, 0.55f, -1.6f, false, false, false, true, false, false, false },
            { "kick + moving bassline", 132.0f, 0.8f, 0.55f, -1.7f, false, false, false, false, true, false, false },
            { "quiet kick around -30 dBFS", 118.0f, (float) std::pow (10.0, -30.0 / 20.0), 0.030f, -2.0f },
            { "loud kick", 128.0f, 0.98f, 0.72f, -2.0f },
            { "short punchy kick", 150.0f, 0.85f, 0.65f, -1.3f },
            { "long boomy kick", 82.0f, 0.85f, 0.65f, -2.2f, true, false, false, false, false, false, false },
            { "noisy kick + sub", 110.0f, 0.8f, 0.65f, -1.9f, false, false, false, false, false, true }
        };

        for (const auto& c : cases)
        {
            beginTest (juce::String (c.name));

            for (double sr : { 44100.0, 48000.0, 88200.0, 96000.0 })
            {
                std::vector<float> bass, kick;
                makeMaterial (c, sr, bass, kick);

                const auto [windowStart, windowLength] = firstHitWindow ((int) bass.size(), sr);
                const auto fullBefore = PhaseFixEngine::scoreSettings (bass.data(), kick.data(),
                                                                       (int) bass.size(), sr, {});
                const auto before = PhaseFixEngine::scoreSettings (bass.data() + windowStart,
                                                                   kick.data() + windowStart,
                                                                   windowLength, sr, {});
                const auto fix = PhaseFixEngine::analyze (bass.data(), kick.data(), (int) bass.size(),
                                                          sr, 10.0f, InterpolationType::Linear, true);

                PhaseFixRenderSettings settings;
                settings.bassPolarityInvert = fix.bassPolarityInvert;
                settings.bassDelayMs = fix.bassDelayMs;
                settings.phaseFilterEnabled = fix.phaseFilterEnabled;
                settings.phaseFilterFreqHz = fix.phaseFilterFreqHz;
                settings.phaseFilterQ = fix.phaseFilterQ;
                settings.phaseFilterStages = fix.phaseFilterStages;
                settings.delayInterpolation = InterpolationType::Linear;

                const auto fullVerified = PhaseFixEngine::scoreSettings (bass.data(), kick.data(),
                                                                         (int) bass.size(), sr, settings);
                const auto verified = PhaseFixEngine::scoreSettings (bass.data() + windowStart,
                                                                     kick.data() + windowStart,
                                                                     windowLength, sr, settings);

                std::vector<float> rendered;
                PhaseFixEngine::renderCandidate (bass.data(), (int) bass.size(), sr, settings, rendered);

                const float attackDeltaDb = earlyPeakDbDelta (bass, rendered, sr);

                logMessage (juce::String (c.name)
                            + " sr=" + juce::String (sr, 0)
                            + " before=" + juce::String (before.matchPercent, 1)
                            + " predicted=" + juce::String (fix.predictedAfterMatchPercent, 1)
                            + " verified=" + juce::String (verified.matchPercent, 1)
                            + " fullBefore=" + juce::String (fullBefore.matchPercent, 1)
                            + " fullAfter=" + juce::String (fullVerified.matchPercent, 1)
                            + " delayMs=" + juce::String (fix.bassDelayMs, 2)
                            + " polarity=" + juce::String (fix.bassPolarityInvert ? "invert" : "normal")
                            + " allpass=" + juce::String (fix.phaseFilterEnabled ? "on" : "off")
                            + " confidence=" + juce::String (fix.confidence, 2)
                            + " attackDeltaDb=" + juce::String (attackDeltaDb, 2));

                const juce::String details = juce::String (c.name)
                    + " sr=" + juce::String (sr, 0)
                    + " valid=" + juce::String (fix.valid ? "true" : "false")
                    + " enough=" + juce::String (fix.enoughSignal ? "true" : "false")
                    + " before=" + juce::String (before.matchPercent, 2)
                    + " predicted=" + juce::String (fix.predictedAfterMatchPercent, 2)
                    + " verified=" + juce::String (verified.matchPercent, 2)
                    + " delayMs=" + juce::String (fix.bassDelayMs, 2)
                    + " polarity=" + juce::String (fix.bassPolarityInvert ? "invert" : "normal")
                    + " confidence=" + juce::String (fix.confidence, 2)
                    + " applyAllowed=" + juce::String (fix.applyAllowed ? "true" : "false")
                    + " message=" + fix.message;

                expect (fix.valid, details);
                expect (fix.enoughSignal, details);
                if (c.expectStrong)
                {
                    expectLessThan (std::abs (verified.matchPercent - fix.predictedAfterMatchPercent), 6.0f, c.name);
                    expectGreaterOrEqual (verified.matchPercent, before.matchPercent - 1.0f, c.name);
                    expectGreaterThan (verified.matchPercent, before.matchPercent + 2.0f, c.name);
                }
                else
                {
                    expect (! fix.applyAllowed || verified.matchPercent >= before.matchPercent - 1.0f,
                            details);
                }
                expectGreaterThan (attackDeltaDb, -1.5f, c.name);
                if (c.expectStrong)
                    expect (! fix.largeTimingOffset, c.name);
            }
        }

        beginTest ("Analyze performance at 96 kHz synthetic material");
        {
            const MaterialCase perfCase { "96 kHz mixed material", 128.0f, 0.85f, 0.65f,
                                          -2.0f, false, true, true };
            std::vector<float> bass, kick;
            makeMaterial (perfCase, 96000.0, bass, kick);

            const auto start = juce::Time::getMillisecondCounterHiRes();
            const auto fix = PhaseFixEngine::analyze (bass.data(), kick.data(), (int) bass.size(),
                                                      96000.0, 10.0f, InterpolationType::Linear, true);
            const auto elapsedMs = juce::Time::getMillisecondCounterHiRes() - start;

            logMessage ("96 kHz analyze elapsedMs=" + juce::String (elapsedMs, 2)
                        + " valid=" + juce::String (fix.valid ? "true" : "false")
                        + " confidence=" + juce::String (fix.confidence, 2));

            expect (fix.valid);
           #if JUCE_DEBUG
            expectLessThan (elapsedMs, 2500.0);
           #else
            expectLessThan (elapsedMs, 500.0);
           #endif
        }
    }
};

static SyntheticMaterialValidationTests syntheticMaterialValidationTestsInstance;
