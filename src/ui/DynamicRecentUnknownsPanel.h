#pragma once

#include <functional>
#include <memory>

#include "DynamicWorkspaceHelpers.h"

// Phase 12, Section 15: Recent Unknown Events UI. Shows the bounded,
// non-persistent coalesced cluster list and offers Create Manual State
// (only meaningful once a cluster has enough repeatable evidence - the
// transaction API itself is the source of truth for that, this UI never
// second-guesses it) / Ignore / Clear. "Assign to Existing State" is
// explicitly out of scope for this phase (see DYNAMIC_DESIGN_FREEZE.md).
class DynamicRecentUnknownsPanel : public juce::Component
{
public:
    DynamicRecentUnknownsPanel();
    ~DynamicRecentUnknownsPanel() override;

    void setModel (const DynamicWorkspaceViewModel& model);

    void paint (juce::Graphics&) override;
    void resized() override;

    std::function<void (uint64_t)> onCreateManualState;
    std::function<void (uint64_t)> onIgnore;
    std::function<void()> onClear;

private:
    class ClusterRow;

    static constexpr int kMaxVisibleRows = 6;

    DynamicWorkspaceViewModel model;
    std::array<std::unique_ptr<ClusterRow>, kMaxVisibleRows> rows;
    juce::Label headerLabel;
    juce::TextButton clearAllButton { "Clear All" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DynamicRecentUnknownsPanel)
};
