#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "DynamicFingerprintExtractor.h"
#include "DynamicStateMap.h"

// Worker-owned value contract. Fingerprint validity is intentionally separate
// from timing/correction eligibility: a recognizable State can be formed even
// when correction evidence is absent or unsuitable for production.
struct DynamicLearnHit
{
    int sequence = -1;
    int64_t triggerSample = -1;
    DynamicFingerprintPrototype fingerprint;
    DynamicFingerprintValidity fingerprintValidity = DynamicFingerprintValidity::Incomplete;
    bool timingEligible = false;
    float absoluteDelayMs = 0.0f;
    bool polarityInvert = false;
    float timingConfidence = 0.0f;
    float lowBandEnergy = 1.0f;
    bool correctionBeneficial = false;
    float allpassFreqHz = 0.0f;
    float allpassQ = 0.0f;
    bool hasLikelyMidi = false;
    int likelyMidi = 0;
    bool hasLikelyPitchHz = false;
    float likelyPitchHz = 0.0f;
};

struct DynamicLearnFormationContext
{
    double sampleRate = 48000.0;
    bool crossoverEnabled = true;
    float crossoverHz = 150.0f;
    bool allpassEnabled = true;
    float fallbackAllpassFreqHz = 100.0f;
    float fallbackAllpassQ = 0.7f;
    int allpassStages = 2;
    int delayInterpolationIndex = 0;
    DynamicStateMap previousMap = makeEmptyDynamicStateMap();
};

struct DynamicLearnFormationDiagnostics
{
    int overflowClusterCount = 0;
    int ambiguousHitCount = 0;
    bool cancelled = false;
};

struct DynamicLearnFormationResult
{
    bool valid = false;
    DynamicStateMap map = makeEmptyDynamicStateMap();
    DynamicLearnFormationDiagnostics diagnostics;
};

namespace DynamicLearnFormationDetail
{
    // Normalized v1 L1 constants. The seed radius leaves practical separation
    // between the repeated signed descriptors while the ambiguity margin keeps
    // an overlap from becoming an arbitrary state decision.
    inline constexpr float kSeedDistance = 0.075f;
    inline constexpr float kMergeDistance = 0.030f;
    inline constexpr float kAssignmentMargin = 0.0125f;
    inline constexpr float kIdentityReuseDistance = 0.050f;
    inline constexpr float kIdentityReuseMargin = 0.015f;
    inline constexpr float kMinimumMatchThreshold = 0.020f;
    inline constexpr float kMaximumMatchThreshold = 0.250f;

    inline bool cancelled (const std::function<bool()>& shouldCancel)
    {
        return shouldCancel && shouldCancel();
    }

    inline float clamp01 (float value) noexcept
    {
        return std::clamp (std::isfinite (value) ? value : 0.0f, 0.0f, 1.0f);
    }

    inline float median (std::vector<float> values)
    {
        if (values.empty())
            return 0.0f;
        std::sort (values.begin(), values.end());
        const size_t middle = values.size() / 2;
        return values.size() % 2 != 0 ? values[middle] : 0.5f * (values[middle - 1] + values[middle]);
    }

    inline bool fingerprintLess (const DynamicFingerprintPrototype& a,
                                 const DynamicFingerprintPrototype& b) noexcept
    {
        for (int i = 0; i < DynamicFingerprintContract::kFeatureCount; ++i)
            if (a.features[(size_t) i] != b.features[(size_t) i])
                return a.features[(size_t) i] < b.features[(size_t) i];
        return false;
    }

    inline DynamicFingerprintPrototype centerFor (const std::vector<int>& members,
                                                  const std::vector<DynamicLearnHit>& hits)
    {
        DynamicFingerprintPrototype center;
        center.valid = ! members.empty();
        center.featureCount = center.valid ? DynamicFingerprintContract::kFeatureCount : 0;
        for (int feature = 0; feature < DynamicFingerprintContract::kFeatureCount; ++feature)
        {
            std::vector<float> values;
            values.reserve (members.size());
            for (const int index : members)
                values.push_back (hits[(size_t) index].fingerprint.features[(size_t) feature]);
            center.features[(size_t) feature] = std::clamp (median (std::move (values)), -1.0f, 1.0f);
        }
        return center;
    }

    struct Cluster
    {
        std::vector<int> members;
        DynamicFingerprintPrototype prototype;
        float repeatability = 0.0f;
        float ambiguity = 0.0f;
        int64_t firstTrigger = std::numeric_limits<int64_t>::max();
    };

    inline void refreshCluster (Cluster& cluster, const std::vector<DynamicLearnHit>& hits)
    {
        cluster.prototype = centerFor (cluster.members, hits);
        cluster.firstTrigger = std::numeric_limits<int64_t>::max();
        std::vector<float> distances;
        std::vector<float> delays;
        for (const int index : cluster.members)
        {
            const auto& hit = hits[(size_t) index];
            cluster.firstTrigger = std::min (cluster.firstTrigger, hit.triggerSample);
            distances.push_back (dynamicFingerprintDistanceV1 (hit.fingerprint, cluster.prototype));
            if (hit.timingEligible && std::isfinite (hit.absoluteDelayMs))
                delays.push_back (hit.absoluteDelayMs);
        }
        const float fingerprintSpread = median (distances);
        float correctionSpread = 0.0f;
        if (delays.size() >= 3)
        {
            const float target = median (delays);
            for (auto& delay : delays)
                delay = std::abs (delay - target) / 3.0f;
            correctionSpread = median (delays);
        }
        cluster.repeatability = clamp01 (1.0f - std::min (1.0f, fingerprintSpread / kSeedDistance)
                                         - 0.25f * correctionSpread);
    }

    inline bool confidentAutoMatch (const Cluster& cluster, const DynamicStateMap& previous,
                                    uint64_t& matchedId) noexcept
    {
        float nearest = std::numeric_limits<float>::infinity();
        float second = std::numeric_limits<float>::infinity();
        uint64_t id = 0;
        for (const auto& state : previous.states)
        {
            if (! state.occupied || state.origin != DynamicStateOrigin::Auto)
                continue;
            const float distance = dynamicFingerprintDistanceV1 (cluster.prototype, state.fingerprint);
            if (distance < nearest || (distance == nearest && state.stableStateId < id))
            {
                second = nearest;
                nearest = distance;
                id = state.stableStateId;
            }
            else if (distance < second)
            {
                second = distance;
            }
        }
        if (id != 0 && nearest <= kIdentityReuseDistance
            && (! std::isfinite (second) || second - nearest >= kIdentityReuseMargin))
        {
            matchedId = id;
            return true;
        }
        return false;
    }
}

inline DynamicLearnFormationResult formDynamicStateMap (
    const std::vector<DynamicLearnHit>& input, const DynamicLearnFormationContext& context,
    const std::function<bool()>& shouldCancel = {})
{
    using namespace DynamicLearnFormationDetail;
    DynamicLearnFormationResult result;
    DynamicStateMap map = makeEmptyDynamicStateMap();
    map.globalBase.crossoverEnabled = context.crossoverEnabled;
    map.globalBase.crossoverHz = std::clamp (context.crossoverHz, DynamicStateMapContract::kCrossoverMinHz,
                                              DynamicStateMapContract::kCrossoverMaxHz);
    map.globalBase.allpassEnabled = context.allpassEnabled;
    map.globalBase.globalAllpassFreqHz = std::clamp (context.fallbackAllpassFreqHz,
                                                      DynamicStateMapContract::kAllpassFrequencyMinHz,
                                                      DynamicStateMapContract::kAllpassFrequencyMaxHz);
    map.globalBase.globalAllpassQ = std::clamp (context.fallbackAllpassQ,
                                                DynamicStateMapContract::kAllpassQMin,
                                                DynamicStateMapContract::kAllpassQMax);
    map.globalBase.allpassStages = std::clamp (context.allpassStages, 2, 4);
    map.globalBase.delayInterpolationIndex = context.delayInterpolationIndex == 1 ? 1 : 0;
    map.globalBase.learnedSampleRate = context.sampleRate;

    std::vector<DynamicLearnHit> hits;
    hits.reserve (input.size());
    map.diagnostics.analyzedHitCount = (uint32_t) input.size();
    for (const auto& hit : input)
    {
        if (cancelled (shouldCancel))
        {
            result.diagnostics.cancelled = true;
            return result;
        }
        if (hit.fingerprintValidity == DynamicFingerprintValidity::Valid
            && isRuntimeEligibleDynamicFingerprintV1 (hit.fingerprint, DynamicFingerprintContract::kExtractorVersion))
            hits.push_back (hit);
        else
            ++map.diagnostics.rejectedHitCount;
    }
    if (hits.size() < DynamicStateMapContract::kCandidateMinimumRepeatableHits)
    {
        map.diagnostics.diagnostic = DynamicMapDiagnostic::InsufficientMaterial;
        result.map = map;
        return result;
    }

    std::sort (hits.begin(), hits.end(), [] (const auto& a, const auto& b)
    {
        if (fingerprintLess (a.fingerprint, b.fingerprint)) return true;
        if (fingerprintLess (b.fingerprint, a.fingerprint)) return false;
        return a.triggerSample != b.triggerSample ? a.triggerSample < b.triggerSample : a.sequence < b.sequence;
    });

    std::vector<Cluster> clusters;
    for (int index = 0; index < (int) hits.size(); ++index)
    {
        if (cancelled (shouldCancel)) { result.diagnostics.cancelled = true; return result; }
        int nearest = -1;
        float nearestDistance = std::numeric_limits<float>::infinity();
        for (int cluster = 0; cluster < (int) clusters.size(); ++cluster)
        {
            const float distance = dynamicFingerprintDistanceV1 (hits[(size_t) index].fingerprint,
                                                                  clusters[(size_t) cluster].prototype);
            if (distance < nearestDistance)
            {
                nearestDistance = distance;
                nearest = cluster;
            }
        }
        if (nearest < 0 || nearestDistance > kSeedDistance)
        {
            Cluster cluster;
            cluster.members.push_back (index);
            cluster.prototype = hits[(size_t) index].fingerprint;
            clusters.push_back (cluster);
        }
        else
        {
            clusters[(size_t) nearest].members.push_back (index);
            clusters[(size_t) nearest].prototype = centerFor (clusters[(size_t) nearest].members, hits);
        }
    }

    for (int iteration = 0; iteration < 4; ++iteration)
    {
        std::vector<std::vector<int>> assigned (clusters.size());
        for (int index = 0; index < (int) hits.size(); ++index)
        {
            int nearest = -1;
            float first = std::numeric_limits<float>::infinity();
            float second = std::numeric_limits<float>::infinity();
            for (int cluster = 0; cluster < (int) clusters.size(); ++cluster)
            {
                const float distance = dynamicFingerprintDistanceV1 (hits[(size_t) index].fingerprint,
                                                                      clusters[(size_t) cluster].prototype);
                if (distance < first) { second = first; first = distance; nearest = cluster; }
                else if (distance < second) second = distance;
            }
            if (nearest >= 0 && first <= kSeedDistance
                && (! std::isfinite (second) || second - first >= kAssignmentMargin))
                assigned[(size_t) nearest].push_back (index);
            else
                ++result.diagnostics.ambiguousHitCount;
        }
        bool changed = false;
        for (int cluster = 0; cluster < (int) clusters.size(); ++cluster)
        {
            changed |= assigned[(size_t) cluster] != clusters[(size_t) cluster].members;
            clusters[(size_t) cluster].members = std::move (assigned[(size_t) cluster]);
            if (! clusters[(size_t) cluster].members.empty())
                refreshCluster (clusters[(size_t) cluster], hits);
        }
        if (! changed)
            break;
    }

    clusters.erase (std::remove_if (clusters.begin(), clusters.end(), [] (const Cluster& cluster)
    {
        return cluster.members.size() < DynamicStateMapContract::kCandidateMinimumRepeatableHits;
    }), clusters.end());
    if (clusters.empty())
    {
        map.diagnostics.diagnostic = DynamicMapDiagnostic::NonRepeatablePhaseRelationship;
        result.map = map;
        return result;
    }

    // Merge indistinguishable centers before calibration. This prevents a pair
    // that cannot satisfy the matcher margin from being published as two states.
    bool merged = true;
    while (merged)
    {
        merged = false;
        for (size_t a = 0; a < clusters.size() && ! merged; ++a)
            for (size_t b = a + 1; b < clusters.size(); ++b)
                if (dynamicFingerprintDistanceV1 (clusters[a].prototype, clusters[b].prototype) <= kMergeDistance)
                {
                    clusters[a].members.insert (clusters[a].members.end(), clusters[b].members.begin(), clusters[b].members.end());
                    refreshCluster (clusters[a], hits);
                    clusters.erase (clusters.begin() + (ptrdiff_t) b);
                    merged = true;
                    break;
                }
    }
    for (auto& cluster : clusters)
        refreshCluster (cluster, hits);

    for (auto& cluster : clusters)
    {
        float nearest = std::numeric_limits<float>::infinity();
        for (const auto& other : clusters)
            if (&cluster != &other)
                nearest = std::min (nearest, dynamicFingerprintDistanceV1 (cluster.prototype, other.prototype));
        cluster.ambiguity = std::isfinite (nearest) ? clamp01 (1.0f - nearest / kSeedDistance) : 0.0f;
    }

    std::sort (clusters.begin(), clusters.end(), [] (const Cluster& a, const Cluster& b)
    {
        if (a.members.size() != b.members.size()) return a.members.size() > b.members.size();
        if (a.repeatability != b.repeatability) return a.repeatability > b.repeatability;
        if (a.firstTrigger != b.firstTrigger) return a.firstTrigger < b.firstTrigger;
        return fingerprintLess (a.prototype, b.prototype);
    });

    uint64_t nextId = context.previousMap.nextStateId == 0 ? 1 : context.previousMap.nextStateId;
    for (const auto& state : context.previousMap.states)
    {
        if (state.occupied && state.origin == DynamicStateOrigin::Manual)
            map.states[(size_t) (&state - context.previousMap.states.data())] = state;
        if (state.occupied && state.stableStateId >= nextId && state.stableStateId < std::numeric_limits<uint64_t>::max())
            nextId = state.stableStateId + 1;
    }

    std::vector<int> freeSlots;
    for (int slot = 0; slot < DynamicStateMapContract::kMaxPersistentStates; ++slot)
        if (! map.states[(size_t) slot].occupied)
            freeSlots.push_back (slot);

    const size_t retainedCount = std::min (clusters.size(), freeSlots.size());
    result.diagnostics.overflowClusterCount = (int) clusters.size() - (int) retainedCount;
    std::vector<uint64_t> assignedAutoIds;
    std::vector<float> delayTargets;
    std::vector<float> delayWeights;
    int polarityWeight = 0;
    int nonPolarityWeight = 0;
    for (size_t clusterIndex = 0; clusterIndex < retainedCount; ++clusterIndex)
        for (const int member : clusters[clusterIndex].members)
        {
            const auto& hit = hits[(size_t) member];
            if (! hit.timingEligible || ! std::isfinite (hit.absoluteDelayMs)) continue;
            delayTargets.push_back (hit.absoluteDelayMs);
            delayWeights.push_back (std::max (0.01f, hit.lowBandEnergy));
            if (hit.polarityInvert) ++polarityWeight; else ++nonPolarityWeight;
        }
    map.globalBase.polarityInvert = polarityWeight > nonPolarityWeight;

    std::vector<float> baseCandidates { DynamicStateMapContract::kGlobalBaseDelayMinMs,
                                        DynamicStateMapContract::kGlobalBaseDelayMaxMs };
    for (const float target : delayTargets)
    {
        baseCandidates.push_back (std::clamp (target, DynamicStateMapContract::kGlobalBaseDelayMinMs,
                                              DynamicStateMapContract::kGlobalBaseDelayMaxMs));
        baseCandidates.push_back (std::clamp (target - 3.0f, DynamicStateMapContract::kGlobalBaseDelayMinMs,
                                              DynamicStateMapContract::kGlobalBaseDelayMaxMs));
        baseCandidates.push_back (std::clamp (target + 3.0f, DynamicStateMapContract::kGlobalBaseDelayMinMs,
                                              DynamicStateMapContract::kGlobalBaseDelayMaxMs));
    }
    baseCandidates.push_back (std::clamp (median (delayTargets), DynamicStateMapContract::kGlobalBaseDelayMinMs,
                                          DynamicStateMapContract::kGlobalBaseDelayMaxMs));
    std::sort (baseCandidates.begin(), baseCandidates.end());
    baseCandidates.erase (std::unique (baseCandidates.begin(), baseCandidates.end()), baseCandidates.end());
    float chosenBase = 0.0f;
    float bestCoverage = -1.0f;
    float bestResidual = std::numeric_limits<float>::infinity();
    for (const float candidate : baseCandidates)
    {
        float coverage = 0.0f, residual = 0.0f;
        for (size_t index = 0; index < delayTargets.size(); ++index)
        {
            const float weight = delayWeights[index];
            const float delta = std::abs (delayTargets[index] - candidate);
            if (delta <= 3.0f + 1.0e-5f) coverage += weight;
            residual += weight * delta;
        }
        if (coverage > bestCoverage || (coverage == bestCoverage && residual < bestResidual)
            || (coverage == bestCoverage && residual == bestResidual
                && (std::abs (candidate) < std::abs (chosenBase)
                    || (std::abs (candidate) == std::abs (chosenBase) && candidate < chosenBase))))
        {
            chosenBase = candidate; bestCoverage = coverage; bestResidual = residual;
        }
    }
    map.globalBase.globalBaseDelayMs = chosenBase;

    for (size_t clusterIndex = 0; clusterIndex < retainedCount; ++clusterIndex)
    {
        const auto& cluster = clusters[clusterIndex];
        DynamicState state;
        state.occupied = true;
        state.fingerprint = cluster.prototype;
        state.origin = DynamicStateOrigin::Auto;
        state.hitCount = (uint32_t) cluster.members.size();
        state.evidence = state.hitCount >= DynamicStateMapContract::kStableAutoMinimumRepeatableHits
            ? DynamicStateEvidence::Stable : DynamicStateEvidence::Candidate;
        state.repeatability = cluster.repeatability;
        state.ambiguity = cluster.ambiguity;

        uint64_t reused = 0;
        const bool reusedExisting = confidentAutoMatch (cluster, context.previousMap, reused)
            && std::find (assignedAutoIds.begin(), assignedAutoIds.end(), reused) == assignedAutoIds.end();
        if (! reusedExisting)
        {
            if (nextId == 0 || nextId == std::numeric_limits<uint64_t>::max())
                continue;
            reused = nextId++;
        }
        state.stableStateId = reused;
        assignedAutoIds.push_back (reused);

        std::vector<float> delays, frequencies, qs;
        int matchingPolarity = 0;
        int midi = -1, midiCount = 0;
        std::vector<float> pitches;
        for (const int member : cluster.members)
        {
            const auto& hit = hits[(size_t) member];
            if (hit.timingEligible && hit.correctionBeneficial && std::isfinite (hit.absoluteDelayMs)
                && hit.polarityInvert == map.globalBase.polarityInvert
                && DynamicStateMapContract::isInRange (hit.allpassFreqHz, DynamicStateMapContract::kAllpassFrequencyMinHz,
                                                        DynamicStateMapContract::kAllpassFrequencyMaxHz)
                && DynamicStateMapContract::isInRange (hit.allpassQ, DynamicStateMapContract::kAllpassQMin,
                                                        DynamicStateMapContract::kAllpassQMax))
            {
                delays.push_back (hit.absoluteDelayMs);
                frequencies.push_back (hit.allpassFreqHz);
                qs.push_back (hit.allpassQ);
                ++matchingPolarity;
            }
            if (hit.hasLikelyPitchHz && std::isfinite (hit.likelyPitchHz) && hit.likelyPitchHz > 0.0f)
                pitches.push_back (hit.likelyPitchHz);
        }
        for (const int candidateMember : cluster.members)
        {
            const auto& candidate = hits[(size_t) candidateMember];
            if (! candidate.hasLikelyMidi) continue;
            int count = 0;
            for (const int otherMember : cluster.members)
                if (hits[(size_t) otherMember].hasLikelyMidi
                    && hits[(size_t) otherMember].likelyMidi == candidate.likelyMidi) ++count;
            if (count > midiCount || (count == midiCount && candidate.likelyMidi < midi))
            { midi = candidate.likelyMidi; midiCount = count; }
        }
        state.hasLikelyMidi = midiCount * 2 > (int) cluster.members.size();
        state.likelyMidi = state.hasLikelyMidi ? midi : 0;
        state.hasLikelyPitchHz = ! pitches.empty();
        state.likelyPitchHz = state.hasLikelyPitchHz ? median (pitches) : 0.0f;

        const float target = median (delays);
        const float delta = target - map.globalBase.globalBaseDelayMs;
        if (matchingPolarity >= 3 && std::isfinite (target)
            && DynamicStateMapContract::isInRange (delta, DynamicStateMapContract::kStateDelayDeltaMinMs,
                                                    DynamicStateMapContract::kStateDelayDeltaMaxMs))
        {
            state.learnedPackage = { delta, median (frequencies), median (qs) };
            state.hasLearnedPackage = isValidDynamicZonePackage (state.learnedPackage);
        }
        if (! state.hasLearnedPackage)
            state.manualTrim = makeZeroDynamicManualTrim();
        map.states[(size_t) freeSlots[clusterIndex]] = state;
    }

    map.nextStateId = nextId == 0 ? 1 : nextId;
    int occupied = getOccupiedDynamicStateCount (map);
    map.diagnostics.repeatableClusterCount = (uint32_t) occupied;
    map.diagnostics.unstableHitCount = (uint32_t) result.diagnostics.ambiguousHitCount;
    bool anyPackage = false;
    for (const auto& state : map.states)
        anyPackage |= state.occupied && state.hasLearnedPackage;
    map.diagnostics.diagnostic = anyPackage ? DynamicMapDiagnostic::None : DynamicMapDiagnostic::NoConfidentAutoFix;

    float maxWithin = 0.0f;
    float nearestBetween = std::numeric_limits<float>::infinity();
    for (size_t cluster = 0; cluster < retainedCount; ++cluster)
    {
        for (const int member : clusters[cluster].members)
            maxWithin = std::max (maxWithin, dynamicFingerprintDistanceV1 (
                hits[(size_t) member].fingerprint, clusters[cluster].prototype));
        for (size_t other = cluster + 1; other < retainedCount; ++other)
            nearestBetween = std::min (nearestBetween, dynamicFingerprintDistanceV1 (
                clusters[cluster].prototype, clusters[other].prototype));
    }
    const float separationLimit = std::isfinite (nearestBetween) ? nearestBetween * 0.45f : kMaximumMatchThreshold;
    const float thresholdUpper = std::max (kMinimumMatchThreshold,
                                           std::min (kMaximumMatchThreshold, separationLimit));
    map.calibration.absoluteDistanceThreshold = std::clamp (std::max (kMinimumMatchThreshold, maxWithin + 0.010f),
                                                            kMinimumMatchThreshold,
                                                            thresholdUpper);
    map.calibration.ambiguityMargin = std::min (map.calibration.absoluteDistanceThreshold,
                                                std::max (0.005f, std::isfinite (nearestBetween)
                                                    ? nearestBetween * 0.20f : 0.010f));
    map.calibration.valid = map.calibration.absoluteDistanceThreshold > 0.0f
        && map.calibration.ambiguityMargin > 0.0f;
    // Structural validation intentionally requires the persisted valid bit, so
    // set it before the final self-check rather than creating a circular false.
    map.valid = occupied > 0;
    map.valid = map.valid && isStructurallyValidDynamicStateMap (map);
    result.valid = map.valid;
    result.map = map;
    return result;
}
