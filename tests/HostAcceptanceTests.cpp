#include "TestCommon.h"
#include "audio/AudioTestHarness.h"
#include "audio/AudioMetrics.h"
#include "audio/TestPlayHead.h"
#include "dsp/DynamicStateSerialization.h"
#include "dsp/DynamicFingerprintExtractor.h"
#include "dsp/DynamicFingerprintMatcher.h"
#include "dsp/DynamicPackageMorpher.h"
#include "dsp/DynamicProductionRuntime.h"
#include "dsp/DynamicFingerprintTrigger.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>

namespace
{
constexpr double kRates[] = { 44100.0, 48000.0, 96000.0 };
constexpr int kBlocks[] = { 64, 128, 256, 512 };
constexpr float kPeakLimit = 6.0f;

struct HostedSidechainFixture
{
    static constexpr double kEventSpacingSeconds = 0.120;
    static constexpr int kEventCount = 28;
    double sampleRate = 48000.0;
    int eventSpacing = 0;
    std::vector<float> bass, kick;
    std::vector<int> eventSamples, triggerSamples;

    explicit HostedSidechainFixture (double rate) : sampleRate (rate)
    {
        eventSpacing = (int) std::lround (sampleRate * kEventSpacingSeconds);
        bass.resize ((size_t) (eventSpacing * (kEventCount + 2)));
        kick.resize (bass.size());
        for (int i = 0; i < (int) bass.size(); ++i)
            bass[(size_t) i] = 0.25f * std::sin ((float) (2.0 * juce::MathConstants<double>::pi * 50.0 * i / sampleRate));

        const int burstSamples = (int) std::lround (sampleRate * 0.020);
        for (int event = 0; event < kEventCount; ++event)
        {
            const int start = (event + 1) * eventSpacing;
            eventSamples.push_back (start);
            for (int local = 0; local < burstSamples && start + local < (int) kick.size(); ++local)
            {
                const double seconds = (double) local / sampleRate;
                kick[(size_t) (start + local)] = (float) (0.82 * std::exp (-seconds / 0.003)
                    * std::sin (2.0 * juce::MathConstants<double>::pi * 65.0 * seconds));
            }
        }

        TransientDetector detector;
        configureDynamicFingerprintTrigger (detector, sampleRate);
        for (int i = 0; i < (int) kick.size(); ++i)
            if (detector.processSample (kick[(size_t) i]))
                triggerSamples.push_back (i);
    }
};

HostedSidechainFixture makeHostedSidechainFixture (double sampleRate)
{
    return HostedSidechainFixture { sampleRate };
}

juce::File vst3File()
{
    return juce::File (KICKLOCK_HOST_VST3_PATH).getParentDirectory().getParentDirectory();
}

std::unique_ptr<juce::AudioPluginInstance> instantiate (juce::AudioPluginFormatManager& formats,
                                                          const juce::PluginDescription& description,
                                                          double rate, int block,
                                                          juce::String& error)
{
    return formats.createPluginInstance (description, rate, block, error);
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

DynamicStateMap canonicalMap (const HostedSidechainFixture& fixture)
{
    DynamicFingerprintObservation observation;
    if (! extractDynamicFingerprintOffline (fixture.bass.data(), fixture.kick.data(),
                                             (int) fixture.bass.size(), fixture.sampleRate,
                                             fixture.triggerSamples.front(), observation)
        || ! observation.fingerprint.isValid())
        return makeEmptyDynamicStateMap();
    auto map = AudioTestHarness::buildProvenCorrectiveMap (fixture.sampleRate);
    map.states[0].fingerprint = observation.fingerprint.toPrototype();
    map.states[0].correctionPolicy = DynamicCorrectionPolicy::LearnedState;
    return map;
}

struct RuntimeProbe
{
    DynamicProductionDiagnostics diagnostics;
    DynamicSelectorDiagnostics selector;
    uint64_t selectedStateId = 0;
    bool allCallsSucceeded = true;
};

RuntimeProbe probeProductionRuntime (const HostedSidechainFixture& fixture, const DynamicStateMap& map, int block)
{
    RuntimeProbe probe;
    DynamicProductionRuntime runtime;
    if (! runtime.prepare (fixture.sampleRate, block, 2))
    {
        probe.allCallsSucceeded = false;
        return probe;
    }
    runtime.activateMap (map);
    juce::AudioBuffer<float> input (2, block), output (2, block);
    for (int offset = 0; offset < (int) fixture.bass.size(); offset += block)
    {
        const int count = juce::jmin (block, (int) fixture.bass.size() - offset);
        for (int channel = 0; channel < 2; ++channel)
            for (int sample = 0; sample < count; ++sample)
                input.setSample (channel, sample, fixture.bass[(size_t) (offset + sample)]);
        if (! runtime.process (input, fixture.bass.data() + offset, fixture.kick.data() + offset,
                              true, 1.0, output, count))
            probe.allCallsSucceeded = false;
    }
    probe.diagnostics = runtime.getDiagnostics();
    probe.selector = runtime.getSelectorDiagnostics();
    probe.selectedStateId = runtime.getSelectedStableStateId();
    return probe;
}

double waveformDistance (const std::vector<float>& a, const std::vector<float>& b,
                         int start, int count)
{
    if (a.size() != b.size() || start < 0 || count <= 0 || start + count > (int) a.size())
        return std::numeric_limits<double>::infinity();
    double sumSquares = 0.0;
    for (int i = 0; i < count; ++i)
    {
        const double delta = (double) a[(size_t) (start + i)] - b[(size_t) (start + i)];
        sumSquares += delta * delta;
    }
    return std::sqrt (sumSquares / (double) count);
}

double settledMedianDistance (const std::vector<float>& a, const std::vector<float>& b,
                              const HostedSidechainFixture& fixture, int latencySamples)
{
    std::vector<double> distances;
    constexpr int kWarmupEvents = 4;
    const int window = (int) std::lround (fixture.sampleRate * 0.030);
    for (int event = kWarmupEvents; event < (int) fixture.eventSamples.size(); ++event)
    {
        const int start = fixture.eventSamples[(size_t) event] + latencySamples;
        if (start + window <= (int) a.size() && start + window <= (int) b.size())
            distances.push_back (waveformDistance (a, b, start, window));
    }
    std::sort (distances.begin(), distances.end());
    return distances.empty() ? std::numeric_limits<double>::infinity()
                             : distances[distances.size() / 2];
}

bool installCorrectiveMap (juce::AudioPluginInstance& instance, const DynamicStateMap& map)
{
    juce::MemoryBlock state;
    instance.getStateInformation (state);
    std::unique_ptr<juce::XmlElement> hostXml (juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize()));
    if (hostXml == nullptr || ! hostXml->hasTagName ("VST3PluginState"))
        return false;
    auto* component = hostXml->getChildByName ("IComponent");
    juce::MemoryBlock componentState;
    if (component == nullptr || ! componentState.fromBase64Encoding (component->getAllSubText()))
        return false;
    std::unique_ptr<juce::XmlElement> xml (juce::AudioProcessor::getXmlFromBinary (componentState.getData(), (int) componentState.getSize()));
    auto tree = xml != nullptr ? juce::ValueTree::fromXml (*xml) : juce::ValueTree{};
    if (! tree.isValid())
        return false;

    for (int i = tree.getNumChildren(); --i >= 0;)
        if (tree.getChild(i).hasType (juce::Identifier (DynamicStateMapKeys::tree)))
            tree.removeChild (i, nullptr);
    tree.appendChild (dynamicStateMapToValueTree (map), nullptr);
    for (int i = 0; i < tree.getNumChildren(); ++i)
    {
        auto child = tree.getChild (i);
        if (child.hasType ("PARAM") && child.getProperty ("id").toString() == "correction_mode")
            child.setProperty ("value", 1.0f, nullptr);
    }
    std::unique_ptr<juce::XmlElement> updated (tree.createXml());
    juce::MemoryBlock encoded;
    juce::AudioProcessor::copyXmlToBinary (*updated, encoded);
    component->deleteAllTextElements();
    component->addTextElement (encoded.toBase64Encoding());
    juce::MemoryBlock hostEncoded;
    juce::AudioProcessor::copyXmlToBinary (*hostXml, hostEncoded);
    instance.setStateInformation (hostEncoded.getData(), (int) hostEncoded.getSize());
    return true;
}

enum class HostedKickRoute { Sidechain, Silent, WrongChannel };

const char* routeName (HostedKickRoute route)
{
    switch (route)
    {
        case HostedKickRoute::Sidechain: return "positive";
        case HostedKickRoute::Silent: return "silent";
        case HostedKickRoute::WrongChannel: return "wrong-channel";
    }
    return "unknown";
}

struct ChannelSignalInfo
{
    double rms = 0.0;
    float peak = 0.0f;
    int firstNonZero = -1;
    bool silent = true;
};

ChannelSignalInfo measureChannel (const float* samples, int count)
{
    ChannelSignalInfo result;
    double sumSquares = 0.0;
    for (int i = 0; i < count; ++i)
    {
        const float sample = samples[i];
        sumSquares += (double) sample * sample;
        result.peak = juce::jmax (result.peak, std::abs (sample));
        if (sample != 0.0f && result.firstNonZero < 0)
            result.firstNonZero = i;
    }
    result.silent = result.firstNonZero < 0;
    result.rms = std::sqrt (sumSquares / juce::jmax (1, count));
    return result;
}

juce::String describeBus (juce::AudioPluginInstance& instance, juce::AudioBuffer<float>& processBuffer,
                          bool input, int index)
{
    auto* bus = instance.getBus (input, index);
    if (bus == nullptr)
        return input ? "input-missing" : "output-missing";

    const auto view = instance.getBusBuffer (processBuffer, input, index);
    juce::String text = (input ? "input" : "output") + juce::String (index)
        + " name=" + bus->getName()
        + " enabled=" + juce::String ((int) bus->isEnabled())
        + " layout=" + bus->getCurrentLayout().getDescription()
        + " channels=" + juce::String (view.getNumChannels())
        + " flattenedFirst=" + juce::String (instance.getChannelIndexInProcessBlockBuffer (input, index, 0));
    for (int channel = 0; channel < view.getNumChannels(); ++channel)
    {
        const auto signal = measureChannel (view.getReadPointer (channel), view.getNumSamples());
        text += " ch" + juce::String (channel)
            + " rms=" + juce::String (signal.rms, 7)
            + " peak=" + juce::String (signal.peak, 7)
            + " first=" + juce::String (signal.firstNonZero)
            + " silent=" + juce::String ((int) signal.silent);
    }
    return text;
}

struct HostedStateRead
{
    DynamicStateMap map;
    juce::String children;
    int bytes = 0;
};

HostedStateRead readHostedDynamicMap (juce::AudioPluginInstance& instance)
{
    HostedStateRead result;
    juce::MemoryBlock state;
    instance.getStateInformation (state);
    result.bytes = (int) state.getSize();
    std::unique_ptr<juce::XmlElement> hostXml (juce::AudioProcessor::getXmlFromBinary (state.getData(), (int) state.getSize()));
    if (hostXml == nullptr || ! hostXml->hasTagName ("VST3PluginState"))
        return result;
    for (auto* child = hostXml->getFirstChildElement(); child != nullptr; child = child->getNextElement())
        result.children += (result.children.isEmpty() ? "" : ",") + child->getTagName();
    auto* component = hostXml->getChildByName ("IComponent");
    juce::MemoryBlock componentState;
    if (component == nullptr || ! componentState.fromBase64Encoding (component->getAllSubText()))
        return result;
    std::unique_ptr<juce::XmlElement> xml (juce::AudioProcessor::getXmlFromBinary (componentState.getData(), (int) componentState.getSize()));
    const auto tree = xml != nullptr ? juce::ValueTree::fromXml (*xml) : juce::ValueTree{};
    for (int i = 0; i < tree.getNumChildren(); ++i)
    {
        if (tree.getChild (i).hasType (juce::Identifier (DynamicStateMapKeys::tree)))
            result.map = dynamicStateMapFromValueTree (tree.getChild (i));
    }
    return result;
}

std::vector<float> process (juce::AudioPluginInstance& instance, const HostedSidechainFixture& fixture,
                            int block, HostedKickRoute route, TestPlayHead* playHead = nullptr,
                            juce::String* firstBlockInput = nullptr)
{
    instance.prepareToPlay (fixture.sampleRate, block);
    instance.setPlayHead (playHead);
    juce::MidiBuffer midi;
    std::vector<float> rendered;
    const int total = (int) fixture.bass.size();
    jassert (fixture.bass.size() == fixture.kick.size());
    const int diagnosticOffset = fixture.eventSamples.empty() ? 0
        : fixture.eventSamples.front() / block * block;
    for (int offset = 0; offset < total; offset += block)
    {
        const int n = juce::jmin (block, total - offset);
        if (playHead != nullptr)
            playHead->setFrame ({ offset, n, true, false });
        const int requiredChannels = juce::jmax (instance.getTotalNumInputChannels(), instance.getTotalNumOutputChannels());
        juce::AudioBuffer<float> audio (requiredChannels, n);
        juce::AudioBuffer<float> offBusKick (route == HostedKickRoute::WrongChannel ? 1 : 0, n);
        const auto mainOffset = instance.getChannelIndexInProcessBlockBuffer (true, 0, 0);
        const auto sidechainOffset = instance.getChannelIndexInProcessBlockBuffer (true, 1, 0);
        for (int i = 0; i < n; ++i)
        {
            const int source = offset + i;
            const float bass = fixture.bass[(size_t) source];
            const float kick = fixture.kick[(size_t) source];
            audio.setSample (mainOffset, i, bass); audio.setSample (mainOffset + 1, i, bass);
            if (route == HostedKickRoute::Sidechain)
                audio.setSample (sidechainOffset, i, kick), audio.setSample (sidechainOffset + 1, i, kick);
            else if (route == HostedKickRoute::WrongChannel)
                offBusKick.setSample (0, i, kick);
        }
        if (offset == diagnosticOffset)
        {
            auto inputDescription = "sourceOffset=" + juce::String (offset)
                + " processChannels=" + juce::String (audio.getNumChannels())
                + " inputBuses=" + juce::String (instance.getBusCount (true))
                + " outputBuses=" + juce::String (instance.getBusCount (false))
                + " " + describeBus (instance, audio, true, 0)
                + " " + describeBus (instance, audio, true, 1);
            if (route == HostedKickRoute::WrongChannel)
            {
                const auto misplaced = measureChannel (offBusKick.getReadPointer (0), n);
                inputDescription += " offBusKick rms=" + juce::String (misplaced.rms, 7)
                    + " peak=" + juce::String (misplaced.peak, 7)
                    + " first=" + juce::String (misplaced.firstNonZero);
            }
            if (firstBlockInput != nullptr)
                *firstBlockInput = "host-route=" + juce::String (routeName (route)) + " " + inputDescription;
        }
        instance.processBlock (audio, midi);
        for (int i = 0; i < n; ++i) rendered.push_back (audio.getSample (0, i));
    }
    instance.releaseResources();
    return rendered;
}
}

class HostAcceptanceTests final : public juce::UnitTest
{
public:
    HostAcceptanceTests() : juce::UnitTest ("HostAcceptance", "HostAcceptance") {}

    void runTest() override
    {
        beginTest ("VST3 discovery, metadata, buses, processing, transport and state round trip");
        const auto path = vst3File();
        expect (path.isDirectory(), "exact VST3 path exists: " + path.getFullPathName());
        if (! path.isDirectory()) return;

        juce::AudioPluginFormatManager formats;
        formats.addFormat (std::make_unique<juce::VST3PluginFormat>());
        juce::KnownPluginList known;
        juce::OwnedArray<juce::PluginDescription> found;
        const auto module = juce::File (KICKLOCK_HOST_VST3_PATH).getChildFile ("KickLock.vst3");
        expect (module.existsAsFile(), "VST3 module exists: " + module.getFullPathName());
        expect (known.scanAndAddFile (module.getFullPathName(), false, found, *formats.getFormat (0)), "VST3 scan succeeds");
        expectEquals (found.size(), 1, "exactly one VST3 description");
        if (found.isEmpty()) return;
        const auto description = *found[0];
        logMessage ("VST3=" + path.getFullPathName() + " name=" + description.name
                    + " manufacturer=" + description.manufacturerName + " version=" + description.version);
        expectEquals (description.name, juce::String ("KickLock"));
        expectEquals (description.manufacturerName, juce::String ("OpenSource"));
        expectEquals (description.version, juce::String ("0.3.1"));
        expectEquals (description.pluginFormatName, juce::String ("VST3"));
        expect (! description.isInstrument, "KickLock is discovered as an effect");
        expect (description.uniqueId != 0, "non-zero VST3 unique id");

        juce::String error;
        auto instance = instantiate (formats, description, 48000.0, 256, error);
        expect (instance != nullptr, "VST3 instantiates: " + error);
        if (instance == nullptr) return;
        expect (configureStereoSidechain (*instance), "stereo main I/O plus stereo sidechain accepted");
        expectEquals (instance->getBusCount (true), 2, "hosted instance exposes main and sidechain input buses");
        expect (instance->getBus (true, 1)->isEnabled(), "sidechain bus enabled");
        instance->prepareToPlay (48000.0, 512);
        expectEquals (instance->getLatencySamples(), (int) std::lround (48000.0 * 0.020), "reported latency matches the prepared 20 ms product contract");
        instance->releaseResources();

        const auto fixture = makeHostedSidechainFixture (48000.0);
        const auto map = canonicalMap (fixture);
        DynamicFingerprintObservation observation;
        expect (! fixture.triggerSamples.empty(), "canonical fixture produces a production trigger");
        expect (extractDynamicFingerprintOffline (fixture.bass.data(), fixture.kick.data(), (int) fixture.bass.size(),
                                                  fixture.sampleRate, fixture.triggerSamples.front(), observation),
                "canonical hosted fixture fingerprint extracts at the production trigger");
        const auto match = matchDynamicFingerprint (observation, map);
        expect (match.decision == DynamicMatchDecision::Matched,
                "canonical hosted fixture matches the seeded State through the production matcher");
        expect (match.correctionAvailable && ! match.selectedBypassed,
                "canonical fixture match is correction eligible");
        expect (isRuntimeEligibleDynamicStateMap (map), "seeded map is eligible for the production runtime");
        const auto runtimeProbe = probeProductionRuntime (fixture, map, 256);
        logMessage ("direct-runtime accepted=" + juce::String ((int64) runtimeProbe.diagnostics.acceptedCaptures)
                    + " completed=" + juce::String ((int64) runtimeProbe.diagnostics.completedObservations)
                    + " invalid=" + juce::String ((int64) runtimeProbe.diagnostics.invalidFingerprints)
                    + " configFailures=" + juce::String ((int64) runtimeProbe.diagnostics.configurationFailures)
                    + " selected=" + juce::String ((int64) runtimeProbe.selectedStateId)
                    + " selectorDecision=" + juce::String ((int) runtimeProbe.selector.lastDecision));
        expect (runtimeProbe.allCallsSucceeded, "canonical fixture processes through the production runtime");
        expectEquals ((int64) runtimeProbe.selectedStateId, (int64) map.states[0].stableStateId,
                      "canonical fixture selects its State through the production runtime");
        expect (installCorrectiveMap (*instance, map), "host VST3 component state accepts the seeded Dynamic State Map");
        const auto directRoundTrip = dynamicStateMapFromValueTree (dynamicStateMapToValueTree (map));
        const auto hostedRead = readHostedDynamicMap (*instance);
        const auto& restoredMap = hostedRead.map;
        const auto& seededState = map.states[0];
        const auto& restoredState = restoredMap.states[0];
        const auto seededPackage = resolveDynamicPackage (map, seededState.stableStateId, 1.0, fixture.sampleRate);
        const auto restoredPackage = resolveDynamicPackage (restoredMap, restoredState.stableStateId, 1.0, fixture.sampleRate);
        logMessage ("seeded state id=" + juce::String ((int64) seededState.stableStateId)
                    + " policy=" + juce::String ((int) seededState.correctionPolicy)
                    + " enabled=" + juce::String ((int) seededState.enabled)
                    + " bypassed=" + juce::String ((int) seededState.bypassed)
                    + " hits=" + juce::String ((int) seededState.hitCount)
                    + " learned=" + juce::String ((int) seededState.hasLearnedPackage)
                    + " globalDelay=" + juce::String (map.globalBase.globalBaseDelayMs, 5)
                    + " effectiveDelay=" + juce::String (seededPackage.effectiveAbsoluteDelayMs, 5)
                    + " effectiveFreq=" + juce::String (seededPackage.effectiveAllpassFreqHz, 5)
                    + " effectiveQ=" + juce::String (seededPackage.effectiveAllpassQ, 5));
        logMessage ("direct state valid=" + juce::String ((int) directRoundTrip.valid)
                    + " count=" + juce::String (getOccupiedDynamicStateCount (directRoundTrip))
                    + " hostedBytes=" + juce::String (hostedRead.bytes)
                    + " hostedChildren=" + hostedRead.children);
        logMessage ("restored state valid=" + juce::String ((int) restoredMap.valid)
                    + " id=" + juce::String ((int64) restoredState.stableStateId)
                    + " policy=" + juce::String ((int) restoredState.correctionPolicy)
                    + " enabled=" + juce::String ((int) restoredState.enabled)
                    + " bypassed=" + juce::String ((int) restoredState.bypassed)
                    + " hits=" + juce::String ((int) restoredState.hitCount)
                    + " learned=" + juce::String ((int) restoredState.hasLearnedPackage)
                    + " effectiveDelay=" + juce::String (restoredPackage.effectiveAbsoluteDelayMs, 5)
                    + " effectiveFreq=" + juce::String (restoredPackage.effectiveAllpassFreqHz, 5)
                    + " effectiveQ=" + juce::String (restoredPackage.effectiveAllpassQ, 5));
        expectEquals ((int) getOccupiedDynamicStateCount (restoredMap), (int) getOccupiedDynamicStateCount (map),
                      "hosted state readback preserves Dynamic State count");
        expectEquals ((int64) restoredState.stableStateId, (int64) seededState.stableStateId,
                      "hosted state readback preserves stable State id");
        expect (restoredState.correctionPolicy == DynamicCorrectionPolicy::LearnedState
                    && restoredState.enabled && ! restoredState.bypassed && restoredState.hasLearnedPackage,
                "hosted State is enabled, unbypassed, learned, and correction eligible");
        expect (std::abs (restoredPackage.effectiveAbsoluteDelayMs - map.globalBase.globalBaseDelayMs) > 1.0e-4
                    || std::abs (restoredPackage.effectiveAllpassFreqHz - map.globalBase.globalAllpassFreqHz) > 1.0e-4,
                "effective LearnedState package is materially distinct from Global");
        TestPlayHead positivePlayHead (48000.0, 120.0), silentPlayHead (48000.0, 120.0), wrongPlayHead (48000.0, 120.0);
        juce::String positiveInput, silentInput, wrongInput;
        const auto withKick = process (*instance, fixture, 256, HostedKickRoute::Sidechain, &positivePlayHead, &positiveInput);
        const auto withoutKick = process (*instance, fixture, 256, HostedKickRoute::Silent, &silentPlayHead, &silentInput);
        const auto wrongChannel = process (*instance, fixture, 256, HostedKickRoute::WrongChannel, &wrongPlayHead, &wrongInput);
        logMessage (positiveInput);
        logMessage (silentInput);
        logMessage (wrongInput);
        expect (AudioMetrics::allFinite (withKick) && AudioMetrics::allFinite (withoutKick), "hosted output is finite");
        expectLessThan (AudioMetrics::peakAbs (withKick), kPeakLimit, "hosted output peak bounded");
        expect (withKick != withoutKick, "kick sidechain changes hosted production processing versus silent-sidechain control");
        expect (silentInput.fromFirstOccurrenceOf ("processChannels", false, false)
                    == wrongInput.fromFirstOccurrenceOf ("processChannels", false, false)
                        .upToFirstOccurrenceOf (" offBusKick", false, false),
                "negative controls have identical actual JUCE bus input content");
        expect (withoutKick == wrongChannel, "wrong-channel control is equivalent to silent sidechain");

        auto fallbackMap = map;
        fallbackMap.states[0].hasLearnedPackage = false;
        fallbackMap.states[0].learnedPackage = {};
        fallbackMap.states[0].manualTrim = makeZeroDynamicManualTrim();
        fallbackMap.states[0].correctionPolicy = DynamicCorrectionPolicy::GlobalFallback;
        expect (isRuntimeEligibleDynamicStateMap (fallbackMap), "GlobalFallback diagnostic map is runtime eligible");
        const auto fallbackPackage = resolveDynamicPackage (fallbackMap, fallbackMap.states[0].stableStateId, 1.0, fixture.sampleRate);
        logMessage ("global package delay=" + juce::String (map.globalBase.globalBaseDelayMs, 5)
                    + " freq=" + juce::String (map.globalBase.globalAllpassFreqHz, 5)
                    + " q=" + juce::String (map.globalBase.globalAllpassQ, 5)
                    + " stages=" + juce::String (map.globalBase.allpassStages)
                    + " fallback effective delay=" + juce::String (fallbackPackage.effectiveAbsoluteDelayMs, 5)
                    + " freq=" + juce::String (fallbackPackage.effectiveAllpassFreqHz, 5)
                    + " q=" + juce::String (fallbackPackage.effectiveAllpassQ, 5));
        auto fallbackInstance = instantiate (formats, description, 48000.0, 256, error);
        expect (fallbackInstance != nullptr && configureStereoSidechain (*fallbackInstance), "GlobalFallback diagnostic instance configures");
        std::vector<float> fallbackOutput;
        if (fallbackInstance != nullptr)
        {
            expect (installCorrectiveMap (*fallbackInstance, fallbackMap), "GlobalFallback map restores through hosted VST3 state");
            TestPlayHead fallbackPlayHead (48000.0, 120.0);
            fallbackOutput = process (*fallbackInstance, fixture, 256, HostedKickRoute::Sidechain, &fallbackPlayHead);
        }
        constexpr double kPositiveDistanceThreshold = 1.0e-5;
        constexpr double kNegativeEquivalenceTolerance = 1.0e-7;
        const int latency = (int) std::lround (fixture.sampleRate * 0.020);
        const double positiveSilent = settledMedianDistance (withKick, withoutKick, fixture, latency);
        const double positiveWrong = settledMedianDistance (withKick, wrongChannel, fixture, latency);
        const double negativeDistance = settledMedianDistance (withoutKick, wrongChannel, fixture, latency);
        const double positiveFallback = settledMedianDistance (withKick, fallbackOutput, fixture, latency);
        logMessage ("settled median distances positive-silent=" + juce::String (positiveSilent, 9)
                    + " positive-wrong=" + juce::String (positiveWrong, 9)
                    + " silent-wrong=" + juce::String (negativeDistance, 9)
                    + " positive-globalFallback=" + juce::String (positiveFallback, 9));
        expectGreaterThan (positiveSilent, kPositiveDistanceThreshold, "settled Positive differs from Silent");
        expectGreaterThan (positiveWrong, kPositiveDistanceThreshold, "settled Positive differs from Wrong-channel");
        expect (negativeDistance <= kNegativeEquivalenceTolerance, "settled negative controls are equivalent");
        expectGreaterThan (positiveFallback, kPositiveDistanceThreshold, "settled LearnedState differs from GlobalFallback");

        for (const auto rate : kRates)
            for (const auto block : kBlocks)
            {
                const auto rateFixture = makeHostedSidechainFixture (rate);
                const auto rateMap = canonicalMap (rateFixture);
                DynamicFingerprintObservation rateObservation;
                expect (extractDynamicFingerprintOffline (rateFixture.bass.data(), rateFixture.kick.data(),
                                                          (int) rateFixture.bass.size(), rate, rateFixture.triggerSamples.front(), rateObservation));
                expect (matchDynamicFingerprint (rateObservation, rateMap).decision == DynamicMatchDecision::Matched,
                        "rate-specific fixture matches its persisted State");
                instance->prepareToPlay (rate, 512);
                expectEquals (instance->getLatencySamples(), (int) std::lround (rate * 0.020), "prepared latency contract");
                instance->releaseResources();
                const auto output = process (*instance, rateFixture, block, HostedKickRoute::Sidechain);
                expect (AudioMetrics::allFinite (output), "finite hosted output at " + juce::String (rate, 0) + "/" + juce::String (block));
                expectLessThan (AudioMetrics::peakAbs (output), kPeakLimit, "bounded hosted output");
                expectEquals (instance->getLatencySamples(), (int) std::lround (rate * 0.020), "latency contract");
            }

        juce::MemoryBlock saved;
        instance->getStateInformation (saved);
        instance.reset();
        auto restored = instantiate (formats, description, 48000.0, 256, error);
        expect (restored != nullptr, "re-instantiation succeeds: " + error);
        if (restored != nullptr)
        {
            expect (configureStereoSidechain (*restored), "re-instantiated sidechain config accepted");
            restored->setStateInformation (saved.getData(), (int) saved.getSize());
            juce::MemoryBlock roundTrip;
            restored->getStateInformation (roundTrip);
            expect (saved == roundTrip, "persistent VST3 state survives destroy/recreate/restore exactly");
            const auto recreatedRead = readHostedDynamicMap (*restored);
            expect (recreatedRead.map.valid
                        && recreatedRead.map.states[0].stableStateId == map.states[0].stableStateId
                        && recreatedRead.map.states[0].correctionPolicy == DynamicCorrectionPolicy::LearnedState
                        && recreatedRead.map.states[0].hasLearnedPackage,
                    "destroy/recreate preserves the Dynamic State semantic model");
            restored->setStateInformation (saved.getData(), juce::jmin (8, (int) saved.getSize()));
            const auto safe = process (*restored, fixture, 128, HostedKickRoute::Silent);
            expect (AudioMetrics::allFinite (safe), "truncated hosted state fails safely");
        }

        for (int i = 0; i < 100; ++i)
        {
            auto cycle = instantiate (formats, description, 48000.0, 64, error);
            expect (cycle != nullptr, "cycle instantiate " + juce::String (i));
            if (cycle != nullptr) { expect (configureStereoSidechain (*cycle)); (void) process (*cycle, fixture, 64, (i & 1) != 0 ? HostedKickRoute::Sidechain : HostedKickRoute::Silent); }
        }
    }
};

static HostAcceptanceTests hostAcceptanceTests;
