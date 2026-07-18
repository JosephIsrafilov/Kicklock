#include "TestCommon.h"

#include "DynamicReleaseFixture.h"
#include "TestAllocationCounter.h"

#include "dsp/DynamicFingerprintMatcher.h"
#include "dsp/DynamicLearnFormation.h"
#include "dsp/DynamicProductionRuntime.h"
#include "dsp/DynamicStateSerialization.h"
#include "ui/DynamicWorkspace.h"

#include <chrono>
#include <limits>

namespace
{
    DynamicFingerprintPrototype releasePrototype (float seed)
    {
        DynamicFingerprintPrototype result;
        result.valid = true;
        result.featureCount = DynamicFingerprintContract::kFeatureCount;
        for (int i = 0; i < DynamicFingerprintContract::kFeatureCount; ++i)
            result.features[(size_t) i] = std::clamp (seed + 0.003f * (float) i, -0.95f, 0.95f);
        return result;
    }

    DynamicLearnHit releaseHit (int sequence, int64_t trigger, float seed,
                                bool correctionCapable, int midi = -1)
    {
        DynamicLearnHit result;
        result.sequence = sequence;
        result.triggerSample = trigger;
        result.fingerprint = releasePrototype (seed);
        result.fingerprintValidity = DynamicFingerprintValidity::Valid;
        result.timingEligible = true;
        result.absoluteDelayMs = -1.8f + 0.7f * std::floor ((seed + 1.0f) * 2.0f);
        result.timingConfidence = 0.9f;
        result.lowBandEnergy = 1.0f;
        result.correctionBeneficial = correctionCapable;
        result.allpassFreqHz = 90.0f + 10.0f * std::floor ((seed + 1.0f) * 2.0f);
        result.allpassQ = 0.8f;
        result.hasLikelyMidi = midi >= 0;
        result.likelyMidi = midi;
        return result;
    }

    DynamicStateMap makeReleaseMap()
    {
        std::vector<DynamicLearnHit> hits;
        for (int family = 0; family < 4; ++family)
            for (int repeat = 0; repeat < 5; ++repeat)
                hits.push_back (releaseHit (family * 10 + repeat, family * 10000 + repeat,
                                            -0.78f + 0.39f * (float) family + 0.001f * (float) repeat,
                                            true, -1));
        for (int repeat = 0; repeat < 5; ++repeat)
            hits.push_back (releaseHit (50 + repeat, 50000 + repeat, 0.74f + 0.001f * (float) repeat,
                                        false, -1));

        DynamicLearnFormationContext context;
        context.sampleRate = 48000.0;
        context.crossoverEnabled = true;
        context.crossoverHz = 150.0f;
        context.allpassEnabled = true;
        context.fallbackAllpassFreqHz = 100.0f;
        context.fallbackAllpassQ = 0.7f;
        const auto formed = formDynamicStateMap (hits, context);
        return formed.map;
    }

    DynamicFingerprintObservation observationFor (const DynamicFingerprintPrototype& prototype)
    {
        DynamicFingerprintObservation observation;
        observation.fingerprint.validity = DynamicFingerprintValidity::Valid;
        observation.fingerprint.extractorVersion = DynamicFingerprintContract::kExtractorVersion;
        observation.fingerprint.featureCount = DynamicFingerprintContract::kFeatureCount;
        observation.fingerprint.features = prototype.features;
        return observation;
    }

    void setParameter (KickLockAudioProcessor& processor, const char* id, float value)
    {
        if (auto* parameter = processor.apvts.getParameter (id))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    }

    void prepare (KickLockAudioProcessor& processor, double sampleRate, int blockSize)
    {
        processor.enableAllBuses();
        processor.setRateAndBufferSizeDetails (sampleRate, blockSize);
        processor.prepareToPlay (sampleRate, blockSize);
    }

    bool finiteAndBounded (const std::vector<float>& samples, float limit = 8.0f)
    {
        for (const auto sample : samples)
            if (! std::isfinite (sample) || std::abs (sample) > limit)
                return false;
        return true;
    }

    void driveProcessor (KickLockAudioProcessor& processor, const DynamicReleaseFixture& fixture,
                         int blockSize, std::vector<float>* output = nullptr)
    {
        const int channels = juce::jmax (processor.getTotalNumInputChannels(), processor.getTotalNumOutputChannels());
        juce::AudioBuffer<float> buffer (channels, blockSize);
        juce::MidiBuffer midi;
        for (int offset = 0; offset < (int) fixture.bass.size(); offset += blockSize)
        {
            const int count = std::min (blockSize, (int) fixture.bass.size() - offset);
            buffer.clear();
            for (int i = 0; i < count; ++i)
            {
                const auto bass = fixture.bass[(size_t) (offset + i)];
                const auto kick = fixture.kick[(size_t) (offset + i)];
                buffer.setSample (0, i, bass);
                if (channels > 1) buffer.setSample (1, i, bass);
                if (channels > 2) buffer.setSample (2, i, kick);
                if (channels > 3) buffer.setSample (3, i, kick);
            }
            processor.processBlock (buffer, midi);
            if (output != nullptr)
                for (int i = 0; i < count; ++i)
                    output->push_back (buffer.getSample (0, i));
        }
    }

    float normalizedDerivative (const std::vector<float>& samples)
    {
        constexpr float epsilon = 1.0e-4f;
        float maximum = 0.0f;
        for (size_t i = 1; i < samples.size(); ++i)
        {
            const float scale = std::max ({ epsilon, std::abs (samples[i]), std::abs (samples[i - 1]) });
            maximum = std::max (maximum, std::abs (samples[i] - samples[i - 1]) / scale);
        }
        return maximum;
    }
}

class DynamicReleaseGateTests : public juce::UnitTest
{
public:
    DynamicReleaseGateTests() : juce::UnitTest ("DynamicReleaseGates", "Phase11") {}

    void runTest() override
    {
        beginTest ("Deterministic fixture covers four comparable production-style families at every supported rate");
        for (const double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
        {
            const auto fixture = makeDynamicReleaseFixture (rate);
            expectEquals ((int) fixture.sampleRate, (int) rate);
            expect (! fixture.bass.empty() && fixture.bass.size() == fixture.kick.size());
            std::array<int, 5> mainCounts {};
            for (const auto& event : fixture.events)
            {
                if (event.family <= DynamicReleaseFixtureFamily::RecognizedGlobalOnly)
                    ++mainCounts[(size_t) event.family];
            }
            for (int count : mainCounts)
                expectGreaterOrEqual (count, 5, "each training family has five valid occurrences");
            const auto [minimum, maximum] = std::minmax_element (mainCounts.begin(), mainCounts.begin() + 4);
            expect (*minimum > 0 && (float) *maximum / (float) *minimum < 1.2f,
                    "no main family owns the material");
            expectEquals ((int) fixture.transportBoundaries.size(), 2);
        }

        beginTest ("Formation is deterministic, fingerprint-identified, and has no 60 percent dominant-family dependency");
        const auto formedMap = makeReleaseMap();
        expect (isStructurallyValidDynamicStateMap (formedMap));
        expectGreaterOrEqual (getOccupiedDynamicStateCount (formedMap), 5);
        int corrected = 0;
        int globalOnly = 0;
        for (const auto& state : formedMap.states)
        {
            if (! state.occupied)
                continue;
            expect (! state.hasLikelyMidi, "fixture semantics never require MIDI metadata");
            if (state.hasLearnedPackage)
            {
                ++corrected;
                expect (state.evidence == DynamicStateEvidence::Stable);
                expect (isValidDynamicZonePackage (state.learnedPackage));
            }
            else
                ++globalOnly;
        }
        expectGreaterOrEqual (corrected, 4);
        expectGreaterOrEqual (globalOnly, 1);

        beginTest ("Canonical fixture completes the actual Learn capture and New Dynamic pending-result lifecycle");
        {
            const auto fixture = makeDynamicReleaseFixture (48000.0);
            KickLockAudioProcessor processor;
            prepare (processor, 48000.0, 256);
            setParameter (processor, "correction_mode", 1.0f);
            expect (processor.beginLearn());
            driveProcessor (processor, fixture, 256);
            expect (processor.stopLearn());
            for (int attempt = 0; attempt < 15000 && learnStateIsBusy (processor.getLearnState()); ++attempt)
                juce::Thread::sleep (2);
            const auto resolved = processor.getPendingLearnResult();
            logMessage ("fixture Learn state=" + juce::String ((int) processor.getLearnState())
                        + " captured=" + juce::String (processor.getLearnProgress().capturedHits)
                        + " message=" + resolved.message);
            expect (processor.getLearnState() == LearnState::ResultReady,
                    "canonical deterministic fixture must resolve as New Dynamic Learn material");
            if (processor.getLearnState() == LearnState::ResultReady)
            {
                const auto pending = processor.getPendingLearnResult();
                expect (pending.hasDynamicStateMap && isStructurallyValidDynamicStateMap (pending.dynamicMap));
                expectGreaterOrEqual (getOccupiedDynamicStateCount (pending.dynamicMap), 4);
            }
        }

        beginTest ("Candidate, recognizable Global-only, Unknown, and Ambiguous outcomes retain frozen routing semantics");
        {
            auto routingMap = makeReleaseMap();
            int candidateSlot = -1;
            for (int slot = 0; slot < DynamicStateMapContract::kMaxPersistentStates; ++slot)
                if (routingMap.states[(size_t) slot].occupied && routingMap.states[(size_t) slot].hasLearnedPackage)
                {
                    candidateSlot = slot;
                    routingMap.states[(size_t) slot].evidence = DynamicStateEvidence::Candidate;
                    routingMap.states[(size_t) slot].hitCount = DynamicStateMapContract::kCandidateMinimumRepeatableHits;
                    break;
                }
            expect (candidateSlot >= 0);
            if (candidateSlot >= 0)
            {
                const auto candidate = matchDynamicFingerprint (observationFor (routingMap.states[(size_t) candidateSlot].fingerprint), routingMap);
                expect (candidate.decision == DynamicMatchDecision::Matched && ! candidate.correctionAvailable);
            }

            int noCorrectionSlot = -1;
            for (int slot = 0; slot < DynamicStateMapContract::kMaxPersistentStates; ++slot)
                if (routingMap.states[(size_t) slot].occupied && ! routingMap.states[(size_t) slot].hasLearnedPackage)
                    noCorrectionSlot = slot;
            expect (noCorrectionSlot >= 0);
            if (noCorrectionSlot >= 0)
            {
                routingMap.states[(size_t) noCorrectionSlot].evidence = DynamicStateEvidence::Stable;
                routingMap.states[(size_t) noCorrectionSlot].hitCount = DynamicStateMapContract::kStableAutoMinimumRepeatableHits;
                const auto globalOnlyMatch = matchDynamicFingerprint (observationFor (routingMap.states[(size_t) noCorrectionSlot].fingerprint), routingMap);
                expect (globalOnlyMatch.decision == DynamicMatchDecision::Matched && ! globalOnlyMatch.correctionAvailable);
            }

            DynamicFingerprintPrototype foreign = releasePrototype (0.98f);
            const auto unknown = matchDynamicFingerprint (observationFor (foreign), routingMap);
            expect (unknown.decision == DynamicMatchDecision::Unknown || unknown.decision == DynamicMatchDecision::Ambiguous);
        }

        beginTest ("New Dynamic processor lifecycle preserves PDC, source priority, snapshots, save/reload, and finite output");
        for (const auto [rate, block] : { std::pair { 44100.0, 7 }, std::pair { 48000.0, 64 },
                                           std::pair { 96000.0, 127 }, std::pair { 192000.0, 512 } })
        {
            const auto fixture = makeDynamicReleaseFixture (rate);
            auto lifecycleMap = makeReleaseMap();
            lifecycleMap.globalBase.learnedSampleRate = rate;
            KickLockAudioProcessor first;
            prepare (first, rate, block);
            setParameter (first, "correction_mode", 1.0f);
            setParameter (first, "dynamic_strength", 1.0f);
            expect (first.publishDynamicStateMapForTesting (lifecycleMap));
            std::vector<float> firstOutput;
            firstOutput.reserve (fixture.bass.size());
            driveProcessor (first, fixture, block, &firstOutput);
            expectEquals (first.getLatencySamples(), (int) std::ceil (rate * 0.020));
            expect (first.getActiveDynamicMapSourceForTesting() == DynamicMapSource::NewDynamicStateMap);
            expect (finiteAndBounded (firstOutput));
            const auto snapshot = first.getDynamicRuntimeSnapshotForTesting();
            expect (snapshot.mapValid && snapshot.source == DynamicMapSource::NewDynamicStateMap);
            expectEquals (snapshot.stateCount, getOccupiedDynamicStateCount (lifecycleMap));
            expect (! first.getActiveNoteMapForTesting().valid, "New Dynamic never fabricates a legacy note map");

            juce::MemoryBlock state;
            first.getStateInformation (state);
            KickLockAudioProcessor restored;
            prepare (restored, rate, block);
            restored.setStateInformation (state.getData(), (int) state.getSize());
            std::vector<float> restoredOutput;
            restoredOutput.reserve (fixture.bass.size());
            driveProcessor (restored, fixture, block, &restoredOutput);
            expect (restored.getActiveDynamicMapSourceForTesting() == DynamicMapSource::NewDynamicStateMap);
            expectEquals ((int64_t) restored.getActiveDynamicStateMapForTesting().states[0].stableStateId,
                          (int64_t) lifecycleMap.states[0].stableStateId);
            expect (finiteAndBounded (restoredOutput));
            expect (restored.getDynamicVerifiedMeasurementForTesting (lifecycleMap.states[0].stableStateId).availability
                    == DynamicMeasurementAvailability::Unavailable,
                    "Verified evidence is a fresh non-serialized runtime sidecar");
        }

        beginTest ("Block partitioning is semantically stable across the required 48 kHz matrix");
        {
            const auto fixture = makeDynamicReleaseFixture (48000.0);
            std::vector<float> reference;
            for (const int block : { 1, 7, 32, 64, 127, 256, 512, 2048 })
            {
                KickLockAudioProcessor processor;
                prepare (processor, 48000.0, block);
                setParameter (processor, "correction_mode", 1.0f);
                expect (processor.publishDynamicStateMapForTesting (makeReleaseMap()));
                std::vector<float> output;
                output.reserve (fixture.bass.size());
                driveProcessor (processor, fixture, block, &output);
                expect (finiteAndBounded (output));
                expect (processor.getActiveDynamicMapSourceForTesting() == DynamicMapSource::NewDynamicStateMap);
                if (reference.empty())
                    reference = output;
                else
                {
                    float maximumDifference = 0.0f;
                    for (size_t i = 0; i < output.size(); ++i)
                        maximumDifference = std::max (maximumDifference, std::abs (reference[i] - output[i]));
                    expectLessThan (maximumDifference, 2.0e-4f, "same-platform partitioned output stays within floating tolerance");
                }
            }
        }

        beginTest ("Continuity, strength, queue bounds, lifecycle, and callback allocation gates remain safe");
        {
            const auto fixture = makeDynamicReleaseFixture (48000.0);
            KickLockAudioProcessor processor;
            prepare (processor, 48000.0, 256);
            setParameter (processor, "correction_mode", 1.0f);
            auto allocationMap = makeReleaseMap();
            expect (processor.publishDynamicStateMapForTesting (allocationMap));
            driveProcessor (processor, fixture, 256);

            const int channels = juce::jmax (processor.getTotalNumInputChannels(), processor.getTotalNumOutputChannels());
            juce::AudioBuffer<float> buffer (channels, 256);
            juce::MidiBuffer midi;
            buffer.clear();
            processor.processBlock (buffer, midi); // warm all prepared branches and queues
            {
                ScopedTestAllocationCounter allocations;
                for (int iteration = 0; iteration < 16; ++iteration)
                {
                    buffer.clear();
                    processor.processBlock (buffer, midi);
                    processor.processBlockBypassed (buffer, midi);
                }
                const auto result = allocations.snapshot();
                expectEquals ((int) result.count, 0, "warmed processBlock and bypass callbacks allocate zero heap memory");
            }

            std::vector<float> rendered;
            rendered.reserve (fixture.bass.size());
            setParameter (processor, "dynamic_strength", 0.0f);
            driveProcessor (processor, fixture, 256, &rendered);
            setParameter (processor, "dynamic_strength", 0.5f);
            driveProcessor (processor, fixture, 256, &rendered);
            setParameter (processor, "dynamic_strength", 1.0f);
            driveProcessor (processor, fixture, 256, &rendered);
            expect (finiteAndBounded (rendered));
            expectLessThan (normalizedDerivative (rendered), 2.05f,
                            "normalized discontinuity remains bounded across runtime transitions");

            DynamicMeasurementScoreQueue queue;
            queue.prepare (DynamicMeasurementQueueContract::kScoreQueueCapacity);
            DynamicMeasurementScoredCapture score;
            for (int i = 0; i < DynamicMeasurementQueueContract::kScoreQueueCapacity + 1; ++i)
                queue.push (score);
            expectGreaterThan (queue.getDroppedCount(), 0, "adversarial queue overflow drops safely instead of overwriting");

            for (int iteration = 0; iteration < 3; ++iteration)
            {
                processor.releaseResources();
                processor.prepareToPlay (48000.0, 128 + iteration * 64);
            }
            expectEquals (processor.getLatencySamples(), 960);

            DynamicWorkspace workspace;
            workspace.setSize (1180, 820);
            workspace.setModel ({ });
            std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
            editor.reset();
        }
    }
};

static DynamicReleaseGateTests dynamicReleaseGateTests;
