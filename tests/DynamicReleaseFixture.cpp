#include "DynamicReleaseFixture.h"

#include <algorithm>
#include <cmath>

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    float signedNoise (uint32_t& state) noexcept
    {
        state = state * 1664525u + 1013904223u;
        return ((float) ((state >> 8) & 0xffffu) / 32767.5f - 1.0f) * 0.0025f;
    }

    struct FamilyShape
    {
        double bassHz;
        double kickHz;
        double bassPhase;
        double timingOffsetMs;
        double harmonic;
    };

    FamilyShape shapeFor (DynamicReleaseFixtureFamily family) noexcept
    {
        switch (family)
        {
            case DynamicReleaseFixtureFamily::FamilyA: return { 46.0, 57.0, 0.15, -1.6, 0.18 };
            case DynamicReleaseFixtureFamily::FamilyB: return { 61.0, 69.0, 1.10,  0.8, 0.34 };
            case DynamicReleaseFixtureFamily::FamilyC: return { 78.0, 52.0, -1.35, 2.1, 0.49 };
            case DynamicReleaseFixtureFamily::FamilyD: return { 96.0, 83.0, 2.25, -0.5, 0.27 };
            case DynamicReleaseFixtureFamily::RecognizedGlobalOnly: return { 112.0, 61.0, -2.35, 1.3, 0.42 };
            case DynamicReleaseFixtureFamily::Foreign: return { 151.0, 105.0, 0.75, -2.7, 0.58 };
            case DynamicReleaseFixtureFamily::Ambiguous: return { 69.0, 64.0, 0.62, 0.1, 0.39 };
        }
        return { 55.0, 55.0, 0.0, 0.0, 0.0 };
    }

    bool correctionCapable (DynamicReleaseFixtureFamily family) noexcept
    {
        return family == DynamicReleaseFixtureFamily::FamilyA
            || family == DynamicReleaseFixtureFamily::FamilyB
            || family == DynamicReleaseFixtureFamily::FamilyC
            || family == DynamicReleaseFixtureFamily::FamilyD;
    }
}

DynamicReleaseFixture makeDynamicReleaseFixture (double sampleRate)
{
    DynamicReleaseFixture fixture;
    fixture.sampleRate = sampleRate;
    if (! std::isfinite (sampleRate) || sampleRate < 40000.0)
        return fixture;

    const int spacing = (int) std::lround (0.260 * sampleRate);
    const int start = (int) std::lround (0.090 * sampleRate);
    const int eventTail = (int) std::lround (0.180 * sampleRate);
    const int total = start + 28 * spacing + eventTail;
    fixture.bass.assign ((size_t) total, 0.0f);
    fixture.kick.assign ((size_t) total, 0.0f);

    const DynamicReleaseFixtureFamily sequence[] = {
        DynamicReleaseFixtureFamily::FamilyA, DynamicReleaseFixtureFamily::FamilyB,
        DynamicReleaseFixtureFamily::FamilyC, DynamicReleaseFixtureFamily::FamilyD,
        DynamicReleaseFixtureFamily::RecognizedGlobalOnly
    };
    uint32_t noiseState = fixture.seed;

    for (int eventIndex = 0; eventIndex < 25; ++eventIndex)
    {
        const auto family = sequence[eventIndex % 5];
        const auto shape = shapeFor (family);
        const int eventSample = start + eventIndex * spacing;
        fixture.events.push_back ({ eventSample, family, correctionCapable (family) });

        const double amplitude = 0.42 + 0.035 * (double) (eventIndex % 4);
        const int bassOffset = (int) std::lround (shape.timingOffsetMs * sampleRate / 1000.0);
        for (int local = 0; local < eventTail; ++local)
        {
            const int index = eventSample + local;
            if (index >= total)
                break;
            const double seconds = (double) local / sampleRate;
            const double bassTime = (double) (local - bassOffset) / sampleRate;
            const double bassEnvelope = std::exp (-seconds * (13.0 + 0.8 * (eventIndex % 3)));
            const double bass = amplitude * bassEnvelope * (
                std::sin (2.0 * kPi * shape.bassHz * bassTime + shape.bassPhase)
                + shape.harmonic * std::sin (2.0 * kPi * shape.bassHz * 2.0 * bassTime + 0.3));
            const double kickEnvelope = std::exp (-seconds * (32.0 + 3.0 * (eventIndex % 3)));
            const double kick = 0.78 * kickEnvelope * (
                std::sin (2.0 * kPi * shape.kickHz * seconds)
                + 0.22 * std::sin (2.0 * kPi * shape.kickHz * 2.6 * seconds));
            fixture.bass[(size_t) index] += (float) std::tanh (bass * 1.08) + signedNoise (noiseState);
            fixture.kick[(size_t) index] += (float) std::tanh (kick * 1.03) + signedNoise (noiseState);
        }
    }

    for (int extra = 0; extra < 3; ++extra)
    {
        const auto family = extra == 0 ? DynamicReleaseFixtureFamily::Foreign
                                       : DynamicReleaseFixtureFamily::Ambiguous;
        const auto shape = shapeFor (family);
        const int eventSample = start + (25 + extra) * spacing;
        fixture.events.push_back ({ eventSample, family, false });
        for (int local = 0; local < eventTail && eventSample + local < total; ++local)
        {
            const int index = eventSample + local;
            const double seconds = (double) local / sampleRate;
            const double envelope = std::exp (-seconds * 18.0);
            fixture.bass[(size_t) index] += (float) (0.38 * envelope * std::sin (2.0 * kPi * shape.bassHz * seconds + shape.bassPhase));
            fixture.kick[(size_t) index] += (float) (0.72 * std::exp (-seconds * 34.0) * std::sin (2.0 * kPi * shape.kickHz * seconds));
        }
    }

    fixture.transportBoundaries = { start + 9 * spacing, start + 18 * spacing };
    return fixture;
}
