#pragma once

#include <array>
#include <algorithm>

#include "AlignmentAnalyzer.h"
#include "AnalyzerInstruction.h"
#include "MultiBandCorrelation.h"

struct HitAnalysisResult
{
    bool valid = false;
    int sequence = 0;
    AnalyzerInstruction instruction;
    MultiBandCorrelationResult multiBand;
};

class PerHitAnalyzer
{
public:
    static HitAnalysisResult analyzeHit (const float* bass,
                                         const float* kick,
                                         int numSamples,
                                         double sampleRate,
                                         int sequence = 0)
    {
        HitAnalysisResult result;
        result.sequence = sequence;

        const auto alignment = AlignmentAnalyzer::analyze (bass, kick, numSamples, sampleRate, 50.0f,
                                                           30.0f, 120.0f, 16384);
        result.instruction = AnalyzerInstructionBuilder::build (alignment);
        result.multiBand = MultiBandCorrelation::analyze (bass, kick, numSamples, sampleRate);
        result.valid = alignment.valid;

        if (result.valid)
        {
            result.instruction.confidence = std::clamp (
                0.7f * result.instruction.confidence + 0.3f * result.multiBand.confidence,
                0.0f, 1.0f);
        }

        return result;
    }
};

class HitAnalysisHistory
{
public:
    static constexpr int capacity = 8;

    void reset()
    {
        nextIndex = 0;
        count = 0;
        rollingDelayMs = 0.0f;
        rollingMatchPercent = 50.0f;
        rollingConfidence = 0.0f;
        results = {};
    }

    void push (const HitAnalysisResult& result)
    {
        if (! result.valid)
            return;

        results[(size_t) nextIndex] = result;
        nextIndex = (nextIndex + 1) % capacity;
        count = std::min (count + 1, capacity);
        recompute();
    }

    int getCount() const noexcept { return count; }
    float getRollingDelayMs() const noexcept { return rollingDelayMs; }
    float getRollingMatchPercent() const noexcept { return rollingMatchPercent; }
    float getRollingConfidence() const noexcept { return rollingConfidence; }

private:
    void recompute()
    {
        float delaySum = 0.0f;
        float matchSum = 0.0f;
        float confidenceSum = 0.0f;

        for (int i = 0; i < count; ++i)
        {
            const auto& result = results[(size_t) i];
            delaySum += result.instruction.delayMs;
            matchSum += result.instruction.afterMatchPercent;
            confidenceSum += result.instruction.confidence;
        }

        const float inv = count > 0 ? 1.0f / (float) count : 0.0f;
        rollingDelayMs = delaySum * inv;
        rollingMatchPercent = count > 0 ? matchSum * inv : 50.0f;
        rollingConfidence = confidenceSum * inv;
    }

    std::array<HitAnalysisResult, capacity> results {};
    int nextIndex = 0;
    int count = 0;
    float rollingDelayMs = 0.0f;
    float rollingMatchPercent = 50.0f;
    float rollingConfidence = 0.0f;
};
