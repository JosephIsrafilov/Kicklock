#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "PhaseFixEngine.h"

// =============================================================================
// FixedTimingRotatorSearch (plan v1.2, Phase 3 Pass B helper).
//
// Given a single completed hit window and a FIXED global Delay + Polarity (the
// result of Pass A), search only the allpass rotator's frequency / Q / stage
// count for the best low-end match. The global timing is never re-searched: the
// bass delay and polarity fed to the scorer stay exactly what Pass A chose, so
// this pass can only ever move the rotator, not the alignment.
//
// The search sweeps every allowed stage count once and reports the best
// candidate per stage AND the overall best. The caller (LearnPipelineCore) uses
// the per-stage bests to pick ONE global stage count across all hits, then reads
// each hit's candidate at that stage so per-note entries vary only in
// allpassFreqHz / allpassQ / rotatorHelps.
//
// "rotatorHelps" is true only when the best rotator beats the rotator-off
// baseline by at least kMinGainPoints match points, so a rotator is only ever
// recorded as helping when it measurably improves the alignment.
//
// Pure, worker-thread-only, deterministic: no RNG, no globals, and ties are
// broken by grid order (lowest frequency then lowest Q), so identical input
// always yields the identical result.
// =============================================================================

struct RotatorCandidate
{
    bool  valid = false;          // beat the rotator-off baseline
    bool  helps = false;          // beat the baseline by >= kMinGainPoints
    int   stages = 2;
    float allpassFreqHz = 0.0f;
    float allpassQ = 0.7f;
    float matchPercent = 0.0f;    // absolute match of this candidate
    float gainPoints = 0.0f;      // matchPercent - baseline
    float confidence = 0.0f;
};

struct FixedTimingRotatorResult
{
    bool  valid = false;
    float baselineMatchPercent = 0.0f;   // fixed delay/polarity, rotator OFF
    float baselineConfidence = 0.0f;

    RotatorCandidate best;                // best across every searched stage
    std::vector<RotatorCandidate> perStage; // best per stage, in `stages` order
};

class FixedTimingRotatorSearch
{
public:
    // Minimum match-point gain over the rotator-off baseline for a rotator to be
    // recorded as helping (mirrors PhaseFixEngine's own +3 pt rotator gate).
    static constexpr float kMinGainPoints = 3.0f;

    static const std::vector<float>& defaultFrequencies()
    {
        // Low-end correction centres inside the valid allpass range [20, 500] Hz.
        static const std::vector<float> f {
            40.0f, 50.0f, 60.0f, 70.0f, 80.0f, 90.0f, 100.0f,
            120.0f, 140.0f, 160.0f, 180.0f, 200.0f, 240.0f };
        return f;
    }

    static const std::vector<float>& defaultQs()
    {
        // Sane Q spread; capped at 2.0 so the search cannot pick a pathological
        // high-Q resonance (matches the Static analyzer's own preference).
        static const std::vector<float> q { 0.5f, 0.7f, 1.0f, 1.5f, 2.0f };
        return q;
    }

    static const std::vector<int>& defaultStages()
    {
        static const std::vector<int> s { 2, 3, 4 };
        return s;
    }

    static FixedTimingRotatorResult search (const float* bass,
                                            const float* kick,
                                            int numSamples,
                                            double sampleRate,
                                            float fixedDelayMs,
                                            bool fixedPolarityInvert,
                                            InterpolationType interpolation,
                                            const std::vector<int>& stages = defaultStages(),
                                            const std::vector<float>& freqs = defaultFrequencies(),
                                            const std::vector<float>& qs = defaultQs())
    {
        FixedTimingRotatorResult result;

        if (bass == nullptr || kick == nullptr || numSamples <= 32
            || ! (sampleRate > 0.0) || stages.empty() || freqs.empty() || qs.empty())
            return result;

        std::vector<float> bassScratch, kickScratch;

        // Baseline: the FIXED Pass-A delay/polarity with the rotator OFF.
        PhaseFixRenderSettings baseSettings;
        baseSettings.bassPolarityInvert = fixedPolarityInvert;
        baseSettings.bassDelayMs = fixedDelayMs;
        baseSettings.phaseFilterEnabled = false;
        baseSettings.delayInterpolation = interpolation;

        const auto baseScore = PhaseFixEngine::scoreSettings (
            bass, kick, numSamples, sampleRate, baseSettings,
            PhaseFixEngine::absoluteManualMaxDelayMs, bassScratch, kickScratch);

        if (! std::isfinite (baseScore.matchPercent))
            return result;

        result.valid = true;
        result.baselineMatchPercent = baseScore.matchPercent;
        result.baselineConfidence = baseScore.confidence;
        result.perStage.reserve (stages.size());

        for (int stageCount : stages)
        {
            RotatorCandidate bestForStage;
            bestForStage.stages = stageCount;
            bestForStage.matchPercent = baseScore.matchPercent;   // start at baseline
            bestForStage.confidence = baseScore.confidence;
            bestForStage.allpassFreqHz = freqs.front();           // safe in-range default
            bestForStage.allpassQ = qs.front();

            for (float freq : freqs)
            {
                for (float q : qs)
                {
                    PhaseFixRenderSettings settings;
                    settings.bassPolarityInvert = fixedPolarityInvert; // FIXED
                    settings.bassDelayMs = fixedDelayMs;               // FIXED
                    settings.phaseFilterEnabled = true;
                    settings.phaseFilterFreqHz = freq;
                    settings.phaseFilterQ = q;
                    settings.phaseFilterStages = stageCount;
                    settings.delayInterpolation = interpolation;

                    const auto candidate = PhaseFixEngine::scoreSettings (
                        bass, kick, numSamples, sampleRate, settings,
                        PhaseFixEngine::absoluteManualMaxDelayMs, bassScratch, kickScratch);

                    if (! std::isfinite (candidate.matchPercent))
                        continue;

                    // Strictly-greater with a tie epsilon: grid order (lowest
                    // freq, then lowest Q) wins ties -> deterministic.
                    if (candidate.matchPercent > bestForStage.matchPercent + 1.0e-4f)
                    {
                        bestForStage.valid = true;
                        bestForStage.allpassFreqHz = freq;
                        bestForStage.allpassQ = q;
                        bestForStage.matchPercent = candidate.matchPercent;
                        bestForStage.confidence = candidate.confidence;
                    }
                }
            }

            bestForStage.gainPoints = bestForStage.matchPercent - baseScore.matchPercent;
            bestForStage.helps = bestForStage.valid && bestForStage.gainPoints >= kMinGainPoints;
            result.perStage.push_back (bestForStage);
        }

        // Overall best across stages: higher match wins, ties prefer the earlier
        // (fewer-stage) entry so the choice stays deterministic and minimal.
        result.best = result.perStage.front();
        for (const auto& candidate : result.perStage)
            if (candidate.matchPercent > result.best.matchPercent + 1.0e-4f)
                result.best = candidate;

        return result;
    }

    // Reads the best candidate for a specific stage count out of a completed
    // search result. Falls back to the overall best if the stage was not searched
    // (never happens with matching stage lists, but keeps the caller safe).
    static RotatorCandidate candidateForStage (const FixedTimingRotatorResult& result, int stageCount)
    {
        for (const auto& candidate : result.perStage)
            if (candidate.stages == stageCount)
                return candidate;
        return result.best;
    }
};
