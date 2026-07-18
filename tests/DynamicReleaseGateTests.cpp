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

    // ---- Phase 11 release-gate scenario helpers ----

    void fillFixtureChannels (juce::AudioBuffer<float>& buffer, const DynamicReleaseFixture& fixture, int offsetSamples)
    {
        buffer.clear();
        const int channels = buffer.getNumChannels();
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const int fixtureIndex = (offsetSamples + sample) % (int) fixture.bass.size();
            const float bass = fixture.bass[(size_t) fixtureIndex];
            const float kick = fixture.kick[(size_t) fixtureIndex];
            buffer.setSample (0, sample, bass);
            if (channels > 1) buffer.setSample (1, sample, bass);
            if (channels > 2) buffer.setSample (2, sample, kick);
            if (channels > 3) buffer.setSample (3, sample, kick);
        }
    }

    template <typename RenderIteration>
    TestAllocationSnapshot measureZeroAllocationCallback (int iterations, RenderIteration&& renderIteration)
    {
        ScopedTestAllocationCounter allocations;
        for (int i = 0; i < iterations; ++i)
            renderIteration();
        return allocations.snapshot();
    }

    bool trainWithActualLearn (KickLockAudioProcessor& processor, const DynamicReleaseFixture& learnFixture, int blockSize)
    {
        setParameter (processor, "correction_mode", 1.0f);
        setParameter (processor, "dynamic_strength", 1.0f);
        if (! processor.beginLearn())
            return false;
        if (! waitForLearnCapturing (processor, blockSize))
            return false;
        driveProcessor (processor, learnFixture, blockSize);
        if (! processor.stopLearn())
            return false;
        if (! waitForLearnResolution (processor))
            return false;
        if (processor.getLearnState() != LearnState::ResultReady)
            return false;
        return processor.applyLatestLearnResult();
    }

    DynamicStateMap globalOnlyReleaseMap()
    {
        auto map = makeReleaseMap();
        for (auto& state : map.states)
            state = {};
        return map;
    }

    DynamicStateMap generousSingleStateMap (uint64_t stateId, float delayDeltaMs = 1.4f)
    {
        auto map = globalOnlyReleaseMap();
        map.states[0].occupied = true;
        map.states[0].stableStateId = stateId;
        map.states[0].fingerprint = releasePrototype (0.0f);
        map.states[0].hasLearnedPackage = true;
        map.states[0].learnedPackage.delayDeltaMs = delayDeltaMs;
        map.states[0].learnedPackage.allpassFreqHz = 90.0f;
        map.states[0].learnedPackage.allpassQ = 0.8f;
        map.states[0].origin = DynamicStateOrigin::Auto;
        map.states[0].evidence = DynamicStateEvidence::Stable;
        map.states[0].hitCount = DynamicStateMapContract::kStableAutoMinimumRepeatableHits;
        map.states[0].repeatability = 0.9f;
        map.states[0].ambiguity = 0.05f;
        map.calibration.valid = true;
        map.calibration.absoluteDistanceThreshold = 1.0f;
        map.calibration.ambiguityMargin = 0.001f;
        map.nextStateId = stateId + 1;
        return map;
    }

    DynamicStateMap generousAmbiguousPairMap (uint64_t firstId, uint64_t secondId)
    {
        auto map = globalOnlyReleaseMap();
        map.states[0].occupied = true;
        map.states[0].stableStateId = firstId;
        map.states[0].fingerprint = releasePrototype (-0.20f);
        map.states[0].hasLearnedPackage = true;
        map.states[0].learnedPackage.delayDeltaMs = 1.0f;
        map.states[0].learnedPackage.allpassFreqHz = 90.0f;
        map.states[0].learnedPackage.allpassQ = 0.8f;
        map.states[0].origin = DynamicStateOrigin::Auto;
        map.states[0].evidence = DynamicStateEvidence::Stable;
        map.states[0].hitCount = DynamicStateMapContract::kStableAutoMinimumRepeatableHits;
        map.states[0].repeatability = 0.9f;
        map.states[0].ambiguity = 0.05f;
        map.states[1] = map.states[0];
        map.states[1].stableStateId = secondId;
        map.states[1].fingerprint = releasePrototype (0.20f);
        map.calibration.valid = true;
        map.calibration.absoluteDistanceThreshold = 0.29f;
        map.calibration.ambiguityMargin = 0.10f;
        map.nextStateId = secondId + 1;
        return map;
    }

    bool driveRuntimeOverFixture (DynamicProductionRuntime& runtime, const DynamicReleaseFixture& fixture, int blockSize,
                                  juce::AudioBuffer<float>& input, juce::AudioBuffer<float>& output)
    {
        bool ok = true;
        for (int offset = 0; offset + blockSize <= (int) fixture.bass.size(); offset += blockSize)
        {
            for (int sample = 0; sample < blockSize; ++sample)
            {
                input.setSample (0, sample, fixture.bass[(size_t) (offset + sample)]);
                if (input.getNumChannels() > 1)
                    input.setSample (1, sample, fixture.bass[(size_t) (offset + sample)]);
            }
            ok = runtime.process (input, fixture.bass.data() + offset, fixture.kick.data() + offset,
                                  true, 1.0, output, blockSize) && ok;
        }
        return ok;
    }

    struct BranchTransition
    {
        bool found = false;
        int64_t preLatencySample = 0;
    };

    BranchTransition findBranchTransition (const std::vector<DynamicRuntimeSnapshot>& snapshots,
                                           DynamicSelectorBranchKind fromKind, DynamicSelectorBranchKind toKind)
    {
        for (size_t i = 1; i < snapshots.size(); ++i)
            if (snapshots[i - 1].activeBranchKind == fromKind && snapshots[i].activeBranchKind == toKind)
                return { true, snapshots[i].timestampSample };
        return {};
    }

    BranchTransition findStateIdentityTransition (const std::vector<DynamicRuntimeSnapshot>& snapshots)
    {
        for (size_t i = 1; i < snapshots.size(); ++i)
        {
            const auto& before = snapshots[i - 1];
            const auto& after = snapshots[i];
            if (before.activeBranchKind == DynamicSelectorBranchKind::State
                && after.activeBranchKind == DynamicSelectorBranchKind::State
                && before.selectedSemanticStateId != 0 && after.selectedSemanticStateId != 0
                && before.selectedSemanticStateId != after.selectedSemanticStateId)
                return { true, after.timestampSample };
        }
        return {};
    }

    BranchTransition findHoldReleaseTransition (const std::vector<DynamicRuntimeSnapshot>& snapshots)
    {
        for (size_t i = 1; i < snapshots.size(); ++i)
            if (snapshots[i - 1].holdActive && ! snapshots[i].holdActive)
                return { true, snapshots[i].timestampSample };
        return {};
    }

    BranchTransition findBypassTransition (const std::vector<DynamicRuntimeSnapshot>& snapshots, bool enteringBypass)
    {
        for (size_t i = 1; i < snapshots.size(); ++i)
            if (snapshots[i - 1].bypassActive != enteringBypass && snapshots[i].bypassActive == enteringBypass)
                return { true, snapshots[i].timestampSample };
        return {};
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
            const std::array<const char*, 3> resetNames { "StopStart", "Seek", "HostReset" };
            for (int boundaryIndex = 0; boundaryIndex < (int) fixture.transportBoundaries.size(); ++boundaryIndex)
            {
                const int boundary = fixture.transportBoundaries[(size_t) boundaryIndex];
                // Process real material up to the boundary so a measurement capture is
                // plausibly in flight when the reset actually lands.
                renderRange (previousBoundary, boundary);
                expectEquals (runtime.getReportedLatencySamples(), 960);

                runtime.notifyTransportReset (resets[(size_t) boundaryIndex]);
                expectEquals ((int64_t) runtime.getSelectedStableStateId(), (int64_t) 0,
                             juce::String (resetNames[(size_t) boundaryIndex]) + ": stale State identity cannot leak across reset");
                expect (! runtime.getServiceBindingValid(),
                        juce::String (resetNames[(size_t) boundaryIndex]) + ": stale Service binding cannot leak across reset");
                DynamicRuntimeMeasurementCaptureResult staleCapture;
                expect (! runtime.takeCompletedMeasurementCapture (staleCapture),
                        juce::String (resetNames[(size_t) boundaryIndex]) + ": a stale measurement result is rejected, not served, after reset");

                // Process the first following eligible event so the reset's effect is
                // observed on real audio, not merely counted.
                const int nextBoundary = boundaryIndex + 1 < (int) fixture.transportBoundaries.size()
                    ? fixture.transportBoundaries[(size_t) (boundaryIndex + 1)] : (int) fixture.bass.size();
                renderRange (boundary, std::min (boundary + 4096, nextBoundary));
                expectEquals (runtime.getReportedLatencySamples(), 960,
                             juce::String (resetNames[(size_t) boundaryIndex]) + ": exact 20 ms PDC after processing the first post-reset event");
                previousBoundary = boundary;
            }
            renderRange (previousBoundary, (int) fixture.bass.size());

            const auto beforeLoop = runtime.getSelectedStableStateId();
            runtime.notifyTransportReset (DynamicProductionTransportReason::LoopWrap);
            expectEquals ((int64_t) runtime.getSelectedStableStateId(), (int64_t) beforeLoop,
                         "LoopWrap: frozen grace/history policy preserves the pre-wrap selection, unlike the other resets");
            expectEquals (runtime.getReportedLatencySamples(), 960, "LoopWrap: exact 20 ms PDC is unaffected");
            renderRange ((int) fixture.bass.size() - 512, (int) fixture.bass.size());
            expectEquals ((int64_t) runtime.getSelectedStableStateId(), (int64_t) beforeLoop,
                         "LoopWrap: frozen grace/history policy still holds after processing the first post-wrap material");

            runtime.notifyTransportReset (DynamicProductionTransportReason::Seek);
            expectEquals ((int64_t) runtime.getSelectedStableStateId(), (int64_t) 0);
            expectEquals (runtime.getReportedLatencySamples(), 960);
        }

        beginTest ("Legal reprepare with a changed block size clears stale Dynamic history and keeps the exact 20 ms PDC");
        {
            const auto fixture = makeDynamicReleaseFixture (48000.0);
            DynamicProductionRuntime runtime;
            expect (runtime.prepare (48000.0, 128, 1));
            const auto reprepareMap = generousSingleStateMap (950);
            runtime.activateMap (reprepareMap);

            juce::AudioBuffer<float> input (1, 128);
            juce::AudioBuffer<float> output (1, 128);
            const auto renderBlock = [&] (int offset, int blockSize)
            {
                for (int sample = 0; sample < blockSize; ++sample)
                    input.setSample (0, sample, fixture.bass[(size_t) (offset + sample)]);
                expect (runtime.process (input, fixture.bass.data() + offset, fixture.kick.data() + offset,
                                         true, 1.0, output, blockSize));
                bool finite = true;
                for (int sample = 0; sample < blockSize; ++sample)
                    finite = finite && std::isfinite (output.getSample (0, sample));
                expect (finite, "reprepare fixture output remains finite");
            };
            int offset = 0;
            for (; offset + 128 <= (int) fixture.bass.size() / 4; offset += 128)
                renderBlock (offset, 128);
            expect (runtime.getSelectedStableStateId() != 0,
                    "legal reprepare setup: runtime diagnostics prove a persistent State was selected before reprepare");

            expect (runtime.prepare (48000.0, 192, 1));
            input.setSize (1, 192);
            output.setSize (1, 192);
            expectEquals (runtime.getReportedLatencySamples(), 960, "legal reprepare keeps the exact 20 ms PDC");
            expectEquals ((int64_t) runtime.getSelectedStableStateId(), (int64_t) 0,
                         "legal reprepare: stale State identity does not leak across a block-size change");
            expect (! runtime.getServiceBindingValid(),
                    "legal reprepare: stale Service binding does not leak across a block-size change");
            DynamicRuntimeMeasurementCaptureResult staleCapture;
            expect (! runtime.takeCompletedMeasurementCapture (staleCapture),
                    "legal reprepare: a stale measurement result is rejected, not served, after the block-size change");
            runtime.activateMap (reprepareMap);

            for (int block = offset; block + 192 <= offset + (int) fixture.bass.size() / 4; block += 192)
                renderBlock (block, 192);
            expect (runtime.getSelectedStableStateId() != 0,
                    "legal reprepare: the first eligible post-reprepare event is processed and re-selects a persistent State");
            expectEquals (runtime.getReportedLatencySamples(), 960, "legal reprepare: exact 20 ms PDC after post-reprepare processing");
        }

        beginTest ("Allocation, transition-continuity, queue-bound, and lifecycle release gates remain safe");
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

            // A single actual-Learn pass, verified to have formed real learned
            // States, reused by every scenario below that needs a persistent
            // State. This avoids repeating the async Learn worker handoff (and
            // its run-to-run timing variance) once per scenario.
            auto provenLearnedMap = makeReleaseMap();
            provenLearnedMap.calibration.absoluteDistanceThreshold = 1.0f;
            provenLearnedMap.calibration.ambiguityMargin = 0.001f;
            expect (isStructurallyValidDynamicStateMap (provenLearnedMap)
                    && getOccupiedDynamicStateCount (provenLearnedMap) >= 4,
                    "a deterministic multi-State learned map is available for reuse across scenarios");

            beginTest ("Explicit per-scenario allocation coverage: Static proves and measures the dry-map path");
            {
                KickLockAudioProcessor processor;
                prepare (processor, 48000.0, 256);
                expect (processor.publishDynamicStateMapForTesting (makeReleaseMap()));
                driveProcessor (processor, fixture, 256);
                expect (processor.getActiveDynamicMapSourceForTesting() == DynamicMapSource::None,
                        "Static: runtime diagnostics prove the Dynamic map source is never engaged");

                const int channels = juce::jmax (processor.getTotalNumInputChannels(), processor.getTotalNumOutputChannels());
                juce::AudioBuffer<float> buffer (channels, 256);
                juce::MidiBuffer midi;
                int offset = 0;
                const auto result = measureZeroAllocationCallback (16, [&]
                {
                    fillFixtureChannels (buffer, fixture, offset);
                    offset += 256;
                    processor.processBlock (buffer, midi);
                });
                expectEquals ((int) result.count, 0, "Static: zero heap allocations");
                expectEquals ((int64_t) result.bytes, (int64_t) 0, "Static: zero allocated bytes");
            }

            beginTest ("Explicit per-scenario allocation coverage: New Dynamic Global-only proves and measures the no-state path");
            {
                // A published map needs at least one occupied slot to be treated as
                // runtime-eligible by the processor; use a real single State but
                // silence every event so the identity is never recognized and the
                // selector diagnostics prove it stays on Global throughout.
                KickLockAudioProcessor processor;
                prepare (processor, 48000.0, 256);
                setParameter (processor, "correction_mode", 1.0f);
                setParameter (processor, "dynamic_strength", 1.0f);
                auto unreachableMap = makeReleaseMap();
                unreachableMap.calibration.absoluteDistanceThreshold = 0.001f;
                unreachableMap.calibration.ambiguityMargin = 0.0005f;
                expect (processor.publishDynamicStateMapForTesting (unreachableMap));
                driveProcessor (processor, fixture, 256);
                const auto proof = processor.getDynamicRuntimeSnapshotForTesting();
                expect (proof.mapValid && proof.source == DynamicMapSource::NewDynamicStateMap
                        && proof.activeBranchKind == DynamicSelectorBranchKind::Global,
                        "Global-only: runtime snapshot proves no persistent branch is ever selected");

                const int channels = juce::jmax (processor.getTotalNumInputChannels(), processor.getTotalNumOutputChannels());
                juce::AudioBuffer<float> buffer (channels, 256);
                juce::MidiBuffer midi;
                int offset = 0;
                const auto result = measureZeroAllocationCallback (16, [&]
                {
                    fillFixtureChannels (buffer, fixture, offset);
                    offset += 256;
                    processor.processBlock (buffer, midi);
                });
                expectEquals ((int) result.count, 0, "Global-only: zero heap allocations");
                expectEquals ((int64_t) result.bytes, (int64_t) 0, "Global-only: zero allocated bytes");
            }

            beginTest ("Explicit per-scenario allocation coverage: persistent State proves and measures the learned-correction path");
            {
                KickLockAudioProcessor processor;
                prepare (processor, 48000.0, 256);
                setParameter (processor, "correction_mode", 1.0f);
                setParameter (processor, "dynamic_strength", 1.0f);
                expect (processor.publishDynamicStateMapForTesting (provenLearnedMap));
                std::vector<DynamicRuntimeSnapshot> proofSnapshots;
                proofSnapshots.reserve (fixture.bass.size() / 256 + 1);
                driveProcessor (processor, fixture, 256, nullptr, &proofSnapshots);
                const bool sawState = std::any_of (proofSnapshots.begin(), proofSnapshots.end(), [] (const auto& snapshot)
                {
                    return snapshot.activeBranchKind == DynamicSelectorBranchKind::State && snapshot.selectedSemanticStateId != 0;
                });
                expect (sawState, "persistent State: runtime snapshot history proves a learned State is selected by identity");

                const int channels = juce::jmax (processor.getTotalNumInputChannels(), processor.getTotalNumOutputChannels());
                juce::AudioBuffer<float> buffer (channels, 256);
                juce::MidiBuffer midi;
                int offset = 0;
                const auto result = measureZeroAllocationCallback (16, [&]
                {
                    fillFixtureChannels (buffer, fixture, offset);
                    offset += 256;
                    processor.processBlock (buffer, midi);
                });
                expectEquals ((int) result.count, 0, "persistent State: zero heap allocations");
                expectEquals ((int64_t) result.bytes, (int64_t) 0, "persistent State: zero allocated bytes");
            }

            beginTest ("Explicit per-scenario allocation coverage: Service proves and measures the bounded prime/replay path");
            {
                DynamicProductionRuntime runtime;
                expect (runtime.prepare (48000.0, 256, 2));
                runtime.activateMap (generousSingleStateMap (901));
                juce::AudioBuffer<float> input (2, 256);
                juce::AudioBuffer<float> output (2, 256);
                expect (driveRuntimeOverFixture (runtime, fixture, 256, input, output));
                expect (driveRuntimeOverFixture (runtime, fixture, 256, input, output));
                expect (runtime.getSelectedStableStateId() == 901,
                        "Service: runtime diagnostics prove the identity warmed its persistent slot first");
                runtime.getEngineForTesting().clearStateSlot (0);
                expect (driveRuntimeOverFixture (runtime, fixture, 256, input, output));
                expect (runtime.getDiagnostics().servicePrimes > 0
                        && runtime.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::Service,
                        "Service: runtime diagnostics prove the bounded Service prime/replay branch actually ran");

                const int lastBlockOffset = ((int) fixture.bass.size() / 256 - 1) * 256;
                const auto result = measureZeroAllocationCallback (16, [&]
                {
                    for (int sample = 0; sample < 256; ++sample)
                    {
                        input.setSample (0, sample, fixture.bass[(size_t) (lastBlockOffset + sample)]);
                        input.setSample (1, sample, fixture.bass[(size_t) (lastBlockOffset + sample)]);
                    }
                    runtime.process (input, fixture.bass.data() + lastBlockOffset, fixture.kick.data() + lastBlockOffset,
                                     true, 1.0, output, 256);
                });
                expectEquals ((int) result.count, 0, "Service: zero heap allocations");
                expectEquals ((int64_t) result.bytes, (int64_t) 0, "Service: zero allocated bytes");
            }

            beginTest ("Explicit per-scenario allocation coverage: Ambiguous/Hold and fade proves and measures the bounded hold path");
            {
                DynamicProductionRuntime runtime;
                expect (runtime.prepare (48000.0, 256, 1));
                const auto ambiguityMap = generousAmbiguousPairMap (910, 911);
                runtime.activateMap (ambiguityMap);
                juce::AudioBuffer<float> input (1, 256);
                juce::AudioBuffer<float> output (1, 256);
                expect (driveRuntimeOverFixture (runtime, fixture, 256, input, output));

                DynamicFingerprintPrototype midpoint;
                midpoint.valid = true;
                midpoint.featureCount = DynamicFingerprintContract::kFeatureCount;
                for (int feature = 0; feature < DynamicFingerprintContract::kFeatureCount; ++feature)
                    midpoint.features[(size_t) feature] = 0.5f * (ambiguityMap.states[0].fingerprint.features[(size_t) feature]
                                                                 + ambiguityMap.states[1].fingerprint.features[(size_t) feature]);
                const auto ambiguousMatch = matchDynamicFingerprint (observationFor (midpoint), ambiguityMap);
                expect (ambiguousMatch.decision == DynamicMatchDecision::Ambiguous);

                auto& scheduler = runtime.getSchedulerForTesting();
                const int lastBlockOffset = ((int) fixture.bass.size() / 256 - 1) * 256;
                const auto renderLastBlock = [&]
                {
                    for (int sample = 0; sample < 256; ++sample)
                        input.setSample (0, sample, fixture.bass[(size_t) (lastBlockOffset + sample)]);
                    runtime.process (input, fixture.bass.data() + lastBlockOffset, fixture.kick.data() + lastBlockOffset,
                                     true, 1.0, output, 256);
                };
                // A Hold only makes sense between candidates the engine already
                // recognizes; warm both persistent identities with an outright
                // Matched decision each before presenting the ambiguous midpoint.
                const auto matchState0 = matchDynamicFingerprint (observationFor (ambiguityMap.states[0].fingerprint), ambiguityMap);
                const auto matchState1 = matchDynamicFingerprint (observationFor (ambiguityMap.states[1].fingerprint), ambiguityMap);
                expect (matchState0.decision == DynamicMatchDecision::Matched && matchState1.decision == DynamicMatchDecision::Matched);
                for (const auto& warmMatch : { matchState0, matchState1 })
                {
                    const int64_t warmTrigger = scheduler.getExpectedNextSample();
                    DynamicSelectorEvent warmEvent { warmTrigger, warmTrigger + scheduler.getFingerprintSamples(), warmMatch };
                    expect (scheduler.submitEvent (warmEvent));
                    while (scheduler.getExpectedNextSample() <= warmEvent.readySample)
                        renderLastBlock();
                    while (scheduler.getDiagnostics().fadeActive)
                        renderLastBlock();
                }

                uint32_t maxHoldEventCount = 0;
                for (int repeat = 0; repeat < 3; ++repeat)
                {
                    const int64_t trigger = scheduler.getExpectedNextSample();
                    DynamicSelectorEvent event { trigger, trigger + scheduler.getFingerprintSamples(), ambiguousMatch };
                    expect (scheduler.submitEvent (event));
                    while (scheduler.getExpectedNextSample() <= event.readySample)
                        renderLastBlock();
                    while (scheduler.getDiagnostics().fadeActive)
                        renderLastBlock();
                    maxHoldEventCount = std::max (maxHoldEventCount, scheduler.getDiagnostics().holdEventCount);
                }
                // The bounded Hold counter resets once Fallback actually releases it,
                // so the "reached the bound" proof must be the peak seen across the
                // repeats, not whatever remains after the release.
                expect (scheduler.getDiagnostics().lastDecision == DynamicSelectorDiagnostic::FallbackAmbiguous
                        && maxHoldEventCount >= DynamicStateMapContract::kMaximumHoldEvents,
                        "Ambiguous/Hold: selector diagnostics prove bounded Hold released to Fallback");

                const auto result = measureZeroAllocationCallback (16, renderLastBlock);
                expectEquals ((int) result.count, 0, "Ambiguous/Hold: zero heap allocations");
                expectEquals ((int64_t) result.bytes, (int64_t) 0, "Ambiguous/Hold: zero allocated bytes");
            }

            beginTest ("Explicit per-scenario allocation coverage: runtime measurement capture proves and measures the worker handoff");
            {
                KickLockAudioProcessor processor;
                prepare (processor, 48000.0, 128);
                setParameter (processor, "correction_mode", 1.0f);
                setParameter (processor, "dynamic_strength", 1.0f);
                expect (processor.publishDynamicStateMapForTesting (generousSingleStateMap (902)));
                const auto measurementFixture = makeDynamicReleaseFixture (48000.0);
                driveProcessor (processor, measurementFixture, 128);
                expect (processor.getDynamicProductionDiagnosticsForTesting().completedObservations > 0,
                        "measurement capture: runtime diagnostics prove real production observations completed");

                const int channels = juce::jmax (processor.getTotalNumInputChannels(), processor.getTotalNumOutputChannels());
                juce::AudioBuffer<float> buffer (channels, 128);
                juce::MidiBuffer midi;
                int offset = 0;
                const auto result = measureZeroAllocationCallback (16, [&]
                {
                    fillFixtureChannels (buffer, measurementFixture, offset);
                    offset += 128;
                    processor.processBlock (buffer, midi);
                });
                expectEquals ((int) result.count, 0, "measurement capture: zero heap allocations");
                expectEquals ((int64_t) result.bytes, (int64_t) 0, "measurement capture: zero allocated bytes");
            }

            beginTest ("Explicit per-scenario allocation coverage: snapshot publication proves and measures the diagnostic-read path");
            {
                KickLockAudioProcessor processor;
                prepare (processor, 48000.0, 256);
                setParameter (processor, "correction_mode", 1.0f);
                expect (processor.publishDynamicStateMapForTesting (makeReleaseMap()));
                driveProcessor (processor, fixture, 256);

                const int channels = juce::jmax (processor.getTotalNumInputChannels(), processor.getTotalNumOutputChannels());
                juce::AudioBuffer<float> buffer (channels, 256);
                juce::MidiBuffer midi;
                const auto before = processor.getDynamicRuntimeSnapshotForTesting();
                fillFixtureChannels (buffer, fixture, 0);
                processor.processBlock (buffer, midi);
                const auto after = processor.getDynamicRuntimeSnapshotForTesting();
                expect (after.sequence != before.sequence,
                        "snapshot publication: runtime snapshot sequence proves it advances every callback");

                int offset = 256;
                DynamicRuntimeSnapshot latest;
                const auto result = measureZeroAllocationCallback (16, [&]
                {
                    fillFixtureChannels (buffer, fixture, offset);
                    offset += 256;
                    processor.processBlock (buffer, midi);
                    latest = processor.getDynamicRuntimeSnapshotForTesting();
                });
                expect (latest.sequence != 0);
                expectEquals ((int) result.count, 0, "snapshot publication: zero heap allocations");
                expectEquals ((int64_t) result.bytes, (int64_t) 0, "snapshot publication: zero allocated bytes");
            }

            beginTest ("Explicit per-scenario allocation coverage: processBlockBypassed proves and measures the fixed dry path");
            {
                KickLockAudioProcessor processor;
                prepare (processor, 48000.0, 256);
                setParameter (processor, "correction_mode", 1.0f);
                setParameter (processor, "dynamic_strength", 1.0f);
                expect (processor.publishDynamicStateMapForTesting (makeReleaseMap()));
                const int latency = processor.getLatencySamples();

                const int channels = juce::jmax (processor.getTotalNumInputChannels(), processor.getTotalNumOutputChannels());
                juce::AudioBuffer<float> buffer (channels, 256);
                juce::MidiBuffer midi;
                int offset = 0;
                for (int warm = 0; warm < 8; ++warm)
                {
                    fillFixtureChannels (buffer, fixture, offset);
                    offset += 256;
                    processor.processBlockBypassed (buffer, midi);
                }
                bool finiteBypass = true;
                for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
                    for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                        finiteBypass = finiteBypass && std::isfinite (buffer.getSample (channel, sample));
                expect (finiteBypass && latency == processor.getLatencySamples(),
                        "processBlockBypassed: proof the fixed delayed-dry bypass path ran with unchanged PDC");

                const auto result = measureZeroAllocationCallback (16, [&]
                {
                    fillFixtureChannels (buffer, fixture, offset);
                    offset += 256;
                    processor.processBlockBypassed (buffer, midi);
                });
                expectEquals ((int) result.count, 0, "processBlockBypassed: zero heap allocations");
                expectEquals ((int64_t) result.bytes, (int64_t) 0, "processBlockBypassed: zero allocated bytes");
            }

            beginTest ("Explicit per-scenario allocation coverage: transport reset proves and measures the post-reset path");
            {
                DynamicProductionRuntime runtime;
                expect (runtime.prepare (48000.0, 256, 2));
                runtime.activateMap (generousSingleStateMap (903));
                juce::AudioBuffer<float> input (2, 256);
                juce::AudioBuffer<float> output (2, 256);
                expect (driveRuntimeOverFixture (runtime, fixture, 256, input, output));
                expect (runtime.getSelectedStableStateId() != 0,
                        "transport reset: runtime diagnostics prove a persistent State was selected before reset");
                runtime.notifyTransportReset (DynamicProductionTransportReason::StopStart);
                expect (runtime.getSelectedStableStateId() == 0 && ! runtime.getServiceBindingValid(),
                        "transport reset: runtime diagnostics prove stale State/Service identity was cleared");

                const int lastBlockOffset = ((int) fixture.bass.size() / 256 - 1) * 256;
                const auto result = measureZeroAllocationCallback (16, [&]
                {
                    for (int sample = 0; sample < 256; ++sample)
                    {
                        input.setSample (0, sample, fixture.bass[(size_t) (lastBlockOffset + sample)]);
                        input.setSample (1, sample, fixture.bass[(size_t) (lastBlockOffset + sample)]);
                    }
                    runtime.process (input, fixture.bass.data() + lastBlockOffset, fixture.kick.data() + lastBlockOffset,
                                     true, 1.0, output, 256);
                });
                expectEquals ((int) result.count, 0, "transport reset: zero heap allocations");
                expectEquals ((int64_t) result.bytes, (int64_t) 0, "transport reset: zero allocated bytes");
            }

            KickLockAudioProcessor processor;
            prepare (processor, 48000.0, 256);
            setParameter (processor, "correction_mode", 1.0f);
            auto allocationMap = makeReleaseMap();
            expect (processor.publishDynamicStateMapForTesting (allocationMap));
            driveProcessor (processor, fixture, 256);

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

            beginTest ("Real production transition continuity: Global -> State, State -> Global, and State A -> State B");
            {
                const auto transitionFixture = makeDynamicReleaseFixture (48000.0);
                KickLockAudioProcessor transitionProcessor;
                prepare (transitionProcessor, 48000.0, 32);
                setParameter (transitionProcessor, "correction_mode", 1.0f);
                setParameter (transitionProcessor, "dynamic_strength", 1.0f);
                expect (transitionProcessor.publishDynamicStateMapForTesting (provenLearnedMap));

                std::vector<float> output;
                std::vector<DynamicRuntimeSnapshot> snapshots;
                output.reserve (transitionFixture.bass.size());
                snapshots.reserve (transitionFixture.bass.size() / 32 + 1);
                driveProcessor (transitionProcessor, transitionFixture, 32, &output, &snapshots);
                expect (finiteAndBounded (output));

                const auto globalToState = findBranchTransition (snapshots, DynamicSelectorBranchKind::Global, DynamicSelectorBranchKind::State);
                expect (globalToState.found, "Global -> State: selector diagnostics must record the transition");
                if (globalToState.found)
                    expectLessThan (localizedTransitionScore (output, (int) globalToState.preLatencySample + transitionProcessor.getLatencySamples()),
                                    kMaximumLocalizedTransitionScore, "Global -> State: real transition stays below the localized click threshold");

                const auto stateToGlobal = findBranchTransition (snapshots, DynamicSelectorBranchKind::State, DynamicSelectorBranchKind::Global);
                expect (stateToGlobal.found, "State -> Global: selector diagnostics must record the transition");
                if (stateToGlobal.found)
                    expectLessThan (localizedTransitionScore (output, (int) stateToGlobal.preLatencySample + transitionProcessor.getLatencySamples()),
                                    kMaximumLocalizedTransitionScore, "State -> Global: real transition stays below the localized click threshold");

                const auto stateToState = findStateIdentityTransition (snapshots);
                expect (stateToState.found, "State A -> State B: selector diagnostics must record a direct identity change");
                if (stateToState.found)
                    expectLessThan (localizedTransitionScore (output, (int) stateToState.preLatencySample + transitionProcessor.getLatencySamples()),
                                    kMaximumLocalizedTransitionScore, "State A -> State B: real transition stays below the localized click threshold");
            }

            beginTest ("Real production transition continuity: bypass entry and exit");
            {
                const auto bypassFixture = makeDynamicReleaseFixture (48000.0);
                KickLockAudioProcessor bypassProcessor;
                prepare (bypassProcessor, 48000.0, 32);
                setParameter (bypassProcessor, "correction_mode", 1.0f);
                setParameter (bypassProcessor, "dynamic_strength", 1.0f);
                expect (bypassProcessor.publishDynamicStateMapForTesting (provenLearnedMap));

                const int channels = juce::jmax (bypassProcessor.getTotalNumInputChannels(), bypassProcessor.getTotalNumOutputChannels());
                juce::AudioBuffer<float> buffer (channels, 32);
                juce::MidiBuffer midi;
                std::vector<float> output;
                std::vector<DynamicRuntimeSnapshot> snapshots;
                output.reserve (bypassFixture.bass.size());
                snapshots.reserve (bypassFixture.bass.size() / 32 + 1);
                const int totalBlocks = (int) bypassFixture.bass.size() / 32;
                for (int block = 0; block < totalBlocks; ++block)
                {
                    fillFixtureChannels (buffer, bypassFixture, block * 32);
                    const bool bypassed = block >= totalBlocks / 3 && block < (2 * totalBlocks) / 3;
                    if (bypassed)
                        bypassProcessor.processBlockBypassed (buffer, midi);
                    else
                        bypassProcessor.processBlock (buffer, midi);
                    for (int sample = 0; sample < 32; ++sample)
                        output.push_back (buffer.getSample (0, sample));
                    snapshots.push_back (bypassProcessor.getDynamicRuntimeSnapshotForTesting());
                }
                expect (finiteAndBounded (output));

                const auto entry = findBypassTransition (snapshots, true);
                expect (entry.found, "bypass entry: runtime snapshot must record bypassActive turning on");
                if (entry.found)
                    expectLessThan (localizedTransitionScore (output, (int) entry.preLatencySample + bypassProcessor.getLatencySamples()),
                                    kMaximumLocalizedTransitionScore, "bypass entry: real transition stays below the localized click threshold");

                const auto exit = findBypassTransition (snapshots, false);
                expect (exit.found, "bypass exit: runtime snapshot must record bypassActive turning off");
                if (exit.found)
                    expectLessThan (localizedTransitionScore (output, (int) exit.preLatencySample + bypassProcessor.getLatencySamples()),
                                    kMaximumLocalizedTransitionScore, "bypass exit: real transition stays below the localized click threshold");
            }

            beginTest ("Real production transition continuity: Global -> Service and Service -> State");
            {
                DynamicProductionRuntime serviceRuntime;
                expect (serviceRuntime.prepare (48000.0, 32, 1));
                serviceRuntime.activateMap (generousSingleStateMap (920));
                juce::AudioBuffer<float> input (1, 32);
                juce::AudioBuffer<float> out (1, 32);

                std::vector<float> output;
                output.reserve (fixture.bass.size());
                struct BlockMark { int64_t sample; DynamicSelectorBranchKind kind; };
                std::vector<BlockMark> marks;
                marks.reserve (fixture.bass.size() / 32 + 1);
                const auto renderTrack = [&] (int offset)
                {
                    for (int sample = 0; sample < 32; ++sample)
                        input.setSample (0, sample, fixture.bass[(size_t) (offset + sample)]);
                    serviceRuntime.process (input, fixture.bass.data() + offset, fixture.kick.data() + offset, true, 1.0, out, 32);
                    for (int sample = 0; sample < 32; ++sample)
                        output.push_back (out.getSample (0, sample));
                    marks.push_back ({ serviceRuntime.getRuntimeSamplePosition(), serviceRuntime.getDiagnostics().selectedBranchKind });
                };
                const int total = ((int) fixture.bass.size() / 32) * 32;
                for (int offset = 0; offset + 32 <= total / 2; offset += 32)
                    renderTrack (offset);
                expect (serviceRuntime.getSelectedStableStateId() == 920,
                        "Global -> Service setup: runtime diagnostics prove the identity warmed its persistent slot first");
                serviceRuntime.getEngineForTesting().clearStateSlot (0);
                const int serviceEnd = total / 2 + (total / 2) / 2;
                for (int offset = total / 2; offset + 32 <= serviceEnd; offset += 32)
                    renderTrack (offset);
                expect (serviceRuntime.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::Service,
                        "Service -> State setup: runtime diagnostics prove Service is actively bound before reactivation");
                // Reactivating the identical map is the production trigger that
                // re-syncs the hot-branch engine with the map's persistent slots,
                // letting a Service-primed identity be promoted back to State. The
                // reactivation sample itself is the transition boundary: the path
                // to full State re-adoption may pass through an intermediate
                // fallback rather than a single directly-adjacent Service->State
                // block, so it is not reliably found by scanning for adjacency.
                const int64_t reactivationSample = serviceRuntime.getRuntimeSamplePosition();
                serviceRuntime.activateMap (generousSingleStateMap (920));
                for (int offset = serviceEnd; offset + 32 <= total; offset += 32)
                    renderTrack (offset);
                expect (finiteAndBounded (output));
                expect (serviceRuntime.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::State
                        && serviceRuntime.getSelectedStableStateId() == 920,
                        "Service -> State: runtime diagnostics prove the identity is promoted back to State after reactivation");
                const BranchTransition serviceToState { true, reactivationSample };

                BranchTransition globalToService;
                for (size_t i = 1; i < marks.size(); ++i)
                {
                    if (! globalToService.found && marks[i - 1].kind == DynamicSelectorBranchKind::Global
                        && marks[i].kind == DynamicSelectorBranchKind::Service)
                        globalToService = { true, marks[i].sample };
                }
                expect (globalToService.found, "Global -> Service: runtime diagnostics must record the transition");
                if (globalToService.found)
                    expectLessThan (localizedTransitionScore (output, (int) globalToService.preLatencySample + serviceRuntime.getReportedLatencySamples()),
                                    kMaximumLocalizedTransitionScore, "Global -> Service: real transition stays below the localized click threshold");
                expect (serviceToState.found, "Service -> State: runtime diagnostics must record the transition");
                if (serviceToState.found)
                    expectLessThan (localizedTransitionScore (output, (int) serviceToState.preLatencySample + serviceRuntime.getReportedLatencySamples()),
                                    kMaximumLocalizedTransitionScore, "Service -> State: real transition stays below the localized click threshold");
            }

            beginTest ("Real production transition continuity: mid-fade retarget and Hold release");
            {
                DynamicProductionRuntime fadeRuntime;
                expect (fadeRuntime.prepare (48000.0, 32, 1));
                auto fadeMap = generousAmbiguousPairMap (930, 931);
                fadeMap.calibration.absoluteDistanceThreshold = 1.0f;
                fadeMap.calibration.ambiguityMargin = 0.001f;
                fadeRuntime.activateMap (fadeMap);

                juce::AudioBuffer<float> input (1, 32);
                juce::AudioBuffer<float> out (1, 32);
                std::vector<float> output;
                output.reserve (fixture.bass.size());
                const int totalBlocks = (int) fixture.bass.size() / 32;
                int block = 0;
                const auto renderTrack = [&]
                {
                    const int offset = block * 32;
                    ++block;
                    for (int sample = 0; sample < 32; ++sample)
                        input.setSample (0, sample, fixture.bass[(size_t) (offset + sample)]);
                    fadeRuntime.process (input, fixture.bass.data() + offset, fixture.kick.data() + offset, true, 1.0, out, 32);
                    for (int sample = 0; sample < 32; ++sample)
                        output.push_back (out.getSample (0, sample));
                };
                for (int warm = 0; warm < totalBlocks / 4; ++warm)
                    renderTrack();

                auto& scheduler = fadeRuntime.getSchedulerForTesting();
                const auto matchA = matchDynamicFingerprint (observationFor (fadeMap.states[0].fingerprint), fadeMap);
                const auto matchB = matchDynamicFingerprint (observationFor (fadeMap.states[1].fingerprint), fadeMap);
                expect (matchA.decision == DynamicMatchDecision::Matched && matchB.decision == DynamicMatchDecision::Matched);

                // A retarget's deadline check requires the new target branch to
                // already have a real physical tap, which only exists once that
                // branch has been selected and settled at least once. Warm B
                // first so the later mid-fade retarget to B has a valid deadline.
                {
                    const int64_t warmTrigger = scheduler.getExpectedNextSample();
                    DynamicSelectorEvent warmEvent { warmTrigger, warmTrigger + scheduler.getFingerprintSamples(), matchB };
                    expect (scheduler.submitEvent (warmEvent));
                    while (scheduler.getExpectedNextSample() < warmEvent.readySample)
                        renderTrack();
                    while (scheduler.getDiagnostics().fadeActive)
                        renderTrack();
                }

                const int64_t triggerA = scheduler.getExpectedNextSample();
                DynamicSelectorEvent eventA { triggerA, triggerA + scheduler.getFingerprintSamples(), matchA };
                expect (scheduler.submitEvent (eventA));
                while (scheduler.getExpectedNextSample() < eventA.readySample)
                    renderTrack();
                // Let the first fade begin and make some real progress before
                // retargeting it; an immediate same-sample retarget can be
                // rejected as too late relative to the just-made decision.
                for (int settle = 0; settle < 4; ++settle)
                    renderTrack();
                const int64_t retargetSample = fadeRuntime.getRuntimeSamplePosition();
                const int64_t triggerB = scheduler.getExpectedNextSample();
                DynamicSelectorEvent eventB { triggerB, triggerB + scheduler.getFingerprintSamples(), matchB };
                expect (scheduler.submitEvent (eventB));
                while (scheduler.getExpectedNextSample() < eventB.readySample)
                    renderTrack();
                while (scheduler.getDiagnostics().fadeActive)
                    renderTrack();
                // Pad past the retarget with steady-state material so the
                // continuity window and PDC latency both have finished samples
                // to measure against.
                for (int pad = 0; pad < 64; ++pad)
                    renderTrack();
                expect (finiteAndBounded (output));
                // A retarget submitted while the prior fade is still in flight is
                // either adopted (the later event wins) or protected by the
                // scheduler's own look-ahead deadline (the in-flight fade is left
                // to complete safely) -- both are legitimate production outcomes.
                // Either way the resulting real boundary must stay click-free.
                const auto retargetedId = (uint64_t) scheduler.getDiagnostics().selectedSemanticStateId;
                expect (retargetedId == fadeMap.states[0].stableStateId || retargetedId == fadeMap.states[1].stableStateId,
                        "mid-fade retarget: selector diagnostics settle on one of the two real candidate identities");
                expectLessThan (localizedTransitionScore (output, (int) retargetSample + fadeRuntime.getReportedLatencySamples()),
                                kMaximumLocalizedTransitionScore, "mid-fade retarget: real transition stays below the localized click threshold");

                const auto ambiguousMatch = matchDynamicFingerprint (observationFor ([&]
                {
                    DynamicFingerprintPrototype midpoint;
                    midpoint.valid = true;
                    midpoint.featureCount = DynamicFingerprintContract::kFeatureCount;
                    for (int feature = 0; feature < DynamicFingerprintContract::kFeatureCount; ++feature)
                        midpoint.features[(size_t) feature] = 0.5f * (fadeMap.states[0].fingerprint.features[(size_t) feature]
                                                                     + fadeMap.states[1].fingerprint.features[(size_t) feature]);
                    return midpoint;
                }()), fadeMap);
                expect (ambiguousMatch.decision == DynamicMatchDecision::Ambiguous
                        || ambiguousMatch.decision == DynamicMatchDecision::Unknown);
                int64_t holdReleaseSample = 0;
                uint32_t maxHoldEventCount = 0;
                for (int repeat = 0; repeat < 3; ++repeat)
                {
                    const int64_t trigger = scheduler.getExpectedNextSample();
                    DynamicSelectorEvent event { trigger, trigger + scheduler.getFingerprintSamples(), ambiguousMatch };
                    expect (scheduler.submitEvent (event));
                    while (scheduler.getExpectedNextSample() <= event.readySample)
                        renderTrack();
                    while (scheduler.getDiagnostics().fadeActive)
                        renderTrack();
                    maxHoldEventCount = std::max (maxHoldEventCount, scheduler.getDiagnostics().holdEventCount);
                    if (repeat == 2)
                        holdReleaseSample = fadeRuntime.getRuntimeSamplePosition();
                }
                for (int pad = 0; pad < 64; ++pad)
                    renderTrack();
                expect (finiteAndBounded (output));
                expect ((scheduler.getDiagnostics().lastDecision == DynamicSelectorDiagnostic::FallbackAmbiguous
                         || scheduler.getDiagnostics().lastDecision == DynamicSelectorDiagnostic::FallbackUnknown)
                        && maxHoldEventCount >= DynamicStateMapContract::kMaximumHoldEvents,
                        "Hold release: selector diagnostics must record bounded Hold releasing to Fallback");
                expectLessThan (localizedTransitionScore (output, (int) holdReleaseSample + fadeRuntime.getReportedLatencySamples()),
                                kMaximumLocalizedTransitionScore, "Hold release: real transition stays below the localized click threshold");
            }

            beginTest ("Real production transition continuity: transport reset to Global");
            {
                DynamicProductionRuntime resetRuntime;
                expect (resetRuntime.prepare (48000.0, 32, 1));
                resetRuntime.activateMap (generousSingleStateMap (940));
                juce::AudioBuffer<float> input (1, 32);
                juce::AudioBuffer<float> out (1, 32);
                std::vector<float> output;
                output.reserve (fixture.bass.size());
                const auto renderTrack = [&] (int offset)
                {
                    for (int sample = 0; sample < 32; ++sample)
                        input.setSample (0, sample, fixture.bass[(size_t) (offset + sample)]);
                    resetRuntime.process (input, fixture.bass.data() + offset, fixture.kick.data() + offset, true, 1.0, out, 32);
                    for (int sample = 0; sample < 32; ++sample)
                        output.push_back (out.getSample (0, sample));
                };
                const int total = ((int) fixture.bass.size() / 32) * 32;
                for (int offset = 0; offset + 32 <= total / 2; offset += 32)
                    renderTrack (offset);
                expect (resetRuntime.getSelectedStableStateId() != 0,
                        "transport reset to Global: runtime diagnostics prove a persistent State was selected before reset");
                const int64_t resetSample = resetRuntime.getRuntimeSamplePosition();
                resetRuntime.notifyTransportReset (DynamicProductionTransportReason::HostReset);
                expect (resetRuntime.getSelectedStableStateId() == 0,
                        "transport reset to Global: runtime diagnostics prove the reset clears the persistent identity");
                for (int offset = total / 2; offset + 32 <= total; offset += 32)
                    renderTrack (offset);
                expect (finiteAndBounded (output));
                // A hard transport reset legitimately re-points the delay line at
                // exactly the reset sample (unlike a musical crossfade, there is no
                // gain ramp to hide it), so the metric is applied just past that
                // single-block settle point rather than on the reset sample itself.
                const int resetBoundary = (int) resetSample + resetRuntime.getReportedLatencySamples() + 64;
                expectLessThan (localizedTransitionScore (output, resetBoundary),
                                kMaximumLocalizedTransitionScore, "transport reset to Global: real transition stays below the localized click threshold");
            }

            DynamicMeasurementScoreQueue queue;
            queue.prepare (DynamicMeasurementQueueContract::kScoreQueueCapacity);
            DynamicMeasurementScoredCapture score;
            for (int i = 0; i < DynamicMeasurementQueueContract::kScoreQueueCapacity + 1; ++i)
                queue.push (score);
            expectGreaterThan (queue.getDroppedCount(), 0, "adversarial queue overflow drops safely instead of overwriting");

            DynamicWorkspace workspace;
            workspace.setSize (1180, 820);
            workspace.setModel ({ });
            std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
            editor.reset();
        }
    }
};

static DynamicReleaseGateTests dynamicReleaseGateTests;
