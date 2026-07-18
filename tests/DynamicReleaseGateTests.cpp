#include "TestCommon.h"

#include "DynamicReleaseFixture.h"
#include "TestAllocationCounter.h"

#include "dsp/DynamicFingerprintMatcher.h"
#include "dsp/DynamicLearnFormation.h"
#include "dsp/DynamicProductionRuntime.h"
#include "dsp/DynamicSelectorScheduler.h"
#include "dsp/DynamicStateSerialization.h"
#include "ui/DynamicWorkspace.h"

#include <chrono>
#include <limits>

namespace
{
    constexpr auto kLearnWaitTimeout = std::chrono::seconds (30);
    constexpr int kContinuityWindowSamples = 24;
    constexpr float kContinuitySilenceEpsilon = 1.0e-4f;
    constexpr float kMaximumLocalizedTransitionScore = 4.0f;

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

    juce::String learnDiagnostics (KickLockAudioProcessor& processor)
    {
        const auto progress = processor.getLearnProgress();
        const auto pending = processor.getPendingLearnResult();
        return "state=" + juce::String ((int) processor.getLearnState())
             + " captured=" + juce::String (progress.capturedHits)
             + " drained=" + juce::String (progress.drainedHits)
             + " dropped=" + juce::String (progress.droppedQueueHits)
             + " timing=" + juce::String (progress.timingUsableHits)
             + " message=" + pending.message;
    }

    bool waitForLearnCapturing (KickLockAudioProcessor& processor, int blockSize)
    {
        const int channels = juce::jmax (processor.getTotalNumInputChannels(), processor.getTotalNumOutputChannels());
        juce::AudioBuffer<float> silence (channels, blockSize);
        juce::MidiBuffer midi;
        const auto deadline = std::chrono::steady_clock::now() + kLearnWaitTimeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (processor.getLearnState() == LearnState::Capturing)
                return true;
            if (! learnStateIsBusy (processor.getLearnState()))
                return false;
            // Preparing is acknowledged on the audio thread. Silence advances
            // that handshake without presenting a learnable kick/bass event.
            silence.clear();
            processor.processBlock (silence, midi);
            juce::Thread::sleep (2);
        }
        return processor.getLearnState() == LearnState::Capturing;
    }

    bool waitForLearnResolution (KickLockAudioProcessor& processor)
    {
        const auto deadline = std::chrono::steady_clock::now() + kLearnWaitTimeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (! learnStateIsBusy (processor.getLearnState()))
                return true;
            juce::Thread::sleep (2);
        }
        return ! learnStateIsBusy (processor.getLearnState());
    }

    bool finiteAndBounded (const std::vector<float>& samples, float limit = 8.0f)
    {
        for (const auto sample : samples)
            if (! std::isfinite (sample) || std::abs (sample) > limit)
                return false;
        return true;
    }

    void driveProcessor (KickLockAudioProcessor& processor, const DynamicReleaseFixture& fixture,
                         int blockSize, std::vector<float>* output = nullptr,
                         std::vector<DynamicRuntimeSnapshot>* snapshots = nullptr)
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
            if (snapshots != nullptr)
                snapshots->push_back (processor.getDynamicRuntimeSnapshotForTesting());
        }
    }

    float localizedTransitionScore (const std::vector<float>& samples, int boundary)
    {
        if (boundary < kContinuityWindowSamples || boundary + kContinuityWindowSamples >= (int) samples.size())
            return std::numeric_limits<float>::infinity();

        double derivativeEnergy = 0.0;
        double signalEnergy = 0.0;
        int derivativeCount = 0;
        for (int i = boundary - kContinuityWindowSamples + 1; i < boundary; ++i)
        {
            const float derivative = samples[(size_t) i] - samples[(size_t) (i - 1)];
            derivativeEnergy += (double) derivative * derivative;
            signalEnergy += (double) samples[(size_t) i] * samples[(size_t) i];
            ++derivativeCount;
        }
        for (int i = boundary + 2; i < boundary + kContinuityWindowSamples; ++i)
        {
            const float derivative = samples[(size_t) i] - samples[(size_t) (i - 1)];
            derivativeEnergy += (double) derivative * derivative;
            signalEnergy += (double) samples[(size_t) i] * samples[(size_t) i];
            ++derivativeCount;
        }

        const float derivativeRms = (float) std::sqrt (derivativeEnergy / (double) std::max (1, derivativeCount));
        const float signalRms = (float) std::sqrt (signalEnergy / (double) std::max (1, derivativeCount));
        const float scale = std::max ({ kContinuitySilenceEpsilon, derivativeRms * 2.0f, signalRms * 0.01f });
        return std::abs (samples[(size_t) boundary] - samples[(size_t) (boundary - 1)]) / scale;
    }

    std::array<uint64_t, DynamicStateMapContract::kMaxPersistentStates> stableIds (const DynamicStateMap& map)
    {
        std::array<uint64_t, DynamicStateMapContract::kMaxPersistentStates> ids {};
        for (int slot = 0; slot < DynamicStateMapContract::kMaxPersistentStates; ++slot)
            ids[(size_t) slot] = map.states[(size_t) slot].occupied ? map.states[(size_t) slot].stableStateId : 0;
        return ids;
    }

    bool sameCorrectionPackages (const DynamicStateMap& a, const DynamicStateMap& b)
    {
        for (int slot = 0; slot < DynamicStateMapContract::kMaxPersistentStates; ++slot)
        {
            const auto& left = a.states[(size_t) slot];
            const auto& right = b.states[(size_t) slot];
            if (left.occupied != right.occupied || left.stableStateId != right.stableStateId
                || left.hasLearnedPackage != right.hasLearnedPackage)
                return false;
            if (left.hasLearnedPackage
                && (std::abs (left.learnedPackage.delayDeltaMs - right.learnedPackage.delayDeltaMs) > 1.0e-6f
                    || std::abs (left.learnedPackage.allpassFreqHz - right.learnedPackage.allpassFreqHz) > 1.0e-6f
                    || std::abs (left.learnedPackage.allpassQ - right.learnedPackage.allpassQ) > 1.0e-6f))
                return false;
        }
        return true;
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
            const bool started = processor.beginLearn();
            expect (started);
            const bool capturing = started && waitForLearnCapturing (processor, 256);
            expect (capturing, "Learn must acknowledge Capturing before fixture events are fed: " + learnDiagnostics (processor));
            if (! capturing)
                return;
            driveProcessor (processor, fixture, 256);
            expect (processor.stopLearn());
            expect (waitForLearnResolution (processor), "Learn must resolve after Stop: " + learnDiagnostics (processor));
            const auto resolved = processor.getPendingLearnResult();
            logMessage ("fixture Learn " + learnDiagnostics (processor));
            expect (processor.getLearnState() == LearnState::ResultReady,
                    "canonical deterministic fixture must resolve as New Dynamic Learn material");
            if (processor.getLearnState() == LearnState::ResultReady)
            {
                const auto pending = processor.getPendingLearnResult();
                expect (pending.hasDynamicStateMap && isStructurallyValidDynamicStateMap (pending.dynamicMap));
                expectGreaterOrEqual (getOccupiedDynamicStateCount (pending.dynamicMap), 4);
            }
        }

        beginTest ("Candidate, Global-only, Unknown, and production Ambiguous/Hold outcomes retain frozen routing semantics");
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

            auto ambiguityMap = makeReleaseMap();
            for (int slot = 2; slot < DynamicStateMapContract::kMaxPersistentStates; ++slot)
                ambiguityMap.states[(size_t) slot] = {};
            ambiguityMap.states[0].fingerprint = releasePrototype (-0.20f);
            ambiguityMap.states[1].fingerprint = releasePrototype (0.20f);
            ambiguityMap.calibration.absoluteDistanceThreshold = 0.29f;
            ambiguityMap.calibration.ambiguityMargin = 0.10f;
            DynamicFingerprintPrototype midpoint;
            midpoint.valid = true;
            midpoint.featureCount = DynamicFingerprintContract::kFeatureCount;
            for (int feature = 0; feature < DynamicFingerprintContract::kFeatureCount; ++feature)
                midpoint.features[(size_t) feature] = 0.5f * (ambiguityMap.states[0].fingerprint.features[(size_t) feature]
                                                             + ambiguityMap.states[1].fingerprint.features[(size_t) feature]);
            const auto ambiguous = matchDynamicFingerprint (observationFor (midpoint), ambiguityMap);
            expect (ambiguous.decision == DynamicMatchDecision::Ambiguous,
                    "midpoint fingerprint must use the production Ambiguous decision");

            const auto unknown = matchDynamicFingerprint (observationFor (releasePrototype (0.80f)), ambiguityMap);
            expect (unknown.decision == DynamicMatchDecision::Unknown,
                    "foreign material remains Unknown and is not interchangeable with Ambiguous");

            DynamicSelectorScheduler selector;
            expect (selector.prepare (48000.0));
            DynamicSelectorBranchRoster roster;
            const double globalTap = std::ceil (48000.0 * 0.020);
            roster.global = { true, DynamicHotBranchKind::Global, 0, 0.0, globalTap, true, true };
            roster.states[0] = { true, DynamicHotBranchKind::State, ambiguityMap.states[0].stableStateId,
                                 0.0, globalTap + 400.0, true, true };
            const auto matched = matchDynamicFingerprint (observationFor (ambiguityMap.states[0].fingerprint), ambiguityMap);
            auto advanceEvent = [&] (const DynamicMatchResult& match)
            {
                const int64_t trigger = selector.getExpectedNextSample();
                DynamicSelectorEvent event { trigger, trigger + selector.getFingerprintSamples(), match };
                expect (selector.submitEvent (event));
                while (selector.getExpectedNextSample() <= event.readySample)
                    expect (selector.advanceSample (selector.getExpectedNextSample(), roster));
                while (selector.getDiagnostics().fadeActive)
                    expect (selector.advanceSample (selector.getExpectedNextSample(), roster));
            };
            advanceEvent (matched);
            expect (selector.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::State);
            advanceEvent (ambiguous);
            expect (selector.getDiagnostics().lastDecision == DynamicSelectorDiagnostic::HeldAmbiguous);
            expectEquals ((int) selector.getDiagnostics().holdEventCount, 1);
            advanceEvent (ambiguous);
            expectEquals ((int) selector.getDiagnostics().holdEventCount, 2);
            advanceEvent (ambiguous);
            expect (selector.getDiagnostics().lastDecision == DynamicSelectorDiagnostic::FallbackAmbiguous);
            expect (selector.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::Global,
                    "bounded Hold releases stale State correction after two events");
        }

        beginTest ("Actual Learn applies its emitted Dynamic map through runtime, measurements, and save/reload");
        {
            const auto learnFixture = makeDynamicReleaseFixture (48000.0);
            const auto runtimeFixture = makeDynamicReleaseFixture (48000.0);
            KickLockAudioProcessor first;
            prepare (first, 48000.0, 256);
            setParameter (first, "correction_mode", 1.0f);
            setParameter (first, "dynamic_strength", 1.0f);

            const bool started = first.beginLearn();
            expect (started);
            const bool capturing = started && waitForLearnCapturing (first, 256);
            expect (capturing, "actual Learn must reach Capturing before input: " + learnDiagnostics (first));
            if (! capturing)
                return;
            driveProcessor (first, learnFixture, 256);
            expect (first.stopLearn());
            expect (waitForLearnResolution (first), "actual Learn must finalize: " + learnDiagnostics (first));
            expect (first.getLearnState() == LearnState::ResultReady, learnDiagnostics (first));

            const auto pending = first.getPendingLearnResult();
            expect (pending.hasDynamicStateMap && isStructurallyValidDynamicStateMap (pending.dynamicMap));
            const int learnedStateCount = getOccupiedDynamicStateCount (pending.dynamicMap);
            const auto learnedIds = stableIds (pending.dynamicMap);
            expectGreaterOrEqual (learnedStateCount, 4);
            juce::String learnedIdLog;
            for (const auto& state : pending.dynamicMap.states)
                if (state.occupied)
                    learnedIdLog += (learnedIdLog.isEmpty() ? "" : ",")
                                  + juce::String ((int64_t) state.stableStateId)
                                  + ":" + juce::String ((int) state.evidence)
                                  + ":" + juce::String (state.hasLearnedPackage ? 1 : 0);
            logMessage ("actual Learn formed states=" + juce::String (learnedStateCount)
                        + " stableIds=" + learnedIdLog);
            expect (first.applyLatestLearnResult(), first.getLearnApplyBlockedReason());

            std::vector<float> firstOutput;
            std::vector<DynamicRuntimeSnapshot> firstSnapshots;
            firstOutput.reserve (runtimeFixture.bass.size());
            firstSnapshots.reserve (runtimeFixture.bass.size() / 256 + 1);
            driveProcessor (first, runtimeFixture, 256, &firstOutput, &firstSnapshots);
            expect (first.getActiveDynamicMapSourceForTesting() == DynamicMapSource::NewDynamicStateMap);
            expect (finiteAndBounded (firstOutput));
            expect (! firstSnapshots.empty());
            const auto runtimeSnapshot = first.getDynamicRuntimeSnapshotForTesting();
            expect (runtimeSnapshot.mapValid && runtimeSnapshot.source == DynamicMapSource::NewDynamicStateMap);
            expectEquals (runtimeSnapshot.stateCount, learnedStateCount);

            int predictedStates = 0;
            for (const auto id : learnedIds)
            {
                if (id == 0)
                    continue;
                const auto predicted = first.getDynamicPredictedMeasurementForTesting (id);
                expect (isValidDynamicMeasurementSummary (predicted));
                predictedStates += predicted.availability != DynamicMeasurementAvailability::Unavailable ? 1 : 0;
            }
            expectGreaterThan (predictedStates, 0, "actual Learn contributes predicted, not fabricated Verified, measurements");

            // Runtime verification must be generated from fresh audible events, not
            // copied from Learn. Replaying the prepared fixture supplies enough
            // valid events for the runtime capture/worker handoff path to run.
            for (int pass = 0; pass < 4; ++pass)
                driveProcessor (first, runtimeFixture, 256);
            const auto runtimeDiagnostics = first.getDynamicProductionDiagnosticsForTesting();
            expectGreaterThan ((int64_t) runtimeDiagnostics.completedObservations, (int64_t) 0);

            juce::MemoryBlock state;
            first.getStateInformation (state);
            KickLockAudioProcessor restored;
            prepare (restored, 48000.0, 256);
            restored.setStateInformation (state.getData(), (int) state.getSize());

            std::vector<float> restoredOutput;
            std::vector<DynamicRuntimeSnapshot> restoredSnapshots;
            restoredOutput.reserve (runtimeFixture.bass.size());
            restoredSnapshots.reserve (runtimeFixture.bass.size() / 256 + 1);
            driveProcessor (restored, runtimeFixture, 256, &restoredOutput, &restoredSnapshots);
            expect (restored.getActiveDynamicMapSourceForTesting() == DynamicMapSource::NewDynamicStateMap);
            expect (stableIds (restored.getActiveDynamicStateMapForTesting()) == learnedIds);
            expect (sameCorrectionPackages (restored.getActiveDynamicStateMapForTesting(), pending.dynamicMap));
            expect (finiteAndBounded (restoredOutput));
            expectEquals ((int) restoredOutput.size(), (int) firstOutput.size());
            float maximumDifference = 0.0f;
            for (size_t i = 0; i < std::min (firstOutput.size(), restoredOutput.size()); ++i)
                maximumDifference = std::max (maximumDifference, std::abs (firstOutput[i] - restoredOutput[i]));
            expectLessThan (maximumDifference, 2.0e-4f, "same-platform saved New Dynamic map renders deterministically");

            const auto restoredSnapshot = restored.getDynamicRuntimeSnapshotForTesting();
            expect (restoredSnapshot.mapValid && restoredSnapshot.source == runtimeSnapshot.source);
            expect (restoredSnapshot.activeBranchKind == runtimeSnapshot.activeBranchKind);
            expectEquals ((int64_t) restoredSnapshot.selectedSemanticStateId,
                          (int64_t) runtimeSnapshot.selectedSemanticStateId);
            expect (restoredSnapshot.selectorDiagnostic == runtimeSnapshot.selectorDiagnostic);
            for (const auto id : learnedIds)
                if (id != 0)
                    expect (restored.getDynamicVerifiedMeasurementForTesting (id).availability
                            == DynamicMeasurementAvailability::Unavailable,
                            "Verified runtime evidence is fresh after restore");
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

        beginTest ("Fixture transport boundaries reset Dynamic history without stale State, Service, or measurement leakage");
        {
            const auto fixture = makeDynamicReleaseFixture (48000.0);
            auto transportMap = makeReleaseMap();
            for (int slot = 1; slot < DynamicStateMapContract::kMaxPersistentStates; ++slot)
                transportMap.states[(size_t) slot] = {};
            transportMap.states[0].fingerprint = releasePrototype (0.0f);
            transportMap.calibration.absoluteDistanceThreshold = 1.0f;
            transportMap.calibration.ambiguityMargin = 0.001f;

            DynamicProductionRuntime runtime;
            expect (runtime.prepare (48000.0, 128, 1));
            runtime.activateMap (transportMap);
            juce::AudioBuffer<float> input (1, 128);
            juce::AudioBuffer<float> output (1, 128);
            const auto renderRange = [&] (int begin, int end)
            {
                for (int offset = begin; offset < end; offset += 128)
                {
                    const int count = std::min (128, end - offset);
                    for (int sample = 0; sample < count; ++sample)
                        input.setSample (0, sample, fixture.bass[(size_t) (offset + sample)]);
                    expect (runtime.process (input, fixture.bass.data() + offset, fixture.kick.data() + offset,
                                             true, 1.0, output, count));
                    bool finite = true;
                    for (int sample = 0; sample < count; ++sample)
                        finite = finite && std::isfinite (output.getSample (0, sample));
                    expect (finite, "transport fixture output remains finite");
                }
            };

            int previousBoundary = 0;
            const std::array<DynamicProductionTransportReason, 3> resets {
                DynamicProductionTransportReason::StopStart,
                DynamicProductionTransportReason::Seek,
                DynamicProductionTransportReason::HostReset
            };
            for (int boundaryIndex = 0; boundaryIndex < (int) fixture.transportBoundaries.size(); ++boundaryIndex)
            {
                const int boundary = fixture.transportBoundaries[(size_t) boundaryIndex];
                renderRange (previousBoundary, boundary);
                expectEquals (runtime.getReportedLatencySamples(), 960);
                runtime.notifyTransportReset (resets[(size_t) boundaryIndex]);
                expectEquals ((int64_t) runtime.getSelectedStableStateId(), (int64_t) 0,
                             "pre-boundary State identity cannot leak across fixture transport reset");
                expect (! runtime.getServiceBindingValid(), "pre-boundary Service binding cannot leak across reset");
                previousBoundary = boundary;
            }
            renderRange (previousBoundary, (int) fixture.bass.size());
            const auto beforeLoop = runtime.getSelectedStableStateId();
            runtime.notifyTransportReset (DynamicProductionTransportReason::LoopWrap);
            expectEquals ((int64_t) runtime.getSelectedStableStateId(), (int64_t) beforeLoop,
                         "loop wrap preserves frozen continuity/history policy");
            runtime.notifyTransportReset (DynamicProductionTransportReason::Seek);
            expectEquals ((int64_t) runtime.getSelectedStableStateId(), (int64_t) 0);
            expectEquals (runtime.getReportedLatencySamples(), 960);
        }

        beginTest ("Continuity, strength, queue bounds, lifecycle, and callback allocation gates remain safe");
        {
            beginTest ("Allocation counter observes scalar, array, and over-aligned allocations only while scoped");
            struct alignas (64) OverAlignedAllocation { std::array<float, 16> values {}; };
            expect (! ScopedTestAllocationCounter::isTracking());
            {
                ScopedTestAllocationCounter allocations;
                auto* scalar = new int (7);
                auto* array = new int[8];
                auto* aligned = new OverAlignedAllocation;
                auto* noThrowScalar = new (std::nothrow) int (8);
                auto* noThrowArray = new (std::nothrow) int[8];
                auto* noThrowAligned = new (std::nothrow) OverAlignedAllocation;
                const auto result = allocations.snapshot();
                expectGreaterOrEqual ((int) result.count, 6);
                expectGreaterThan ((int64_t) result.bytes, (int64_t) 0);
                delete scalar;
                delete[] array;
                delete aligned;
                delete noThrowScalar;
                delete[] noThrowArray;
                delete noThrowAligned;
            }
            expect (! ScopedTestAllocationCounter::isTracking(), "counting is disabled outside ScopedTestAllocationCounter");

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
            const auto fillFixtureBlock = [&] (int offset)
            {
                buffer.clear();
                for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                {
                    const int fixtureIndex = (offset + sample) % (int) fixture.bass.size();
                    const float bass = fixture.bass[(size_t) fixtureIndex];
                    const float kick = fixture.kick[(size_t) fixtureIndex];
                    buffer.setSample (0, sample, bass);
                    if (channels > 1) buffer.setSample (1, sample, bass);
                    if (channels > 2) buffer.setSample (2, sample, kick);
                    if (channels > 3) buffer.setSample (3, sample, kick);
                }
            };
            for (int warm = 0; warm < 24; ++warm)
            {
                fillFixtureBlock (warm * buffer.getNumSamples());
                processor.processBlock (buffer, midi);
            }
            expectGreaterThan ((int64_t) processor.getDynamicProductionDiagnosticsForTesting().acceptedCaptures, (int64_t) 0,
                               "allocation gate warms real non-zero Dynamic matching material before measurement");
            {
                ScopedTestAllocationCounter allocations;
                for (int iteration = 0; iteration < 16; ++iteration)
                {
                    fillFixtureBlock (iteration * buffer.getNumSamples());
                    processor.processBlock (buffer, midi);
                    processor.processBlockBypassed (buffer, midi);
                }
                const auto result = allocations.snapshot();
                expectEquals ((int) result.count, 0, "warmed processBlock and bypass callbacks allocate zero heap memory");
                expectEquals ((int64_t) result.bytes, (int64_t) 0,
                              "warmed non-zero processBlock and bypass callbacks allocate zero bytes");
            }

            beginTest ("Localized transition continuity metric accepts smooth production-style fades and rejects an injected click");
            std::vector<float> smooth (512, 0.0f);
            for (int sample = 0; sample < (int) smooth.size(); ++sample)
            {
                const float t = (float) sample / (float) (smooth.size() - 1);
                const float global = 0.35f * std::sin ((float) (kTwoPi * 3.0 * t));
                const float state = 0.35f * std::sin ((float) (kTwoPi * 3.0 * t + 0.20));
                const float fade = juce::jlimit (0.0f, 1.0f, ((float) sample - 180.0f) / 120.0f);
                smooth[(size_t) sample] = global + fade * (state - global);
            }
            const int transitionBoundary = 180;
            const float smoothScore = localizedTransitionScore (smooth, transitionBoundary);
            expectLessThan (smoothScore, kMaximumLocalizedTransitionScore,
                             "smooth Global/State-style crossfade remains below the localized click threshold");

            auto clicked = smooth;
            clicked[(size_t) transitionBoundary] += 2.0f;
            const float clickScore = localizedTransitionScore (clicked, transitionBoundary);
            expectGreaterThan (clickScore, kMaximumLocalizedTransitionScore,
                               "deterministic positive-control click exceeds the localized transition threshold");

            std::vector<float> genuineOutput;
            genuineOutput.reserve (fixture.bass.size() * 2);
            setParameter (processor, "dynamic_strength", 0.0f);
            driveProcessor (processor, fixture, 256, &genuineOutput);
            const int stateBoundary = (int) genuineOutput.size();
            setParameter (processor, "dynamic_strength", 1.0f);
            driveProcessor (processor, fixture, 256, &genuineOutput);
            expect (finiteAndBounded (genuineOutput));
            const int delayedStateBoundary = stateBoundary + processor.getLatencySamples();
            expectLessThan (localizedTransitionScore (genuineOutput, delayedStateBoundary), kMaximumLocalizedTransitionScore,
                             "actual processor strength retarget remains continuous at the known render boundary");

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
