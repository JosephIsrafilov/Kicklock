#include "TestCommon.h"

#include "ui/DynamicWorkspaceHelpers.h"

namespace
{
    DynamicRuntimeSnapshot newMapSnapshot()
    {
        DynamicRuntimeSnapshot snapshot;
        snapshot.source = DynamicMapSource::NewDynamicStateMap;
        snapshot.mapValid = true;
        snapshot.stateCount = 1;
        snapshot.sidechainPresent = true;
        return snapshot;
    }

    DynamicStateMap makeRuntimeMap()
    {
        auto map = makeEmptyDynamicStateMap();
        map.valid = true;
        map.globalBase.crossoverEnabled = true;
        map.globalBase.crossoverHz = 150.0f;
        map.globalBase.allpassEnabled = true;
        map.globalBase.globalAllpassFreqHz = 100.0f;
        map.globalBase.globalAllpassQ = 0.7f;
        map.globalBase.allpassStages = 2;
        map.calibration.valid = true;
        map.calibration.absoluteDistanceThreshold = 0.01f;
        map.calibration.ambiguityMargin = 0.005f;
        auto& state = map.states[0];
        state.occupied = true;
        state.stableStateId = 9001;
        state.fingerprint.valid = true;
        state.fingerprint.featureCount = DynamicStateMapContract::kMaxFingerprintFeatures;
        for (int i = 0; i < DynamicStateMapContract::kMaxFingerprintFeatures; ++i)
            state.fingerprint.features[(size_t) i] = -1.0f;
        state.hasLearnedPackage = true;
        state.learnedPackage.delayDeltaMs = 1.0f;
        state.learnedPackage.allpassFreqHz = 80.0f;
        state.learnedPackage.allpassQ = 1.2f;
        state.origin = DynamicStateOrigin::Auto;
        state.evidence = DynamicStateEvidence::Stable;
        state.hitCount = 6;
        state.repeatability = 0.9f;
        state.ambiguity = 0.1f;
        map.nextStateId = 9002;
        return map;
    }

    void prepareProcessor (KickLockAudioProcessor& processor, bool sidechain,
                           int blockSize)
    {
        processor.enableAllBuses();
        processor.setRateAndBufferSizeDetails (kSampleRate, blockSize);
        processor.prepareToPlay (kSampleRate, blockSize);
        if (! sidechain)
        {
            const bool configured = processor.setChannelLayoutOfBus (
                true, 1, juce::AudioChannelSet::disabled());
            jassert (configured);
            juce::ignoreUnused (configured);
        }
    }

    DynamicInputStatus processStatus (float bassLevel, float kickLevel,
                                      bool sidechain)
    {
        constexpr int blockSize = 512;
        KickLockAudioProcessor processor;
        prepareProcessor (processor, sidechain, blockSize);

        const int channels = juce::jmax (processor.getTotalNumInputChannels(),
                                         processor.getTotalNumOutputChannels());
        juce::AudioBuffer<float> buffer (channels, blockSize);
        buffer.clear();
        for (int i = 0; i < blockSize; ++i)
        {
            buffer.setSample (0, i, bassLevel);
            if (channels > 1)
                buffer.setSample (1, i, bassLevel);
            if (sidechain && channels > 2)
                buffer.setSample (2, i, kickLevel);
            if (sidechain && channels > 3)
                buffer.setSample (3, i, kickLevel);
        }

        juce::MidiBuffer midi;
        processor.processBlock (buffer, midi);
        const auto snapshot = processor.getDynamicRuntimeSnapshotForTesting();
        return snapshot.inputStatus;
    }

    void processLevels (KickLockAudioProcessor& processor, float bassLevel,
                        float kickLevel, int blockSize)
    {
        const int channels = juce::jmax (processor.getTotalNumInputChannels(),
                                         processor.getTotalNumOutputChannels());
        juce::AudioBuffer<float> buffer (channels, blockSize);
        buffer.clear();
        for (int i = 0; i < blockSize; ++i)
        {
            buffer.setSample (0, i, bassLevel);
            if (channels > 1)
                buffer.setSample (1, i, bassLevel);
            if (channels > 2)
                buffer.setSample (2, i, kickLevel);
            if (channels > 3)
                buffer.setSample (3, i, kickLevel);
        }
        juce::MidiBuffer midi;
        processor.processBlock (buffer, midi);
    }
}

class DynamicInputStatusTests : public juce::UnitTest
{
public:
    DynamicInputStatusTests() : juce::UnitTest ("DynamicInputStatus", "0.4.0") {}

    void runTest() override
    {
        beginTest ("Fixed input-status values are stable and exhaustive");
        expectEquals ((int) DynamicInputStatus::NoSidechain, 0);
        expectEquals ((int) DynamicInputStatus::WaitingForKick, 1);
        expectEquals ((int) DynamicInputStatus::WaitingForBass, 2);
        expectEquals ((int) DynamicInputStatus::SignalTooLow, 3);
        expectEquals ((int) DynamicInputStatus::Active, 4);
        for (const auto status : { DynamicInputStatus::NoSidechain,
                                   DynamicInputStatus::WaitingForKick,
                                   DynamicInputStatus::WaitingForBass,
                                   DynamicInputStatus::SignalTooLow,
                                   DynamicInputStatus::Active })
            expect (isValidDynamicInputStatus (status));
        expect (! isValidDynamicInputStatus (static_cast<DynamicInputStatus> (255)));

        beginTest ("Held activity keeps normal kick gaps active, then expires");
        {
            SignalActivityTracker tracker;
            tracker.prepare (kSampleRate, 1500.0f, 1.0e-3f);
            constexpr int block = 512;
            for (int beat = 0; beat < 3; ++beat)
            {
                tracker.pushBlock (0.3f, block);
                for (int i = 0; i < (int) (kSampleRate * 0.8 / block); ++i)
                {
                    tracker.pushBlock (0.0f, block);
                    expect (tracker.isActive(), "a normal 800 ms gap must stay held");
                }
            }

            for (int i = 0; i < (int) (kSampleRate * 1.6 / block); ++i)
                tracker.pushBlock (0.0f, block);
            expect (! tracker.isActive(), "activity must expire after the 1.5 s hold");
        }

        beginTest ("Processor classifies no-signal, low-signal, and active input");
        expectEquals ((int) processStatus (0.0f, 0.0f, false),
                      (int) DynamicInputStatus::NoSidechain);
        expectEquals ((int) processStatus (0.1f, 0.0f, true),
                      (int) DynamicInputStatus::WaitingForKick);
        expectEquals ((int) processStatus (0.0f, 0.1f, true),
                      (int) DynamicInputStatus::WaitingForBass);
        expectEquals ((int) processStatus (2.0e-3f, 2.0e-3f, true),
                      (int) DynamicInputStatus::SignalTooLow);
        expectEquals ((int) processStatus (0.1f, 0.1f, true),
                      (int) DynamicInputStatus::Active);

        beginTest ("Bus support accepts symmetric main layouts and optional sidechain layouts");
        {
            KickLockAudioProcessor processor;
            processor.enableAllBuses();
            for (const auto sidechain : { juce::AudioChannelSet::disabled(),
                                          juce::AudioChannelSet::mono(),
                                          juce::AudioChannelSet::stereo() })
            {
                for (const auto main : { juce::AudioChannelSet::mono(),
                                         juce::AudioChannelSet::stereo() })
                {
                    auto layout = processor.getBusesLayout();
                    layout.inputBuses.set (0, main);
                    layout.outputBuses.set (0, main);
                    layout.inputBuses.set (1, sidechain);
                    expect (processor.isBusesLayoutSupported (layout));
                }
            }

            auto asymmetric = processor.getBusesLayout();
            asymmetric.inputBuses.set (0, juce::AudioChannelSet::mono());
            asymmetric.outputBuses.set (0, juce::AudioChannelSet::stereo());
            asymmetric.inputBuses.set (1, juce::AudioChannelSet::disabled());
            expect (! processor.isBusesLayoutSupported (asymmetric));
            asymmetric.inputBuses.set (0, juce::AudioChannelSet::stereo());
            asymmetric.outputBuses.set (0, juce::AudioChannelSet::mono());
            expect (! processor.isBusesLayoutSupported (asymmetric));
        }

        beginTest ("Usable-input timeout resets Dynamic to Global and return starts clean");
        {
            constexpr int blockSize = 512;
            KickLockAudioProcessor processor;
            prepareProcessor (processor, true, blockSize);
            if (auto* mode = processor.apvts.getParameter ("correction_mode"))
                mode->setValueNotifyingHost (mode->convertTo0to1 (1.0f));
            expect (processor.publishDynamicStateMapForTesting (makeRuntimeMap()));

            processLevels (processor, 0.1f, 0.1f, blockSize);
            auto snapshot = processor.getDynamicRuntimeSnapshotForTesting();
            expect (snapshot.inputStatus == DynamicInputStatus::Active);
            const auto timestampBeforeGap = snapshot.timestampSample;

            for (int i = 0; i < (int) (kSampleRate * 1.0 / blockSize); ++i)
            {
                processLevels (processor, 0.0f, 0.0f, blockSize);
                const auto betweenHits = processor.getDynamicRuntimeSnapshotForTesting();
                expect (betweenHits.inputStatus == DynamicInputStatus::Active,
                        "short normal gaps do not reset held Dynamic input");
                expect (betweenHits.timestampSample > timestampBeforeGap,
                        "normal gaps keep the monotonic runtime timeline");
            }

            for (int i = 0; i < (int) (kSampleRate * 0.8 / blockSize); ++i)
            {
                processLevels (processor, 0.0f, 0.0f, blockSize);
                (void) processor.getDynamicRuntimeSnapshotForTesting();
            }
            snapshot = processor.getDynamicRuntimeSnapshotForTesting();
            expect (snapshot.inputStatus == DynamicInputStatus::WaitingForKick,
                    "timeout status=" + juce::String ((int) snapshot.inputStatus)
                        + " kickHeld=" + juce::String (processor.isKickActive() ? 1 : 0)
                        + " bassHeld=" + juce::String (processor.isBassActive() ? 1 : 0)
                        + " kickRms=" + juce::String (processor.getKickSignalRms(), 6)
                        + " bassRms=" + juce::String (processor.getBassSignalRms(), 6));
            expect (snapshot.activeBranchKind == DynamicSelectorBranchKind::Global,
                    "timeout branch=" + juce::String ((int) snapshot.activeBranchKind)
                        + " state=" + juce::String ((juce::int64) snapshot.activeSemanticStateId));
            expectEquals ((int64_t) snapshot.activeSemanticStateId, (int64_t) 0);
            expect (! snapshot.holdActive);

            processLevels (processor, 0.1f, 0.1f, blockSize);
            snapshot = processor.getDynamicRuntimeSnapshotForTesting();
            expect (snapshot.inputStatus == DynamicInputStatus::Active);
            expect (snapshot.activeBranchKind == DynamicSelectorBranchKind::Global,
                    "return branch=" + juce::String ((int) snapshot.activeBranchKind)
                        + " state=" + juce::String ((juce::int64) snapshot.activeSemanticStateId)
                        + " fade=" + juce::String (processor.getDynamicSelectorDiagnosticsForTesting().fadeActive ? 1 : 0)
                        + " decision=" + juce::String ((int) processor.getDynamicSelectorDiagnosticsForTesting().lastDecision)
                        + "; returning signal does not revive the old State without a new match");
        }

        beginTest ("Workspace status follows the 0.4.0 priority contract");
        {
            auto snapshot = newMapSnapshot();
            snapshot.inputStatus = DynamicInputStatus::NoSidechain;
            expectEquals (dynamicWorkspaceRuntimeStatus (snapshot), juce::String ("NO SIDECHAIN"));
            snapshot.inputStatus = DynamicInputStatus::WaitingForKick;
            expectEquals (dynamicWorkspaceRuntimeStatus (snapshot), juce::String ("WAITING FOR KICK"));
            snapshot.inputStatus = DynamicInputStatus::WaitingForBass;
            expectEquals (dynamicWorkspaceRuntimeStatus (snapshot), juce::String ("WAITING FOR BASS"));
            snapshot.inputStatus = DynamicInputStatus::SignalTooLow;
            expectEquals (dynamicWorkspaceRuntimeStatus (snapshot), juce::String ("SIGNAL TOO LOW"));

            snapshot.inputStatus = DynamicInputStatus::Active;
            snapshot.holdActive = true;
            expectEquals (dynamicWorkspaceRuntimeStatus (snapshot), juce::String ("HOLD"));
            snapshot.holdActive = false;
            snapshot.activeBranchKind = DynamicSelectorBranchKind::State;
            snapshot.activeSemanticStateId = 1;
            expectEquals (dynamicWorkspaceRuntimeStatus (snapshot), juce::String ("ACTIVE STATE"));
            snapshot.bypassActive = true;
            expectEquals (dynamicWorkspaceRuntimeStatus (snapshot), juce::String ("BYPASSED"));
            snapshot.source = DynamicMapSource::None;
            expectEquals (dynamicWorkspaceRuntimeStatus (snapshot), juce::String ("NO MAP"));
        }
    }
};

static DynamicInputStatusTests dynamicInputStatusTests;
