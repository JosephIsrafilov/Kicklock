#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

#include "DynamicFingerprintExtractor.h"

// Worker-only replay of the canonical raw Learn timeline. It deliberately uses
// the same capture bank as Phase 7, so the front end starts once at the Learn
// reset boundary and never cold-starts a filter for an individual hit window.
struct DynamicLearnFingerprintSample
{
    int sequence = -1;
    int64_t triggerSample = -1;
    DynamicRuntimeFingerprint fingerprint;
};

inline std::vector<DynamicLearnFingerprintSample> extractDynamicLearnFingerprints (
    const float* rawBass, const float* rawKick, int numSamples, double sampleRate,
    const std::vector<std::pair<int, int64_t>>& triggers)
{
    std::vector<DynamicLearnFingerprintSample> results;
    if (rawBass == nullptr || rawKick == nullptr || numSamples <= 0 || triggers.empty())
        return results;

    std::vector<std::pair<int, int64_t>> ordered = triggers;
    std::sort (ordered.begin(), ordered.end(), [] (const auto& a, const auto& b)
    {
        return a.second != b.second ? a.second < b.second : a.first < b.first;
    });

    DynamicFingerprintCaptureBank bank;
    bank.prepare (sampleRate);
    size_t nextTrigger = 0;
    for (int sample = 0; sample < numSamples; ++sample)
    {
        while (nextTrigger < ordered.size() && ordered[nextTrigger].second == sample)
        {
            if (bank.requestCapture (ordered[nextTrigger].second) == DynamicCaptureRequest::Accepted)
                ++nextTrigger;
            else
                break;
        }

        bank.pushSample (rawBass[sample], rawKick[sample]);
        DynamicFingerprintObservation observation;
        while (bank.takeCompleted (observation))
        {
            const auto it = std::lower_bound (ordered.begin(), ordered.end(), observation.triggerSample,
                                              [] (const auto& trigger, int64_t value)
                                              {
                                                  return trigger.second < value;
                                              });
            if (it != ordered.end() && it->second == observation.triggerSample)
                results.push_back ({ it->first, observation.triggerSample, observation.fingerprint });
        }
    }

    std::sort (results.begin(), results.end(), [] (const auto& a, const auto& b)
    {
        return a.sequence != b.sequence ? a.sequence < b.sequence : a.triggerSample < b.triggerSample;
    });
    return results;
}
