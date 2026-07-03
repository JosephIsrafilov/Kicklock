#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include <juce_core/juce_core.h>

#include "AlignmentAnalyzer.h"
#include "MultiBandCorrelation.h"

struct PhaseFixResult
{
    bool valid = false;
    bool enoughSignal = false;
    bool bassPolarityInvert = false;
    float bassDelayMs = 0.0f;
    bool phaseFilterEnabled = false;
    float phaseFilterFreqHz = 200.0f;
    float phaseFilterQ = 0.7f;
    int phaseFilterStages = 2;

    float beforeMatchPercent = 50.0f;
    float afterMatchPercent = 50.0f;
    float improvementPercent = 0.0f;
    float confidence = 0.0f;

    bool requiresTimelineMove = false;
    float suggestedKickMoveMs = 0.0f;

    juce::String message;
};

class PhaseFixEngine
{
public:
    static PhaseFixResult analyze (const float* bass,
                                   const float* kick,
                                   int numSamples,
                                   double sampleRate,
                                   float maxDelayMs = 50.0f)
    {
        PhaseFixResult result;

        if (bass == nullptr || kick == nullptr || numSamples <= 32 || sampleRate <= 0.0)
        {
            result.message = "Waiting for strong kick and bass signal.";
            return result;
        }

        const auto before = score (bass, kick, numSamples, sampleRate);
        result.beforeMatchPercent = before.match;
        result.enoughSignal = before.confidence >= 0.05f;

        if (! result.enoughSignal)
        {
            result.message = "Low confidence: kick or bass is too quiet.";
            return result;
        }

        const auto unconstrained = AlignmentAnalyzer::analyze (bass, kick, numSamples,
                                                               sampleRate, maxDelayMs,
                                                               30.0f, 120.0f, 16384);

        Candidate best;
        best.match = before.match;
        best.confidence = before.confidence;

        std::vector<float> work ((size_t) numSamples);

        // Stage 1: search settings that are always safe to apply to the bass:
        // polarity plus non-negative bass delay.
        for (bool invert : { false, true })
        {
            for (float delayMs = 0.0f; delayMs <= maxDelayMs + 0.0001f; delayMs += 0.10f)
                testCandidate (bass, kick, numSamples, sampleRate, invert, delayMs,
                               false, 200.0f, 0.7f, 2, work, best);
        }

        const float fineStart = std::max (0.0f, best.delayMs - 0.25f);
        const float fineEnd = std::min (maxDelayMs, best.delayMs + 0.25f);
        for (bool invert : { false, true })
        {
            for (float delayMs = fineStart; delayMs <= fineEnd + 0.0001f; delayMs += 0.01f)
                testCandidate (bass, kick, numSamples, sampleRate, invert, delayMs,
                               false, 200.0f, 0.7f, 2, work, best);
        }

        // Stage 2: phase-filter search around the best safe delay/polarity.
        constexpr float freqs[] = { 30.0f, 40.0f, 50.0f, 60.0f, 80.0f, 100.0f, 120.0f, 150.0f, 200.0f };
        constexpr float qs[] = { 0.5f, 0.7f, 1.0f, 1.5f, 2.0f, 4.0f };
        constexpr int stages[] = { 2, 3, 4 };

        const float phaseBaseline = best.match;
        const float phaseDelayStart = std::max (0.0f, best.delayMs - 0.25f);
        const float phaseDelayEnd = std::min (maxDelayMs, best.delayMs + 0.25f);
        for (bool invert : { false, true })
        for (float freq : freqs)
        {
            if ((double) freq >= sampleRate * 0.5)
                continue;

            for (float q : qs)
                for (int stageCount : stages)
                {
                    testCandidate (bass, kick, numSamples, sampleRate, invert,
                                   0.0f, true, freq, q, stageCount, work, best);

                    for (float delayMs = phaseDelayStart; delayMs <= phaseDelayEnd + 0.0001f; delayMs += 0.05f)
                        testCandidate (bass, kick, numSamples, sampleRate, invert,
                                       delayMs, true, freq, q, stageCount, work, best);
                }
        }

        if (best.phaseEnabled && best.match < phaseBaseline + 2.0f)
        {
            // Avoid enabling phase filtering for tiny analyzer noise wins.
            best.phaseEnabled = false;
            best.match = phaseBaseline;
        }

        result.valid = true;
        result.bassPolarityInvert = best.invert;
        result.bassDelayMs = std::max (0.0f, best.delayMs);
        result.phaseFilterEnabled = best.phaseEnabled;
        result.phaseFilterFreqHz = best.phaseFreqHz;
        result.phaseFilterQ = best.phaseQ;
        result.phaseFilterStages = best.phaseStages;
        result.afterMatchPercent = best.match;
        result.improvementPercent = result.afterMatchPercent - result.beforeMatchPercent;
        result.confidence = std::clamp (0.65f * ((result.afterMatchPercent - 50.0f) / 50.0f)
                                        + 0.35f * (result.improvementPercent / 25.0f),
                                        0.0f, 1.0f);

        if (unconstrained.valid && unconstrained.delayMs < -0.02f
            && result.improvementPercent < 5.0f)
        {
            result.requiresTimelineMove = true;
            result.suggestedKickMoveMs = std::abs (unconstrained.delayMs);
        }

        result.message = makeMessage (result);
        return result;
    }

    static bool canApply (const PhaseFixResult& result) noexcept
    {
        return result.valid
            && result.enoughSignal
            && ! result.requiresTimelineMove
            && result.afterMatchPercent >= 70.0f
            && result.improvementPercent >= 5.0f;
    }

private:
    struct Score
    {
        float match = 50.0f;
        float confidence = 0.0f;
    };

    struct Candidate
    {
        float match = -std::numeric_limits<float>::infinity();
        float confidence = 0.0f;
        bool invert = false;
        float delayMs = 0.0f;
        bool phaseEnabled = false;
        float phaseFreqHz = 200.0f;
        float phaseQ = 0.7f;
        int phaseStages = 2;
    };

    static Score score (const float* bass, const float* kick, int numSamples, double sampleRate)
    {
        const auto multi = MultiBandCorrelation::analyze (bass, kick, numSamples, sampleRate);
        return { multi.weightedMatchPercent, multi.confidence };
    }

    static void testCandidate (const float* bass,
                               const float* kick,
                               int numSamples,
                               double sampleRate,
                               bool invert,
                               float delayMs,
                               bool phaseEnabled,
                               float phaseFreqHz,
                               float phaseQ,
                               int phaseStages,
                               std::vector<float>& work,
                               Candidate& best)
    {
        renderBassCandidate (bass, numSamples, sampleRate, invert, delayMs,
                             phaseEnabled, phaseFreqHz, phaseQ, phaseStages, work);

        const auto s = score (work.data(), kick, numSamples, sampleRate);
        if (s.match > best.match)
        {
            best.match = s.match;
            best.confidence = s.confidence;
            best.invert = invert;
            best.delayMs = delayMs;
            best.phaseEnabled = phaseEnabled;
            best.phaseFreqHz = phaseFreqHz;
            best.phaseQ = phaseQ;
            best.phaseStages = phaseStages;
        }
    }

    static void renderBassCandidate (const float* bass,
                                     int numSamples,
                                     double sampleRate,
                                     bool invert,
                                     float delayMs,
                                     bool phaseEnabled,
                                     float phaseFreqHz,
                                     float phaseQ,
                                     int phaseStages,
                                     std::vector<float>& out)
    {
        const float sign = invert ? -1.0f : 1.0f;
        const float delaySamples = (float) (sampleRate * (double) delayMs / 1000.0);

        for (int i = 0; i < numSamples; ++i)
        {
            const float read = (float) i - delaySamples;
            if (read <= 0.0f)
            {
                out[(size_t) i] = read == 0.0f ? sign * bass[0] : 0.0f;
                continue;
            }

            const int i0 = (int) std::floor (read);
            const int i1 = i0 + 1;
            if (i1 >= numSamples)
            {
                out[(size_t) i] = sign * bass[numSamples - 1];
                continue;
            }

            const float frac = read - (float) i0;
            out[(size_t) i] = sign * (bass[i0] + frac * (bass[i1] - bass[i0]));
        }

        if (phaseEnabled)
            applyAllpassCascade (out, sampleRate, phaseFreqHz, phaseQ, phaseStages);
    }

    static void applyAllpassCascade (std::vector<float>& x, double fs,
                                     float freqHz, float q, int stages)
    {
        const double n = 1.0 / std::tan (pi * (double) freqHz / fs);
        const double nSq = n * n;
        const double invQ = 1.0 / (double) q;
        const double c1 = 1.0 / (1.0 + invQ * n + nSq);
        const double b0 = c1 * (1.0 - n * invQ + nSq);
        const double b1 = c1 * 2.0 * (1.0 - nSq);

        for (int s = 0; s < std::clamp (stages, 2, 4); ++s)
        {
            double z1 = 0.0;
            double z2 = 0.0;
            for (auto& sample : x)
            {
                const double in = sample;
                const double out = b0 * in + z1;
                z1 = b1 * in - b1 * out + z2;
                z2 = in - b0 * out;
                sample = (float) out;
            }
        }
    }

    static juce::String makeMessage (const PhaseFixResult& result)
    {
        if (! result.enoughSignal)
            return "Low confidence: kick or bass is too quiet.";

        if (result.requiresTimelineMove)
            return "Move kick later by " + juce::String (result.suggestedKickMoveMs, 2)
                   + " ms in your DAW. This plugin can only process bass.";

        if (result.afterMatchPercent >= 85.0f && result.improvementPercent < 5.0f)
            return "Already close: " + juce::String ((int) std::round (result.beforeMatchPercent))
                   + "% aligned. No fix needed.";

        if (result.afterMatchPercent < 70.0f || result.improvementPercent < 5.0f)
            return "Low confidence / no meaningful fix found.";

        juce::String msg = "Fix found: ";
        bool wroteAction = false;

        if (result.bassPolarityInvert)
        {
            msg << "Flip bass polarity";
            wroteAction = true;
        }

        if (result.bassDelayMs > 0.02f)
        {
            if (wroteAction) msg << " + ";
            msg << "delay bass " << juce::String (result.bassDelayMs, 2) << " ms";
            wroteAction = true;
        }

        if (result.phaseFilterEnabled)
        {
            if (wroteAction) msg << " + ";
            msg << "phase filter " << juce::String ((int) std::round (result.phaseFilterFreqHz))
                << " Hz / Q " << juce::String (result.phaseFilterQ, 1);
            wroteAction = true;
        }

        if (! wroteAction)
            msg << "fine adjustment";

        msg << ". Match " << juce::String ((int) std::round (result.beforeMatchPercent))
            << "% -> " << juce::String ((int) std::round (result.afterMatchPercent)) << "%.";

        return msg;
    }

    static constexpr double pi = 3.14159265358979323846;
};
