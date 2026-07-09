#include "TestCommon.h"

namespace
{
    constexpr std::array<double, 4> kRates { 44100.0, 48000.0, 88200.0, 96000.0 };

    float sampleLinear (const std::vector<float>& x, double index) noexcept
    {
        if (index < 0.0 || index >= (double) (x.size() - 1))
            return 0.0f;

        const int i = (int) std::floor (index);
        const float frac = (float) (index - (double) i);
        return x[(size_t) i] + frac * (x[(size_t) i + 1] - x[(size_t) i]);
    }

    void makePhaseSlopeHit (std::vector<float>& bass,
                            std::vector<float>& kick,
                            double sampleRate,
                            float delaySamples,
                            bool inverted,
                            int numSamples = 131072)
    {
        bass.assign ((size_t) numSamples, 0.0f);
        kick.assign ((size_t) numSamples, 0.0f);

        const int fftSize = 1 << (int) std::ceil (std::log2 ((double) (2 * numSamples - 1)));
        const double targetHz[] = { 41.0, 53.0, 67.0, 79.0, 93.0, 111.0 };
        const double phases[] = { 0.0, 0.7, 1.8, 2.3, 3.1, 4.4 };

        for (int i = 0; i < numSamples; ++i)
        {
            double b = 0.0;
            double k = 0.0;
            for (size_t f = 0; f < std::size (targetHz); ++f)
            {
                const double amp = 1.0 / (1.0 + (double) f);
                const int bin = std::max (1, (int) std::lround (targetHz[f] * (double) fftSize / sampleRate));
                const double omega = kTwoPi * (double) bin / (double) fftSize;
                b += amp * std::sin (omega * (double) i + phases[f]);
                k += amp * std::sin (omega * ((double) i - (double) delaySamples) + phases[f]);
            }

            bass[(size_t) i] = (float) (0.18 * b);
            kick[(size_t) i] = (inverted ? -1.0f : 1.0f) * (float) (0.18 * k);
        }
    }

    void makeKickBassFixture (std::vector<float>& bass,
                              std::vector<float>& kick,
                              double sampleRate,
                              float delayMs,
                              bool inverted = false,
                              float amp = 0.8f,
                              bool longTail = false,
                              bool distorted = false,
                              bool noisy = false,
                              double bassFrequency = 60.0)
    {
        const int n = (int) std::lround (sampleRate * 0.65);
        const int start = (int) std::lround (sampleRate * 0.080);
        const float delaySamples = delayMs * (float) sampleRate / 1000.0f;
        bass.assign ((size_t) n, 0.0f);
        kick.assign ((size_t) n, 0.0f);

        juce::Random rng (0x51a7);
        std::vector<float> cleanBass ((size_t) n, 0.0f);

        for (int i = 0; i < n; ++i)
        {
            const int local = i - start;
            if (local < 0)
                continue;

            const double t = (double) local / sampleRate;
            const double kickEnv = std::exp (-t * (longTail ? 13.0 : 36.0));
            const double bassEnv = std::exp (-t * (longTail ? 4.0 : 10.0));

            kick[(size_t) i] = amp * (float) (kickEnv * (std::sin (kTwoPi * 72.0 * t)
                                                        + 0.25 * std::sin (kTwoPi * 145.0 * t)));

            float b = amp * (float) (bassEnv * std::sin (kTwoPi * bassFrequency * t));
            if (distorted)
                b = std::tanh (2.8f * (b + 0.25f * amp * (float) std::sin (kTwoPi * 2.0 * bassFrequency * t)))
                    / std::tanh (2.8f);
            cleanBass[(size_t) i] = b;
        }

        for (int i = 0; i < n; ++i)
        {
            bass[(size_t) i] = sampleLinear (cleanBass, (double) i - (double) delaySamples);
            if (inverted)
                bass[(size_t) i] = -bass[(size_t) i];
            if (noisy)
                kick[(size_t) i] += 0.08f * amp * (float) (rng.nextDouble() * 2.0 - 1.0);
        }
    }

    float medianAbsError (const std::vector<float>& values, float target)
    {
        std::vector<float> errors;
        for (float v : values)
            if (std::isfinite (v))
                errors.push_back (std::abs (v - target));

        if (errors.empty())
            return std::numeric_limits<float>::infinity();

        std::sort (errors.begin(), errors.end());
        return errors[errors.size() / 2];
    }
}

class OfflineReliabilityTests : public juce::UnitTest
{
public:
    OfflineReliabilityTests() : juce::UnitTest ("OfflineReliability", "DSP") {}

    void runTest() override
    {
        beginTest ("Phase-slope refiner recovers signed fractional delays across rates and polarity");
        {
            const float delays[] = { -12.7f, -3.3f, 0.0f, 0.4f, 7.25f };

            for (double sr : kRates)
            {
                for (float delay : delays)
                {
                    for (bool inverted : { false, true })
                    {
                        std::vector<std::vector<float>> bassHits, kickHits;
                        std::vector<FrequencyDomainPhaseRefiner::Hit> hits;
                        bassHits.resize (4);
                        kickHits.resize (4);

                        for (int h = 0; h < 4; ++h)
                        {
                            makePhaseSlopeHit (bassHits[(size_t) h], kickHits[(size_t) h], sr, delay, inverted);
                            hits.push_back ({ bassHits[(size_t) h].data(),
                                              kickHits[(size_t) h].data(),
                                              (int) bassHits[(size_t) h].size(),
                                              delay,
                                              inverted });
                        }

                        const auto refined = FrequencyDomainPhaseRefiner::refine (hits, sr);
                        expect (refined.valid);
                        expectWithinAbsoluteError (refined.medianDelaySamples, delay, 0.05f,
                                                   "sr=" + juce::String (sr, 0)
                                                   + " delay=" + juce::String (delay, 2)
                                                   + " inverted=" + juce::String (inverted ? "true" : "false"));
                        expectGreaterThan (refined.phaseConfidence.front(), 0.80f);
                    }
                }
            }
        }

        beginTest ("Known millisecond delay is equivalent across sample rates");
        {
            constexpr float expectedMs = 2.35f;
            for (double sr : kRates)
            {
                const float delaySamples = expectedMs * (float) sr / 1000.0f;
                std::vector<float> bass, kick;
                makePhaseSlopeHit (bass, kick, sr, delaySamples, false);
                std::vector<FrequencyDomainPhaseRefiner::Hit> hits
                {
                    { bass.data(), kick.data(), (int) bass.size(), delaySamples, false }
                };

                const auto refined = FrequencyDomainPhaseRefiner::refine (hits, sr);
                expect (refined.valid);
                const float recoveredMs = refined.medianDelaySamples * 1000.0f / (float) sr;
                expectWithinAbsoluteError (recoveredMs, expectedMs, 0.02f,
                                           "sr=" + juce::String (sr, 0));
            }
        }

        beginTest ("Polarity inversion is recovered");
        {
            std::vector<float> bass, kick;
            makeKickBassFixture (bass, kick, 48000.0, 0.0f, true);

            const auto result = AlignmentAnalyzer::analyze (bass.data(), kick.data(), (int) bass.size(), 48000.0,
                                                            10.0f, 30.0f, 120.0f, 32768, false);
            expect (result.valid);
            expect (result.invertPolarity);
            expectGreaterThan (result.afterMatch, 70.0f);
        }

        beginTest ("Quiet, long-tail, distorted, and noisy fixtures remain analyzable");
        {
            struct Fixture
            {
                const char* name;
                float amp;
                bool longTail;
                bool distorted;
                bool noisy;
            };

            const Fixture fixtures[] =
            {
                { "quiet -30 dBFS", (float) std::pow (10.0, -30.0 / 20.0), false, false, false },
                { "long 808 tail", 0.7f, true, false, false },
                { "distorted 808", 0.7f, true, true, false },
                { "noisy kick", 0.7f, false, false, true },
            };

            for (const auto& fixture : fixtures)
            {
                std::vector<float> bass, kick;
                makeKickBassFixture (bass, kick, 48000.0, 1.8f, false, fixture.amp,
                                     fixture.longTail, fixture.distorted, fixture.noisy);

                const auto result = PhaseFixEngine::analyze (bass.data(), kick.data(), (int) bass.size(),
                                                             48000.0, 10.0f, InterpolationType::Linear, false);
                expect (result.valid, fixture.name);
                expect (result.enoughSignal, fixture.name);
                expectGreaterThan (result.confidence, 0.0f, fixture.name);
            }
        }

        beginTest ("Clean consistent material is higher-confidence than ambiguous material");
        {
            std::vector<float> cleanBass, cleanKick;
            makeKickBassFixture (cleanBass, cleanKick, 48000.0, 1.4f, false, 0.8f, false, false, false, 62.0);
            const auto clean = PhaseFixEngine::analyze (cleanBass.data(), cleanKick.data(), (int) cleanBass.size(),
                                                        48000.0, 10.0f, InterpolationType::Linear, false);

            std::vector<HitObservation> ambiguous;
            for (int i = 0; i < 6; ++i)
            {
                HitObservation h;
                h.hitIndex = i;
                h.delayMs = (i % 2 == 0) ? -4.0f : 4.0f;
                h.energy = 1.0f;
                h.signalConfidence = 0.6f;
                ambiguous.push_back (h);
            }
            const auto unstable = HitConsensus::analyze (ambiguous);

            expect (clean.valid);
            expectGreaterThan (clean.confidence, unstable.consensusConfidence);
            expectLessThan (unstable.dominantClusterShare, 0.70f);
        }

        beginTest ("Low hit counts avoid overconfident coherence weighting");
        {
            constexpr float delay = 3.35f;
            for (int count = 1; count <= 4; ++count)
            {
                std::vector<std::vector<float>> bassHits ((size_t) count);
                std::vector<std::vector<float>> kickHits ((size_t) count);
                std::vector<FrequencyDomainPhaseRefiner::Hit> hits;

                for (int h = 0; h < count; ++h)
                {
                    makePhaseSlopeHit (bassHits[(size_t) h], kickHits[(size_t) h], 48000.0, delay, false);
                    if (h == count - 1 && count < 4)
                        kickHits[(size_t) h][200] += 0.7f;

                    hits.push_back ({ bassHits[(size_t) h].data(),
                                      kickHits[(size_t) h].data(),
                                      (int) bassHits[(size_t) h].size(),
                                      delay,
                                      false });
                }

                const auto refined = FrequencyDomainPhaseRefiner::refine (
                    hits, 48000.0, 30.0f, 120.0f, FrequencyDomainPhaseRefiner::WeightingMode::Coherence);

                expect (refined.valid);
                expectLessThan (medianAbsError (refined.delaySamples, delay), 0.08f,
                                "count=" + juce::String (count));

                if (count < 4)
                    expectLessThan (refined.phaseConfidence.front(), 0.995f);
            }
        }

        beginTest ("Dominant cluster survives one noisy outlier");
        {
            std::vector<HitObservation> hits;
            for (int i = 0; i < 6; ++i)
            {
                HitObservation h;
                h.hitIndex = i;
                h.delayMs = i == 5 ? 8.0f : 2.0f + 0.02f * (float) i;
                h.energy = 1.0f;
                h.signalConfidence = 0.9f;
                hits.push_back (h);
            }

            const auto consensus = HitConsensus::analyze (hits);
            expect (consensus.hasConsensus);
            expectGreaterOrEqual (consensus.dominantClusterShare, 0.70f);
            expectGreaterThan (consensus.consensusConfidence, 0.65f);
        }

        beginTest ("Polarity half-period equivalents cluster as one physical solution");
        {
            std::vector<HitObservation> hits;
            for (int i = 0; i < 6; ++i)
            {
                HitObservation h;
                h.hitIndex = i;
                h.polarityInvert = (i % 2) == 1;
                h.delayMs = h.polarityInvert ? -10.0f : 0.0f; // half-period equivalent at 50 Hz
                h.energy = 1.0f;
                h.signalConfidence = 0.85f;
                h.dominantFrequencyHz = 50.0f;
                hits.push_back (h);
            }

            const auto consensus = HitConsensus::analyze (hits);
            expect (consensus.hasConsensus);
            expectEquals ((int) consensus.clusters.size(), 1);
            expectGreaterThan (consensus.consensusConfidence, 0.75f);
        }

        beginTest ("Automatic rotator search does not choose pathological high-Q or four-stage candidates");
        {
            constexpr int n = 16384;
            std::vector<float> bass ((size_t) n, 0.0f), kick ((size_t) n, 0.0f);

            AllpassPhaseRotator rot;
            rot.prepare (48000.0, 4);
            rot.setParameters (80.0f, 4.0f);

            juce::Random rng (12345);
            for (int i = 0; i < n; ++i)
            {
                const double t = (double) i / 48000.0;
                const float env = (float) std::exp (-t * 4.0);
                const float x = env * (float) (rng.nextDouble() * 2.0 - 1.0);
                bass[(size_t) i] = x;
                kick[(size_t) i] = rot.processSample (x);
            }

            const auto result = AlignmentAnalyzer::analyze (bass.data(), kick.data(), n, 48000.0,
                                                            10.0f, 30.0f, 120.0f, 16384, true);
            expect (result.valid);
            if (result.adjustRotator)
            {
                expectLessOrEqual (result.rotatorQ, 2.0f);
                expectLessOrEqual (result.rotatorStages, 3);
            }
        }
    }
};

static OfflineReliabilityTests offlineReliabilityTestsInstance;
