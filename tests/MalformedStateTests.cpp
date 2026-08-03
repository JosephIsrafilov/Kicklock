#include "TestCommon.h"

#include "DynamicStateSerialization.h"

#include <limits>
#include <memory>
#include <random>

namespace
{
    struct StateBinaryCodec : juce::AudioProcessor
    {
        using juce::AudioProcessor::copyXmlToBinary;
        using juce::AudioProcessor::getXmlFromBinary;
    };

    DynamicStateMap validMap()
    {
        DynamicStateMap map = makeEmptyDynamicStateMap();
        map.valid = true;
        map.calibration = { true, 0.25f, 0.05f };
        map.globalBase.globalBaseDelayMs = -2.0f;

        auto& state = map.states[0];
        state.occupied = true;
        state.stableStateId = 1;
        state.fingerprint.valid = true;
        state.fingerprint.featureCount = DynamicStateMapContract::kMaxFingerprintFeatures;
        state.fingerprint.features = { -0.35f, 0.20f, 0.10f, -0.15f, 0.05f, 0.0f, 0.0f, 0.0f };
        state.hasLearnedPackage = true;
        state.learnedPackage = { 1.0f, 120.0f, 1.1f };
        state.evidence = DynamicStateEvidence::Stable;
        state.hitCount = DynamicStateMapContract::kStableAutoMinimumRepeatableHits;
        state.repeatability = 0.85f;
        state.ambiguity = 0.1f;
        state.correctionPolicy = DynamicCorrectionPolicy::LearnedState;
        map.nextStateId = 2;
        return map;
    }

    juce::ValueTree decode (const juce::MemoryBlock& binary)
    {
        std::unique_ptr<juce::XmlElement> xml (
            StateBinaryCodec::getXmlFromBinary (binary.getData(), (int) binary.getSize()));
        return xml != nullptr ? juce::ValueTree::fromXml (*xml) : juce::ValueTree {};
    }

    juce::MemoryBlock encode (const juce::ValueTree& tree)
    {
        juce::MemoryBlock binary;
        std::unique_ptr<juce::XmlElement> xml (tree.createXml());
        StateBinaryCodec::copyXmlToBinary (*xml, binary);
        return binary;
    }

    juce::ValueTree findChild (const juce::ValueTree& root, const char* type)
    {
        for (int index = 0; index < root.getNumChildren(); ++index)
            if (root.getChild (index).hasType (juce::Identifier (type)))
                return root.getChild (index);
        return {};
    }

    juce::ValueTree firstState (const juce::ValueTree& map)
    {
        return findChild (map, DynamicStateMapKeys::state);
    }

    juce::MemoryBlock savedState (KickLockAudioProcessor& processor, const DynamicStateMap& map)
    {
        processor.publishDynamicStateMapForTesting (map);
        juce::MemoryBlock state;
        processor.getStateInformation (state);
        return state;
    }

    bool audioOutputIsFinite (KickLockAudioProcessor& processor)
    {
        juce::AudioBuffer<float> buffer (4, 128);
        for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
        {
            const float value = std::sin ((float) sample * 0.07f) * 0.2f;
            buffer.setSample (0, sample, value);
            buffer.setSample (1, sample, value);
            buffer.setSample (2, sample, sample == 16 ? 0.8f : 0.0f);
            buffer.setSample (3, sample, sample == 16 ? 0.8f : 0.0f);
        }
        juce::MidiBuffer midi;
        processor.processBlock (buffer, midi);
        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
                if (! std::isfinite (buffer.getSample (channel, sample)))
                    return false;
        return true;
    }

    bool parametersAreFinite (const KickLockAudioProcessor& processor)
    {
        for (const auto* id : { "delay_ms", "allpass_freq", "rotatorQ", "dynamic_strength", "correction_mode" })
        {
            const auto* value = processor.apvts.getRawParameterValue (id);
            if (value == nullptr || ! std::isfinite (value->load()))
                return false;
        }
        return true;
    }
}

class MalformedStateTests : public juce::UnitTest
{
public:
    MalformedStateTests() : juce::UnitTest ("MalformedState", "Fuzz") {}

    void runTest() override
    {
        beginTest ("Malformed Dynamic State maps fail closed and valid state recovers");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.prepareToPlay (48000.0, 128);
            const auto valid = validMap();
            const auto baseline = savedState (processor, valid);

            expect (isStructurallyValidDynamicStateMap (processor.getMessageOwnedDynamicStateMapForTesting()));

            auto expectRejected = [&] (const juce::MemoryBlock& payload, const juce::String& label)
            {
                processor.setStateInformation (payload.getData(), (int) payload.getSize());
                expect (! processor.getMessageOwnedDynamicStateMapForTesting().valid, label + ": Dynamic map fails closed");
                expect (parametersAreFinite (processor), label + ": persistent parameters remain finite");
                expect (audioOutputIsFinite (processor), label + ": stopped restore leaves audio output finite");

                processor.setStateInformation (baseline.getData(), (int) baseline.getSize());
                const auto restored = processor.getMessageOwnedDynamicStateMapForTesting();
                expect (isStructurallyValidDynamicStateMap (restored), label + ": a subsequent valid restore recovers");
                expectEquals ((int64) restored.states[0].stableStateId, (int64) 1);
            };

            auto mutate = [&] (const std::function<void (juce::ValueTree&, juce::ValueTree&)>& edit,
                                const juce::String& label)
            {
                auto root = decode (baseline);
                auto map = findChild (root, DynamicStateMapKeys::tree);
                edit (root, map);
                expectRejected (encode (root), label);
            };

            mutate ([] (juce::ValueTree&, juce::ValueTree& map)
            {
                map.removeProperty (DynamicStateMapKeys::globalBaseDelayMs, nullptr);
            }, "missing Global package field");

            mutate ([] (juce::ValueTree&, juce::ValueTree& map)
            {
                firstState (map).removeProperty (juce::String (DynamicStateMapKeys::fingerprintFeaturePrefix)
                                                     + juce::String (0), nullptr);
            }, "missing fingerprint field");

            mutate ([] (juce::ValueTree&, juce::ValueTree& map)
            {
                auto state = firstState (map);
                auto duplicate = state.createCopy();
                map.appendChild (duplicate, nullptr);
            }, "duplicate stableStateId");

            mutate ([] (juce::ValueTree&, juce::ValueTree& map)
            {
                const auto state = firstState (map);
                for (int count = 0; count < DynamicStateMapContract::kMaxPersistentStates; ++count)
                    map.appendChild (state.createCopy(), nullptr);
            }, "excessive State count");

            mutate ([] (juce::ValueTree&, juce::ValueTree& map)
            {
                firstState (map).setProperty (DynamicStateMapKeys::correctionPolicy, 99, nullptr);
            }, "invalid correction policy enum");

            mutate ([] (juce::ValueTree&, juce::ValueTree& map)
            {
                firstState (map).setProperty (DynamicStateMapKeys::policyRejectionReason, 99, nullptr);
            }, "invalid rejection reason enum");

            for (const auto& token : { juce::String ("nan"), juce::String ("inf"), juce::String ("-inf") })
                mutate ([&] (juce::ValueTree&, juce::ValueTree& map)
                {
                    firstState (map).setProperty (DynamicStateMapKeys::learnedAllpassQ, token, nullptr);
                }, "non-finite persisted value " + token);

            mutate ([] (juce::ValueTree&, juce::ValueTree& map)
            {
                firstState (map).setProperty (DynamicStateMapKeys::learnedDelayDeltaMs, 999.0, nullptr);
            }, "out-of-range State delay");

            mutate ([] (juce::ValueTree&, juce::ValueTree& map)
            {
                map.setProperty (DynamicStateMapKeys::globalAllpassFreqHz, 1.0, nullptr);
            }, "out-of-range Global frequency");

            auto truncated = baseline;
            truncated.setSize (juce::jmax ((size_t) 1, baseline.getSize() / 2), false);
            expectRejected (truncated, "truncated binary state");

            const char invalidXml[] = "not a JUCE binary XML state";
            juce::MemoryBlock invalidPayload (invalidXml, sizeof (invalidXml));
            expectRejected (invalidPayload, "invalid XML/binary payload");
        }

        beginTest ("Deterministic random byte payloads and repeated restore sequences remain safe");
        {
            constexpr uint32_t kSeed = 0x4b4c465au;
            logMessage ("  deterministic fuzz seed=0x4b4c465a cases=128");
            std::mt19937 random (kSeed);

            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            processor.prepareToPlay (48000.0, 128);
            const auto baseline = savedState (processor, validMap());

            for (int iteration = 0; iteration < 128; ++iteration)
            {
                const size_t byteCount = (size_t) (1 + random() % 2048);
                juce::MemoryBlock payload (byteCount);
                auto* bytes = static_cast<uint8_t*> (payload.getData());
                for (size_t byte = 0; byte < byteCount; ++byte)
                    bytes[byte] = (uint8_t) random();

                processor.setStateInformation (payload.getData(), (int) payload.getSize());
                expect (parametersAreFinite (processor), "random restore " + juce::String (iteration) + ": finite parameters");
                expect (audioOutputIsFinite (processor), "random restore " + juce::String (iteration) + ": finite output");

                processor.setStateInformation (baseline.getData(), (int) baseline.getSize());
                expect (isStructurallyValidDynamicStateMap (processor.getMessageOwnedDynamicStateMapForTesting()),
                        "valid restore remains available after random payload " + juce::String (iteration));
            }
        }
    }
};

static MalformedStateTests malformedStateTests;
