#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"

#include "TestPlayHead.h"
#include "AudioFixtureGenerators.h"

#include <string>
#include <vector>

// =============================================================================
// AudioTestHarness: the shared real-audio acceptance harness.
//
// Everything here drives the ACTUAL production processor: real WAV decode, real
// async Learn, real Apply, the real Dynamic selector / hot branches / crossfade
// / fallback, and a real injected AudioPlayHead so transport (loop wrap, seek,
// stop/start) is classified by the production path, never by calling
// notifyTransportReset() directly. Nothing about the selector or DSP is mocked.
//
// Fixture source: a real recorded override (tests/assets/audio/<name>_bass.wav
// and _kick.wav) always wins; otherwise the deterministic generator renders the
// fixture to a real .wav under <cwd>/audio-test-fixtures/ and the harness loads
// it back through the genuine WAV codec.
// =============================================================================

struct AudioHarnessConfig
{
    double sampleRate = 48000.0;
    int blockSize = 512;
    std::vector<int> blockSizeSequence;   // if non-empty, cycled per block (variable block size)
    bool requireRealOverride = false;     // fail (vs synth-fallback) when a real override is missing
};

struct StateTimelineEntry
{
    int blockIndex = 0;
    int64_t hostTimeInSamples = 0;
    int64_t internalTimestampSample = 0;   // dynamicRuntime.getRuntimeSamplePosition() at this block
    int loopIteration = 0;
    bool firstBlockOfIteration = false;
    int numSamples = 0;

    int source = 0;                        // DynamicMapSource
    bool mapValid = false;
    uint64_t selectedSemanticStateId = 0;
    uint64_t activeSemanticStateId = 0;
    int activeBranchKind = 0;              // DynamicSelectorBranchKind
    int matcherDecision = 0;               // DynamicMatchDecision
    int selectorDiagnostic = 0;            // DynamicSelectorDiagnostic
    bool holdActive = false;
    bool fallbackActive = true;
};

struct ResolvedFixture
{
    std::string name;
    bool fromRealOverride = false;
    double sampleRate = 48000.0;
    double bpm = 120.0;
    std::vector<float> bass;
    std::vector<float> kick;
    std::vector<GeneratedAudioFixture::Event> events;   // ground truth
    int loopStartSample = 0;
    int loopEndSample = 0;                               // 0 => whole buffer
    juce::File bassWav;
    juce::File kickWav;

    int totalSamples() const { return (int) std::min (bass.size(), kick.size()); }
    int effectiveLoopEnd() const { return loopEndSample > 0 ? loopEndSample : totalSamples(); }
};

struct LearnOutcome
{
    bool reachedResultReady = false;
    bool applied = false;
    DynamicStateMap map;
    juce::String diagnostics;
};

struct RenderResult
{
    std::vector<float> output;
    std::vector<float> dryBassReference;  // exactly what was fed in, same splice pattern as output
    std::vector<StateTimelineEntry> timeline;
    double processedSeconds = 0.0;
    double wallSeconds = 0.0;
    double realtimeFactor() const { return wallSeconds > 0.0 ? processedSeconds / wallSeconds : 0.0; }
};

class AudioTestHarness
{
public:
    AudioTestHarness (juce::UnitTest& testIn, AudioHarnessConfig cfgIn)
        : test (testIn), cfg (cfgIn) {}

    static juce::File fixturesDir();
    static juce::File resultsDir();
    static juce::File realOverrideDir();

    // Loads the real override if present, else generates + renders to WAV and
    // loads it back. Emits the machine-readable manifest.json once per run.
    ResolvedFixture resolveFixture (const std::string& name);

    // Real async Learn over the fixture, then Apply. reachedResultReady is false
    // (with diagnostics) if Learn did not resolve - callers report that as an
    // infrastructure-timeout condition, never as a silent baseline pass.
    LearnOutcome learnAndApply (KickLockAudioProcessor& processor, const ResolvedFixture& fx);

    // Publishes a precomputed map directly (fast path for determinism/render
    // tests that must isolate render behaviour from Learn-worker timing).
    bool applyMapDirect (KickLockAudioProcessor& processor, const DynamicStateMap& map);

    TransportPlan planLinear (const ResolvedFixture& fx);
    TransportPlan planLoop (const ResolvedFixture& fx, int iterations);
    TransportPlan planWithSeek (const ResolvedFixture& fx, double seekAtFraction);
    TransportPlan planWithStopStart (const ResolvedFixture& fx, double stopAtFraction, int silentBlocks);

    RenderResult render (KickLockAudioProcessor& processor, const ResolvedFixture& fx, const TransportPlan& plan);

    void prepareProcessor (KickLockAudioProcessor& processor) const;

    // A proven, well-formed corrective map (genuine beneficial packages + a wide
    // recognition margin) paired with the release fixture. Real Learn on the
    // synthetic fixtures forms recognized-global States (the small synthetic
    // phase offsets are not deemed beneficially correctable), which routes to
    // Global; this map instead reliably drives real State-branch selection,
    // crossfades and hot-branch DSP so the selection/continuity machinery is
    // exercised with teeth. It is NOT audio-matched, so it is used to validate
    // selection/crossfade/loop-wrap MECHANICS and click-safety - never as a
    // phase-improvement claim (which requires genuinely correctable material,
    // supplied via a real recorded override).
    static DynamicStateMap buildProvenCorrectiveMap (double sampleRate);

    // Writes the full failure artifact bundle for one fixture.
    void dumpArtifacts (const std::string& fixtureName, const ResolvedFixture& fx,
                        const RenderResult& r, const juce::var& metricsJson,
                        const juce::String& stateTimelineCsv, const juce::String& transportTimelineCsv,
                        const juce::StringArray& failedThresholds);

private:
    int nextBlockSize (int blockIndex) const;

    juce::UnitTest& test;
    AudioHarnessConfig cfg;
    static bool manifestEmitted;
};
