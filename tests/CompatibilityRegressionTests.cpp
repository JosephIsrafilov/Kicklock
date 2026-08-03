#include "TestCommon.h"

namespace
{
    struct StateCodec : juce::AudioProcessor
    {
        using juce::AudioProcessor::copyXmlToBinary;
        using juce::AudioProcessor::getXmlFromBinary;
    };

    void setParameter (KickLockAudioProcessor& processor, const char* id, float value)
    {
        if (auto* parameter = processor.apvts.getParameter (id))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    }

    float rawParameter (const KickLockAudioProcessor& processor, const char* id)
    {
        const auto* parameter = processor.apvts.getRawParameterValue (id);
        return parameter != nullptr ? parameter->load() : std::numeric_limits<float>::quiet_NaN();
    }

    juce::ValueTree parameterState (const juce::ValueTree& state, const char* id)
    {
        for (const auto child : state)
            if (child.hasType ("PARAM") && child.getProperty ("id").toString() == id)
                return child;
        return {};
    }

    void setStoredParameter (juce::ValueTree& state, const char* id, float value)
    {
        auto child = parameterState (state, id);
        if (! child.isValid())
        {
            child = juce::ValueTree ("PARAM");
            child.setProperty ("id", id, nullptr);
            state.appendChild (child, nullptr);
        }
        child.setProperty ("value", value, nullptr);
    }

    void removeStoredParameter (juce::ValueTree& state, const char* id)
    {
        for (int i = state.getNumChildren() - 1; i >= 0; --i)
        {
            const auto child = state.getChild (i);
            if (child.hasType ("PARAM") && child.getProperty ("id").toString() == id)
                state.removeChild (i, nullptr);
        }
    }

    juce::MemoryBlock encodeState (const juce::ValueTree& state)
    {
        juce::MemoryBlock data;
        std::unique_ptr<juce::XmlElement> xml (state.createXml());
        StateCodec::copyXmlToBinary (*xml, data);
        return data;
    }

    void prepare (KickLockAudioProcessor& processor, int blockSize = 512)
    {
        processor.enableAllBuses();
        processor.setRateAndBufferSizeDetails (kSampleRate, blockSize);
        processor.prepareToPlay (kSampleRate, blockSize);
    }

    void fillComparisonBuffer (juce::AudioBuffer<float>& buffer)
    {
        const int n = buffer.getNumSamples();
        for (int i = 0; i < n; ++i)
        {
            const float value = 0.35f * std::sin ((float) i * 0.017f)
                + 0.08f * std::sin ((float) i * 0.071f);
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.setSample (ch, i, value);
        }
    }
}

class CompatibilityRegressionTests : public juce::UnitTest
{
public:
    CompatibilityRegressionTests() : juce::UnitTest ("CompatibilityRegression", "0.4.0") {}

    void runTest() override
    {
        beginTest ("Canonical and legacy IDs both remain available");
        {
            KickLockAudioProcessor processor;
            for (const auto* id : { "delay_ms", "polarity_invert", "allpass_enable", "allpass_freq",
                                    "delayMs", "polarityInvert", "phaseFilterEnabled", "rotatorFreq" })
                expect (processor.apvts.getParameter (id) != nullptr, id);
        }

        beginTest ("New Apply writes canonical IDs and leaves legacy-only automation intact");
        {
            KickLockAudioProcessor processor;
            setParameter (processor, "delayMs", -7.0f);
            setParameter (processor, "polarityInvert", 0.0f);
            setParameter (processor, "phaseFilterEnabled", 0.0f);
            setParameter (processor, "rotatorFreq", 70.0f);

            PhaseFixResult fix;
            fix.valid = true;
            fix.enoughSignal = true;
            fix.bassDelayMs = 3.25f;
            fix.bassPolarityInvert = true;
            fix.phaseFilterEnabled = true;
            fix.phaseFilterFreqHz = 240.0f;
            fix.phaseFilterQ = 1.8f;
            fix.phaseFilterStages = 3;
            fix.beforeMatchPercent = 40.0f;
            fix.afterMatchPercent = 90.0f;
            fix.improvementPercent = 50.0f;
            fix.confidence = 0.9f;
            PhaseFixEngine::updateDerivedResultFields (fix);
            processor.setLatestFixResultForTesting (fix);

            expect (processor.applyLatestFix());
            expectWithinAbsoluteError (rawParameter (processor, "delay_ms"), 3.25f, 0.01f);
            expectWithinAbsoluteError (rawParameter (processor, "polarity_invert"), 1.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParameter (processor, "allpass_enable"), 1.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParameter (processor, "allpass_freq"), 240.0f, 0.01f);
            expectWithinAbsoluteError (rawParameter (processor, "delayMs"), -7.0f, 0.01f);
            expectWithinAbsoluteError (rawParameter (processor, "polarityInvert"), 0.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParameter (processor, "phaseFilterEnabled"), 0.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParameter (processor, "rotatorFreq"), 70.0f, 0.01f);
        }

        beginTest ("Legacy-only state migrates to canonical values with equivalent output");
        {
            constexpr float delay = 3.25f;
            constexpr float frequency = 220.0f;
            constexpr float q = 2.0f;

            KickLockAudioProcessor source;
            setParameter (source, "delay_ms", delay);
            setParameter (source, "polarity_invert", 1.0f);
            setParameter (source, "allpass_enable", 1.0f);
            setParameter (source, "allpass_freq", frequency);
            setParameter (source, "rotatorQ", q);
            setParameter (source, "rotatorStages", 1.0f);
            auto canonicalState = source.apvts.copyState();
            auto legacyOnlyState = canonicalState.createCopy();
            for (const auto* id : { "delay_ms", "polarity_invert", "allpass_enable", "allpass_freq" })
                removeStoredParameter (legacyOnlyState, id);
            setStoredParameter (legacyOnlyState, "delayMs", delay);
            setStoredParameter (legacyOnlyState, "polarityInvert", 1.0f);
            setStoredParameter (legacyOnlyState, "phaseFilterEnabled", 1.0f);
            setStoredParameter (legacyOnlyState, "rotatorFreq", frequency);

            KickLockAudioProcessor migrated;
            const auto legacyData = encodeState (legacyOnlyState);
            migrated.setStateInformation (legacyData.getData(), (int) legacyData.getSize());
            expectWithinAbsoluteError (rawParameter (migrated, "delay_ms"), delay, 0.01f);
            expectWithinAbsoluteError (rawParameter (migrated, "polarity_invert"), 1.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParameter (migrated, "allpass_enable"), 1.0f, 1.0e-7f);
            expectWithinAbsoluteError (rawParameter (migrated, "allpass_freq"), frequency, 0.01f);

            KickLockAudioProcessor canonical;
            const auto canonicalData = encodeState (canonicalState);
            canonical.setStateInformation (canonicalData.getData(), (int) canonicalData.getSize());
            prepare (migrated);
            prepare (canonical);

            juce::AudioBuffer<float> migratedBuffer (4, 512);
            juce::AudioBuffer<float> canonicalBuffer (4, 512);
            fillComparisonBuffer (migratedBuffer);
            canonicalBuffer.makeCopyOf (migratedBuffer);
            juce::MidiBuffer midi;
            migrated.processBlock (migratedBuffer, midi);
            canonical.processBlock (canonicalBuffer, midi);

            float maximumDifference = 0.0f;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 512; ++i)
                    maximumDifference = std::max (maximumDifference,
                                                  std::abs (migratedBuffer.getSample (ch, i)
                                                            - canonicalBuffer.getSample (ch, i)));
            expectLessThan (maximumDifference, 1.0e-5f,
                            "legacy-only and canonical state render equivalently");
        }
    }
};

static CompatibilityRegressionTests compatibilityRegressionTests;
