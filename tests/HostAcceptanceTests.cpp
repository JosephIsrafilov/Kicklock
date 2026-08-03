#include "TestCommon.h"

#include "dsp/DynamicFingerprintExtractor.h"
#include "dsp/DynamicFingerprintTrigger.h"
#include "dsp/DynamicStateSerialization.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>

#ifndef KICKLOCK_HOST_VST3_PATH
 #define KICKLOCK_HOST_VST3_PATH ""
#endif

namespace
{
    struct HostedFixture
    {
        double sampleRate = 48000.0;
        int spacing = 0;
        std::vector<float> bass;
        std::vector<float> kick;
    };

    HostedFixture makeFixture (double sampleRate)
    {
        HostedFixture fixture;
        fixture.sampleRate = sampleRate;
        fixture.spacing = (int) std::lround (sampleRate * 0.24);
        const int total = fixture.spacing * 16;
        fixture.bass.resize ((size_t) total, 0.0f);
        fixture.kick.resize ((size_t) total, 0.0f);

        const double twoPi = juce::MathConstants<double>::twoPi;
        for (int i = 0; i < total; ++i)
        {
            const double t = (double) i / sampleRate;
            fixture.bass[(size_t) i] = 0.35f * (float) std::sin (twoPi * 55.0 * t)
                + 0.08f * (float) std::sin (twoPi * 110.0 * t);
        }

        const int firstHit = (int) std::lround (sampleRate * 0.08);
        const int burst = (int) std::lround (sampleRate * 0.012);
        for (int start = firstHit; start < total; start += fixture.spacing)
            for (int j = 0; j < burst && start + j < total; ++j)
            {
                const double envelope = std::exp (-22.0 * (double) j / sampleRate);
                const double t = (double) j / sampleRate;
                fixture.kick[(size_t) (start + j)] = (float) (0.85 * envelope
                    * (0.75 + 0.25 * std::sin (twoPi * 90.0 * t)));
            }
        return fixture;
    }

    DynamicFingerprintPrototype extractFixtureFingerprint (const HostedFixture& fixture)
    {
        TransientDetector detector;
        configureDynamicFingerprintTrigger (detector, fixture.sampleRate);
        int64_t trigger = -1;
        for (int i = 0; i < (int) fixture.kick.size(); ++i)
            if (detector.processSample (fixture.kick[(size_t) i]))
            {
                trigger = i;
                break;
            }

        DynamicFingerprintObservation observation;
        if (trigger < 0
            || ! extractDynamicFingerprintOffline (fixture.bass.data(), fixture.kick.data(),
                                                    (int) fixture.bass.size(), fixture.sampleRate,
                                                    trigger, observation))
            return {};
        return observation.fingerprint.toPrototype();
    }

    DynamicStateMap makeFixtureMap (const HostedFixture& fixture)
    {
        DynamicStateMap map = makeEmptyDynamicStateMap();
        map.valid = true;
        map.globalBase.globalBaseDelayMs = 0.0f;
        map.globalBase.polarityInvert = false;
        map.globalBase.crossoverEnabled = true;
        map.globalBase.crossoverHz = 150.0f;
        map.globalBase.allpassEnabled = true;
        map.globalBase.globalAllpassFreqHz = 100.0f;
        map.globalBase.globalAllpassQ = 0.7f;
        map.globalBase.allpassStages = 2;
        map.globalBase.delayInterpolationIndex = 0;
        map.globalBase.learnedSampleRate = fixture.sampleRate;
        map.calibration.valid = true;
        map.calibration.absoluteDistanceThreshold = 0.9f;
        map.calibration.ambiguityMargin = 0.05f;

        auto& state = map.states[0];
        state.occupied = true;
        state.stableStateId = 4040;
        state.fingerprint = extractFixtureFingerprint (fixture);
        state.hasLearnedPackage = state.fingerprint.valid;
        state.learnedPackage.delayDeltaMs = 1.0f;
        state.learnedPackage.allpassFreqHz = 80.0f;
        state.learnedPackage.allpassQ = 1.2f;
        state.origin = DynamicStateOrigin::Auto;
        state.evidence = DynamicStateEvidence::Stable;
        state.enabled = true;
        state.bypassed = false;
        state.hitCount = 6;
        state.repeatability = 0.9f;
        state.ambiguity = 0.1f;
        map.nextStateId = 4041;
        return map;
    }

    juce::File findVst3Bundle()
    {
        juce::File cursor (KICKLOCK_HOST_VST3_PATH);
        for (int i = 0; i < 10 && cursor.isDirectory(); ++i)
        {
            if (cursor.getFileName() == "KickLock.vst3")
                return cursor;
            const auto direct = cursor.getChildFile ("KickLock.vst3");
            if (direct.isDirectory())
                return direct;
            cursor = cursor.getParentDirectory();
        }
        return {};
    }

    juce::File findVst3Module (const juce::File& bundle)
    {
        const auto windowsModule = bundle.getChildFile ("Contents")
            .getChildFile ("x86_64-win").getChildFile ("KickLock.vst3");
        if (windowsModule.existsAsFile())
            return windowsModule;

        const auto macModule = bundle.getChildFile ("Contents")
            .getChildFile ("MacOS").getChildFile ("KickLock");
        if (macModule.existsAsFile())
            return macModule;

        for (const auto& file : bundle.findChildFiles (juce::File::findFiles, true, "*") )
            if (file.getFileName() == "KickLock.vst3" || file.getFileName() == "KickLock")
                return file;
        return {};
    }

    bool configureStereoSidechain (juce::AudioPluginInstance& instance)
    {
        instance.enableAllBuses();
        auto layout = instance.getBusesLayout();
        if (layout.inputBuses.size() < 2 || layout.outputBuses.isEmpty())
            return false;
        layout.inputBuses.set (0, juce::AudioChannelSet::stereo());
        layout.inputBuses.set (1, juce::AudioChannelSet::stereo());
        layout.outputBuses.set (0, juce::AudioChannelSet::stereo());
        return instance.setBusesLayout (layout);
    }

    juce::ValueTree readComponentState (juce::AudioPluginInstance& instance)
    {
        juce::MemoryBlock data;
        instance.getStateInformation (data);
        std::unique_ptr<juce::XmlElement> wrapper (
            juce::AudioProcessor::getXmlFromBinary (data.getData(), (int) data.getSize()));
        if (wrapper == nullptr)
            return {};

        const auto* component = wrapper->getChildByName ("IComponent");
        if (component == nullptr)
            return {};

        juce::MemoryBlock componentData;
        if (! componentData.fromBase64Encoding (component->getAllSubText()))
            return {};

        std::unique_ptr<juce::XmlElement> xml (
            juce::AudioProcessor::getXmlFromBinary (componentData.getData(),
                                                    (int) componentData.getSize()));
        return xml != nullptr ? juce::ValueTree::fromXml (*xml) : juce::ValueTree {};
    }

    void installMap (juce::AudioPluginInstance& instance, const DynamicStateMap& map)
    {
        auto state = readComponentState (instance);
        if (! state.isValid())
            return;

        for (int i = state.getNumChildren() - 1; i >= 0; --i)
            if (state.getChild (i).hasType (juce::Identifier (DynamicStateMapKeys::tree)))
                state.removeChild (i, nullptr);
        state.appendChild (dynamicStateMapToValueTree (map), nullptr);
        for (auto child : state)
            if (child.hasType ("PARAM") && child.getProperty ("id").toString() == "correction_mode")
                child.setProperty ("value", 1.0f, nullptr);

        std::unique_ptr<juce::XmlElement> updatedComponent (state.createXml());
        juce::MemoryBlock componentData;
        juce::AudioProcessor::copyXmlToBinary (*updatedComponent, componentData);

        auto wrapper = std::make_unique<juce::XmlElement> ("VST3PluginState");
        wrapper->createNewChildElement ("IComponent")
            ->addTextElement (componentData.toBase64Encoding());

        juce::MemoryBlock existingData;
        instance.getStateInformation (existingData);
        std::unique_ptr<juce::XmlElement> existing (
            juce::AudioProcessor::getXmlFromBinary (existingData.getData(),
                                                    (int) existingData.getSize()));
        if (existing != nullptr)
        {
            const auto* controller = existing->getChildByName ("IEditController");
            if (controller != nullptr)
                wrapper->addChildElement (new juce::XmlElement (*controller));
        }

        juce::MemoryBlock encoded;
        juce::AudioProcessor::copyXmlToBinary (*wrapper, encoded);
        instance.setStateInformation (encoded.getData(), (int) encoded.getSize());
    }

    bool allFinite (const std::vector<float>& samples)
    {
        for (const auto sample : samples)
            if (! std::isfinite (sample))
                return false;
        return true;
    }

    float maxAdjacentDelta (const std::vector<float>& samples)
    {
        float maximum = 0.0f;
        for (size_t i = 1; i < samples.size(); ++i)
            maximum = std::max (maximum, std::abs (samples[i] - samples[i - 1]));
        return maximum;
    }

    float maxDifference (const std::vector<float>& left, const std::vector<float>& right)
    {
        float maximum = 0.0f;
        const auto count = std::min (left.size(), right.size());
        for (size_t i = 0; i < count; ++i)
            maximum = std::max (maximum, std::abs (left[i] - right[i]));
        return maximum;
    }

    std::vector<float> processFixture (juce::AudioPluginInstance& instance,
                                       const HostedFixture& fixture, bool sendKick,
                                       int blockSize)
    {
        std::vector<float> output;
        output.reserve (fixture.bass.size());
        const int mainOffset = instance.getChannelIndexInProcessBlockBuffer (true, 0, 0);
        const int sidechainOffset = instance.getChannelIndexInProcessBlockBuffer (true, 1, 0);
        juce::AudioBuffer<float> buffer (juce::jmax (instance.getTotalNumInputChannels(),
                                                     instance.getTotalNumOutputChannels()),
                                         blockSize);
        juce::MidiBuffer midi;
        for (int offset = 0; offset < (int) fixture.bass.size(); offset += blockSize)
        {
            const int count = juce::jmin (blockSize, (int) fixture.bass.size() - offset);
            buffer.clear();
            for (int i = 0; i < count; ++i)
            {
                buffer.setSample (mainOffset, i, fixture.bass[(size_t) (offset + i)]);
                buffer.setSample (mainOffset + 1, i, fixture.bass[(size_t) (offset + i)]);
                if (sendKick)
                {
                    buffer.setSample (sidechainOffset, i, fixture.kick[(size_t) (offset + i)]);
                    buffer.setSample (sidechainOffset + 1, i, fixture.kick[(size_t) (offset + i)]);
                }
            }
            instance.processBlock (buffer, midi);
            for (int i = 0; i < count; ++i)
                output.push_back (buffer.getSample (mainOffset, i));
        }
        return output;
    }

    std::vector<float> processBassOnly (juce::AudioPluginInstance& instance,
                                        const HostedFixture& fixture, int samples,
                                        int blockSize)
    {
        std::vector<float> output;
        output.reserve ((size_t) samples);
        const int mainOffset = instance.getChannelIndexInProcessBlockBuffer (true, 0, 0);
        const int sidechainOffset = instance.getChannelIndexInProcessBlockBuffer (true, 1, 0);
        juce::AudioBuffer<float> buffer (juce::jmax (instance.getTotalNumInputChannels(),
                                                     instance.getTotalNumOutputChannels()),
                                         blockSize);
        juce::MidiBuffer midi;
        for (int offset = 0; offset < samples; offset += blockSize)
        {
            const int count = juce::jmin (blockSize, samples - offset);
            buffer.clear();
            for (int i = 0; i < count; ++i)
            {
                const double t = (double) (offset + i) / fixture.sampleRate;
                const float bass = 0.35f * (float) std::sin (juce::MathConstants<double>::twoPi * 55.0 * t);
                buffer.setSample (mainOffset, i, bass);
                buffer.setSample (mainOffset + 1, i, bass);
                buffer.setSample (sidechainOffset, i, 0.0f);
                buffer.setSample (sidechainOffset + 1, i, 0.0f);
            }
            instance.processBlock (buffer, midi);
            for (int i = 0; i < count; ++i)
                output.push_back (buffer.getSample (mainOffset, i));
        }
        return output;
    }
}

class HostAcceptanceTests : public juce::UnitTest
{
public:
    HostAcceptanceTests() : juce::UnitTest ("HostAcceptance", "0.4.0") {}

    void runTest() override
    {
        beginTest ("Hosted VST3 validates metadata, buses, Dynamic loss/recovery, and state reload");
        const auto bundle = findVst3Bundle();
        expect (bundle.isDirectory(), "built KickLock.vst3 bundle exists");
        if (! bundle.isDirectory())
            return;

        const auto module = findVst3Module (bundle);
        expect (module.existsAsFile(), "VST3 module exists");
        if (! module.existsAsFile())
            return;

        juce::AudioPluginFormatManager formats;
        formats.addFormat (std::make_unique<juce::VST3PluginFormat>());
        juce::KnownPluginList known;
        juce::OwnedArray<juce::PluginDescription> descriptions;
        expect (known.scanAndAddFile (module.getFullPathName(), false, descriptions,
                                      *formats.getFormat (0)), "VST3 scan succeeds");
        expectEquals (descriptions.size(), 1, "one KickLock VST3 description");
        if (descriptions.isEmpty())
            return;

        const auto description = *descriptions[0];
        expectEquals (description.name, juce::String ("KickLock"));
        expectEquals (description.manufacturerName, juce::String ("OpenSource"));
        expectEquals (description.version, juce::String ("0.4.0"));
        expectEquals (description.pluginFormatName, juce::String ("VST3"));
        expect (! description.isInstrument);

        juce::String error;
        auto instance = formats.createPluginInstance (description, 48000.0, 256, error);
        expect (instance != nullptr, "VST3 instantiates: " + error);
        if (instance == nullptr)
            return;
        expect (configureStereoSidechain (*instance), "stereo main I/O plus stereo sidechain accepted");
        expectEquals (instance->getBusCount (true), 2);
        expectEquals (instance->getLatencySamples(), 0);

        const auto fixture = makeFixture (48000.0);
        const auto map = makeFixtureMap (fixture);
        expect (map.valid && map.states[0].hasLearnedPackage && map.states[0].fingerprint.valid);
        expect (isStructurallyValidDynamicStateMap (map), "fixture DynamicStateMap is structurally valid");
        installMap (*instance, map); // equivalent to the persisted Learn -> Apply result
        {
            const auto installedTree = readComponentState (*instance);
            bool mapRoundTripped = false;
            bool dynamicMode = false;
            for (int i = 0; i < installedTree.getNumChildren(); ++i)
            {
                const auto child = installedTree.getChild (i);
                if (child.hasType (juce::Identifier (DynamicStateMapKeys::tree)))
                    mapRoundTripped = dynamicStateMapFromValueTree (child).valid;
                if (child.hasType ("PARAM") && child.getProperty ("id").toString() == "correction_mode")
                    dynamicMode = (float) child.getProperty ("value") > 0.5f;
            }
            expect (mapRoundTripped, "installed DynamicStateMap survives state round-trip");
            expect (dynamicMode, "installed state selects canonical Dynamic mode");
        }
        instance->prepareToPlay (fixture.sampleRate, 256);
        expectEquals (instance->getLatencySamples(), 960, "20 ms PDC is reported by the hosted instance");

        const auto activeOutput = processFixture (*instance, fixture, true, 256);
        expect (allFinite (activeOutput), "active hosted output is finite");

        auto globalControl = formats.createPluginInstance (description, fixture.sampleRate, 256, error);
        expect (globalControl != nullptr, "global fallback control instantiates");
        if (globalControl != nullptr)
        {
            expect (configureStereoSidechain (*globalControl));
            installMap (*globalControl, map);
            globalControl->prepareToPlay (fixture.sampleRate, 256);
            const auto globalOutput = processFixture (*globalControl, fixture, false, 256);
            expect (allFinite (globalOutput));
            expectGreaterThan (maxDifference (activeOutput, globalOutput), 1.0e-5f,
                               "hosted sidechain drives the applied State branch versus Global fallback");
        }

        const auto lossOutput = processBassOnly (*instance, fixture,
                                                 (int) std::lround (fixture.sampleRate * 1.7), 256);
        expect (allFinite (lossOutput), "signal-loss output is finite");
        const auto resumedOutput = processFixture (*instance, fixture, true, 256);
        expect (allFinite (resumedOutput), "returned-signal output is finite");
        expectLessThan (maxAdjacentDelta (activeOutput), 8.0f, "active transition stays finite and bounded");
        expectLessThan (maxAdjacentDelta (lossOutput), 8.0f, "Global fallback stays finite and bounded");
        expectLessThan (maxAdjacentDelta (resumedOutput), 8.0f, "new-match recovery stays finite and bounded");

        juce::MemoryBlock saved;
        instance->getStateInformation (saved);
        auto restored = formats.createPluginInstance (description, fixture.sampleRate, 256, error);
        expect (restored != nullptr, "VST3 re-instantiates for save/reload");
        if (restored != nullptr)
        {
            expect (configureStereoSidechain (*restored));
            restored->setStateInformation (saved.getData(), (int) saved.getSize());
            restored->prepareToPlay (fixture.sampleRate, 256);
            const auto roundTrip = processFixture (*restored, fixture, true, 256);
            expect (allFinite (roundTrip), "save/reload output is finite");
            expectLessThan (maxAdjacentDelta (roundTrip), 8.0f);
        }
    }
};

static HostAcceptanceTests hostAcceptanceTests;
