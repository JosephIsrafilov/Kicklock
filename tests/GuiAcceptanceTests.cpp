#include "TestCommon.h"

#include "ui/DynamicWorkspace.h"

namespace
{
    DynamicWorkspaceViewModel modelForGui()
    {
        DynamicWorkspaceViewModel model;
        model.mode = CorrectionMode::Dynamic;
        model.runtime.source = DynamicMapSource::NewDynamicStateMap;
        model.runtime.mapValid = true;
        model.runtime.stateCount = 1;
        model.runtime.inputStatus = DynamicInputStatus::Active;
        model.runtime.sidechainPresent = true;
        model.runtime.states[0].occupied = true;
        model.runtime.states[0].stableStateId = 4242;
        model.runtime.states[0].slot = 0;
        model.runtime.states[0].hasCorrection = true;
        model.runtime.states[0].evidence = DynamicStateEvidence::Stable;
        return model;
    }
}

class GuiAcceptanceTests : public juce::UnitTest
{
public:
    GuiAcceptanceTests() : juce::UnitTest ("GuiAcceptance", "0.4.0") {}

    void runTest() override
    {
        beginTest ("Dynamic Workspace renders the new input status and cards");
        {
            DynamicWorkspace workspace;
            workspace.setSize (1180, 820);
            workspace.setModel (modelForGui());
            expectEquals (workspace.getVisibleCardCount(), 1);
            expectEquals (workspace.getNumChildComponents(), 10);
            workspace.selectDetailState (4242);
            expectEquals ((int64_t) workspace.getSelectedDetailStableStateId(), (int64_t) 4242);

            auto model = modelForGui();
            model.runtime.inputStatus = DynamicInputStatus::WaitingForBass;
            workspace.setModel (model);
            expectEquals (workspace.getVisibleCardCount(), 1);
        }

        beginTest ("Editor construction and close/reopen use canonical parameter IDs");
        {
            KickLockAudioProcessor processor;
            processor.prepareToPlay (48000.0, 128);
            for (const auto* id : { "delay_ms", "polarity_invert", "allpass_enable", "allpass_freq",
                                    "correction_mode", "dynamic_strength" })
                expect (processor.apvts.getParameter (id) != nullptr, id);

            for (int i = 0; i < 3; ++i)
            {
                std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
                expect (editor != nullptr);
                editor->setSize (900 + i * 140, 680 + i * 70);
                if (auto* mode = processor.apvts.getParameter ("correction_mode"))
                    mode->setValueNotifyingHost (mode->convertTo0to1 (1.0f));
                editor.reset();
            }
        }
    }
};

static GuiAcceptanceTests guiAcceptanceTests;
