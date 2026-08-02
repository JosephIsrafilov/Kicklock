#pragma once

#include <cstdint>
#include <string>
#include <vector>

// =============================================================================
// Deterministic synthetic fixture generators for the real-audio harness.
//
// Each generator returns raw mono bass + kick signals plus a ground-truth event
// list (kick position, a ground-truth "family" label used for state-selection
// accuracy, and whether that family is expected to be correction-capable). The
// harness renders these to real .wav files and loads them back through the
// genuine WAV codec, so the only thing synthetic here is the source material -
// every downstream stage (Learn, map, selector, hot branches, DSP) is the real
// production path.
//
// A real recorded override (tests/assets/audio/<name>_bass.wav + _kick.wav)
// always wins over the generator when present, per the manifest resolver.
// =============================================================================

struct GeneratedAudioFixture
{
    double sampleRate = 48000.0;
    double bpm = 120.0;

    std::vector<float> bass;
    std::vector<float> kick;

    struct Event
    {
        int sample = 0;
        int groundTruthLabel = -1;   // -1 = no ground truth (foreign/ambiguous/free)
        bool correctionCapable = false;
    };
    std::vector<Event> events;

    // Loop region in fixture samples. loopEndSample == 0 means "whole buffer".
    int loopStartSample = 0;
    int loopEndSample = 0;
};

// Builds the named fixture at the given sample rate. Unknown names return an
// empty fixture (caller asserts non-empty). Deterministic: same name + rate
// always produces bit-identical output.
GeneratedAudioFixture generateAudioFixture (const std::string& name, double sampleRate);

// Every built-in fixture name, in a stable order.
std::vector<std::string> listAudioFixtureNames();
