#include "DynamicRecentUnknownsPanel.h"

namespace
{
    const auto kPanel = juce::Colour (0xff171d23);
    const auto kBorder = juce::Colour (0xff2b3540);
    const auto kText = juce::Colour (0xffedf2f7);
    const auto kMuted = juce::Colour (0xff97a5b2);
    const auto kAmber = juce::Colour (0xfff59e0b);
}

class DynamicRecentUnknownsPanel::ClusterRow : public juce::Component
{
public:
    explicit ClusterRow (DynamicRecentUnknownsPanel& ownerIn) : owner (ownerIn)
    {
        label.setColour (juce::Label::textColourId, kText);
        label.setFont (juce::Font (juce::FontOptions (10.0f)));
        addAndMakeVisible (label);

        createButton.setButtonText ("Create Manual State");
        createButton.setName ("Create Manual State from this Unknown cluster");
        createButton.setDescription ("Forms a new Manual State from this repeatable Unknown/Ambiguous cluster.");
        createButton.onClick = [this] { if (owner.onCreateManualState) owner.onCreateManualState (cluster.eventId); };
        addAndMakeVisible (createButton);

        ignoreButton.setButtonText ("Ignore");
        ignoreButton.setName ("Ignore this Unknown cluster");
        ignoreButton.setDescription ("Removes this cluster from the Recent Unknowns list without creating a State.");
        ignoreButton.onClick = [this] { if (owner.onIgnore) owner.onIgnore (cluster.eventId); };
        addAndMakeVisible (ignoreButton);
    }

    void setCluster (const DynamicRecentUnknownCluster& next)
    {
        cluster = next;
        setVisible (true);
        label.setText (
            "Unknown x" + juce::String (cluster.repeatCount)
                + "  dist " + juce::String (cluster.bestDistance, 2)
                + "  " + (cluster.outcome == DynamicRecentUnknownOutcome::Ambiguous ? "Ambiguous" : "Unknown"),
            juce::dontSendNotification);
        const bool canCreate = cluster.repeatCount >= DynamicStateMapContract::kManualMinimumRepeatableHits;
        createButton.setEnabled (canCreate);
        createButton.setTooltip (canCreate ? juce::String()
            : juce::String ("Need at least 3 repeatable hits (currently " + juce::String (cluster.repeatCount) + ")."));
        repaint();
    }

    void resized() override
    {
        auto area = getLocalBounds();
        ignoreButton.setBounds (area.removeFromRight (60).reduced (1, 2));
        area.removeFromRight (4);
        createButton.setBounds (area.removeFromRight (130).reduced (1, 2));
        area.removeFromRight (4);
        label.setBounds (area);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (kBorder.withAlpha (0.6f));
        g.drawLine (0.0f, (float) getHeight() - 1.0f, (float) getWidth(), (float) getHeight() - 1.0f);
    }

private:
    DynamicRecentUnknownsPanel& owner;
    DynamicRecentUnknownCluster cluster;
    juce::Label label;
    juce::TextButton createButton;
    juce::TextButton ignoreButton;
};

DynamicRecentUnknownsPanel::DynamicRecentUnknownsPanel()
{
    setName ("Recent Unknown Events");
    setDescription ("Repeatable unrecognized kick/bass conflicts, most recent first.");

    headerLabel.setColour (juce::Label::textColourId, kAmber);
    headerLabel.setFont (juce::Font (juce::FontOptions (11.0f)).boldened());
    addAndMakeVisible (headerLabel);

    clearAllButton.setName ("Clear Recent Unknowns");
    clearAllButton.onClick = [this] { if (onClear) onClear(); };
    addAndMakeVisible (clearAllButton);

    for (auto& row : rows)
    {
        row = std::make_unique<ClusterRow> (*this);
        addAndMakeVisible (*row);
        row->setVisible (false);
    }
}

DynamicRecentUnknownsPanel::~DynamicRecentUnknownsPanel() = default;

void DynamicRecentUnknownsPanel::setModel (const DynamicWorkspaceViewModel& newModel)
{
    model = newModel;
    const int count = (int) model.recentUnknownClusters.size();
    headerLabel.setText ("Recent Unknown Events (" + juce::String (count) + ")"
        + (model.recentUnknownOverflowCount > 0
            ? "  -  " + juce::String ((int64_t) model.recentUnknownOverflowCount) + " dropped (full)"
            : juce::String()),
        juce::dontSendNotification);
    clearAllButton.setEnabled (count > 0);

    for (int i = 0; i < kMaxVisibleRows; ++i)
    {
        if (i < count)
            rows[(size_t) i]->setCluster (model.recentUnknownClusters[(size_t) i]);
        else
            rows[(size_t) i]->setVisible (false);
    }
    // resized() only positions rows that are currently visible; the parent's
    // setBounds() only re-triggers this when its size actually changes, so a
    // row becoming visible/hidden with no accompanying container resize
    // would otherwise keep stale or empty (0,0,0,0) bounds.
    resized();
    repaint();
}

void DynamicRecentUnknownsPanel::resized()
{
    auto area = getLocalBounds().reduced (10, 6);
    auto headerRow = area.removeFromTop (18);
    clearAllButton.setBounds (headerRow.removeFromRight (70));
    headerLabel.setBounds (headerRow);

    area.removeFromTop (4);
    for (auto& row : rows)
    {
        if (! row->isVisible())
            continue;
        row->setBounds (area.removeFromTop (20));
        area.removeFromTop (2);
    }
}

void DynamicRecentUnknownsPanel::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour (kPanel);
    g.fillRoundedRectangle (bounds, 7.0f);
    g.setColour (kBorder);
    g.drawRoundedRectangle (bounds, 7.0f, 1.0f);

    if (model.recentUnknownClusters.empty())
    {
        g.setColour (kMuted);
        g.setFont (juce::Font (juce::FontOptions (10.0f)));
        g.drawText ("No repeatable unrecognized conflicts right now.",
                    getLocalBounds().reduced (10).removeFromBottom (60), juce::Justification::centred);
    }
}
