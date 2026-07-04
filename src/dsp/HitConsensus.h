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
            result.consensusConfidence = hits[0].signalConfidence * 0.5f; // Penalty for single hit
            return result;
        }

        // 1. KDE (Kernel Density Estimation)
        float minDelay = hits[0].delayMs;
        float maxDelay = hits[0].delayMs;
        for (const auto& h : hits)
        {
            minDelay = std::min(minDelay, h.delayMs);
            maxDelay = std::max(maxDelay, h.delayMs);
        }

        const float bandwidth = 0.4f;
        const float step = 0.05f;
        const int numPoints = (int)std::ceil((maxDelay - minDelay) / step) + 1 + 20; // Padding
        const float startGrid = minDelay - 10.0f * step;
        
        std::vector<float> density(numPoints, 0.0f);
        for (int i = 0; i < numPoints; ++i)
        {
            float x = startGrid + (float)i * step;
            float d = 0.0f;
            for (const auto& h : hits)
            {
                float dist = std::abs(x - h.delayMs);
                if (dist < bandwidth)
                {
                    // Triangle kernel weighted by energy
                    d += h.energy * (1.0f - dist / bandwidth);
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
                float dist = std::abs(hits[i].delayMs - result.clusters[c].centroidDelayMs);
                if (dist < bestDist)
                {
                    bestDist = dist;
                    bestCluster = (int)c;
                }
            }
            
            if (bestCluster >= 0 && bestDist < 0.6f) // Max assignment distance
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
            // Split by polarity if needed
            int numInverted = 0;
            float energyNormal = 0.0f;
            float energyInvert = 0.0f;
            for (int idx : c.memberIndices)
            {
                if (hits[(size_t)idx].polarityInvert) { numInverted++; energyInvert += hits[(size_t)idx].energy; }
                else { energyNormal += hits[(size_t)idx].energy; }
            }

            auto addSubCluster = [&](bool targetPolarity)
            {
                HitCluster sub;
                sub.centroidPolarity = targetPolarity;
                
                int numPhaseEnabled = 0;
                float totalFreq = 0.0f;
                float freqWeightSum = 0.0f;

                for (int idx : c.memberIndices)
                {
                    const auto& h = hits[(size_t)idx];
                    if (h.polarityInvert == targetPolarity)
                    {
                        sub.memberIndices.push_back(idx);
                        sub.totalEnergy += h.energy;
                        sub.averageSignalConfidence += h.signalConfidence;
                        sub.centroidDelayMs += h.delayMs * h.energy;
                        
                        if (h.phaseFilterEnabled)
                        {
                            numPhaseEnabled++;
                            totalFreq += h.phaseFilterFreqHz * h.energy;
                            freqWeightSum += h.energy;
                        }
                    }
                }
                
                if (!sub.memberIndices.empty())
                {
                    sub.averageSignalConfidence /= (float)sub.memberIndices.size();
                    sub.centroidDelayMs /= sub.totalEnergy; // Weighted average delay
                    
                    // Internal spread
                    for (int idx : sub.memberIndices)
                    {
                        float diff = hits[(size_t)idx].delayMs - sub.centroidDelayMs;
                        sub.internalSpread += diff * diff;
                    }
                    sub.internalSpread = std::sqrt(sub.internalSpread / (float)sub.memberIndices.size());

                    sub.centroidPhaseEnabled = (numPhaseEnabled > (int)sub.memberIndices.size() / 2);
                    if (freqWeightSum > 0.0f)
                        sub.centroidPhaseFreqHz = totalFreq / freqWeightSum;
                    else
                        sub.centroidPhaseFreqHz = 200.0f;
                        
                    finalClusters.push_back(sub);
                }
            };

            if (numInverted > 0) addSubCluster(true);
            if (numInverted < (int)c.memberIndices.size()) addSubCluster(false);
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
        float tightness = std::max(0.0f, 1.0f - dom.internalSpread / 1.0f);
        
        result.consensusConfidence = 0.35f * clusterShare + 
                                     0.25f * tightness + 
                                     0.25f * energyShare + 
                                     0.15f * dom.averageSignalConfidence;
                                     
        // Clamp it
        result.consensusConfidence = std::max(0.0f, std::min(1.0f, result.consensusConfidence));

        return result;
    }
};
