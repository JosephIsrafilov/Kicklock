#include "TestCommon.h"

#include "DynamicLearnFingerprintExtraction.h"
#include "DynamicLearnFormation.h"
#include "DynamicFingerprintMatcher.h"
#include "DynamicPackageMorpher.h"
#include "DynamicStateSerialization.h"

namespace
{
    DynamicFingerprintPrototype prototype (float seed)
    {
        DynamicFingerprintPrototype value;
        value.valid = true;
        value.featureCount = DynamicFingerprintContract::kFeatureCount;
        for (int i = 0; i < DynamicFingerprintContract::kFeatureCount; ++i)
            value.features[(size_t) i] = std::clamp (seed + 0.004f * (float) i, -1.0f, 1.0f);
        return value;
    }

    DynamicLearnHit hit (int sequence, int64_t sample, float seed, float delay = 0.0f,
                         bool package = true, bool polarity = false)
    {
        DynamicLearnHit value;
        value.sequence = sequence;
        value.triggerSample = sample;
        value.fingerprint = prototype (seed);
        value.fingerprintValidity = DynamicFingerprintValidity::Valid;
        value.timingEligible = true;
        value.absoluteDelayMs = delay;
        value.polarityInvert = polarity;
        value.timingConfidence = 0.9f;
        value.lowBandEnergy = 1.0f;
        value.correctionBeneficial = package;
        value.allpassFreqHz = 100.0f;
        value.allpassQ = 0.7f;
        return value;
    }

    DynamicFingerprintObservation observation (const DynamicFingerprintPrototype& value)
    {
        DynamicFingerprintObservation result;
        result.fingerprint.validity = DynamicFingerprintValidity::Valid;
        result.fingerprint.featureCount = DynamicFingerprintContract::kFeatureCount;
        result.fingerprint.features = value.features;
        return result;
    }

    DynamicLearnFormationContext context()
    {
        DynamicLearnFormationContext result;
        result.sampleRate = 48000.0;
        result.crossoverEnabled = true;
        result.crossoverHz = 150.0f;
        result.fallbackAllpassFreqHz = 100.0f;
        result.fallbackAllpassQ = 0.7f;
        return result;
    }
}

class DynamicLearnFormationTests : public juce::UnitTest
{
public:
    DynamicLearnFormationTests() : juce::UnitTest ("DynamicLearnFormation", "Phase8") {}

    void runTest() override
    {
        beginTest ("Continuous Learn extraction equals runtime capture at supported rates");
        for (const double rate : { 44100.0, 48000.0, 96000.0, 192000.0 })
        {
            const int trigger = (int) std::lround (rate * 0.020);
            const int count = trigger + DynamicFingerprintWindow::forSampleRate (rate).windowSamples + 16;
            std::vector<float> bass ((size_t) count), kick ((size_t) count);
            for (int i = 0; i < count; ++i)
            {
                const double t = (double) i / rate;
                bass[(size_t) i] = (float) (0.7 * std::sin (kTwoPi * 73.0 * t));
                kick[(size_t) i] = (float) (0.8 * std::sin (kTwoPi * 73.0 * t)
                                          + (i == trigger ? 0.75 : 0.0));
            }
            const auto learned = extractDynamicLearnFingerprints (bass.data(), kick.data(), count, rate,
                                                                   { { 7, trigger } });
            DynamicFingerprintObservation runtime;
            expect (extractDynamicFingerprintOffline (bass.data(), kick.data(), count, rate, trigger, runtime));
            expectEquals ((int) learned.size(), 1);
            if (! learned.empty())
            {
                expectEquals ((int) learned[0].triggerSample, trigger, "trigger sample is included as index zero");
                expectEquals ((int) learned[0].fingerprint.validity, (int) runtime.fingerprint.validity);
                for (int f = 0; f < DynamicFingerprintContract::kFeatureCount; ++f)
                    expectWithinAbsoluteError (learned[0].fingerprint.features[(size_t) f],
                                               runtime.fingerprint.features[(size_t) f], 1.0e-7f);
            }
        }

        beginTest ("Three repeatable hits form a recognizable Candidate without correction");
        {
            std::vector<DynamicLearnHit> hits;
            for (int i = 0; i < 3; ++i) hits.push_back (hit (i, i * 1000, -0.35f + 0.001f * i));
            const auto formed = formDynamicStateMap (hits, context());
            expect (formed.valid);
            const auto& state = formed.map.states[0];
            expectEquals ((int) state.evidence, (int) DynamicStateEvidence::Candidate);
            const auto match = matchDynamicFingerprint (observation (state.fingerprint), formed.map);
            expectEquals ((int) match.decision, (int) DynamicMatchDecision::Matched);
            expect (! match.correctionAvailable);
            expectEquals ((int) resolveDynamicPackage (formed.map, state.stableStateId, 1.0, 48000.0).decision,
                          (int) DynamicPackageDecision::GlobalNoSelection);
        }

        beginTest ("Five repeatable hits form a Stable State with a valid package");
        {
            std::vector<DynamicLearnHit> hits;
            for (int i = 0; i < 5; ++i) hits.push_back (hit (i, i * 1000, 0.20f + 0.001f * i, 4.0f));
            const auto formed = formDynamicStateMap (hits, context());
            expect (formed.valid);
            const auto& state = formed.map.states[0];
            expectEquals ((int) state.evidence, (int) DynamicStateEvidence::Stable);
            expect (state.hasLearnedPackage);
            expect (isValidDynamicZonePackage (state.learnedPackage));
            expectEquals ((int) resolveDynamicPackage (formed.map, state.stableStateId, 1.0, 48000.0).decision,
                          (int) DynamicPackageDecision::StateCorrection);
        }

        beginTest ("Four equally weighted fingerprint families do not need a dominant timing family");
        {
            std::vector<DynamicLearnHit> hits;
            for (int family = 0; family < 4; ++family)
                for (int repeat = 0; repeat < 5; ++repeat)
                    hits.push_back (hit ((family * 10) + repeat, (family * 10000) + repeat,
                                         -0.70f + family * 0.35f + repeat * 0.001f,
                                         -2.0f + family));
            std::reverse (hits.begin(), hits.end());
            const auto formed = formDynamicStateMap (hits, context());
            expect (formed.valid);
            expectEquals (getOccupiedDynamicStateCount (formed.map), 4);
            for (const auto& state : formed.map.states)
                if (state.occupied)
                    expectEquals ((int) state.evidence, (int) DynamicStateEvidence::Stable);
        }

        beginTest ("Recognized no-correction State routes to Global and serializes");
        {
            std::vector<DynamicLearnHit> hits;
            for (int i = 0; i < 5; ++i) hits.push_back (hit (i, i * 1000, -0.10f + i * 0.001f, 9.0f, false));
            const auto formed = formDynamicStateMap (hits, context());
            expect (formed.valid);
            const auto& state = formed.map.states[0];
            expect (! state.hasLearnedPackage);
            const auto resolution = resolveDynamicPackage (formed.map, state.stableStateId, 1.0, 48000.0);
            expectEquals ((int) resolution.decision, (int) DynamicPackageDecision::GlobalRecognizedNoCorrection);
            const auto restored = dynamicStateMapFromValueTree (dynamicStateMapToValueTree (formed.map));
            expect (isStructurallyValidDynamicStateMap (restored));
            expect (! restored.states[0].hasLearnedPackage);
        }

        beginTest ("Manual States survive reconciliation and Auto IDs survive reordered material");
        {
            std::vector<DynamicLearnHit> hits;
            for (int i = 0; i < 5; ++i) hits.push_back (hit (i, i * 1000, 0.35f + i * 0.001f, 2.0f));
            auto first = formDynamicStateMap (hits, context());
            expect (first.valid);
            const uint64_t learnedId = first.map.states[0].stableStateId;
            DynamicState manual;
            manual.occupied = true;
            manual.stableStateId = 9000000001ull;
            manual.fingerprint = prototype (-0.85f);
            manual.origin = DynamicStateOrigin::Manual;
            manual.evidence = DynamicStateEvidence::Stable;
            manual.hitCount = 5;
            manual.repeatability = 1.0f;
            manual.manualBasePackage = { 0.0f, 100.0f, 0.7f };
            manual.hasManualBasePackage = true;
            first.map.states[7] = manual;
            first.map.nextStateId = 9000000002ull;
            std::reverse (hits.begin(), hits.end());
            auto nextContext = context();
            nextContext.previousMap = first.map;
            const auto second = formDynamicStateMap (hits, nextContext);
            expect (second.valid);
            expectEquals ((int64) second.map.states[0].stableStateId, (int64) learnedId);
            expectEquals ((int64) second.map.states[7].stableStateId, (int64) manual.stableStateId);
            expect (second.map.states[7].hasManualBasePackage);
        }

        beginTest ("Apply publishes Dynamic map, retries a full queue, and Revert restores it");
        {
            std::vector<DynamicLearnHit> hits;
            for (int i = 0; i < 5; ++i)
                hits.push_back (hit (i, i * 1000, 0.45f + i * 0.001f, 1.0f));
            const auto formed = formDynamicStateMap (hits, context());
            expect (formed.valid);

            LearnFinalizeResult pending;
            pending.valid = formed.valid;
            pending.hasDynamicStateMap = true;
            pending.dynamicMap = formed.map;
            LearnSessionContext session;
            session.sampleRate = 48000.0;

            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.setRateAndBufferSizeDetails (48000.0, 64);
            processor.prepareToPlay (48000.0, 64);
            auto previous = formed.map;
            previous.states[0].stableStateId = 77;
            previous.nextStateId = 78;
            for (int i = 0; i < 4; ++i)
                expect (processor.publishDynamicStateMapForTesting (previous));
            processor.setPendingLearnResultForTesting (pending, session);
            expect (! processor.applyLatestLearnResult(), "full queue retains the pending result for retry");

            juce::AudioBuffer<float> audio (juce::jmax (processor.getTotalNumInputChannels(),
                                                        processor.getTotalNumOutputChannels()), 64);
            audio.clear();
            juce::MidiBuffer midi;
            processor.processBlock (audio, midi); // drains the four test updates
            expect (processor.applyLatestLearnResult());
            processor.processBlock (audio, midi);
            expectEquals ((int64) processor.getActiveDynamicStateMapForTesting().states[0].stableStateId,
                          (int64) formed.map.states[0].stableStateId);
            expect (processor.revertLatestFix());
            processor.processBlock (audio, midi);
            expectEquals ((int64) processor.getActiveDynamicStateMapForTesting().states[0].stableStateId,
                          (int64) previous.states[0].stableStateId);
        }
    }
};

static DynamicLearnFormationTests dynamicLearnFormationTests;
