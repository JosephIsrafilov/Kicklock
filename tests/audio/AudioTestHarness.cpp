#include "AudioTestHarness.h"

#include "AudioWavIo.h"
#include "AudioFixtureManifest.h"

#include "dsp/DynamicLearnFormation.h"

#include <chrono>

namespace
{
    void setParam (KickLockAudioProcessor& p, const char* id, float value)
    {
        if (auto* parameter = p.apvts.getParameter (id))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    }

    bool waitForCapturing (KickLockAudioProcessor& p, const std::vector<float>& bass,
                           const std::vector<float>& kick, int blockSize)
    {
        const int channels = juce::jmax (p.getTotalNumInputChannels(), p.getTotalNumOutputChannels());
        juce::AudioBuffer<float> silence (channels, blockSize);
        juce::MidiBuffer midi;
        juce::ignoreUnused (bass, kick);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (p.getLearnState() == LearnState::Capturing)
                return true;
            if (! learnStateIsBusy (p.getLearnState()))
                return false;
            silence.clear();
            p.processBlock (silence, midi);
            juce::Thread::sleep (2);
        }
        return p.getLearnState() == LearnState::Capturing;
    }

    bool waitUntilResolved (KickLockAudioProcessor& p)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (! learnStateIsBusy (p.getLearnState()))
                return true;
            juce::Thread::sleep (2);
        }
        return ! learnStateIsBusy (p.getLearnState());
    }

    void feed (KickLockAudioProcessor& p, const std::vector<float>& bass,
               const std::vector<float>& kick, int blockSize)
    {
        const int total = (int) std::min (bass.size(), kick.size());
        const int channels = juce::jmax (p.getTotalNumInputChannels(), p.getTotalNumOutputChannels());
        for (int offset = 0; offset < total; offset += blockSize)
        {
            const int n = std::min (blockSize, total - offset);
            juce::AudioBuffer<float> buffer (channels, n);
            buffer.clear();
            for (int i = 0; i < n; ++i)
            {
                const float b = bass[(size_t) (offset + i)];
                const float k = kick[(size_t) (offset + i)];
                buffer.setSample (0, i, b);
                if (channels > 1) buffer.setSample (1, i, b);
                if (channels > 2) buffer.setSample (2, i, k);
                if (channels > 3) buffer.setSample (3, i, k);
            }
            juce::MidiBuffer midi;
            p.processBlock (buffer, midi);
        }
    }

    juce::File locateRepoChild (const juce::String& relativePath)
    {
        auto dir = juce::File::getCurrentWorkingDirectory();
        for (int i = 0; i < 7; ++i)
        {
            const auto candidate = dir.getChildFile (relativePath);
            if (candidate.getParentDirectory().isDirectory() || candidate.exists())
                return candidate;
            dir = dir.getParentDirectory();
        }
        return juce::File::getCurrentWorkingDirectory().getChildFile (relativePath);
    }
}

bool AudioTestHarness::manifestEmitted = false;

DynamicStateMap AudioTestHarness::buildProvenCorrectiveMap (double sampleRate)
{
    std::vector<DynamicLearnHit> hits;
    auto mk = [] (int seq, int64_t trig, float seed, bool capable)
    {
        DynamicLearnHit h;
        h.sequence = seq; h.triggerSample = trig;
        h.fingerprint.valid = true;
        h.fingerprint.featureCount = DynamicFingerprintContract::kFeatureCount;
        for (int i = 0; i < DynamicFingerprintContract::kFeatureCount; ++i)
            h.fingerprint.features[(size_t) i] = std::clamp (seed + 0.003f * (float) i, -0.95f, 0.95f);
        h.fingerprintValidity = DynamicFingerprintValidity::Valid;
        h.timingEligible = true;
        h.absoluteDelayMs = -1.8f + 0.7f * std::floor ((seed + 1.0f) * 2.0f);
        h.timingConfidence = 0.9f; h.lowBandEnergy = 1.0f;
        h.correctionBeneficial = capable;
        h.allpassFreqHz = 90.0f + 10.0f * std::floor ((seed + 1.0f) * 2.0f);
        h.allpassQ = 0.8f;
        return h;
    };
    for (int family = 0; family < 4; ++family)
        for (int repeat = 0; repeat < 5; ++repeat)
            hits.push_back (mk (family * 10 + repeat, family * 10000 + repeat,
                                -0.78f + 0.39f * (float) family + 0.001f * (float) repeat, true));

    DynamicLearnFormationContext ctx;
    ctx.sampleRate = sampleRate;
    ctx.crossoverEnabled = true; ctx.crossoverHz = 150.0f;
    ctx.allpassEnabled = true; ctx.fallbackAllpassFreqHz = 100.0f; ctx.fallbackAllpassQ = 0.7f;
    auto map = formDynamicStateMap (hits, ctx).map;
    map.calibration.absoluteDistanceThreshold = 1.0f;
    map.calibration.ambiguityMargin = 0.001f;
    return map;
}

juce::File AudioTestHarness::fixturesDir()
{
    return juce::File::getCurrentWorkingDirectory().getChildFile ("audio-test-fixtures");
}

juce::File AudioTestHarness::resultsDir()
{
    return juce::File::getCurrentWorkingDirectory().getChildFile ("audio-test-results");
}

juce::File AudioTestHarness::realOverrideDir()
{
    return locateRepoChild ("tests/assets/audio");
}

int AudioTestHarness::nextBlockSize (int blockIndex) const
{
    if (cfg.blockSizeSequence.empty())
        return cfg.blockSize;
    return cfg.blockSizeSequence[(size_t) (blockIndex % (int) cfg.blockSizeSequence.size())];
}

void AudioTestHarness::prepareProcessor (KickLockAudioProcessor& processor) const
{
    int maxBlock = cfg.blockSize;
    for (const int b : cfg.blockSizeSequence)
        maxBlock = juce::jmax (maxBlock, b);

    processor.enableAllBuses();
    processor.setRateAndBufferSizeDetails (cfg.sampleRate, maxBlock);
    processor.prepareToPlay (cfg.sampleRate, maxBlock);
    setParam (processor, "correction_mode", 1.0f);
    setParam (processor, "dynamic_strength", 1.0f);
}

ResolvedFixture AudioTestHarness::resolveFixture (const std::string& name)
{
    if (! manifestEmitted)
    {
        manifestEmitted = true;
        juce::Array<juce::var> entries;
        for (const auto& e : audioFixtureManifest())
        {
            auto* obj = new juce::DynamicObject();
            obj->setProperty ("name", juce::String (e.name));
            obj->setProperty ("bassAsset", juce::String (e.bassAssetName()));
            obj->setProperty ("kickAsset", juce::String (e.kickAssetName()));
            obj->setProperty ("nominalBpm", e.nominalBpm);
            obj->setProperty ("isLoopFixture", e.isLoopFixture);
            obj->setProperty ("expectedEventCount", e.expectedEventCount);
            obj->setProperty ("expectedStateMin", e.expectedStateMin);
            obj->setProperty ("expectedStateMax", e.expectedStateMax);
            obj->setProperty ("expectedDiagnosis", expectedDiagnosisLabel (e.expectedDiagnosis));
            obj->setProperty ("needsRealLearn", e.needsRealLearn);
            obj->setProperty ("inSmokeSuite", e.inSmokeSuite);
            obj->setProperty ("gateDegradation", e.thresholds.gateDegradation);
            obj->setProperty ("minMedianLowBandImprovementPercent", e.thresholds.minMedianLowBandImprovementPercent);
            obj->setProperty ("maxWorstCaseDegradationPercent", e.thresholds.maxWorstCaseDegradationPercent);
            obj->setProperty ("maxUnknownRate", e.thresholds.maxUnknownRate);
            obj->setProperty ("maxHoldRate", e.thresholds.maxHoldRate);
            obj->setProperty ("maxGlobalRate", e.thresholds.maxGlobalRate);
            entries.add (juce::var (obj));
        }
        auto dir = fixturesDir();
        dir.createDirectory();
        dir.getChildFile ("manifest.json").replaceWithText (juce::JSON::toString (juce::var (entries)));
    }

    ResolvedFixture fx;
    fx.name = name;
    fx.sampleRate = cfg.sampleRate;

    // Real recorded override wins if both stems exist.
    const auto realBass = realOverrideDir().getChildFile (juce::String (name) + "_bass.wav");
    const auto realKick = realOverrideDir().getChildFile (juce::String (name) + "_kick.wav");
    if (realBass.existsAsFile() && realKick.existsAsFile())
    {
        fx.fromRealOverride = true;
        fx.bass = AudioWavIo::loadMonoAtRate (realBass, cfg.sampleRate);
        fx.kick = AudioWavIo::loadMonoAtRate (realKick, cfg.sampleRate);
        const size_t len = std::min (fx.bass.size(), fx.kick.size());
        fx.bass.resize (len);
        fx.kick.resize (len);
        fx.bassWav = realBass;
        fx.kickWav = realKick;
        if (const auto* entry = findManifestEntry (name))
            fx.bpm = entry->nominalBpm;
        // Ground-truth events are only known for generated fixtures.
        return fx;
    }

    if (cfg.requireRealOverride)
        return fx; // caller asserts non-empty and reports the missing override

    const auto gen = generateAudioFixture (name, cfg.sampleRate);
    fx.bass = gen.bass;
    fx.kick = gen.kick;
    fx.events = gen.events;
    fx.bpm = gen.bpm;
    fx.loopStartSample = gen.loopStartSample;
    fx.loopEndSample = gen.loopEndSample;

    // Render to real WAV and load back through the genuine codec.
    auto dir = fixturesDir();
    dir.createDirectory();
    fx.bassWav = dir.getChildFile (juce::String (name) + "_bass.wav");
    fx.kickWav = dir.getChildFile (juce::String (name) + "_kick.wav");
    AudioWavIo::writeMonoWav (fx.bassWav, gen.bass, cfg.sampleRate);
    AudioWavIo::writeMonoWav (fx.kickWav, gen.kick, cfg.sampleRate);
    fx.bass = AudioWavIo::loadMonoAtRate (fx.bassWav, cfg.sampleRate);
    fx.kick = AudioWavIo::loadMonoAtRate (fx.kickWav, cfg.sampleRate);
    const size_t len = std::min (fx.bass.size(), fx.kick.size());
    fx.bass.resize (len);
    fx.kick.resize (len);
    return fx;
}

LearnOutcome AudioTestHarness::learnAndApply (KickLockAudioProcessor& processor, const ResolvedFixture& fx)
{
    LearnOutcome outcome;
    const int blockSize = cfg.blockSize;

    if (! processor.beginLearn())
    {
        outcome.diagnostics = "beginLearn() rejected";
        return outcome;
    }
    if (! waitForCapturing (processor, fx.bass, fx.kick, blockSize))
    {
        outcome.diagnostics = "never reached Capturing";
        return outcome;
    }
    feed (processor, fx.bass, fx.kick, blockSize);
    processor.stopLearn();
    if (! waitUntilResolved (processor))
    {
        outcome.diagnostics = "Learn worker did not resolve within 30s (infrastructure timeout)";
        return outcome;
    }

    const auto progress = processor.getLearnProgress();
    const auto pending = processor.getPendingLearnResult();
    outcome.diagnostics = "state=" + juce::String ((int) processor.getLearnState())
        + " captured=" + juce::String (progress.capturedHits)
        + " drained=" + juce::String (progress.drainedHits)
        + " message=" + pending.message;

    if (processor.getLearnState() != LearnState::ResultReady)
        return outcome;

    outcome.reachedResultReady = true;
    outcome.map = pending.dynamicMap;
    if (processor.applyLatestLearnResult())
    {
        outcome.applied = true;
        // One block so activeDynamicMapSource updates.
        feed (processor, fx.bass, fx.kick, blockSize);
    }
    return outcome;
}

bool AudioTestHarness::applyMapDirect (KickLockAudioProcessor& processor, const DynamicStateMap& map)
{
    if (! processor.publishDynamicStateMapForTesting (map))
        return false;
    feed (processor, { 0.0f }, { 0.0f }, cfg.blockSize); // drive one block; source updates
    return true;
}

TransportPlan AudioTestHarness::planLinear (const ResolvedFixture& fx)
{
    TransportPlan plan;
    const int total = fx.totalSamples();
    int64_t hostPos = 0;
    int blockIndex = 0;
    for (int read = 0; read < total; )
    {
        const int n = std::min (nextBlockSize (blockIndex), total - read);
        TransportPlan::Block b;
        b.frame = { hostPos, n, true, false };
        b.fixtureReadStart = read;
        b.numSamples = n;
        b.loopIteration = 0;
        b.firstBlockOfIteration = (read == 0);
        plan.blocks.push_back (b);
        hostPos += n;
        read += n;
        ++blockIndex;
    }
    return plan;
}

TransportPlan AudioTestHarness::planLoop (const ResolvedFixture& fx, int iterations)
{
    TransportPlan plan;
    plan.hasLoopRegion = true;
    plan.loopStartSamples = fx.loopStartSample;
    plan.loopEndSamples = fx.effectiveLoopEnd();

    const int loopStart = fx.loopStartSample;
    const int loopEnd = fx.effectiveLoopEnd();
    int blockIndex = 0;

    for (int iter = 0; iter < iterations; ++iter)
    {
        int64_t hostPos = loopStart; // each iteration reports the host position back at loopStart
        for (int read = loopStart; read < loopEnd; )
        {
            const int n = std::min (nextBlockSize (blockIndex), loopEnd - read);
            TransportPlan::Block b;
            b.frame = { hostPos, n, true, true }; // isLooping true throughout
            b.fixtureReadStart = read;
            b.numSamples = n;
            b.loopIteration = iter;
            b.firstBlockOfIteration = (read == loopStart);
            plan.blocks.push_back (b);
            hostPos += n;
            read += n;
            ++blockIndex;
        }
    }
    return plan;
}

TransportPlan AudioTestHarness::planWithSeek (const ResolvedFixture& fx, double seekAtFraction)
{
    TransportPlan plan;
    const int total = fx.totalSamples();
    const int seekAt = juce::jlimit (0, total - 1, (int) std::lround ((double) total * seekAtFraction));
    // Jump both the fixture read position and the host time non-contiguously
    // (isLooping=false) so the production path classifies it as a Seek.
    const int seekTarget = juce::jlimit (0, total - 1, (int) std::lround ((double) total * 0.15));

    int64_t hostPos = 0;
    int blockIndex = 0;
    bool seeked = false;
    for (int read = 0; read < total; )
    {
        if (! seeked && read >= seekAt)
        {
            read = seekTarget;
            hostPos = seekTarget; // non-contiguous host jump, not looping -> Seek
            seeked = true;
        }
        const int n = std::min (nextBlockSize (blockIndex), total - read);
        TransportPlan::Block b;
        b.frame = { hostPos, n, true, false };
        b.fixtureReadStart = read;
        b.numSamples = n;
        b.loopIteration = seeked ? 1 : 0;
        b.firstBlockOfIteration = (blockIndex == 0) || (seeked && read == seekTarget);
        plan.blocks.push_back (b);
        hostPos += n;
        read += n;
        ++blockIndex;
        if (seeked && read >= total) break;
        if (seeked && read >= seekTarget + (total - seekAt)) break; // render a comparable tail then stop
    }
    return plan;
}

TransportPlan AudioTestHarness::planWithStopStart (const ResolvedFixture& fx, double stopAtFraction, int silentBlocks)
{
    TransportPlan plan;
    const int total = fx.totalSamples();
    const int stopAt = juce::jlimit (0, total - 1, (int) std::lround ((double) total * stopAtFraction));
    int64_t hostPos = 0;
    int blockIndex = 0;
    bool inserted = false;
    for (int read = 0; read < total; )
    {
        const int n = std::min (nextBlockSize (blockIndex), total - read);
        if (! inserted && read >= stopAt)
        {
            // Emit silentBlocks of "stopped" transport at the same position; the
            // production path classifies the resume as StopStart.
            for (int s = 0; s < silentBlocks; ++s)
            {
                TransportPlan::Block sb;
                sb.frame = { hostPos, n, false /*not playing*/, false };
                sb.fixtureReadStart = read;
                sb.numSamples = n;
                sb.loopIteration = 0;
                sb.firstBlockOfIteration = false;
                sb.feedSilence = true; // a stopped transport presents silence, not live audio
                plan.blocks.push_back (sb);
                ++blockIndex;
            }
            inserted = true;
        }
        TransportPlan::Block b;
        b.frame = { hostPos, n, true, false };
        b.fixtureReadStart = read;
        b.numSamples = n;
        b.loopIteration = inserted ? 1 : 0;
        b.firstBlockOfIteration = (blockIndex == 0);
        plan.blocks.push_back (b);
        hostPos += n;
        read += n;
        ++blockIndex;
    }
    return plan;
}

RenderResult AudioTestHarness::render (KickLockAudioProcessor& processor, const ResolvedFixture& fx,
                                       const TransportPlan& plan)
{
    RenderResult result;
    const int channels = juce::jmax (processor.getTotalNumInputChannels(), processor.getTotalNumOutputChannels());
    const int total = fx.totalSamples();

    TestPlayHead playHead (cfg.sampleRate, fx.bpm);
    if (plan.hasLoopRegion)
        playHead.setLoopRegion (plan.loopStartSamples, plan.loopEndSamples);
    processor.setPlayHead (&playHead);

    int64_t processedSamples = 0;
    const auto wallStart = std::chrono::steady_clock::now();

    for (int blockIndex = 0; blockIndex < (int) plan.blocks.size(); ++blockIndex)
    {
        const auto& block = plan.blocks[(size_t) blockIndex];
        playHead.setFrame (block.frame);

        const int n = block.numSamples;
        juce::AudioBuffer<float> buffer (channels, n);
        buffer.clear();
        for (int i = 0; i < n; ++i)
        {
            const int readIndex = block.fixtureReadStart + i;
            const float b = (! block.feedSilence && readIndex >= 0 && readIndex < total) ? fx.bass[(size_t) readIndex] : 0.0f;
            const float k = (! block.feedSilence && readIndex >= 0 && readIndex < total) ? fx.kick[(size_t) readIndex] : 0.0f;
            buffer.setSample (0, i, b);
            if (channels > 1) buffer.setSample (1, i, b);
            if (channels > 2) buffer.setSample (2, i, k);
            if (channels > 3) buffer.setSample (3, i, k);
            result.dryBassReference.push_back (b);
        }
        juce::MidiBuffer midi;
        processor.processBlock (buffer, midi);

        for (int i = 0; i < n; ++i)
            result.output.push_back (buffer.getSample (0, i));

        const auto snap = processor.getDynamicRuntimeSnapshotForTesting();
        StateTimelineEntry e;
        e.blockIndex = blockIndex;
        e.hostTimeInSamples = block.frame.timeInSamples;
        e.internalTimestampSample = snap.timestampSample;
        e.loopIteration = block.loopIteration;
        e.firstBlockOfIteration = block.firstBlockOfIteration;
        e.numSamples = n;
        e.source = (int) snap.source;
        e.mapValid = snap.mapValid;
        e.selectedSemanticStateId = snap.selectedSemanticStateId;
        e.activeSemanticStateId = snap.activeSemanticStateId;
        e.activeBranchKind = (int) snap.activeBranchKind;
        e.matcherDecision = (int) snap.matcherDecision;
        e.selectorDiagnostic = (int) snap.selectorDiagnostic;
        e.holdActive = snap.holdActive;
        e.fallbackActive = snap.fallbackActive;
        result.timeline.push_back (e);

        if (block.frame.isPlaying)
            processedSamples += n;
    }

    processor.setPlayHead (nullptr);

    const auto wallEnd = std::chrono::steady_clock::now();
    result.wallSeconds = std::chrono::duration<double> (wallEnd - wallStart).count();
    result.processedSeconds = (double) processedSamples / cfg.sampleRate;
    return result;
}

void AudioTestHarness::dumpArtifacts (const std::string& fixtureName, const ResolvedFixture& fx,
                                      const RenderResult& r, const juce::var& metricsJson,
                                      const juce::String& stateTimelineCsv, const juce::String& transportTimelineCsv,
                                      const juce::StringArray& failedThresholds)
{
    auto dir = resultsDir().getChildFile (juce::String (fixtureName));
    dir.createDirectory();

    // Rendered output WAV.
    AudioWavIo::writeMonoWav (dir.getChildFile ("output.wav"), r.output, cfg.sampleRate);

    // Input excerpts (first ~1.5s of each stem).
    const int excerpt = std::min ((int) (cfg.sampleRate * 1.5), fx.totalSamples());
    if (excerpt > 0)
    {
        AudioWavIo::writeMonoWav (dir.getChildFile ("input_bass_excerpt.wav"),
                                  std::vector<float> (fx.bass.begin(), fx.bass.begin() + excerpt), cfg.sampleRate);
        AudioWavIo::writeMonoWav (dir.getChildFile ("input_kick_excerpt.wav"),
                                  std::vector<float> (fx.kick.begin(), fx.kick.begin() + excerpt), cfg.sampleRate);
    }

    // Before/after windows around the first ground-truth event, if any.
    if (! fx.events.empty())
    {
        const int at = fx.events.front().sample;
        const int win = (int) (cfg.sampleRate * 0.12);
        const int inStart = juce::jmax (0, at - win);
        const int inEnd = std::min (fx.totalSamples(), at + win);
        if (inEnd > inStart)
            AudioWavIo::writeMonoWav (dir.getChildFile ("event0_input_bass.wav"),
                                      std::vector<float> (fx.bass.begin() + inStart, fx.bass.begin() + inEnd), cfg.sampleRate);
        const int outEnd = std::min ((int) r.output.size(), at + win);
        if (outEnd > inStart)
            AudioWavIo::writeMonoWav (dir.getChildFile ("event0_output.wav"),
                                      std::vector<float> (r.output.begin() + inStart, r.output.begin() + outEnd), cfg.sampleRate);
    }

    dir.getChildFile ("metrics.json").replaceWithText (juce::JSON::toString (metricsJson));
    dir.getChildFile ("state_timeline.csv").replaceWithText (stateTimelineCsv);
    dir.getChildFile ("transport_timeline.csv").replaceWithText (transportTimelineCsv);
    dir.getChildFile ("failed_thresholds.txt").replaceWithText (failedThresholds.joinIntoString ("\n"));

    test.logMessage ("  [artifacts] wrote failure bundle to " + dir.getFullPathName());
}
