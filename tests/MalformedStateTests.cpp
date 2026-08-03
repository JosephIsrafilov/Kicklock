#include "TestCommon.h"

namespace
{
    struct StateCodec : juce::AudioProcessor
    {
        using juce::AudioProcessor::copyXmlToBinary;
    };

    juce::MemoryBlock encodeTree (const juce::ValueTree& tree)
    {
        juce::MemoryBlock data;
        std::unique_ptr<juce::XmlElement> xml (tree.createXml());
        StateCodec::copyXmlToBinary (*xml, data);
        return data;
    }

    bool finiteBuffer (const juce::AudioBuffer<float>& buffer)
    {
        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                if (! std::isfinite (buffer.getSample (ch, i)))
                    return false;
        return true;
    }
}

class MalformedStateTests : public juce::UnitTest
{
public:
    MalformedStateTests() : juce::UnitTest ("MalformedState", "0.4.0") {}

    void runTest() override
    {
        beginTest ("Truncated, invalid, and wrong-root state payloads are safe");
        {
            const std::array<std::vector<uint8_t>, 3> payloads {
                std::vector<uint8_t> { 0x00, 0x01, 0x02, 0x03 },
                std::vector<uint8_t> { '<', 'P', 'R', 'O', 'J', 'E', 'C', 'T', '>' },
                std::vector<uint8_t> { 0xff, 0xfe, 0xfd, 0xfc, 0xfb, 0xfa }
            };

            for (const auto& payload : payloads)
            {
                KickLockAudioProcessor processor;
                processor.setStateInformation (payload.data(), (int) payload.size());
                processor.setRateAndBufferSizeDetails (48000.0, 128);
                processor.prepareToPlay (48000.0, 128);
                juce::AudioBuffer<float> buffer (4, 128);
                buffer.clear();
                juce::MidiBuffer midi;
                processor.processBlock (buffer, midi);
                expect (finiteBuffer (buffer));
            }
        }

        beginTest ("Old state without 0.4.0 parameters restores safe factory values");
        {
            KickLockAudioProcessor source;
            auto state = source.apvts.copyState();
            for (int i = state.getNumChildren() - 1; i >= 0; --i)
            {
                const auto child = state.getChild (i);
                if (! child.hasType ("PARAM"))
                    continue;
                const auto id = child.getProperty ("id").toString();
                if (id == "correction_mode" || id == "dynamic_strength")
                    state.removeChild (i, nullptr);
            }

            KickLockAudioProcessor restored;
            const auto data = encodeTree (state);
            restored.setStateInformation (data.getData(), (int) data.getSize());
            expectWithinAbsoluteError (restored.apvts.getRawParameterValue ("dynamic_strength")->load(),
                                       1.0f, 1.0e-7f);
            expectWithinAbsoluteError (restored.apvts.getRawParameterValue ("correction_mode")->load(),
                                       0.0f, 1.0e-7f);
        }
    }
};

static MalformedStateTests malformedStateTests;
