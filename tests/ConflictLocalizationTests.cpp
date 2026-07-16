#include "TestCommon.h"

#include "ConflictRegionLocalizer.h"

#include <chrono>
#include <iostream>

namespace
{
    enum class KickShape { shortPunch, long808 };

    struct SyntheticConflict
    {
        std::vector<float> bass;
        std::vector<float> kick;
        ConflictRegion region = ConflictRegion::none;
        float knownDelayMs = 0.0f;
    };

    SyntheticConflict makeConflict (double sampleRate, KickShape shape, ConflictRegion conflictRegion)
    {
        constexpr float delayMs = 0.0f;
        const int n = (int) std::lround (sampleRate * 0.170);
        const int onset = (int) std::lround (sampleRate * 0.020);
        const int active = (int) std::lround (sampleRate * (shape == KickShape::shortPunch ? 0.090 : 0.135));
        SyntheticConflict pair;
        pair.bass.assign ((size_t) n, 0.0f);
        pair.kick.assign ((size_t) n, 0.0f);
        pair.region = conflictRegion;
        pair.knownDelayMs = delayMs;

        for (int i = onset; i < std::min (n, onset + active); ++i)
        {
            const int local = i - onset;
            const double t = (double) local / sampleRate;
            const double envelope = (1.0 - std::exp (-t * 900.0))
                * std::exp (-t * (shape == KickShape::shortPunch ? 25.0 : 18.0));
            // Non-harmonic low partials remove the periodic lag aliases that
            // a single sine creates in a short attack/body/tail crop.
            pair.kick[(size_t) i] = (float) (0.9 * envelope
                * (std::sin (kTwoPi * 53.0 * t) + 0.55 * std::sin (kTwoPi * 91.0 * t)
                   + 0.20 * std::sin (kTwoPi * 137.0 * t)));
        }

        const int delaySamples = (int) std::lround (delayMs * sampleRate / 1000.0);
        for (int i = 0; i < n; ++i)
            pair.bass[(size_t) i] = pair.kick[(size_t) std::max (0, i - delaySamples)];

        const auto bounds = ConflictRegionLocalizer::regionBoundaries (
            pair.kick.data(), n, sampleRate);
        const int regionStart = conflictRegion == ConflictRegion::attack ? bounds.attackStart
                              : conflictRegion == ConflictRegion::body ? bounds.attackEnd : bounds.bodyEnd;
        const int regionEnd = conflictRegion == ConflictRegion::attack ? bounds.attackEnd
                            : conflictRegion == ConflictRegion::body ? bounds.bodyEnd : bounds.tailEnd;
        const int pad = (int) std::lround (sampleRate * 0.003);
        for (int i = std::max (0, regionStart - pad); i < std::min (n, regionEnd + pad); ++i)
            pair.bass[(size_t) i] *= -1.0f;
        return pair;
    }
}

class ConflictLocalizationTests : public juce::UnitTest
{
public:
    ConflictLocalizationTests() : juce::UnitTest ("ConflictLocalization", "J") {}

    void runTest() override
    {
        beginTest ("Adaptive localizer finds attack/body/tail for punch and 808 kicks");
        for (const auto shape : { KickShape::shortPunch, KickShape::long808 })
        {
            for (const auto region : { ConflictRegion::attack, ConflictRegion::body, ConflictRegion::tail })
            {
                const auto pair = makeConflict (48000.0, shape, region);
                const auto localized = ConflictRegionLocalizer::localize (
                    pair.bass.data(), pair.kick.data(), (int) pair.kick.size(), 48000.0);
                expect (localized.valid, "valid localization");
                expect (localized.region == region, "correct known region");
                expectGreaterThan (localized.severity, 0.005f, "meaningful frame severity");
            }
        }

        beginTest ("Known correction restores the localized sub-window");
        for (const auto shape : { KickShape::shortPunch, KickShape::long808 })
        {
            for (const auto region : { ConflictRegion::attack, ConflictRegion::body, ConflictRegion::tail })
            {
                const auto pair = makeConflict (48000.0, shape, region);
                ConflictLocalizationResult localized;
                localized = ConflictRegionLocalizer::localize (
                    pair.bass.data(), pair.kick.data(), (int) pair.kick.size(), 48000.0);
                expect (localized.valid && localized.region == region, "localized before correction");

                const auto before = MultiBandCorrelation::analyze (
                    pair.bass.data() + localized.startSample,
                    pair.kick.data() + localized.startSample,
                    localized.lengthSamples, 48000.0);
                PhaseFixRenderSettings known;
                known.bassPolarityInvert = true;
                known.bassDelayMs = pair.knownDelayMs;
                known.delayInterpolation = InterpolationType::Linear;
                std::vector<float> corrected;
                PhaseFixEngine::renderCandidate (pair.bass.data() + localized.startSample,
                                                 localized.lengthSamples, 48000.0,
                                                 known, corrected);
                const auto after = MultiBandCorrelation::analyze (
                    corrected.data(), pair.kick.data() + localized.startSample,
                    localized.lengthSamples, 48000.0);
                expectGreaterThan (after.weightedMatchPercent,
                                   before.weightedMatchPercent + 30.0f,
                                   "known correction improves localized match");
                expectGreaterThan (after.weightedMatchPercent, 80.0f,
                                   "known correction converges on localized target");
            }
        }

        beginTest ("Worker-only localization cost is measured separately from runtime budgets");
        {
            constexpr int iterations = 100;
            int cases = 0;
            const auto start = std::chrono::steady_clock::now();
            for (const auto shape : { KickShape::shortPunch, KickShape::long808 })
            {
                for (const auto region : { ConflictRegion::attack, ConflictRegion::body, ConflictRegion::tail })
                {
                    const auto pair = makeConflict (48000.0, shape, region);
                    for (int i = 0; i < iterations; ++i)
                    {
                        const auto localized = ConflictRegionLocalizer::localize (
                            pair.bass.data(), pair.kick.data(), (int) pair.kick.size(), 48000.0);
                        expect (localized.valid);
                    }
                    ++cases;
                }
            }
            const double elapsedMs = std::chrono::duration<double, std::milli> (
                std::chrono::steady_clock::now() - start).count();
            std::cout << "Layer A worker localization benchmark: "
                      << elapsedMs / (double) (cases * iterations)
                      << " ms/hit at 48 kHz" << std::endl;
        }
    }
};

static ConflictLocalizationTests conflictLocalizationTestsInstance;
