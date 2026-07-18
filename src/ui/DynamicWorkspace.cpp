#include "DynamicWorkspace.h"

namespace
{
    const auto kPanel = juce::Colour (0xff171d23);
    const auto kBorder = juce::Colour (0xff2b3540);
    const auto kText = juce::Colour (0xffedf2f7);
    const auto kMuted = juce::Colour (0xff97a5b2);
    const auto kTeal = juce::Colour (0xff2dd4bf);
    const auto kGreen = juce::Colour (0xff22c55e);
    const auto kAmber = juce::Colour (0xfff59e0b);
    const auto kRed = juce::Colour (0xffef4444);

    juce::Colour colourForRole (DynamicWorkspaceColourRole role)
    {
        switch (role)
        {
            case DynamicWorkspaceColourRole::Teal:  return kTeal;
            case DynamicWorkspaceColourRole::Green: return kGreen;
            case DynamicWorkspaceColourRole::Amber: return kAmber;
            case DynamicWorkspaceColourRole::Red:   return kRed;
            case DynamicWorkspaceColourRole::Muted: return kMuted;
        }
        return kMuted;
    }

    bool measurementsEqual (const DynamicMeasurementSummary& left,
                            const DynamicMeasurementSummary& right) noexcept
    {
        return left.availability == right.availability
            && left.assessment == right.assessment
            && left.rejectionReason == right.rejectionReason
            && left.validWindowCount == right.validWindowCount
            && left.rejectedWindowCount == right.rejectedWindowCount
            && left.beforeScore == right.beforeScore
            && left.afterScore == right.afterScore
            && left.improvementPoints == right.improvementPoints
            && left.medianImprovementPoints == right.medianImprovementPoints
            && left.lowerQuantileImprovementPoints == right.lowerQuantileImprovementPoints
            && left.worstImprovementPoints == right.worstImprovementPoints
            && left.improvedWindowRatio == right.improvedWindowRatio
            && left.confidence == right.confidence
            && left.mapGeneration == right.mapGeneration
            && left.lastUpdateSample == right.lastUpdateSample;
    }

    bool cardsEqual (const DynamicStateCard& left, const DynamicStateCard& right) noexcept
    {
        return left.occupied == right.occupied
            && left.stableStateId == right.stableStateId
            && left.slot == right.slot
            && left.origin == right.origin
            && left.evidence == right.evidence
            && left.enabled == right.enabled
            && left.bypassed == right.bypassed
            && left.hitCount == right.hitCount
            && left.repeatability == right.repeatability
            && left.ambiguity == right.ambiguity
            && left.hasCorrection == right.hasCorrection
            && left.hasLikelyMidi == right.hasLikelyMidi
            && left.likelyMidi == right.likelyMidi
            && left.hasLikelyPitchHz == right.hasLikelyPitchHz
            && left.likelyPitchHz == right.likelyPitchHz
            && left.selected == right.selected
            && left.active == right.active
            && left.activeBranchKind == right.activeBranchKind
            && left.hasCurrentMatchDistance == right.hasCurrentMatchDistance
            && left.currentMatchDistance == right.currentMatchDistance
            && measurementsEqual (left.predicted, right.predicted)
            && measurementsEqual (left.verified, right.verified)
            && left.assessment == right.assessment
            && left.rejectionReason == right.rejectionReason;
    }
}

class DynamicWorkspace::StateCardComponent : public juce::Component,
                                             public juce::SettableTooltipClient
{
public:
    explicit StateCardComponent (std::function<void (uint64_t)> onSelectIn)
        : onSelect (std::move (onSelectIn))
    {
        setWantsKeyboardFocus (true);
    }

    void setCard (const DynamicStateCard& next, bool previewIn, bool detailSelectedIn)
    {
        if (cardsEqual (card, next) && preview == previewIn && detailSelected == detailSelectedIn)
            return;
        card = next;
        preview = previewIn;
        detailSelected = detailSelectedIn;
        setVisible (card.occupied);
        if (card.occupied)
        {
            setName (dynamicStateCardTitle (card));
            setDescription ("Dynamic conflict State. " + dynamicStateEvidenceLabel (card.evidence)
                            + ", " + dynamicStateOriginLabel (card.origin)
                            + ", stable ID " + fullDynamicStateId (card.stableStateId));
            setTooltip ("Stable State ID: " + fullDynamicStateId (card.stableStateId));
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        const auto bounds = getLocalBounds().toFloat().reduced (0.5f);
        const auto accent = colourForRole (dynamicWorkspaceColourRole (card));
        const bool active = card.active && ! preview;
        const bool selected = card.selected && ! preview;
        const float outlineWidth = active ? 2.4f : selected ? 1.6f : detailSelected ? 1.4f : 1.0f;
        const auto outline = active ? kTeal : selected ? kTeal.withAlpha (0.78f)
            : detailSelected ? kText.withAlpha (0.72f) : accent.withAlpha (0.55f);

        g.setColour (kPanel);
        g.fillRoundedRectangle (bounds, 6.0f);
        g.setColour (outline);
        g.drawRoundedRectangle (bounds, 6.0f, outlineWidth);

        auto area = getLocalBounds().reduced (9, 7);
        auto top = area.removeFromTop (17);
        g.setColour (kText);
        g.setFont (juce::Font (juce::FontOptions (12.5f)).boldened());
        g.drawText (dynamicStateCardTitle (card), top.removeFromLeft (top.getWidth() - 70), juce::Justification::centredLeft, true);
        g.setColour (kMuted);
        g.setFont (juce::Font (juce::FontOptions (9.5f)).boldened());
        g.drawText ("SLOT " + juce::String (card.slot + 1), top, juce::Justification::centredRight);

        auto badges = area.removeFromTop (15);
        juce::String badgeText = dynamicStateEvidenceLabel (card.evidence) + "  " + dynamicStateOriginLabel (card.origin);
        if (active) badgeText << "  ACTIVE " << dynamicBranchLabel (card.activeBranchKind);
        else if (selected) badgeText << "  SELECTED";
        else if (detailSelected) badgeText << "  DETAIL";
        if (! card.enabled) badgeText << "  DISABLED";
        else if (card.bypassed) badgeText << "  BYPASSED";
        else if (! card.hasCorrection) badgeText << "  GLOBAL ONLY";
        else badgeText << "  CORRECTION";
        g.setColour (accent);
        g.setFont (juce::Font (juce::FontOptions (9.5f)).boldened());
        g.drawText (badgeText, badges, juce::Justification::centredLeft, true);

        auto evidence = area.removeFromTop (15);
        g.setColour (kMuted);
        g.setFont (juce::Font (juce::FontOptions (9.5f)));
        g.drawText ("Hits " + juce::String (card.hitCount) + "  Repeat "
                    + juce::String ((int) std::round (card.repeatability * 100.0f)) + "%  Amb "
                    + juce::String ((int) std::round (card.ambiguity * 100.0f)) + "%",
                    evidence, juce::Justification::centredLeft, true);

        if (card.hasCurrentMatchDistance && ! preview)
        {
            auto distance = area.removeFromTop (13);
            g.drawText ("Match distance " + juce::String (card.currentMatchDistance, 3)
                        + "  " + dynamicBranchLabel (card.activeBranchKind),
                        distance, juce::Justification::centredLeft, true);
        }

        auto drawMeasurement = [&] (const char* title, const DynamicMeasurementSummary& summary, bool verified)
        {
            auto row = area.removeFromTop (28);
            g.setColour (kMuted);
            g.setFont (juce::Font (juce::FontOptions (9.0f)).boldened());
            g.drawText (title, row.removeFromLeft (53), juce::Justification::centredLeft);
            g.setColour (colourForRole (dynamicMeasurementColourRole (summary)));
            g.drawText (formatDynamicMeasurementAvailability (summary, verified), row.removeFromTop (12),
                        juce::Justification::centredLeft, true);
            g.setColour (kText.withAlpha (0.88f));
            g.setFont (juce::Font (juce::FontOptions (9.0f)));
            auto values = formatDynamicBeforeAfter (summary);
            if (dynamicMeasurementShowsScores (summary.availability))
            {
                values << "  " << formatDynamicImprovementPoints (summary.improvementPoints);
                if (summary.confidence > 0.0f)
                    values << "  Conf " << juce::String ((int) std::round (summary.confidence * 100.0f)) << "%";
            }
            g.drawText (values, row, juce::Justification::centredLeft, true);
        };

        drawMeasurement ("PREDICTED", card.predicted, false);
        drawMeasurement ("VERIFIED", card.verified, true);

        auto footer = area;
        g.setColour (accent);
        g.setFont (juce::Font (juce::FontOptions (9.5f)).boldened());
        auto assessment = dynamicCorrectionAssessmentLabel (card.assessment);
        const auto reason = dynamicCorrectionRejectionLabel (card.rejectionReason);
        if (reason.isNotEmpty()) assessment << " - " << reason;
        g.drawText (assessment, footer, juce::Justification::centredLeft, true);
    }

    void mouseUp (const juce::MouseEvent&) override { choose(); }
    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::returnKey || key == juce::KeyPress::spaceKey)
        {
            choose();
            return true;
        }
        return false;
    }

private:
    void choose()
    {
        if (card.occupied && onSelect)
            onSelect (card.stableStateId);
    }

    DynamicStateCard card;
    bool preview = false;
    bool detailSelected = false;
    std::function<void (uint64_t)> onSelect;
};

DynamicWorkspace::DynamicWorkspace()
{
    setName ("Dynamic Workspace");
    setDescription ("Dynamic conflict State workspace. State selection is for inspection only.");

    for (auto& card : cards)
    {
        card = std::make_unique<StateCardComponent> ([this] (uint64_t stableStateId)
        {
            setSelectedDetailStableStateId (stableStateId);
        });
        addAndMakeVisible (*card);
        card->setVisible (false);
    }

    clearButton.setName ("Clear learned Dynamic data");
    clearButton.setDescription ("Clears applied Dynamic and Legacy learned maps after Learn is idle.");
    clearButton.setTooltip ("Clears applied learned Dynamic data. Revert can restore the previous state when available.");
    clearButton.setWantsKeyboardFocus (true);
    clearButton.onClick = [this] { if (onClear) onClear(); };
    addAndMakeVisible (clearButton);

    revertButton.setName ("Revert Dynamic map");
    revertButton.setDescription ("Restores the previous applied Dynamic map and parameters.");
    revertButton.setTooltip ("Restores the previous applied map through the processor publication path.");
    revertButton.setWantsKeyboardFocus (true);
    revertButton.onClick = [this] { if (onRevert) onRevert(); };
    addAndMakeVisible (revertButton);
}

DynamicWorkspace::~DynamicWorkspace() = default;

void DynamicWorkspace::setDynamicStrengthControls (juce::Label* label, juce::Slider* slider)
{
    dynamicStrengthLabel = label;
    dynamicStrengthSlider = slider;
    resized();
}

void DynamicWorkspace::setModel (const DynamicWorkspaceViewModel& next)
{
    const bool headerChanged = model.previewActive != next.previewActive
        || model.previewApplyAvailable != next.previewApplyAvailable
        || model.previewApplyBlocked != next.previewApplyBlocked
        || model.previewBlockedReason != next.previewBlockedReason
        || model.learnState != next.learnState
        || model.runtime.source != next.runtime.source
        || model.runtime.activeSemanticStateId != next.runtime.activeSemanticStateId
        || model.runtime.selectedSemanticStateId != next.runtime.selectedSemanticStateId
        || model.runtime.activeBranchKind != next.runtime.activeBranchKind
        || model.runtime.holdActive != next.runtime.holdActive
        || model.runtime.fallbackActive != next.runtime.fallbackActive
        || model.runtime.sidechainPresent != next.runtime.sidechainPresent
         || model.runtime.bypassActive != next.runtime.bypassActive
         || model.capturedHits != next.capturedHits
         || model.processedHits != next.processedHits
         || model.learnStatusMessage != next.learnStatusMessage
         || model.clearAvailable != next.clearAvailable
        || model.revertAvailable != next.revertAvailable;

    model = next;
    if (selectedDetailStableStateId != 0 && ! containsStableStateId (selectedDetailStableStateId))
        selectedDetailStableStateId = 0;
    clearButton.setEnabled (model.clearAvailable);
    revertButton.setEnabled (model.revertAvailable);

    const bool preview = model.previewActive;
    const auto visibleCards = dynamicWorkspaceCards (model);
    orderedSlots = orderedDynamicWorkspaceSlots (visibleCards);
    for (int displayIndex = 0; displayIndex < (int) cards.size(); ++displayIndex)
    {
        auto presentation = visibleCards[(size_t) orderedSlots[(size_t) displayIndex]];
        if (preview)
        {
            presentation.selected = false;
            presentation.active = false;
            presentation.activeBranchKind = DynamicSelectorBranchKind::Global;
        }
        else
        {
            presentation.selected = dynamicCardIsSelected (presentation, model.runtime);
            presentation.active = dynamicCardIsActive (presentation, model.runtime);
            presentation.activeBranchKind = presentation.active ? model.runtime.activeBranchKind
                                                                  : DynamicSelectorBranchKind::Global;
        }
        cards[(size_t) displayIndex]->setCard (presentation, preview,
                                                selectedDetailStableStateId == presentation.stableStateId);
    }

    const auto header = dynamicWorkspaceHeaderPresentation (model);
    headerSource = header.source;
    headerStatus = header.status;
    headerDetail = header.detail;

    if (headerChanged)
        repaint (getLocalBounds().removeFromTop (62));
    resized();
}

int DynamicWorkspace::getVisibleCardCount() const noexcept
{
    int count = 0;
    for (const auto& card : cards)
        count += card->isVisible() ? 1 : 0;
    return count;
}

bool DynamicWorkspace::containsStableStateId (uint64_t stableStateId) const noexcept
{
    if (stableStateId == 0)
        return false;
    const auto visibleCards = dynamicWorkspaceCards (model);
    return std::any_of (visibleCards.begin(), visibleCards.end(), [stableStateId] (const DynamicStateCard& card)
    {
        return card.occupied && card.stableStateId == stableStateId;
    });
}

void DynamicWorkspace::setSelectedDetailStableStateId (uint64_t stableStateId)
{
    selectedDetailStableStateId = selectedDetailStableStateId == stableStateId ? 0 : stableStateId;
    setModel (model);
}

void DynamicWorkspace::selectDetailState (uint64_t stableStateId)
{
    if (containsStableStateId (stableStateId))
        setSelectedDetailStableStateId (stableStateId);
}

void DynamicWorkspace::resized()
{
    auto area = getLocalBounds().reduced (12, 10);
    auto header = area.removeFromTop (52);
    auto actions = header.removeFromRight (150);
    clearButton.setBounds (actions.removeFromLeft (72).reduced (1, 8));
    revertButton.setBounds (actions.reduced (1, 8));

    if (dynamicStrengthSlider != nullptr)
    {
        auto strength = header.removeFromRight (210);
        if (dynamicStrengthLabel != nullptr)
            dynamicStrengthLabel->setBounds (strength.removeFromTop (14).translated (getX(), getY()));
        dynamicStrengthSlider->setBounds (strength.reduced (0, 1).translated (getX(), getY()));
    }

    area.removeFromTop (6);
    const int minCardWidth = 185;
    cardColumns = juce::jlimit (1, 4, juce::jmax (1, (area.getWidth() + 8) / (minCardWidth + 8)));
    const int rows = (int) std::ceil ((double) cards.size() / (double) cardColumns);
    const int gap = 8;
    const int availableWidth = area.getWidth() - gap * (cardColumns - 1);
    const int cardWidth = juce::jmax (1, juce::jmin (340, availableWidth / cardColumns));
    const int gridWidth = cardWidth * cardColumns + gap * (cardColumns - 1);
    const int originX = area.getX() + juce::jmax (0, (area.getWidth() - gridWidth) / 2);
    const int availableHeight = area.getHeight() - gap * (rows - 1);
    const int cardHeight = juce::jmax (1, availableHeight / rows);

    for (int index = 0; index < (int) cards.size(); ++index)
    {
        const int column = index % cardColumns;
        const int row = index / cardColumns;
        cardBounds[(size_t) index] = { originX + column * (cardWidth + gap),
                                       area.getY() + row * (cardHeight + gap),
                                       cardWidth, cardHeight };
        cards[(size_t) index]->setBounds (cardBounds[(size_t) index]);
    }
}

void DynamicWorkspace::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat();
    g.setColour (kPanel);
    g.fillRoundedRectangle (bounds, 7.0f);
    g.setColour (kBorder);
    g.drawRoundedRectangle (bounds, 7.0f, 1.0f);

    auto header = getLocalBounds().reduced (14, 10).removeFromTop (48);
    g.setColour (model.previewActive ? kAmber : kTeal);
    g.setFont (juce::Font (juce::FontOptions (12.0f)).boldened());
    g.drawText (headerSource, header.removeFromTop (15), juce::Justification::centredLeft, true);
    g.setColour (kText);
    g.setFont (juce::Font (juce::FontOptions (15.0f)).boldened());
    g.drawText (headerStatus, header.removeFromTop (18), juce::Justification::centredLeft, true);
    g.setColour (kMuted);
    g.setFont (juce::Font (juce::FontOptions (9.5f)));
    g.drawText (headerDetail, header, juce::Justification::centredLeft, true);
}
