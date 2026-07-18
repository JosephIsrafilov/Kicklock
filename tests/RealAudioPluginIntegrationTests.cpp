#include "TestCommon.h"


//============================================================================//
// Real-audio plugin integration: exercises the FULL PluginProcessor lifecycle
// (Learn -> Stop -> Apply -> processBlock) against genuine recorded kick and
// bass material, not synthesized signals. This closes the gap flagged in
// docs/CONFLICT_LOCALIZATION_PERFORMANCE.md ("Real-audio validation status"):
// Layer A's attack/body/tail localization is derived from the KICK envelope,
// and every existing "real" fixture (JointRealMaterialLearnTests in
// LearnCoreTests.cpp, SyntheticMaterialValidationTests.cpp) either synthesizes
// the kick or synthesizes both signals -- never a genuine recorded kick
// transient, end to end, through the actual processor.
//
// Gated on two files that are NOT checked into this repository:
//   tests/assets/real_kick.wav
//   tests/assets/real_bass.wav
// Any sample rate/bit depth/channel count JUCE can decode is fine -- both are
// resampled to the test sample rate and summed to mono. They must be the same
// musical loop, aligned to the same downbeat, same approximate length (extra
// length on either file is trimmed to the shorter one). See the asset-spec
// comment on loadRealLoop() below for what makes a good fixture.
//
// Until those two files exist, every beginTest() below logs why it is being
// skipped and returns without recording any expect() -- so the suite stays
// green, not by relaxing a threshold, but by genuinely not claiming a
// real-audio result it cannot back up.
//============================================================================//

namespace
{
    void setValue (KickLockAudioProcessor& p, const char* id, float value)
    {
        if (auto* parameter = p.apvts.getParameter (id))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    }

    bool waitForState (KickLockAudioProcessor& p, LearnState expected, int attempts = 3000)
    {
        for (int i = 0; i < attempts; ++i)
        {
            if (p.getLearnState() == expected)
                return true;
            juce::Thread::sleep (2);
        }
        return p.getLearnState() == expected;
    }

    bool waitUntilNotBusy (KickLockAudioProcessor& p, int attempts = 3000)
    {
        for (int i = 0; i < attempts; ++i)
        {
            if (! learnStateIsBusy (p.getLearnState()))
                return true;
            juce::Thread::sleep (2);
        }
        return ! learnStateIsBusy (p.getLearnState());
    }

    juce::File findAssetFile (const char* filename)
    {
        auto directory = juce::File::getCurrentWorkingDirectory();
        for (int i = 0; i < 6; ++i)
        {
            const auto candidate = directory.getChildFile ("tests/assets").getChildFile (filename);
            if (candidate.existsAsFile())
                return candidate;
            directory = directory.getParentDirectory();
        }
        return {};
    }

    // Decodes any JUCE-readable file to mono float at targetSampleRate.
    // Multi-channel files are summed to mono (equal weight) rather than
    // requiring the fixture to be pre-mixed, so a stereo bounce works with no
    // extra prep.
    std::vector<float> loadMonoAtRate (const juce::File& file, double targetSampleRate)
    {
        if (! file.existsAsFile())
            return {};

        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr || reader->numChannels < 1 || reader->lengthInSamples < 2)
            return {};

        juce::AudioBuffer<float> source ((int) reader->numChannels, (int) reader->lengthInSamples);
        reader->read (&source, 0, source.getNumSamples(), 0, true, true);

        std::vector<float> mono ((size_t) source.getNumSamples(), 0.0f);
        const float weight = 1.0f / (float) source.getNumChannels();
        for (int ch = 0; ch < source.getNumChannels(); ++ch)
            for (int i = 0; i < source.getNumSamples(); ++i)
                mono[(size_t) i] += source.getSample (ch, i) * weight;

        if (std::abs (reader->sampleRate - targetSampleRate) < 0.5)
            return mono;

        const int outSamples = (int) std::lround ((double) mono.size() * targetSampleRate / reader->sampleRate);
        std::vector<float> resampled ((size_t) std::max (0, outSamples));
        for (int i = 0; i < outSamples; ++i)
        {
            const double srcIndex = (double) i * reader->sampleRate / targetSampleRate;
            const int i0 = (int) std::floor (srcIndex);
            const float frac = (float) (srcIndex - (double) i0);
            const float a = (i0 >= 0 && i0 < (int) mono.size()) ? mono[(size_t) i0] : 0.0f;
            const float b = (i0 + 1 >= 0 && i0 + 1 < (int) mono.size()) ? mono[(size_t) (i0 + 1)] : 0.0f;
            resampled[(size_t) i] = a + frac * (b - a);
        }
        return resampled;
    }

    struct RealLoopMaterial
    {
        bool available = false;
        std::vector<float> bass;
        std::vector<float> kick;
        juce::String unavailableReason;
    };

    // Asset spec: a real recorded loop, not a click track. To actually
    // exercise what Layers A/B/C were built for, the loop should have:
    //  - at least 6-8 kick hits (Learn needs enough windows to cluster),
    //  - a bass part that moves across 3+ notes (the original "4+ note bass,
    //    kick not always coincident" case from the start of this thread), and
    //  - some hits where the phase conflict clearly sits in the kick's
    //    attack, and others where it sits later (body/tail) -- e.g. vary how
    //    the bass note's attack lines up with the kick from hit to hit.
    // A tight 2-4 bar loop, bounced twice back to back, is enough length.
    RealLoopMaterial loadRealLoop (double sampleRate)
    {
        RealLoopMaterial material;
        const auto kickFile = findAssetFile ("real_kick.wav");
        const auto bassFile = findAssetFile ("real_bass.wav");

        if (! kickFile.existsAsFile() || ! bassFile.existsAsFile())
        {
            material.unavailableReason =
                "Missing required real-audio fixtures: tests/assets/real_kick.wav and/or "
                "tests/assets/real_bass.wav.";
            return material;
        }

        auto bass = loadMonoAtRate (bassFile, sampleRate);
        auto kick = loadMonoAtRate (kickFile, sampleRate);
        if (bass.empty() || kick.empty())
        {
            material.unavailableReason = "Undecodable real-audio fixtures: tests/assets/real_kick.wav and/or tests/assets/real_bass.wav.";
            return material;
        }

        const size_t length = std::min (bass.size(), kick.size());
        bass.resize (length);
        kick.resize (length);

        material.available = true;
        material.bass = std::move (bass);
        material.kick = std::move (kick);
        return material;
    }

    // Feeds bass on the main bus (channels 0/1) and kick on the sidechain bus
    // (channels 2/3) -- the same routing ProcessorTests.cpp's fillBass/
    // fillKickSidechain use -- in blockSize chunks, exactly like a host
    // would. Optionally captures the processed main-out channel 0.
    void feedRealMaterial (KickLockAudioProcessor& p,
                           const std::vector<float>& bass,
                           const std::vector<float>& kick,
                           int blockSize,
                           std::vector<float>* capturedOutput = nullptr)
    {
        const int total = (int) std::min (bass.size(), kick.size());
        for (int offset = 0; offset < total; offset += blockSize)
        {
            const int n = std::min (blockSize, total - offset);
            juce::AudioBuffer<float> buffer (juce::jmax (p.getTotalNumInputChannels(),
                                                         p.getTotalNumOutputChannels()), n);
            buffer.clear();
            for (int i = 0; i < n; ++i)
            {
                const float b = bass[(size_t) (offset + i)];
                const float k = kick[(size_t) (offset + i)];
                buffer.setSample (0, i, b);
                if (buffer.getNumChannels() > 1) buffer.setSample (1, i, b);
                if (buffer.getNumChannels() > 2) buffer.setSample (2, i, k);
                if (buffer.getNumChannels() > 3) buffer.setSample (3, i, k);
            }
            juce::MidiBuffer midi;
            p.processBlock (buffer, midi);
            if (capturedOutput != nullptr)
                for (int i = 0; i < n; ++i)
                    capturedOutput->push_back (buffer.getSample (0, i));
        }
    }

    void logLearnDiagnostics (juce::UnitTest& test, KickLockAudioProcessor& p)
    {
        const auto progress = p.getLearnProgress();
        const auto pending = p.getPendingLearnResult();
        test.logMessage ("  captured=" + juce::String (progress.capturedHits)
                          + " drained=" + juce::String (progress.drainedHits)
                          + " dropped=" + juce::String (progress.droppedQueueHits)
                          + " ignoredOverlap=" + juce::String (progress.ignoredOverlappingTriggers)
                          + " pitchAccepted=" + juce::String (progress.pitchAcceptedHits)
                          + " rejectedPitch=" + juce::String (progress.rejectedPitchHits)
                          + " timingUsable=" + juce::String (progress.timingUsableHits)
                          + " unusableSignal=" + juce::String (progress.unusableSignalHits)
                          + " message=\"" + pending.message + "\"");
    }

    bool allFinite (const std::vector<float>& signal)
    {
        for (const float v : signal)
            if (! std::isfinite (v))
                return false;
        return true;
    }

    float peakAbs (const std::vector<float>& signal)
    {
        float peak = 0.0f;
        for (const float v : signal)
            peak = std::max (peak, std::abs (v));
        return peak;
    }

    bool requireRealAudioFixtures() noexcept
    {
        return juce::SystemStats::getEnvironmentVariable ("KICKLOCK_REQUIRE_REAL_AUDIO_FIXTURES", {}) == "1";
    }
}

class RealAudioPluginIntegrationTests : public juce::UnitTest
{
public:
    RealAudioPluginIntegrationTests() : juce::UnitTest ("Real-audio plugin integration", "RealAudio") {}

    void runTest() override
    {
        const auto material = loadRealLoop (kSampleRate);
        const bool fixturesRequired = requireRealAudioFixtures();

        beginTest ("Real kick/bass stems are present");
        if (! material.available)
        {
            logMessage ("REAL_AUDIO_STATUS: UNVERIFIED. " + material.unavailableReason);
            if (fixturesRequired)
                expect (false, "KICKLOCK_REQUIRE_REAL_AUDIO_FIXTURES=1 requires decodable tests/assets/real_kick.wav and tests/assets/real_bass.wav.");
            return; // Explicit skip: no expect() calls, so this reports as
                    // 0 assertions rather than a false pass or a build break.
        }
        logMessage ("REAL_AUDIO_STATUS: VERIFIED. Using supplied recorded kick/bass stems.");
        expect (material.bass.size() == material.kick.size());
        expectGreaterThan ((int) material.bass.size(), (int) (kSampleRate * 1.0),
                           "loop should be at least ~1s of real material");

        const auto baseline = MultiBandCorrelation::analyze (
            material.bass.data(), material.kick.data(), (int) material.bass.size(), kSampleRate);

        beginTest ("Dynamic Learn on the real loop reaches ResultReady");
        {
            KickLockAudioProcessor p;
            p.enableAllBuses();
            p.setRateAndBufferSizeDetails (kSampleRate, 512);
            p.prepareToPlay (kSampleRate, 512);
            setValue (p, "correction_mode", 1.0f);
            setValue (p, "dynamic_strength", 1.0f);

            expect (p.beginLearn());
            expect (waitForState (p, LearnState::Capturing), "should reach Capturing");
            feedRealMaterial (p, material.bass, material.kick, 512);
            expect (p.stopLearn());
            expect (waitUntilNotBusy (p), "Learn should resolve, not hang, on real material");

            const auto state = p.getLearnState();
            logMessage ("Resolved state: " + juce::String ((int) state));
            logLearnDiagnostics (*this, p);

            expect (state == LearnState::ResultReady,
                   "Real kick/bass material failed to produce a usable Learn result -- this "
                   "is the exact regression class (compare LearnDiagnosticRootCauseTests.cpp) "
                   "that only synthetic material was previously checked against. If this "
                   "fails, the diagnostics logged above show where hits were lost.");

            if (state == LearnState::ResultReady)
            {
                const auto pending = p.getPendingLearnResult();
                expect (pending.hasDynamicStateMap, "New Dynamic Learn must return dynamicMap, not a legacy note map");
                expect (isStructurallyValidDynamicStateMap (pending.dynamicMap));
                expectGreaterOrEqual (getOccupiedDynamicStateCount (pending.dynamicMap), 1,
                                      "at least one occupied Dynamic State from real material");
                int stable = 0;
                int candidate = 0;
                int globalOnly = 0;
                for (const auto& dynamicState : pending.dynamicMap.states)
                {
                    if (! dynamicState.occupied)
                        continue;
                    stable += dynamicState.evidence == DynamicStateEvidence::Stable ? 1 : 0;
                    candidate += dynamicState.evidence == DynamicStateEvidence::Candidate ? 1 : 0;
                    globalOnly += ! dynamicState.hasLearnedPackage ? 1 : 0;
                }
                expect (stable + candidate > 0);
                logMessage ("Dynamic States: stable=" + juce::String (stable)
                            + " candidate=" + juce::String (candidate)
                            + " recognized-global=" + juce::String (globalOnly));
            }
        }

        beginTest ("Apply measurably improves real-material phase match through the full processor");
        {
            KickLockAudioProcessor p;
            p.enableAllBuses();
            p.setRateAndBufferSizeDetails (kSampleRate, 512);
            p.prepareToPlay (kSampleRate, 512);
            setValue (p, "correction_mode", 1.0f);
            setValue (p, "dynamic_strength", 1.0f);

            expect (p.beginLearn());
            expect (waitForState (p, LearnState::Capturing));
            feedRealMaterial (p, material.bass, material.kick, 512);
            expect (p.stopLearn());
            expect (waitUntilNotBusy (p));

            if (p.getLearnState() != LearnState::ResultReady)
            {
                logMessage ("Skipping Apply comparison: Learn did not reach ResultReady "
                            "(see the previous test's diagnostics).");
            }
            else
            {
                expect (p.applyLatestLearnResult());

                const int latency = p.getLatencySamples();
                std::vector<float> output;
                output.reserve (material.bass.size());
                feedRealMaterial (p, material.bass, material.kick, 512, &output);

                expect (p.getActiveDynamicMapSourceForTesting() == DynamicMapSource::NewDynamicStateMap,
                        "Apply must activate New Dynamic source");
                const auto snapshot = p.getDynamicRuntimeSnapshotForTesting();
                expect (snapshot.source == DynamicMapSource::NewDynamicStateMap && snapshot.mapValid,
                        "runtime snapshot must describe the active New map");

                expect (allFinite (output), "processed real material must stay finite (no NaN/Inf)");
                expectLessThan (peakAbs (output), 8.0f, "processed output should not blow up in level");

                // Align for the delay the plugin's own PDC reports, then compare
                // corrected-bass-vs-kick match against the untouched baseline
                // using the same MultiBandCorrelation the plugin's own Phase
                // Match meter reads from.
                if ((int) output.size() > latency + 4800)
                {
                    std::vector<float> correctedBass (output.begin() + latency, output.end());
                    const size_t alignedLength = std::min (correctedBass.size(), material.kick.size());
                    std::vector<float> alignedKick (material.kick.begin(), material.kick.begin() + (long) alignedLength);
                    correctedBass.resize (alignedLength);

                    const auto corrected = MultiBandCorrelation::analyze (
                        correctedBass.data(), alignedKick.data(), (int) alignedLength, kSampleRate);

                    logMessage ("weightedMatchPercent baseline=" + juce::String (baseline.weightedMatchPercent, 2)
                                + " corrected=" + juce::String (corrected.weightedMatchPercent, 2));

                    expectGreaterThan (corrected.weightedMatchPercent, baseline.weightedMatchPercent,
                                       "Apply should improve real-material phase match, not just synthetic");
                }
                else
                {
                    logMessage ("Loop too short after latency alignment for a before/after match "
                                "comparison -- provide a longer loop (~2s+) if this keeps happening.");
                }
            }
        }

        beginTest ("Post-Apply real-material output is deterministic across two independent processor instances");
        {
            KickLockAudioProcessor p;
            p.enableAllBuses();
            p.setRateAndBufferSizeDetails (kSampleRate, 512);
            p.prepareToPlay (kSampleRate, 512);
            setValue (p, "correction_mode", 1.0f);
            setValue (p, "dynamic_strength", 1.0f);

            expect (p.beginLearn());
            expect (waitForState (p, LearnState::Capturing));
            feedRealMaterial (p, material.bass, material.kick, 512);
            expect (p.stopLearn());
            expect (waitUntilNotBusy (p));

            if (p.getLearnState() != LearnState::ResultReady)
            {
                logMessage ("Skipping determinism check: Learn did not reach ResultReady.");
            }
            else
            {
                expect (p.applyLatestLearnResult());

                std::vector<float> firstPass;
                feedRealMaterial (p, material.bass, material.kick, 512, &firstPass);
                expect (p.getActiveDynamicMapSourceForTesting() == DynamicMapSource::NewDynamicStateMap);

                // Clone the learned/applied state onto an independent instance
                // via the normal save/reload path, instead of re-running Learn,
                // so this isolates runtime determinism from Learn-worker timing.
                juce::MemoryBlock state;
                p.getStateInformation (state);

                KickLockAudioProcessor p2;
                p2.enableAllBuses();
                p2.setRateAndBufferSizeDetails (kSampleRate, 512);
                p2.prepareToPlay (kSampleRate, 512);
                p2.setStateInformation (state.getData(), (int) state.getSize());

                std::vector<float> secondPass;
                feedRealMaterial (p2, material.bass, material.kick, 512, &secondPass);

                expect (p2.getActiveDynamicMapSourceForTesting() == DynamicMapSource::NewDynamicStateMap,
                        "save/reload keeps New Dynamic ahead of legacy compatibility");

                expect (firstPass.size() == secondPass.size());
                const size_t compareLength = std::min (firstPass.size(), secondPass.size());
                for (size_t i = 0; i < compareLength; ++i)
                    expectWithinAbsoluteError (firstPass[i], secondPass[i], 1.0e-6f);
            }
        }

        beginTest ("Learn -> Apply survives representative host block sizes without hangs or non-finite output");
        for (const int blockSize : { 64, 128, 512, 2048 })
        {
            KickLockAudioProcessor p;
            p.enableAllBuses();
            p.setRateAndBufferSizeDetails (kSampleRate, blockSize);
            p.prepareToPlay (kSampleRate, blockSize);
            setValue (p, "correction_mode", 1.0f);
            setValue (p, "dynamic_strength", 1.0f);

            expect (p.beginLearn());
            expect (waitForState (p, LearnState::Capturing));
            feedRealMaterial (p, material.bass, material.kick, blockSize);
            expect (p.stopLearn());
            expect (waitUntilNotBusy (p), "block=" + juce::String (blockSize) + " should not hang");

            if (p.getLearnState() == LearnState::ResultReady)
                expect (p.applyLatestLearnResult());

            std::vector<float> output;
            feedRealMaterial (p, material.bass, material.kick, blockSize, &output);
            expect (allFinite (output), "block=" + juce::String (blockSize) + " must stay finite");
        }
    }
};

static RealAudioPluginIntegrationTests realAudioPluginIntegrationTests;
