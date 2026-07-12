#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

#include "NotePhaseMap.h"
#include "PhaseFixEngine.h"

struct RotatorCandidate
{
    // valid means an actual, safe rotator candidate was found.  A valid search
    // can still have a baseline-only (valid == false, helps == false) result.
    bool valid = false;
    bool helps = false;
    int stages = 2;
    float allpassFreqHz = 0.0f;
    float allpassQ = 0.0f;
    float matchPercent = 0.0f;
    float gainPoints = 0.0f; // safety-adjusted gain over fixed baseline
    float confidence = 0.0f;
};

struct FixedTimingRotatorResult
{
    bool valid = false; // valid fixed delay/polarity baseline
    float baselineMatchPercent = 0.0f;
    float baselineConfidence = 0.0f;
    RotatorCandidate best;
    std::vector<RotatorCandidate> perStage;
};

struct FixedTimingRotatorInput
{
    const float* bass = nullptr;
    const float* kick = nullptr;
    int numSamples = 0;
    float energy = 1.0f;
};

// Worker-only F/Q search. Timing is an input, never an output.  Its defaults,
// candidate validity rules, and safety penalty are shared with Static Analyze.
class FixedTimingRotatorSearch
{
public:
    static constexpr float kMinGainPoints = 3.0f;

    static const std::vector<float>& defaultFrequencies() { return AlignmentAnalyzer::rotatorFrequencies(); }
    static const std::vector<float>& defaultQs() { return AlignmentAnalyzer::rotatorQs(); }
    static const std::vector<int>& defaultStages() { return AlignmentAnalyzer::rotatorStages(); }

    static FixedTimingRotatorResult search (const float* bass,
                                            const float* kick,
                                            int numSamples,
                                            double sampleRate,
                                            float fixedDelayMs,
                                            bool fixedPolarityInvert,
                                            InterpolationType interpolation,
                                            const std::vector<int>& stages = defaultStages(),
                                            const std::vector<float>& freqs = defaultFrequencies(),
                                            const std::vector<float>& qs = defaultQs(),
                                            int forcedStages = 0,
                                            const std::function<bool()>& shouldCancel = {})
    {
        const FixedTimingRotatorInput input { bass, kick, numSamples, 1.0f };
        return searchImpl ({ input }, sampleRate, fixedDelayMs, fixedPolarityInvert,
                           interpolation, stages, freqs, qs, forcedStages, false, shouldCancel);
    }

    // One candidate is evaluated across every independent hit.  Filters are
    // reset per hit, avoiding concatenation boundary artefacts. Scores are an
    // energy-weighted mean and ties retain production grid order.
    static FixedTimingRotatorResult searchCombined (const std::vector<FixedTimingRotatorInput>& inputs,
                                                    double sampleRate,
                                                    float fixedDelayMs,
                                                    bool fixedPolarityInvert,
                                                    InterpolationType interpolation,
                                                    const std::vector<int>& stages = defaultStages(),
                                                    const std::vector<float>& freqs = defaultFrequencies(),
                                                    const std::vector<float>& qs = defaultQs(),
                                                    int forcedStages = 0,
                                                    const std::function<bool()>& shouldCancel = {})
    {
        return searchImpl (inputs, sampleRate, fixedDelayMs, fixedPolarityInvert,
                           interpolation, stages, freqs, qs, forcedStages, true, shouldCancel);
    }

    static RotatorCandidate candidateForStage (const FixedTimingRotatorResult& result, int stageCount)
    {
        for (const auto& candidate : result.perStage)
            if (candidate.stages == stageCount)
                return candidate;
        return {};
    }

private:
    struct Evaluated
    {
        bool valid = false;
        float rawMatch = 0.0f;
        float effectiveMatch = 0.0f;
        float confidence = 0.0f;
    };

    static bool validStage (int stage) noexcept { return stage == 2 || stage == 3; }

    static FixedTimingRotatorResult searchImpl (const std::vector<FixedTimingRotatorInput>& inputs,
                                                double sampleRate,
                                                float fixedDelayMs,
                                                bool fixedPolarityInvert,
                                                InterpolationType interpolation,
                                                const std::vector<int>& requestedStages,
                                                const std::vector<float>& freqs,
                                                 const std::vector<float>& qs,
                                                 int forcedStages,
                                                 bool combined,
                                                 const std::function<bool()>& shouldCancel)
    {
        FixedTimingRotatorResult result;
        if (! (sampleRate > 0.0) || ! std::isfinite (sampleRate) || inputs.empty()
            || freqs.empty() || qs.empty() || (forcedStages != 0 && ! validStage (forcedStages)))
            return result;
        if (shouldCancel && shouldCancel())
            return result;

        std::vector<int> stages;
        for (int stage : requestedStages)
            if (validStage (stage) && (forcedStages == 0 || stage == forcedStages)
                && std::find (stages.begin(), stages.end(), stage) == stages.end())
                stages.push_back (stage);
        if (stages.empty())
            return result;

        const auto baseline = evaluate (inputs, sampleRate, fixedDelayMs, fixedPolarityInvert,
                                        interpolation, false, 0.0f, 0.0f, 2, combined, shouldCancel);
        if (! baseline.valid)
            return result;

        result.valid = true;
        result.baselineMatchPercent = baseline.rawMatch;
        result.baselineConfidence = baseline.confidence;
        result.perStage.reserve (stages.size());

        for (const int stage : stages)
        {
            if (shouldCancel && shouldCancel())
                return {};
            RotatorCandidate best;
            best.stages = stage;
            for (const float freq : freqs)
            {
                if (shouldCancel && shouldCancel())
                    return {};
                if (! std::isfinite (freq) || freq < NoteMap::kAllpassFreqMinHz
                    || freq > NoteMap::kAllpassFreqMaxHz || freq >= sampleRate * 0.5)
                    continue;
                for (const float q : qs)
                {
                    if (! std::isfinite (q) || q < NoteMap::kAllpassQMin || q > NoteMap::kAllpassQMax)
                        continue;
                    const auto candidate = evaluate (inputs, sampleRate, fixedDelayMs, fixedPolarityInvert,
                                                     interpolation, true, freq, q, stage, combined, shouldCancel);
                    if (! candidate.valid)
                        continue;
                    const float gain = candidate.effectiveMatch - baseline.effectiveMatch;
                    if (! best.valid || gain > best.gainPoints + 1.0e-4f)
                    {
                        best.valid = true;
                        best.allpassFreqHz = freq;
                        best.allpassQ = q;
                        best.matchPercent = candidate.rawMatch;
                        best.gainPoints = gain;
                        best.confidence = candidate.confidence;
                    }
                }
            }
            best.helps = best.valid && best.gainPoints >= kMinGainPoints;
            result.perStage.push_back (best);
        }

        result.best.stages = stages.front();
        for (const auto& candidate : result.perStage)
            if (candidate.valid && (! result.best.valid
                || candidate.gainPoints > result.best.gainPoints + 1.0e-4f))
                result.best = candidate;
        result.best.helps = result.best.valid && result.best.gainPoints >= kMinGainPoints;
        return result;
    }

    static Evaluated evaluate (const std::vector<FixedTimingRotatorInput>& inputs,
                               double sampleRate, float delayMs, bool polarity,
                               InterpolationType interpolation, bool useRotator,
                               float freq, float q, int stages, bool,
                               const std::function<bool()>& shouldCancel)
    {
        double weights = 0.0, raw = 0.0, effective = 0.0, confidence = 0.0;
        for (const auto& input : inputs)
        {
            if (shouldCancel && shouldCancel())
                return {};
            if (input.bass == nullptr || input.kick == nullptr || input.numSamples <= 32)
                return {};
            PhaseFixRenderSettings settings;
            settings.bassPolarityInvert = polarity;
            settings.bassDelayMs = delayMs;
            settings.phaseFilterEnabled = useRotator;
            settings.phaseFilterFreqHz = freq;
            settings.phaseFilterQ = q;
            settings.phaseFilterStages = stages;
            settings.delayInterpolation = interpolation;

            std::vector<float> dry, wet, scratchBass, scratchKick;
            PhaseFixRenderSettings base = settings;
            base.phaseFilterEnabled = false;
            PhaseFixEngine::renderCandidate (input.bass, input.numSamples, sampleRate, base, dry);
            PhaseFixEngine::renderCandidate (input.bass, input.numSamples, sampleRate, settings, wet);
            const auto score = PhaseFixEngine::scoreSettings (input.bass, input.kick, input.numSamples,
                                                               sampleRate, settings,
                                                               PhaseFixEngine::absoluteManualMaxDelayMs,
                                                               scratchBass, scratchKick);
            if (! std::isfinite (score.matchPercent) || ! std::isfinite (score.confidence)
                || score.confidence <= 0.0f)
                return {};
            double penalty = 0.0;
            if (useRotator)
                penalty = AlignmentAnalyzer::rotatorSafetyPenalty (dry, wet, sampleRate, q, stages);
            if (! std::isfinite (penalty))
                return {};
            const double weight = std::max (1.0e-6, (double) input.energy);
            weights += weight;
            raw += weight * score.matchPercent;
            effective += weight * ((double) score.matchPercent - penalty * 100.0);
            confidence += weight * score.confidence;
        }
        if (weights <= 0.0)
            return {};
        return { true, (float) (raw / weights), (float) (effective / weights),
                 (float) (confidence / weights) };
    }
};
