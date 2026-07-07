#pragma once

#include <vector>
#include <algorithm>
#include <cmath>
#include <numeric>

struct HitObservation
{
    int hitIndex = -1;
    float delayMs = 0.0f;
    bool polarityInvert = false;
    float phaseFilterFreqHz = 200.0f;
    bool phaseFilterEnabled = false;
    float matchPercent = 0.0f;
    float energy = 0.0f;
    float signalConfidence = 0.0f;
    float dominantFrequencyHz = 60.0f;
};

struct HitCluster
{
    std::vector<int> memberIndices;
    float centroidDelayMs = 0.0f;
    bool centroidPolarity = false;
    float centroidPhaseFreqHz = 200.0f;
    bool centroidPhaseEnabled = false;
    float totalEnergy = 0.0f;
    float internalSpread = 0.0f;
    float averageSignalConfidence = 0.0f;
};

struct ConsensusResult
{
    bool hasConsensus = false;
    int dominantClusterIndex = -1;
    std::vector<HitCluster> clusters;
    std::vector<int> outlierIndices;
    float consensusConfidence = 0.0f;
    float dominantClusterShare = 0.0f;
    float dominantEnergyShare = 0.0f;
};

class HitConsensus
{
public:
    static ConsensusResult analyze (const std::vector<HitObservation>& hits)
    {
        ConsensusResult result;
        if (hits.empty())
            return result;

        if (hits.size() == 1)
        {
            HitCluster c;
            c.memberIndices.push_back(0);
            c.centroidDelayMs = hits[0].delayMs;
            c.centroidPolarity = hits[0].polarityInvert;
            c.centroidPhaseFreqHz = hits[0].phaseFilterFreqHz;
            c.centroidPhaseEnabled = hits[0].phaseFilterEnabled;
            c.totalEnergy = hits[0].energy;
            c.internalSpread = 0.0f;
            c.averageSignalConfidence = hits[0].signalConfidence;
            
            result.clusters.push_back(c);
            result.dominantClusterIndex = 0;
            result.hasConsensus = true;
            result.dominantClusterShare = 1.0f;
            result.dominantEnergyShare = 1.0f;
            result.consensusConfidence = hits[0].signalConfidence * 0.5f; // Penalty for single hit
            return result;
        }

        const float dominantFrequencyHz = estimateDominantFrequency (hits);
        const float periodMs = 1000.0f / dominantFrequencyHz;
        const float halfPeriodMs = 0.5f * periodMs;
        const float bandwidth = std::max (0.35f, periodMs * 0.08f);
        const float assignmentDistance = std::max (0.6f, bandwidth * 1.5f);

        std::vector<float> physicalDelayMs;
        physicalDelayMs.reserve (hits.size());
        for (const auto& h : hits)
            physicalDelayMs.push_back (canonicalPhysicalDelay (h, halfPeriodMs));

        // 1. KDE (Kernel Density Estimation) over physical delay. An inverted
        // observation is equivalent to a non-inverted one shifted by half a
        // period around the dominant low frequency, so cluster that physical
        // solution first and choose the simplest representative later.
        float minDelay = physicalDelayMs[0];
        float maxDelay = physicalDelayMs[0];
        for (auto delay : physicalDelayMs)
        {
            minDelay = std::min(minDelay, delay);
            maxDelay = std::max(maxDelay, delay);
        }

        const float step = 0.05f;
        const int numPoints = (int)std::ceil((maxDelay - minDelay) / step) + 1 + 20; // Padding
        const float startGrid = minDelay - 10.0f * step;
        
        std::vector<float> density((size_t)numPoints, 0.0f);
        for (int i = 0; i < numPoints; ++i)
        {
            float x = startGrid + (float)i * step;
            float d = 0.0f;
            for (size_t h = 0; h < hits.size(); ++h)
            {
                float dist = std::abs(x - physicalDelayMs[h]);
                if (dist < bandwidth)
                {
                    // Triangle kernel weighted by energy
                    d += hits[h].energy * (1.0f - dist / bandwidth);
                }
            }
            density[(size_t)i] = d;
        }

        // 2. Find peaks
        std::vector<float> peaks;
        for (int i = 1; i < numPoints - 1; ++i)
        {
            if (density[(size_t)i] > density[(size_t)(i - 1)] && density[(size_t)i] > density[(size_t)(i + 1)] && density[(size_t)i] > 1e-6f)
            {
                peaks.push_back(startGrid + (float)i * step);
            }
        }
        
        // Edge cases if no peaks found (e.g. perfectly flat)
        if (peaks.empty())
            peaks.push_back(hits[0].delayMs);

        // 3. Assign to clusters
        for (float p : peaks)
        {
            HitCluster c;
            c.centroidDelayMs = p;
            result.clusters.push_back(c);
        }

        for (size_t i = 0; i < hits.size(); ++i)
        {
            float bestDist = 1e9f;
            int bestCluster = -1;
            for (size_t c = 0; c < result.clusters.size(); ++c)
            {
                float dist = std::abs(physicalDelayMs[i] - result.clusters[c].centroidDelayMs);
                if (dist < bestDist)
                {
                    bestDist = dist;
                    bestCluster = (int)c;
                }
            }
            
            if (bestCluster >= 0 && bestDist < assignmentDistance)
            {
                result.clusters[(size_t)bestCluster].memberIndices.push_back((int)i);
            }
            else
            {
                result.outlierIndices.push_back((int)i);
            }
        }

        // 4. Remove empty clusters
        result.clusters.erase(std::remove_if(result.clusters.begin(), result.clusters.end(),
            [](const HitCluster& c) { return c.memberIndices.empty(); }), result.clusters.end());

        // 5. Finalize clusters (compute centroids, splits by polarity, etc.)
        std::vector<HitCluster> finalClusters;
        for (auto& c : result.clusters)
        {
            HitCluster sub;
            int numPhaseEnabled = 0;
            float totalFreq = 0.0f;
            float freqWeightSum = 0.0f;
            float physicalCentroid = 0.0f;
            float normalEnergy = 0.0f;
            float invertedEnergy = 0.0f;

            for (int idx : c.memberIndices)
            {
                const auto& h = hits[(size_t)idx];
                sub.memberIndices.push_back(idx);
                sub.totalEnergy += h.energy;
                sub.averageSignalConfidence += h.signalConfidence;
                physicalCentroid += physicalDelayMs[(size_t) idx] * h.energy;

                if (h.polarityInvert)
                    invertedEnergy += h.energy;
                else
                    normalEnergy += h.energy;

                if (h.phaseFilterEnabled)
                {
                    numPhaseEnabled++;
                    totalFreq += h.phaseFilterFreqHz * h.energy;
                    freqWeightSum += h.energy;
                }
            }

            if (sub.memberIndices.empty() || sub.totalEnergy <= 0.0f)
                continue;

            sub.averageSignalConfidence /= (float) sub.memberIndices.size();
            physicalCentroid /= sub.totalEnergy;

            const float normalDelay = physicalCentroid;
            const float invertedDelay = physicalCentroid - halfPeriodMs;
            const bool preferInvertedByEnergy = invertedEnergy > normalEnergy * 1.2f;
            sub.centroidPolarity = preferInvertedByEnergy
                || (! (normalEnergy > invertedEnergy * 1.2f) && std::abs (invertedDelay) < std::abs (normalDelay));
            sub.centroidDelayMs = sub.centroidPolarity ? invertedDelay : normalDelay;

            for (int idx : sub.memberIndices)
            {
                const float diff = physicalDelayMs[(size_t)idx] - physicalCentroid;
                sub.internalSpread += diff * diff;
            }
            sub.internalSpread = std::sqrt(sub.internalSpread / (float)sub.memberIndices.size());

            sub.centroidPhaseEnabled = (numPhaseEnabled > (int)sub.memberIndices.size() / 2);
            sub.centroidPhaseFreqHz = freqWeightSum > 0.0f ? totalFreq / freqWeightSum : 200.0f;

            finalClusters.push_back(sub);
        }
        result.clusters = finalClusters;

        if (result.clusters.empty())
            return result;

        // 6. Find dominant cluster
        int bestClusterIdx = 0;
        float bestScore = -1.0f;
        float totalSystemEnergy = 0.0f;
        for (const auto& h : hits) totalSystemEnergy += h.energy;
        
        for (size_t i = 0; i < result.clusters.size(); ++i)
        {
            const auto& c = result.clusters[i];
            float sizeScore = (float)c.memberIndices.size() / (float)hits.size();
            float energyScore = c.totalEnergy / std::max(1e-6f, totalSystemEnergy);
            float spreadPenalty = std::max(0.0f, 1.0f - c.internalSpread / 1.0f); // Tightness
            
            float score = (sizeScore * 0.4f + energyScore * 0.4f) * (0.5f + 0.5f * spreadPenalty);
            if (score > bestScore)
            {
                bestScore = score;
                bestClusterIdx = (int)i;
            }
        }
        
        result.dominantClusterIndex = bestClusterIdx;
        result.hasConsensus = true;

        // 7. Calculate overall confidence based on the dominant cluster
        const auto& dom = result.clusters[(size_t)bestClusterIdx];
        float clusterShare = (float)dom.memberIndices.size() / (float)hits.size();
        float energyShare = dom.totalEnergy / std::max(1e-6f, totalSystemEnergy);
        float tightness = std::max(0.0f, 1.0f - dom.internalSpread / assignmentDistance);
        float outlierPenalty = std::clamp ((float) result.outlierIndices.size() / (float) hits.size(), 0.0f, 1.0f);
        result.dominantClusterShare = clusterShare;
        result.dominantEnergyShare = energyShare;
        
        result.consensusConfidence = 0.35f * clusterShare + 
                                     0.25f * tightness + 
                                     0.25f * energyShare + 
                                     0.15f * dom.averageSignalConfidence;
        result.consensusConfidence *= (1.0f - 0.35f * outlierPenalty);
                                     
        // Clamp it
        result.consensusConfidence = std::max(0.0f, std::min(1.0f, result.consensusConfidence));

        return result;
    }

private:
    static float estimateDominantFrequency (const std::vector<HitObservation>& hits) noexcept
    {
        float weighted = 0.0f;
        float weight = 0.0f;
        for (const auto& h : hits)
        {
            const float f = std::clamp (h.dominantFrequencyHz, 30.0f, 120.0f);
            const float w = std::max (h.energy, 1.0e-6f);
            weighted += f * w;
            weight += w;
        }

        return weight > 0.0f ? weighted / weight : 60.0f;
    }

    static float canonicalPhysicalDelay (const HitObservation& hit, float halfPeriodMs) noexcept
    {
        return hit.delayMs + (hit.polarityInvert ? halfPeriodMs : 0.0f);
    }
};
