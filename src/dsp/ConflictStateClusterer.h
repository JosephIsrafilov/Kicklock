#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "ConflictRegion.h"

struct ConflictStateSample
{
    int hitIndex = -1;
    float delayMs = 0.0f;
    bool polarityInvert = false;
    float allpassFreqHz = 0.0f;
    float allpassQ = 0.7f;
    int stages = 2;
    ConflictRegion regionType = ConflictRegion::none;
    float confidence = 0.0f;
    float matchPercent = 0.0f;
    float improvementPoints = 0.0f;
    int noteLabel = -1;
};

struct ConflictStateCluster
{
    std::vector<ConflictStateSample> samples;
};

// Worker-thread-only deterministic clustering. Tolerances are MAD-derived
// (with a small measurement floor) on delay/frequency/Q; region and polarity
// remain categorical. The fixed state budget is enforced by nearest merges.
class ConflictStateClusterer
{
public:
    static std::vector<ConflictStateCluster> cluster (const std::vector<ConflictStateSample>& input,
                                                       int maxStates)
    {
        std::vector<ConflictStateCluster> clusters;
        if (maxStates <= 0)
            return clusters;

        for (const auto& sample : input)
        {
            if (sample.regionType == ConflictRegion::none)
                continue;
            int best = -1;
            float bestDistance = std::numeric_limits<float>::max();
            for (int i = 0; i < (int) clusters.size(); ++i)
            {
                const auto& c = clusters[(size_t) i];
                const auto center = medians (c.samples);
                if (sample.polarityInvert != center.polarity || sample.regionType != center.region)
                    continue;
                const auto tol = tolerances (c.samples);
                const float d = distance (sample, center, tol);
                if (d <= 1.0f && d < bestDistance)
                {
                    best = i;
                    bestDistance = d;
                }
            }
            if (best < 0)
            {
                ConflictStateCluster c;
                c.samples.push_back (sample);
                clusters.push_back (std::move (c));
            }
            else
                clusters[(size_t) best].samples.push_back (sample);
        }

        while ((int) clusters.size() > maxStates)
        {
            int ai = 0, bi = 1;
            float nearest = std::numeric_limits<float>::max();
            for (int a = 0; a < (int) clusters.size(); ++a)
                for (int b = a + 1; b < (int) clusters.size(); ++b)
                {
                    const auto ca = medians (clusters[(size_t) a].samples);
                    const auto cb = medians (clusters[(size_t) b].samples);
                    const auto tol = tolerancesMerged (clusters[(size_t) a].samples, clusters[(size_t) b].samples);
                    float d = distance (ca, cb, tol);
                    if (ca.polarity != cb.polarity) d += 4.0f;
                    if (ca.region != cb.region) d += 4.0f;
                    if (d < nearest) { nearest = d; ai = a; bi = b; }
                }
            auto& dst = clusters[(size_t) ai].samples;
            auto& src = clusters[(size_t) bi].samples;
            dst.insert (dst.end(), src.begin(), src.end());
            clusters.erase (clusters.begin() + bi);
        }
        return clusters;
    }

private:
    struct Center
    {
        float delay = 0.0f, freq = 0.0f, q = 0.7f;
        bool polarity = false;
        ConflictRegion region = ConflictRegion::none;
    };
    struct Tol { float delay = 0.1f, freq = 1.0f, q = 0.25f; };

    static float median (std::vector<float> v)
    {
        if (v.empty()) return 0.0f;
        std::sort (v.begin(), v.end());
        const size_t n = v.size();
        return n % 2 ? v[n / 2] : 0.5f * (v[n / 2 - 1] + v[n / 2]);
    }
    static Center medians (const std::vector<ConflictStateSample>& s)
    {
        Center c;
        std::vector<float> d, f, q;
        int inv = 0;
        for (const auto& x : s) { d.push_back(x.delayMs); f.push_back(x.allpassFreqHz); q.push_back(x.allpassQ); inv += x.polarityInvert ? 1 : 0; }
        c.delay = median(d); c.freq = median(f); c.q = median(q); c.polarity = inv * 2 > (int) s.size();
        if (! s.empty()) c.region = s[s.size() / 2].regionType;
        return c;
    }
    static Tol tolerances (const std::vector<ConflictStateSample>& s)
    {
        const auto c = medians (s);
        std::vector<float> d, f, q;
        for (const auto& x : s) { d.push_back(std::abs(x.delayMs-c.delay)); f.push_back(std::abs(x.allpassFreqHz-c.freq)); q.push_back(std::abs(x.allpassQ-c.q)); }
        return { std::max(1.0f, 3.0f * 1.4826f * median(d)),
                 std::max(5.0f, 3.0f * 1.4826f * median(f)),
                 std::max(0.5f, 3.0f * 1.4826f * median(q)) };
    }
    static Tol tolerancesMerged (const std::vector<ConflictStateSample>& a, const std::vector<ConflictStateSample>& b)
    {
        std::vector<ConflictStateSample> all = a;
        all.insert (all.end(), b.begin(), b.end());
        return tolerances (all);
    }
    static float distance (const ConflictStateSample& x, const Center& c, const Tol& t)
    {
        return std::max ({ std::abs(x.delayMs-c.delay)/t.delay,
                           std::abs(x.allpassFreqHz-c.freq)/t.freq,
                           std::abs(x.allpassQ-c.q)/t.q });
    }
    static float distance (const Center& a, const Center& b, const Tol& t)
    {
        return std::max ({ std::abs(a.delay-b.delay)/t.delay,
                           std::abs(a.freq-b.freq)/t.freq,
                           std::abs(a.q-b.q)/t.q });
    }
};
