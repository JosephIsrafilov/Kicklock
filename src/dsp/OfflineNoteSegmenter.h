#pragma once

#include "OfflineFundamentalEstimator.h"
#include "NotePhaseMap.h"
#include "NoteQuantizer.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

// Offline, non-causal note segmentation over a full Learn-session bass buffer.
// Worker-thread only — never call from processBlock().
//
// Sliding-window YIN (OfflineFundamentalEstimator) → merge adjacent frames that
// quantize to the same MIDI note into {start, end, frequencyHz, confidence}.
// Used by Learn finalize to bucket hits by the note active near each kick,
// instead of the causal live PitchTracker value frozen at the trigger sample.
struct OfflineNoteSegment
{
    int startSample = 0;
    int endSample = 0;          // exclusive
    float frequencyHz = 0.0f;
    float confidence = 0.0f;
    bool octaveCorrected = false;
};

class OfflineNoteSegmenter
{
public:
    // Window must be long enough for OfflineFundamentalEstimator (>= 2 periods
    // at kFundamentalMinHz). hop is the analysis step.
    static constexpr float kDefaultWindowMs = 100.0f;
    static constexpr float kDefaultHopMs = 20.0f;

    static std::vector<OfflineNoteSegment> segment (const float* bass,
                                                    int numSamples,
                                                    double sampleRate,
                                                    float windowMs = kDefaultWindowMs,
                                                    float hopMs = kDefaultHopMs,
                                                    const std::function<bool()>& shouldCancel = {})
    {
        std::vector<OfflineNoteSegment> out;
        if (bass == nullptr || numSamples <= 0
            || ! (sampleRate > 0.0) || ! std::isfinite (sampleRate)
            || ! (windowMs > 0.0f) || ! (hopMs > 0.0f))
            return out;

        const auto cancelled = [&shouldCancel]() noexcept
        { return shouldCancel && shouldCancel(); };

        const int window = juce::jmax (1, (int) std::lround ((double) windowMs * sampleRate / 1000.0));
        const int hop = juce::jmax (1, (int) std::lround ((double) hopMs * sampleRate / 1000.0));
        if (numSamples < window)
            return out;

        struct Frame
        {
            int center = 0;
            float hz = 0.0f;
            float conf = 0.0f;
            int midi = -1;
            bool octaveCorrected = false;
        };

        std::vector<Frame> frames;
        frames.reserve ((size_t) (numSamples / hop) + 1);

        // Cooperative cancellation: this loop is the heavy part (YIN per
        // window over the whole loop). Check every frame — the predicate is
        // cheap vs one estimate; every-N would leave multi-frame teardown lag
        // at 96 kHz / long captures.
        for (int start = 0; start + window <= numSamples; start += hop)
        {
            if (cancelled())
                return {};

            const auto est = OfflineFundamentalEstimator::estimate (
                bass + start, window, sampleRate,
                NoteMap::kFundamentalMinHz, NoteMap::kFundamentalMaxHz);
            if (! est.valid || est.octaveAmbiguous
                || est.confidence < NoteMap::kMinOfflinePitchConfidence)
                continue;

            Frame f;
            f.center = start + window / 2;
            f.hz = est.frequencyHz;
            f.conf = est.confidence;
            f.midi = NoteQuantizer::hzToMidi (est.frequencyHz);
            f.octaveCorrected = est.octaveCorrected;
            if (f.midi < 0)
                continue;
            frames.push_back (f);
        }

        if (cancelled() || frames.empty())
            return out;

        // Segment bounds track analysis-frame centres only (no half-window pad),
        // so silence before a tone is not covered by the first note segment.
        OfflineNoteSegment cur;
        cur.startSample = frames[0].center;
        cur.endSample = juce::jmin (numSamples, frames[0].center + hop);
        cur.frequencyHz = frames[0].hz;
        cur.confidence = frames[0].conf;
        cur.octaveCorrected = frames[0].octaveCorrected;
        int curMidi = frames[0].midi;
        int count = 1;
        double hzSum = (double) frames[0].hz;
        double confSum = (double) frames[0].conf;

        auto flush = [&]()
        {
            if (count <= 0)
                return;
            cur.frequencyHz = (float) (hzSum / (double) count);
            cur.confidence = (float) (confSum / (double) count);
            cur.endSample = juce::jlimit (cur.startSample + 1, numSamples, cur.endSample);
            out.push_back (cur);
        };

        for (size_t i = 1; i < frames.size(); ++i)
        {
            const auto& f = frames[i];
            if (f.midi == curMidi)
            {
                cur.endSample = juce::jmin (numSamples, f.center + hop);
                hzSum += (double) f.hz;
                confSum += (double) f.conf;
                cur.octaveCorrected = cur.octaveCorrected || f.octaveCorrected;
                ++count;
            }
            else
            {
                flush();
                cur.startSample = f.center;
                cur.endSample = juce::jmin (numSamples, f.center + hop);
                cur.frequencyHz = f.hz;
                cur.confidence = f.conf;
                cur.octaveCorrected = f.octaveCorrected;
                curMidi = f.midi;
                count = 1;
                hzSum = (double) f.hz;
                confSum = (double) f.conf;
            }
        }
        flush();
        return out;
    }

    static float coveringFrequency (const std::vector<OfflineNoteSegment>& segments,
                                    int sampleIndex) noexcept
    {
        if (segments.empty() || sampleIndex < 0)
            return 0.0f;
        for (const auto& s : segments)
        {
            if (sampleIndex >= s.startSample && sampleIndex < s.endSample
                && s.frequencyHz > 0.0f && std::isfinite (s.frequencyHz))
                return s.frequencyHz;
        }
        return 0.0f;
    }

    static const OfflineNoteSegment* segmentAt (const std::vector<OfflineNoteSegment>& segments,
                                                int sampleIndex,
                                                int searchForwardSamples = 0) noexcept
    {
        if (segments.empty() || sampleIndex < 0)
            return nullptr;

        auto covering = [&segments] (int index) -> const OfflineNoteSegment*
        {
            for (const auto& segment : segments)
                if (index >= segment.startSample && index < segment.endSample
                    && segment.frequencyHz > 0.0f && std::isfinite (segment.frequencyHz))
                    return &segment;
            return nullptr;
        };

        if (searchForwardSamples <= 0)
            return covering (sampleIndex);

        const int offsets[] = {
            searchForwardSamples / 2,
            searchForwardSamples / 4,
            (3 * searchForwardSamples) / 4,
            juce::jmax (1, searchForwardSamples / 8),
            0
        };
        for (const int offset : offsets)
            if (const auto* segment = covering (sampleIndex + offset))
                return segment;
        return nullptr;
    }

    // Pitch for a kick at sampleIndex. Samples the segment map at probes inside
    // [sampleIndex, sampleIndex + searchForwardSamples] only (Learn post-roll).
    // Does not pull in notes whose energy starts at/after the search limit, so
    // true late-arrangement bass outside the correction window stays pitchless.
    static float frequencyAt (const std::vector<OfflineNoteSegment>& segments,
                              int sampleIndex,
                              int searchForwardSamples = 0) noexcept
    {
        if (segments.empty() || sampleIndex < 0)
            return 0.0f;

        if (searchForwardSamples > 0)
        {
            // Prefer mid post-roll (note change on kick → new note; early swing → tone).
            const int offsets[] = {
                searchForwardSamples / 2,
                searchForwardSamples / 4,
                (3 * searchForwardSamples) / 4,
                juce::jmax (1, searchForwardSamples / 8),
                0
            };
            for (int off : offsets)
            {
                const float hz = coveringFrequency (segments, sampleIndex + off);
                if (hz > 0.0f)
                    return hz;
            }
            return 0.0f;
        }

        return coveringFrequency (segments, sampleIndex);
    }

    // Confidence of the segment that supplied frequencyAt (same probe policy).
    static float confidenceAt (const std::vector<OfflineNoteSegment>& segments,
                               int sampleIndex,
                               int searchForwardSamples = 0) noexcept
    {
        if (segments.empty() || sampleIndex < 0)
            return 0.0f;

        auto confCovering = [&] (int idx) -> float
        {
            for (const auto& s : segments)
                if (idx >= s.startSample && idx < s.endSample
                    && s.confidence > 0.0f && std::isfinite (s.confidence))
                    return s.confidence;
            return 0.0f;
        };

        if (searchForwardSamples > 0)
        {
            const int offsets[] = {
                searchForwardSamples / 2,
                searchForwardSamples / 4,
                (3 * searchForwardSamples) / 4,
                juce::jmax (1, searchForwardSamples / 8),
                0
            };
            for (int off : offsets)
            {
                const float c = confCovering (sampleIndex + off);
                if (c > 0.0f)
                    return c;
            }
            return 0.0f;
        }
        return confCovering (sampleIndex);
    }
};
