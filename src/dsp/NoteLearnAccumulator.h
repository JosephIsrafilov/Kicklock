#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "NotePhaseMap.h"

// Deterministic Pass-B note aggregation.  F/Q candidates are filtered before
// aggregation with independent median/MAD gates: |x - median| <= 3 * 1.4826 *
// MAD.  A zero-MAD grid uses an explicit small tolerance (1 Hz / 0.05 Q), so a
// single rogue grid value cannot survive merely because the median is exact.
class NoteLearnAccumulator
{
public:
    struct Hit
    {
        float fundamentalHz = 0.0f;
        float allpassFreqHz = 0.0f;
        float allpassQ = 0.0f;
        float confidence = 0.0f;
        bool rotatorHelps = false;
    };

    void clear() noexcept { hits.clear(); }
    void add (const Hit& hit) { hits.push_back (hit); }
    int count() const noexcept { return (int) hits.size(); }

    NoteEntry aggregate() const
    {
        const auto retained = filteredHits();
        NoteEntry entry;
        if (retained.empty())
            return entry;

        std::vector<float> fundamentals, freqs, qs;
        fundamentals.reserve (retained.size());
        freqs.reserve (retained.size());
        qs.reserve (retained.size());
        double confidenceSum = 0.0;
        int helps = 0;
        for (const auto& hit : retained)
        {
            fundamentals.push_back (hit.fundamentalHz);
            freqs.push_back (hit.allpassFreqHz);
            qs.push_back (hit.allpassQ);
            confidenceSum += std::clamp ((double) hit.confidence, 0.0, 1.0);
            helps += hit.rotatorHelps ? 1 : 0;
        }

        entry.learned = true;
        entry.fundamentalHz = median (fundamentals);
        entry.allpassFreqHz = median (freqs);
        entry.allpassQ = median (qs);
        entry.confidence = (float) (confidenceSum / (double) retained.size());
        entry.fundamentalSpreadRatio = spreadRatio (fundamentals, entry.fundamentalHz);
        entry.hitCount = (int) retained.size();
        entry.rotatorHelps = helps * 2 > entry.hitCount;
        return entry;
    }

    static bool accept (const NoteEntry& entry)
    {
        return NoteMap::isValidNoteEntry (entry)
            && entry.confidence >= NoteMap::kMinRuntimeConfidence
            && entry.rotatorHelps;
    }

    NoteEntry finalizeEntry() const
    {
        const NoteEntry entry = aggregate();
        return accept (entry) ? entry : NoteEntry {};
    }

private:
    std::vector<Hit> hits;

    static bool finiteCandidate (const Hit& hit) noexcept
    {
        return std::isfinite (hit.fundamentalHz) && std::isfinite (hit.allpassFreqHz)
            && std::isfinite (hit.allpassQ) && std::isfinite (hit.confidence)
            && hit.allpassFreqHz >= NoteMap::kAllpassFreqMinHz
            && hit.allpassFreqHz <= NoteMap::kAllpassFreqMaxHz
            && hit.allpassQ >= NoteMap::kAllpassQMin && hit.allpassQ <= NoteMap::kAllpassQMax;
    }

    std::vector<Hit> filteredHits() const
    {
        std::vector<Hit> retained;
        for (const auto& hit : hits)
            if (finiteCandidate (hit))
                retained.push_back (hit);
        if (retained.empty())
            return retained;

        std::vector<float> freqs, qs;
        freqs.reserve (retained.size());
        qs.reserve (retained.size());
        for (const auto& hit : retained) { freqs.push_back (hit.allpassFreqHz); qs.push_back (hit.allpassQ); }
        const float fMedian = median (freqs), qMedian = median (qs);
        const float fMad = medianAbsoluteDeviation (freqs, fMedian);
        const float qMad = medianAbsoluteDeviation (qs, qMedian);
        const float fLimit = std::max (1.0f, 3.0f * 1.4826f * fMad);
        // The production Q grid's closest useful neighbours are wider than
        // 0.05, so retain one neighbouring grid value when MAD is zero.
        const float qLimit = std::max (0.25f, 3.0f * 1.4826f * qMad);

        retained.erase (std::remove_if (retained.begin(), retained.end(), [&] (const Hit& hit)
        {
            return std::abs (hit.allpassFreqHz - fMedian) > fLimit
                || std::abs (hit.allpassQ - qMedian) > qLimit;
        }), retained.end());
        return retained;
    }

    static float median (std::vector<float> values)
    {
        if (values.empty()) return 0.0f;
        std::sort (values.begin(), values.end());
        const size_t n = values.size();
        return n % 2 ? values[n / 2] : 0.5f * (values[n / 2 - 1] + values[n / 2]);
    }

    static float medianAbsoluteDeviation (const std::vector<float>& values, float centre)
    {
        std::vector<float> distances;
        distances.reserve (values.size());
        for (const auto value : values) distances.push_back (std::abs (value - centre));
        return median (std::move (distances));
    }

    static float spreadRatio (const std::vector<float>& values, float centre)
    {
        if (values.size() < 2 || std::abs (centre) <= 1.0e-6f) return 0.0f;
        double sum = 0.0;
        for (const float value : values) { const double d = value - centre; sum += d * d; }
        const float ratio = (float) (std::sqrt (sum / values.size()) / std::abs (centre));
        return std::isfinite (ratio) ? ratio : 1.0f;
    }
};
