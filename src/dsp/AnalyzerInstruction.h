#pragma once

#include <algorithm>
#include <cmath>
#include <juce_core/juce_core.h>

#include "AlignmentAnalyzer.h"

enum class AnalyzeMode
{
    Monitor,
    RecommendOnly,
    AutoApplySafe
};

enum class AlignmentAction
{
    None,
    ApplyBassDelay,
    RecommendMoveKick,
    RecommendMoveBass,
    FlipBassPolarity,
    RecommendPhaseFilter,
    NotEnoughSignal,
    LowConfidence
};

struct AnalyzerInstruction
{
    AlignmentAction action = AlignmentAction::None;
    float delayMs = 0.0f;
    bool flipPolarity = false;
    bool phaseFilterRecommended = false;
    float phaseFilterFreqHz = 200.0f;
    float phaseFilterQ = 0.7f;
    int phaseFilterStages = 2;
    float beforeMatchPercent = 50.0f;
    float afterMatchPercent = 50.0f;
    float confidence = 0.0f;
    juce::String message;
};

class AnalyzerInstructionBuilder
{
public:
    static AnalyzerInstruction build (const AlignmentResult& result,
                                      AnalyzeMode mode = AnalyzeMode::RecommendOnly)
    {
        AnalyzerInstruction instruction;

        if (! result.valid)
        {
            instruction.action = AlignmentAction::NotEnoughSignal;
            instruction.message = "Waiting for kick sidechain and bass signal.";
            return instruction;
        }

        instruction.delayMs = result.delayMs;
        instruction.flipPolarity = result.invertPolarity;
        instruction.phaseFilterRecommended = result.adjustRotator;
        instruction.phaseFilterFreqHz = result.rotatorFreqHz;
        instruction.phaseFilterQ = result.rotatorQ;
        instruction.phaseFilterStages = result.rotatorStages;
        instruction.beforeMatchPercent = result.beforeMatch;
        instruction.afterMatchPercent = result.afterMatch;
        instruction.confidence = estimateConfidence (result.beforeMatch, result.afterMatch);

        constexpr float minUsefulDelayMs = 0.02f;

        if (instruction.confidence < 0.25f)
            instruction.action = AlignmentAction::LowConfidence;
        else if (result.delayMs > minUsefulDelayMs)
            instruction.action = AlignmentAction::ApplyBassDelay;
        else if (result.delayMs < -minUsefulDelayMs)
            instruction.action = AlignmentAction::RecommendMoveKick;
        else if (result.invertPolarity)
            instruction.action = AlignmentAction::FlipBassPolarity;
        else if (result.adjustRotator)
            instruction.action = AlignmentAction::RecommendPhaseFilter;
        else
            instruction.action = AlignmentAction::None;

        instruction.message = formatMessage (instruction, mode);
        return instruction;
    }

private:
    static float estimateConfidence (float beforeMatch, float afterMatch)
    {
        const float absoluteQuality = std::clamp ((afterMatch - 50.0f) / 50.0f, 0.0f, 1.0f);
        const float improvement = std::clamp ((afterMatch - beforeMatch) / 25.0f, 0.0f, 1.0f);
        return std::clamp (0.65f * absoluteQuality + 0.35f * improvement, 0.0f, 1.0f);
    }

    static juce::String formatMessage (const AnalyzerInstruction& instruction, AnalyzeMode mode)
    {
        if (instruction.action == AlignmentAction::NotEnoughSignal)
            return "Waiting for kick sidechain and bass signal.";

        juce::String message;

        if (mode == AnalyzeMode::AutoApplySafe)
            message << "Safe apply: ";
        else if (mode == AnalyzeMode::Monitor)
            message << "Monitor: ";
        else
            message << "Recommendation: ";

        switch (instruction.action)
        {
            case AlignmentAction::ApplyBassDelay:
                message << "delay bass by " << juce::String (instruction.delayMs, 2) << " ms";
                break;

            case AlignmentAction::RecommendMoveKick:
                message << "move kick later by " << juce::String (std::abs (instruction.delayMs), 2) << " ms";
                break;

            case AlignmentAction::RecommendMoveBass:
                message << "move bass earlier by " << juce::String (std::abs (instruction.delayMs), 2) << " ms";
                break;

            case AlignmentAction::FlipBassPolarity:
                message << "invert bass polarity";
                break;

            case AlignmentAction::RecommendPhaseFilter:
                message << "try phase filter";
                break;

            case AlignmentAction::LowConfidence:
                message << "low confidence, keep current settings";
                break;

            case AlignmentAction::None:
                message << "already close";
                break;

            case AlignmentAction::NotEnoughSignal:
                break;
        }

        if (instruction.flipPolarity && instruction.action != AlignmentAction::FlipBassPolarity)
            message << " + invert bass polarity";

        if (instruction.phaseFilterRecommended
            && instruction.action != AlignmentAction::RecommendPhaseFilter)
        {
            message << " + phase filter "
                    << juce::String ((int) std::round (instruction.phaseFilterFreqHz)) << " Hz/Q"
                    << juce::String (instruction.phaseFilterQ, 1) << "/"
                    << juce::String (instruction.phaseFilterStages) << " st";
        }

        message << " | match "
                << juce::String ((int) std::round (instruction.beforeMatchPercent)) << "% -> "
                << juce::String ((int) std::round (instruction.afterMatchPercent)) << "%"
                << " | confidence " << juce::String ((int) std::round (instruction.confidence * 100.0f)) << "%";

        return message;
    }
};
