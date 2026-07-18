#pragma once

#include <cstdint>
#include <vector>

enum class DynamicReleaseFixtureFamily : uint8_t
{
    FamilyA = 0,
    FamilyB,
    FamilyC,
    FamilyD,
    RecognizedGlobalOnly,
    Foreign,
    Ambiguous
};

struct DynamicReleaseFixtureEvent
{
    int sample = 0;
    DynamicReleaseFixtureFamily family = DynamicReleaseFixtureFamily::FamilyA;
    bool correctionCapable = true;
};

struct DynamicReleaseFixture
{
    double sampleRate = 0.0;
    std::vector<float> bass;
    std::vector<float> kick;
    std::vector<DynamicReleaseFixtureEvent> events;
    std::vector<int> transportBoundaries;
    uint32_t seed = 0x4b4c3131u;
};

DynamicReleaseFixture makeDynamicReleaseFixture (double sampleRate);
