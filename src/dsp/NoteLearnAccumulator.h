#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "NotePhaseMap.h"

// =============================================================================
// NoteLearnAccumulator (plan v1.2, Phase 3 Pass B helper).
//
// Deterministically aggregates every valid hit that quantised to the SAME note
// into one NoteEntry. Each contributing hit supplies its offline fundamental,
// its fixed-timing rotator candidate (allpass frequency / Q searched with the
// global Delay/Polarity held fixed), a confidence, and whether the rotator
// helped.
//
// Aggregation policy (order-independent in value, stable given the input order):
//   - fundamentalHz : median of the contributing fundamentals (robust to a
//                     single detuned/octave outlier);
//   - allpassFreqHz : median of the searched allpass centres;
//   - allpassQ      : median of the searched Qs, clamped to the valid range;
//   - confidence    : mean per-hit confidence, clamped to [0, 1];
//   - spread ratio  : population std-dev of the fundamentals over |median|;
//   - hitCount      : number of finite contributing hits;
//   - rotatorHelps  : strict majority of hits reported the rotator helped.
//
// finalizeEntry() marks the entry learned only when it also passes the section-5
// acceptance limits (minimum hits, spread ceiling, minimum confidence, and the
// fundamental / allpass-frequency / Q ranges). Everything is finite-guarded, so
// no field can ever come out NaN/Inf. This class is pure and worker-thread-only.
// =============================================================================

class NoteLearnAccumulator
{
public:
    struct Hit
    {
        float fundamentalHz = 0.0f;
        float allpassFreqHz = 0.0f;
        float allpassQ = 0.7f;
        float confidence = 0.0f;
        bool  rotatorHelps = false;
    };

    void clear() noexcept { hits.clear(); }
    void add (const Hit& h) { hits.push_back (h); }
    int  count() const noexcept { return (int) hits.size(); }

    // Raw aggregated entry (learned == true) or a default entry (learned ==
    // false) when there is no finite data. Does NOT apply acceptance limits;
    // use accept() / finalizeEntry() for a validated entry.
    NoteEntry aggregate() const
    {
        NoteEntry entry;

        std::vector<float> funds, freqs, qs;
        funds.reserve (hits.size());
        freqs.reserve (hits.size());
        qs.reserve (hits.size());

        double confidenceSum = 0.0;
        int helpCount = 0;

        for (const auto& h : hits)
        {
            if (! std::isfinite (h.fundamentalHz) || ! std::isfinite (h.allpassFreqHz)
                || ! std::isfinite (h.allpassQ) || ! std::isfinite (h.confidence))
                continue;

            funds.push_back (h.fundamentalHz);
            freqs.push_back (h.allpassFreqHz);
            qs.push_back (h.allpassQ);
            confidenceSum += (double) std::clamp (h.confidence, 0.0f, 1.0f);
            if (h.rotatorHelps)
                ++helpCount;
        }

        const int n = (int) funds.size();
        if (n == 0)
            return entry;   // learned == false

        entry.fundamentalHz = median (funds);
        entry.allpassFreqHz = median (freqs);
        entry.allpassQ = std::clamp (median (qs), NoteMap::kAllpassQMin, NoteMap::kAllpassQMax);
        entry.confidence = std::clamp ((float) (confidenceSum / (double) n), 0.0f, 1.0f);
        entry.fundamentalSpreadRatio = spreadRatio (funds, entry.fundamentalHz);
        entry.hitCount = n;
        entry.rotatorHelps = (helpCount * 2 > n);   // strict majority
        entry.learned = true;

        entry = NoteMap::sanitizeNoteEntry (entry);
        entry.learned = true;   // sanitize leaves learned untouched; keep explicit
        return entry;
    }

    // True when the aggregated entry clears every section-5 acceptance limit.
    static bool accept (const NoteEntry& entry)
    {
        return NoteMap::isValidNoteEntry (entry)
            && entry.confidence >= NoteMap::kMinRuntimeConfidence;
    }

    // Aggregate + validate. Returns a learned, in-range entry, or an unlearned
    // default entry when the note fails acceptance (min hits / spread / F-Q /
    // confidence). Never yields NaN/Inf.
    NoteEntry finalizeEntry() const
    {
        NoteEntry entry = aggregate();
        if (! accept (entry))
            return NoteEntry {};   // unlearned
        return entry;
    }

private:
    std::vector<Hit> hits;

    static float median (std::vector<float> values)
    {
        const size_t n = values.size();
        if (n == 0)
            return 0.0f;

        std::sort (values.begin(), values.end());
        if (n % 2 == 1)
            return values[n / 2];
        return 0.5f * (values[n / 2 - 1] + values[n / 2]);
    }

    static float spreadRatio (const std::vector<float>& values, float centre)
    {
        if (values.size() < 2 || ! (std::abs (centre) > 1.0e-6f))
            return 0.0f;

        double acc = 0.0;
        for (float v : values)
        {
            const double d = (double) v - (double) centre;
            acc += d * d;
        }

        const double sd = std::sqrt (acc / (double) values.size());
        const float ratio = (float) (sd / (double) std::abs (centre));
        return std::isfinite (ratio) ? std::max (0.0f, ratio) : 1.0f;
    }
};
