#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <cstdint>
#include <vector>

// =============================================================================
// TestPlayHead: a host-transport simulator for the real-audio acceptance
// harness. The production processor (classifyDynamicTransportForNewRuntime())
// derives every transport decision - stop/start, loop wrap, seek, host reset -
// purely from what getPlayHead()->getPosition() reports block to block. So the
// only honest way to exercise those paths end to end is to feed the real
// processor a real AudioPlayHead whose reported position we script exactly the
// way a DAW would, never by calling notifyTransportReset() directly.
//
// The harness advances this playhead one block at a time. A "transport script"
// is just the ordered list of positions the host will report; the harness
// builds it (linear play, a loop region with N wraps, a seek, a stop/start)
// and this class hands each entry to the processor.
// =============================================================================

struct TestTransportFrame
{
    int64_t timeInSamples = 0;   // host timeline position at the START of this block
    int numSamples = 0;          // block length the host will ask the plugin to render
    bool isPlaying = true;
    bool isLooping = false;
};

class TestPlayHead : public juce::AudioPlayHead
{
public:
    TestPlayHead (double sampleRateIn, double bpmIn) noexcept
        : sampleRate (sampleRateIn), bpm (bpmIn) {}

    void setFrame (const TestTransportFrame& next) noexcept { frame = next; }

    // Optional loop points, reported to the host position when looping so a
    // DAW-accurate PositionInfo is presented (the production code keys off the
    // backward jump + isLooping, but reporting loop points keeps the simulated
    // host honest and lets future assertions read them back).
    void setLoopRegion (int64_t startSamples, int64_t endSamples) noexcept
    {
        hasLoopRegion = true;
        loopStartSamples = startSamples;
        loopEndSamples = endSamples;
    }

    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo info;
        info.setTimeInSamples (frame.timeInSamples);
        info.setTimeInSeconds ((double) frame.timeInSamples / sampleRate);
        info.setIsPlaying (frame.isPlaying);
        info.setIsLooping (frame.isLooping);
        info.setBpm (bpm);
        info.setPpqPosition ((double) frame.timeInSamples / sampleRate * bpm / 60.0);
        if (hasLoopRegion && frame.isLooping)
        {
            LoopPoints points;
            points.ppqStart = (double) loopStartSamples / sampleRate * bpm / 60.0;
            points.ppqEnd = (double) loopEndSamples / sampleRate * bpm / 60.0;
            info.setLoopPoints (points);
        }
        return info;
    }

private:
    double sampleRate = 48000.0;
    double bpm = 120.0;
    TestTransportFrame frame;
    bool hasLoopRegion = false;
    int64_t loopStartSamples = 0;
    int64_t loopEndSamples = 0;
};

// A scripted transport: the ordered sequence of (position, blockSize, playing,
// looping) frames the host will report, plus the ordered index into the source
// fixture each block should read from. Keeping the fixture read-index explicit
// (separate from the host timeInSamples) is what lets a loop region re-read the
// same fixture samples while the reported host position wraps backward - i.e. a
// real loop, not just a fake position jump over fresh audio.
struct TransportPlan
{
    struct Block
    {
        TestTransportFrame frame;
        int fixtureReadStart = 0;  // sample index into the fixture bass/kick to read
        int numSamples = 0;
        int loopIteration = 0;     // 0 for linear; 1.. for each pass through a loop
        bool firstBlockOfIteration = false;
        bool feedSilence = false; // true while transport is stopped: input is silence, not live audio
    };

    std::vector<Block> blocks;
    bool hasLoopRegion = false;
    int64_t loopStartSamples = 0;
    int64_t loopEndSamples = 0;
};
