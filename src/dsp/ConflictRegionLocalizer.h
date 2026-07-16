#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "PhaseFixEngine.h"
#include "TransientDetector.h"
#include "ConflictRegion.h"

// Worker-side Conflict Localization (Dynamic Learn, Layer A).
//
// PhaseFixEngine and AlignmentAnalyzer deliberately remain window analyzers:
// Static Analyze also depends on that contract.  This wrapper identifies the
// conflict-bearing part of one captured kick hit and gives the existing engine
// only that identical bass/kick sub-window.
struct ConflictLocalizationResult
{
    bool valid = false;
    ConflictRegion region = ConflictRegion::none;
    int onsetSample = -1;
    int startSample = 0;
    int lengthSamples = 0;
    float severity = 0.0f;
    float attackSeverity = 0.0f;
    float bodySeverity = 0.0f;
    float tailSeverity = 0.0f;
};

struct ConflictRegionBoundaries
{
    bool valid = false;
    int onsetSample = -1;
    int attackStart = 0;
    int attackEnd = 0;
    int bodyEnd = 0;
    int tailEnd = 0;
};

class ConflictRegionLocalizer
{
public:
    static ConflictRegionBoundaries regionBoundaries (const float* kick,
                                                      int numSamples,
                                                      double sampleRate)
    {
        ConflictRegionBoundaries result;
        if (kick == nullptr || numSamples <= 32 || sampleRate <= 0.0)
            return result;
        const int onset = findOnset (kick, numSamples, sampleRate);
        const auto geometry = makeGeometry (kick, numSamples, sampleRate, onset);
        if (geometry.end <= geometry.start)
            return result;
        result.valid = true;
        result.onsetSample = onset;
        result.attackStart = geometry.start;
        result.attackEnd = geometry.attackEnd;
        result.bodyEnd = geometry.bodyEnd;
        result.tailEnd = geometry.end;
        return result;
    }

    static ConflictLocalizationResult localize (const float* bass,
                                                const float* kick,
                                                int numSamples,
                                                double sampleRate)
    {
        ConflictLocalizationResult result;
        if (bass == nullptr || kick == nullptr || numSamples <= 32 || sampleRate <= 0.0)
            return result;

        const auto boundaries = regionBoundaries (kick, numSamples, sampleRate);
        if (! boundaries.valid)
            return result;
        const int onset = boundaries.onsetSample;
        const Geometry geometry { boundaries.attackStart, boundaries.attackEnd,
                                  boundaries.bodyEnd, boundaries.tailEnd };

        const int frameSamples = std::max (8, (int) std::lround (sampleRate * 0.004));
        const int hopSamples = std::max (4, frameSamples / 2);
        std::vector<Frame> frames;
        double maxOverlapEnergy = 0.0;

        for (int start = geometry.start; start + frameSamples <= geometry.end; start += hopSamples)
        {
            double ab = 0.0, aa = 0.0, bb = 0.0;
            for (int i = 0; i < frameSamples; ++i)
            {
                const double a = bass[(size_t) (start + i)];
                const double b = kick[(size_t) (start + i)];
                ab += a * b;
                aa += a * a;
                bb += b * b;
            }

            const double overlapEnergy = std::sqrt (aa * bb) / (double) frameSamples;
            const double norm = std::sqrt (aa * bb);
            const float correlation = norm > 1.0e-12
                ? (float) std::clamp (ab / norm, -1.0, 1.0) : 0.0f;
            frames.push_back ({ start, overlapEnergy, correlation, 0.0f });
            maxOverlapEnergy = std::max (maxOverlapEnergy, overlapEnergy);
        }

        if (frames.empty() || maxOverlapEnergy <= 1.0e-12)
            return result;

        float regionSums[3] {};
        int regionCounts[3] {};
        int peakFrame = -1;
        for (int i = 0; i < (int) frames.size(); ++i)
        {
            auto& frame = frames[(size_t) i];
            const float overlap = (float) std::clamp (frame.overlapEnergy / maxOverlapEnergy, 0.0, 1.0);
            frame.severity = overlap * std::max (0.0f, -frame.correlation);
            const int regionIndex = regionForSample (frame.start + frameSamples / 2, geometry);
            regionSums[regionIndex] += frame.severity;
            ++regionCounts[regionIndex];

            if (peakFrame < 0 || frame.severity > frames[(size_t) peakFrame].severity)
                peakFrame = i;
        }

        result.attackSeverity = meanSeverity (regionSums[0], regionCounts[0]);
        result.bodySeverity = meanSeverity (regionSums[1], regionCounts[1]);
        result.tailSeverity = meanSeverity (regionSums[2], regionCounts[2]);
        if (peakFrame < 0 || frames[(size_t) peakFrame].severity <= 1.0e-5f)
            return result;

        const int bestRegion = bestRegionIndex (result);

        const int pad = std::max (1, (int) std::lround (sampleRate * 0.003));
        // The severity peak selects the region; pass the complete adaptive
        // region onward.  A few-ms peak alone is insufficient for the shared
        // 30-120 Hz alignment estimator, whereas the complete attack/body/tail
        // segment preserves the locally dominant conflict and enough cycles.
        const int regionStart = bestRegion == 0 ? geometry.start
                              : bestRegion == 1 ? geometry.attackEnd : geometry.bodyEnd;
        const int regionEnd = bestRegion == 0 ? geometry.attackEnd
                            : bestRegion == 1 ? geometry.bodyEnd : geometry.end;
        int start = std::max (geometry.start, regionStart - pad);
        int end = std::min (geometry.end, regionEnd + pad);
        expandToMinimumWindow (start, end, geometry, sampleRate);

        result.valid = end - start > 32;
        result.region = regionFromIndex (bestRegion);
        result.onsetSample = onset;
        result.startSample = start;
        result.lengthSamples = end - start;
        result.severity = frames[(size_t) peakFrame].severity;
        return result;
    }

private:
    struct Geometry { int start = 0, attackEnd = 0, bodyEnd = 0, end = 0; };
    struct Frame { int start; double overlapEnergy; float correlation; float severity; };

    static int findOnset (const float* kick, int n, double sampleRate)
    {
        // Use the production detector settings rather than an absolute peak
        // threshold.  Thus an 808's long tail cannot redefine a later region.
        TransientDetector detector;
        detector.prepare (sampleRate);
        detector.setThreshold (1.0e-7f);
        detector.setMinimumEnergyGate (1.0e-8f);
        detector.setAttackReleaseMs (2.0f, 60.0f);
        detector.setTriggerRatio (1.35f);
        detector.setHoldoffMs (90.0f);
        for (int i = 0; i < n; ++i)
            if (detector.processSample (kick[(size_t) i]))
                return i;

        int peak = 0;
        float peakEnergy = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float energy = kick[(size_t) i] * kick[(size_t) i];
            if (energy > peakEnergy) { peakEnergy = energy; peak = i; }
        }
        return peakEnergy > 1.0e-10f ? peak : -1;
    }

    static Geometry makeGeometry (const float* kick, int n, double sampleRate, int onset)
    {
        Geometry g;
        if (onset < 0)
            return g;
        g.start = std::clamp (onset, 0, n - 1);

        const float release = (float) std::exp (-1.0 / std::max (1.0, sampleRate * 0.020));
        float envelope = 0.0f, peak = 0.0f;
        int peakIndex = g.start;
        std::vector<float> envelopes ((size_t) n, 0.0f);
        for (int i = g.start; i < n; ++i)
        {
            const float energy = kick[(size_t) i] * kick[(size_t) i];
            envelope = energy > envelope ? energy : release * envelope + (1.0f - release) * energy;
            envelopes[(size_t) i] = envelope;
            if (envelope > peak) { peak = envelope; peakIndex = i; }
        }
        if (peak <= 1.0e-12f)
            return {};

        const float tailFloor = peak * 0.0025f;
        const int minimumEnd = std::min (n, g.start + std::max (33, (int) std::lround (sampleRate * 0.030)));
        int lastActive = minimumEnd;
        for (int i = g.start; i < n; ++i)
            if (kick[(size_t) i] * kick[(size_t) i] >= tailFloor)
                lastActive = i + 1;
        g.end = std::clamp (lastActive, minimumEnd, n);

        const int duration = std::max (1, g.end - g.start);
        // Keep enough low-frequency cycles for the unchanged 30-120 Hz
        // alignment engine.  A perceptual kick attack is short, but a 6 ms
        // crop cannot reliably establish phase at 40-100 Hz.
        const int attackMinimum = std::max (1, (int) std::lround (sampleRate * 0.010));
        const int attackMaximum = std::max (attackMinimum, (int) std::lround (sampleRate * 0.028));
        const int peakSpan = std::max (attackMinimum, peakIndex - g.start + attackMinimum);
        const int attackLength = std::clamp (peakSpan, attackMinimum, std::min (attackMaximum, std::max (attackMinimum, duration / 3)));
        g.attackEnd = std::min (g.end, g.start + attackLength);
        g.bodyEnd = std::clamp (g.start + (int) std::lround (duration * 0.68), g.attackEnd + 1, g.end);
        return g;
    }

    static int regionForSample (int sample, const Geometry& g) noexcept
    { return sample < g.attackEnd ? 0 : (sample < g.bodyEnd ? 1 : 2); }
    static float meanSeverity (float sum, int count) noexcept { return count > 0 ? sum / (float) count : 0.0f; }
    static ConflictRegion regionFromIndex (int region) noexcept
    { return region == 0 ? ConflictRegion::attack : region == 1 ? ConflictRegion::body : ConflictRegion::tail; }
    static int bestRegionIndex (const ConflictLocalizationResult& r) noexcept
    { return r.bodySeverity > r.attackSeverity && r.bodySeverity >= r.tailSeverity ? 1
           : r.tailSeverity > r.attackSeverity ? 2 : 0; }
    static void expandToMinimumWindow (int& start, int& end, const Geometry& g, double sampleRate) noexcept
    {
        const int minimum = std::max (33, (int) std::lround (sampleRate * 0.024));
        if (end - start >= minimum || g.end - g.start <= minimum)
            return;
        const int missing = minimum - (end - start);
        start = std::max (g.start, start - missing / 2);
        end = std::min (g.end, start + minimum);
        start = std::max (g.start, end - minimum);
    }
};

class ConflictLocalizedPhaseAnalyzer
{
public:
    static PhaseFixResult analyze (const float* bass,
                                   const float* kick,
                                   int numSamples,
                                   double sampleRate,
                                   float maxDelayMs,
                                   InterpolationType delayInterpolation,
                                   bool searchRotator,
                                   ConflictLocalizationResult* localization = nullptr)
    {
        const auto selected = ConflictRegionLocalizer::localize (bass, kick, numSamples, sampleRate);
        if (localization != nullptr)
            *localization = selected;
        if (selected.valid)
        {
            // Local candidate first: this is the Layer-A path.  Retain the
            // full hit as an objective safety/reference candidate so a crop
            // can never mask an out-of-budget timing advisory or degrade the
            // canonical shared score.
            const auto localized = PhaseFixEngine::analyze (
                bass + selected.startSample, kick + selected.startSample,
                selected.lengthSamples, sampleRate, maxDelayMs,
                delayInterpolation, searchRotator);
            const auto full = PhaseFixEngine::analyze (
                bass, kick, numSamples, sampleRate, maxDelayMs,
                delayInterpolation, searchRotator);
            if (full.largeTimingOffset || full.requiresTimelineMove)
                return full;
            if (! full.valid || ! full.enoughSignal)
            {
                if (full.valid)
                {
                    auto timingSafe = localized;
                    timingSafe.bassDelayMs = full.bassDelayMs;
                    timingSafe.bassPolarityInvert = full.bassPolarityInvert;
                    timingSafe.detectedTimingOffsetMs = full.detectedTimingOffsetMs;
                    timingSafe.largeTimingOffset = full.largeTimingOffset;
                    timingSafe.requiresTimelineMove = full.requiresTimelineMove;
                    return timingSafe;
                }
                return localized;
            }
            if (localized.valid && localized.enoughSignal
                && localized.afterMatchPercent > full.afterMatchPercent + 0.01f)
            {
                // Keep the localized conflict score/quality, but retain the
                // canonical full-window timing axis. A cropped transient can
                // legitimately have a different group-delay optimum; using
                // that as the learned timing would shift otherwise aligned
                // real material by the allpass residual.
                auto timingSelected = localized;
                timingSelected.bassDelayMs = full.bassDelayMs;
                timingSelected.bassPolarityInvert = full.bassPolarityInvert;
                timingSelected.detectedTimingOffsetMs = full.detectedTimingOffsetMs;
                timingSelected.largeTimingOffset = full.largeTimingOffset;
                timingSelected.requiresTimelineMove = full.requiresTimelineMove;
                return timingSelected;
            }
            return full;
        }
        return PhaseFixEngine::analyze (bass, kick, numSamples, sampleRate, maxDelayMs,
                                        delayInterpolation, searchRotator);
    }
};
