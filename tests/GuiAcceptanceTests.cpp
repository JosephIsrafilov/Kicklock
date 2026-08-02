#include "TestCommon.h"

#include "PluginEditor.h"
#include "ui/DynamicWorkspace.h"

#include <memory>

namespace
{
    juce::Component* findChildNamed (juce::Component& root, const juce::String& name)
    {
        if (root.getName() == name)
            return &root;

        for (int i = 0; i < root.getNumChildComponents(); ++i)
            if (auto* found = findChildNamed (*root.getChildComponent (i), name))
                return found;
        return nullptr;
    }

    void setParameterValue (KickLockAudioProcessor& processor, const char* id, float value)
    {
        if (auto* parameter = processor.apvts.getParameter (id))
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
    }

    DynamicWorkspaceViewModel dynamicModelWithStates()
    {
        DynamicWorkspaceViewModel model;
        model.mode = CorrectionMode::Dynamic;
        model.runtime.source = DynamicMapSource::NewDynamicStateMap;
        model.runtime.mapValid = true;
        model.runtime.sidechainPresent = true;
        model.runtime.stateCount = 2;

        for (int slot = 0; slot < 2; ++slot)
        {
            auto& state = model.runtime.states[(size_t) slot];
            state.occupied = true;
            state.slot = slot;
            state.stableStateId = (uint64_t) (101 + slot);
            state.origin = DynamicStateOrigin::Auto;
            state.evidence = DynamicStateEvidence::Stable;
            state.enabled = true;
            state.hitCount = DynamicStateMapContract::kStableAutoMinimumRepeatableHits;
            state.repeatability = 0.8f;
            state.ambiguity = 0.1f;
            state.hasCorrection = true;
            state.assessment = DynamicCorrectionAssessment::PredictedImprovement;
            state.predicted.availability = DynamicMeasurementAvailability::Available;
            state.predicted.assessment = DynamicCorrectionAssessment::PredictedImprovement;
        }
        return model;
    }
}

class GuiAcceptanceTests : public juce::UnitTest
{
public:
    GuiAcceptanceTests() : juce::UnitTest ("GuiAcceptance", "GuiAcceptance") {}

    void runTest() override
    {
        beginTest ("Editor opens at a supported size and exposes stable control names");
        {
            KickLockAudioProcessor processor;
            processor.prepareToPlay (48000.0, 256);
            std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());

            expect (editor != nullptr, "processor creates an editor");
            if (editor == nullptr)
                return;

            expectGreaterOrEqual (editor->getWidth(), 900, "default width meets the supported minimum");
            expectGreaterOrEqual (editor->getHeight(), 680, "default height meets the supported minimum");
            expectLessOrEqual (editor->getWidth(), 2800, "default width is bounded");
            expectLessOrEqual (editor->getHeight(), 1900, "default height is bounded");

            for (const auto& componentName : { juce::String ("Correction Mode"), juce::String ("Dynamic Strength"),
                                               juce::String ("Dynamic Workspace"), juce::String ("Dynamic State Inspector"),
                                               juce::String ("Recent Unknown Events") })
                expect (findChildNamed (*editor, componentName) != nullptr,
                        "reachable named component: " + componentName);
        }

        beginTest ("Mode switching reveals Dynamic workspace and Strength persists through state restore");
        {
            KickLockAudioProcessor processor;
            processor.prepareToPlay (48000.0, 256);
            std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
            expect (editor != nullptr);
            if (editor == nullptr)
                return;

            auto* workspace = findChildNamed (*editor, "Dynamic Workspace");
            expect (workspace != nullptr);
            setParameterValue (processor, "correction_mode", 1.0f);
            // Parameter listeners deliberately marshal presentation work to
            // JUCE's message thread. Recreate on the stored mode so this
            // headless deterministic gate verifies the same construction path
            // without pretending to run a host message loop.
            editor.reset (processor.createEditor());
            workspace = editor != nullptr ? findChildNamed (*editor, "Dynamic Workspace") : nullptr;
            expect (workspace != nullptr && workspace->isVisible(),
                    "Dynamic mode makes the workspace reachable after editor recreation");

            auto* strength = dynamic_cast<juce::Slider*> (findChildNamed (*editor, "Dynamic Strength"));
            expect (strength != nullptr);
            if (strength != nullptr)
            {
                strength->setValue (0.37, juce::sendNotificationSync);
                expectWithinAbsoluteError (processor.apvts.getRawParameterValue ("dynamic_strength")->load(),
                                           0.37f, 1.0e-5f,
                                           "Strength control updates the persistent parameter");
            }

            juce::MemoryBlock state;
            processor.getStateInformation (state);
            setParameterValue (processor, "correction_mode", 0.0f);
            setParameterValue (processor, "dynamic_strength", 1.0f);
            processor.setStateInformation (state.getData(), (int) state.getSize());
            expectWithinAbsoluteError (processor.apvts.getRawParameterValue ("correction_mode")->load(), 1.0f, 1.0e-5f,
                                       "Dynamic mode restores from persistent state");
            expectWithinAbsoluteError (processor.apvts.getRawParameterValue ("dynamic_strength")->load(), 0.37f, 1.0e-5f,
                                       "Strength restores from persistent state");
        }

        beginTest ("State selection is isolated to the selected State model");
        {
            DynamicWorkspace workspace;
            workspace.setSize (1180, 520);
            workspace.setModel (dynamicModelWithStates());
            expectEquals (workspace.getVisibleCardCount(), 2);

            workspace.selectDetailState (101);
            expectEquals ((int64) workspace.getSelectedDetailStableStateId(), (int64) 101);
            workspace.selectDetailState (102);
            expectEquals ((int64) workspace.getSelectedDetailStableStateId(), (int64) 102,
                          "selection moves to the intended State rather than mutating a global selection");
            workspace.selectDetailState (102);
            expectEquals ((int64) workspace.getSelectedDetailStableStateId(), (int64) 0,
                          "selecting the same State only clears inspector selection");
        }

        beginTest ("Repeated editor open-close and supported resizes are safe");
        {
            KickLockAudioProcessor processor;
            processor.prepareToPlay (48000.0, 256);
            for (int iteration = 0; iteration < 50; ++iteration)
            {
                std::unique_ptr<juce::AudioProcessorEditor> editor (processor.createEditor());
                expect (editor != nullptr, "editor opens in cycle " + juce::String (iteration));
                if (editor == nullptr)
                    break;

                editor->setSize (900 + (iteration % 4) * 200, 680 + (iteration % 3) * 120);
                expectGreaterOrEqual (editor->getWidth(), 900);
                expectGreaterOrEqual (editor->getHeight(), 680);
                setParameterValue (processor, "correction_mode", (iteration & 1) != 0 ? 1.0f : 0.0f);
            }
        }
    }
};

static GuiAcceptanceTests guiAcceptanceTests;
