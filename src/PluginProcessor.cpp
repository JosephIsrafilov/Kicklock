#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "dsp/HitConsensus.h"
#include "dsp/MultiBandCorrelation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace
{
    struct AnalysisHitWindow
    {
        int start = 0;
        int length = 0;
        int peak = 0;
    };

    float medianOf (std::vector<float> values)
    {
        if (values.empty())
            return 0.0f;

        std::sort (values.begin(), values.end());
        const auto mid = values.size() / 2;
        return values.size() % 2 == 0
            ? 0.5f * (values[mid - 1] + values[mid])
            : values[mid];
    }

    float averageOf (const std::vector<float>& values)
    {
        if (values.empty())
            return 0.0f;

        return std::accumulate (values.begin(), values.end(), 0.0f) / (float) values.size();
    }

    float standardDeviationOf (const std::vector<float>& values, float mean)
    {
        if (values.size() < 2)
            return 0.0f;

        float accum = 0.0f;
        for (auto value : values)
        {
            const float delta = value - mean;
            accum += delta * delta;
        }

        return std::sqrt (accum / (float) values.size());
    }

    InterpolationType interpolationFromChoice (float choiceValue) noexcept
    {
        return (int) std::lround (choiceValue) == 0 ? InterpolationType::Linear
                                                    : InterpolationType::Allpass;
    }

    float rmsOf (const std::vector<float>& x) noexcept
    {
        if (x.empty())
            return 0.0f;

        double sum = 0.0;
        for (auto v : x)
            sum += (double) v * (double) v;

        return (float) std::sqrt (sum / (double) x.size());
    }

    // P6: when analysis can't produce a usable fix, say specifically what's
    // missing rather than a generic "waiting for signal". Inspects the captured
    // window so the message names the actual absent input (no kick / no bass /
    // not enough material) instead of guessing.
    void refineInsufficientSignalMessage (PhaseFixResult& result,
                                          const std::vector<float>& bass,
                                          const std::vector<float>& kick,
                                          int numSamples,
                                          double sampleRate)
    {
        if (result.enoughSignal)
            return;

        const int minMaterial = (int) (sampleRate * 0.3);
        if (numSamples < minMaterial)
        {
            result.message = "Not enough material captured yet. Keep the loop playing, then analyze again.";
            return;
        }

        constexpr float presenceFloor = 1.5e-3f;
        const bool hasKick = rmsOf (kick) >= presenceFloor;
        const bool hasBass = rmsOf (bass) >= presenceFloor;

        if (! hasKick && ! hasBass)
            result.message = "No kick or bass detected. Feed a kick into the sidechain and bass into the main input.";
        else if (! hasKick)
            result.message = "No kick detected on the sidechain. Route the kick to the sidechain input.";
        else if (! hasBass)
            result.message = "No bass detected on the main input. Route the bass through the plugin.";
        else
            result.message = "Kick and bass are present but too quiet for a reliable low-end phase read.";
    }

    std::vector<AnalysisHitWindow> extractRecentHitWindows (const std::vector<float>& kick,
                                                            double sampleRate,
                                                            int maxHits = 8)
    {
        std::vector<AnalysisHitWindow> hits;
        if (kick.size() < 128 || sampleRate <= 0.0)
            return hits;

        float peak = 0.0f;
        for (auto sample : kick)
            peak = std::max (peak, std::abs (sample));

        if (peak < 1.0e-4f)
            return hits;

        const auto timeMsToCoeff = [sampleRate] (float ms) noexcept
        {
            const double samples = std::max (1.0, sampleRate * (double) ms / 1000.0);
            return (float) std::exp (-1.0 / samples);
        };

        const float peakEnergy = peak * peak;
        const float threshold = std::max (peakEnergy * 0.08f, 1.0e-8f);
        const float minimumEnergyGate = std::max (peakEnergy * 0.02f, 1.0e-10f);
        const float attackCoeff = timeMsToCoeff (2.0f);
        const float releaseCoeff = timeMsToCoeff (60.0f);
        const int holdoffSamples = juce::jmax (1, (int) std::lround (sampleRate * 0.140));
        const int peakSearch = juce::jmax (1, (int) std::lround (sampleRate * 0.008));
        const int preSamples = juce::jmax (1, (int) std::lround (sampleRate * 0.006));
        const int postSamples = juce::jmax (64, (int) std::lround (sampleRate * 0.110));

        float envelope = 0.0f;
        bool wasAbove = false;
        int holdoffRemaining = 0;

        for (int i = 0; i < (int) kick.size(); ++i)
        {
            const float energy = kick[(size_t) i] * kick[(size_t) i];
            const float coeff = energy > envelope ? attackCoeff : releaseCoeff;
            envelope = coeff * envelope + (1.0f - coeff) * energy;

            if (holdoffRemaining > 0)
                --holdoffRemaining;

            const bool above = envelope >= threshold && envelope >= minimumEnergyGate;
            const bool detected = above && ! wasAbove && holdoffRemaining <= 0;
            wasAbove = above;

            if (! detected)
                continue;

            holdoffRemaining = holdoffSamples;

            int localPeak = i;
            float localEnergy = energy;
            const int searchEnd = juce::jmin ((int) kick.size(), i + peakSearch);
            for (int j = i + 1; j < searchEnd; ++j)
            {
                const float e = kick[(size_t) j] * kick[(size_t) j];
                if (e > localEnergy)
                {
                    localEnergy = e;
                    localPeak = j;
                }
            }

            AnalysisHitWindow hit;
            hit.peak = localPeak;
            hit.start = juce::jlimit (0, (int) kick.size() - 1, localPeak - preSamples);
            hit.length = std::min ((int) kick.size() - hit.start, preSamples + postSamples);

            if (hit.length > 96 && localEnergy >= minimumEnergyGate)
                hits.push_back (hit);
        }

        if ((int) hits.size() > maxHits)
            hits.erase (hits.begin(), hits.end() - maxHits);

        return hits;
    }

    void appendHitWindows (const std::vector<float>& bass,
                           const std::vector<float>& kick,
                           const std::vector<AnalysisHitWindow>& hits,
                           std::vector<float>& bassOut,
                           std::vector<float>& kickOut)
    {
        bassOut.clear();
        kickOut.clear();

        size_t totalSamples = 0;
        for (const auto& hit : hits)
            if (hit.start >= 0 && hit.length > 0
                && hit.start + hit.length <= (int) bass.size()
                && hit.start + hit.length <= (int) kick.size())
            {
                totalSamples += (size_t) hit.length;
            }

        bassOut.reserve (totalSamples);
        kickOut.reserve (totalSamples);

        for (const auto& hit : hits)
        {
            if (hit.start < 0 || hit.length <= 0
                || hit.start + hit.length > (int) bass.size()
                || hit.start + hit.length > (int) kick.size())
            {
                continue;
            }

            bassOut.insert (bassOut.end(),
                            bass.begin() + hit.start,
                            bass.begin() + hit.start + hit.length);
            kickOut.insert (kickOut.end(),
                            kick.begin() + hit.start,
                            kick.begin() + hit.start + hit.length);
        }
    }

    PhaseFixResult analyzeAggregatedHits (const std::vector<float>& bass,
                                          const std::vector<float>& kick,
                                          const std::vector<AnalysisHitWindow>& hits,
                                          double sampleRate,
                                          InterpolationType delayInterpolation)
    {
        PhaseFixResult aggregated;

        if (bass.empty() || kick.empty())
        {
            PhaseFixEngine::updateDerivedResultFields (aggregated);
            return aggregated;
        }

        if (hits.empty())
        {
            aggregated = PhaseFixEngine::analyze (bass.data(), kick.data(), (int) bass.size(),
                                                  sampleRate, PhaseFixEngine::defaultAutoFixMaxDelayMs,
                                                  delayInterpolation);
            aggregated.contributingHits = aggregated.enoughSignal ? 1 : 0;
            aggregated.singleHitAnalysis = true;
            aggregated.confidence *= 0.85f;
            PhaseFixEngine::updateDerivedResultFields (aggregated);
            if (aggregated.enoughSignal && ! aggregated.message.containsIgnoreCase ("single-hit"))
                aggregated.message << " Single-hit analysis.";
            return aggregated;
        }

        std::vector<PhaseFixResult> perHitResults;
        std::vector<HitObservation> hitObservations;
        perHitResults.reserve (hits.size());
        hitObservations.reserve (hits.size());
        
        std::vector<float> allHitBass;
        std::vector<float> allHitKick;
        appendHitWindows (bass, kick, hits, allHitBass, allHitKick);

        for (int i = 0; i < (int)hits.size(); ++i)
        {
            const auto& hit = hits[(size_t)i];
            if (hit.start < 0 || hit.length <= 0
                || hit.start + hit.length > (int) bass.size()
                || hit.start + hit.length > (int) kick.size())
            {
                continue;
            }

            auto hitResult = PhaseFixEngine::analyze (bass.data() + hit.start,
                                                      kick.data() + hit.start,
                                                      hit.length,
                                                      sampleRate,
                                                      PhaseFixEngine::defaultAutoFixMaxDelayMs,
                                                      delayInterpolation);
                                                      
            if (hitResult.enoughSignal)
            {
                perHitResults.push_back (hitResult);
                
                auto multiBandResult = MultiBandCorrelation::analyze (bass.data() + hit.start,
                                                                      kick.data() + hit.start,
                                                                      hit.length,
                                                                      sampleRate);
                                                                      
                HitObservation obs;
                obs.hitIndex = i;
                obs.delayMs = hitResult.bassDelayMs;
                obs.polarityInvert = hitResult.bassPolarityInvert;
                obs.phaseFilterFreqHz = hitResult.phaseFilterFreqHz;
                obs.phaseFilterEnabled = hitResult.phaseFilterEnabled;
                obs.matchPercent = hitResult.afterMatchPercent;
                obs.signalConfidence = hitResult.confidence;
                
                // Weight energy toward SUB and LOW bands
                obs.energy = PhaseBands::table[0].weight * multiBandResult.bands[0].kickEnergy
                           + PhaseBands::table[1].weight * multiBandResult.bands[1].kickEnergy
                           + PhaseBands::table[2].weight * multiBandResult.bands[2].kickEnergy
                           + PhaseBands::table[3].weight * multiBandResult.bands[3].kickEnergy;
                           
                hitObservations.push_back (obs);
            }
        }

        if (perHitResults.empty())
        {
            const auto* fallbackBass = allHitBass.empty() ? bass.data() : allHitBass.data();
            const auto* fallbackKick = allHitKick.empty() ? kick.data() : allHitKick.data();
            const int fallbackSamples = allHitBass.empty() ? (int) bass.size() : (int) allHitBass.size();

            aggregated = PhaseFixEngine::analyze (fallbackBass, fallbackKick, fallbackSamples,
                                                  sampleRate, PhaseFixEngine::defaultAutoFixMaxDelayMs,
                                                  delayInterpolation);
            aggregated.contributingHits = 0;
            PhaseFixEngine::updateDerivedResultFields (aggregated);
            return aggregated;
        }

        // --- NEW CLUSTERING CONSENSUS ---
        auto consensus = HitConsensus::analyze(hitObservations);
        
        aggregated.valid = true;
        aggregated.enoughSignal = true;
        aggregated.contributingHits = (int) perHitResults.size();
        aggregated.singleHitAnalysis = perHitResults.size() == 1;

        if (consensus.hasConsensus)
        {
            const auto& domCluster = consensus.clusters[(size_t)consensus.dominantClusterIndex];
            aggregated.unstableRecommendation =
                consensus.clusters.size() > 1
                || domCluster.memberIndices.size() < perHitResults.size()
                || ! consensus.outlierIndices.empty();

            aggregated.bassPolarityInvert = domCluster.centroidPolarity;
            aggregated.bassDelayMs = domCluster.centroidDelayMs;
            aggregated.phaseFilterEnabled = domCluster.centroidPhaseEnabled;
            aggregated.phaseFilterFreqHz = domCluster.centroidPhaseEnabled ? domCluster.centroidPhaseFreqHz : 200.0f;
            aggregated.phaseFilterQ = 0.7f;
            aggregated.phaseFilterStages = 2; // Keep simple for now
            aggregated.confidence = consensus.consensusConfidence;
        }
        else
        {
            aggregated.unstableRecommendation = perHitResults.size() > 1;

            // Priority 2: Highest energy single hit
            int bestIdx = 0;
            float maxE = -1.0f;
            for (size_t i = 0; i < hitObservations.size(); ++i)
            {
                if (hitObservations[i].energy > maxE)
                {
                    maxE = hitObservations[i].energy;
                    bestIdx = (int)i;
                }
            }
            aggregated.bassPolarityInvert = hitObservations[(size_t)bestIdx].polarityInvert;
            aggregated.bassDelayMs = hitObservations[(size_t)bestIdx].delayMs;
            aggregated.phaseFilterEnabled = hitObservations[(size_t)bestIdx].phaseFilterEnabled;
            aggregated.phaseFilterFreqHz = hitObservations[(size_t)bestIdx].phaseFilterFreqHz;
            aggregated.phaseFilterQ = 0.7f;
            aggregated.phaseFilterStages = 2;
            aggregated.confidence = hitObservations[(size_t)bestIdx].signalConfidence * 0.4f; // Penalized fallback
        }

        // Clamp the delay to the fixed-PDC budget, but keep track if we exceeded it.
        if (std::abs (aggregated.bassDelayMs) > PhaseFixEngine::defaultAutoFixMaxDelayMs + 0.25f)
        {
            aggregated.largeTimingOffset = true;
            aggregated.detectedTimingOffsetMs = std::abs (aggregated.bassDelayMs);
            aggregated.bassDelayMs = juce::jlimit (-PhaseFixEngine::defaultAutoFixMaxDelayMs,
                                                   PhaseFixEngine::defaultAutoFixMaxDelayMs,
                                                   aggregated.bassDelayMs);
        }

        PhaseFixRenderSettings settings;
        settings.bassPolarityInvert = aggregated.bassPolarityInvert;
        settings.bassDelayMs = aggregated.bassDelayMs;
        settings.phaseFilterEnabled = aggregated.phaseFilterEnabled;
        settings.phaseFilterFreqHz = aggregated.phaseFilterFreqHz;
        settings.phaseFilterQ = aggregated.phaseFilterQ;
        settings.phaseFilterStages = aggregated.phaseFilterStages;
        settings.delayInterpolation = delayInterpolation;

        float sumBeforeMatch = 0.0f;
        float sumAfterMatch = 0.0f;
        int numScores = 0;

        for (const auto& hit : hits)
        {
            if (hit.start < 0 || hit.length <= 0
                || hit.start + hit.length > (int) bass.size()
                || hit.start + hit.length > (int) kick.size())
            {
                continue;
            }

            const auto before = PhaseFixEngine::scoreSettings (bass.data() + hit.start,
                                                               kick.data() + hit.start,
                                                               hit.length,
                                                               sampleRate,
                                                               {},
                                                               PhaseFixEngine::absoluteManualMaxDelayMs);
            const auto after = PhaseFixEngine::scoreSettings (bass.data() + hit.start,
                                                              kick.data() + hit.start,
                                                              hit.length,
                                                              sampleRate,
                                                              settings,
                                                              PhaseFixEngine::absoluteManualMaxDelayMs);

            sumBeforeMatch += before.matchPercent;
            sumAfterMatch += after.matchPercent;
            numScores++;
        }

        if (numScores > 0)
        {
            aggregated.beforeMatchPercent = sumBeforeMatch / (float)numScores;
            aggregated.afterMatchPercent = sumAfterMatch / (float)numScores;
        }
        else if (!bass.empty() && !kick.empty())
        {
            const auto before = PhaseFixEngine::scoreSettings (bass.data(), kick.data(), (int)bass.size(),
                                                               sampleRate, {}, PhaseFixEngine::absoluteManualMaxDelayMs);
            const auto after = PhaseFixEngine::scoreSettings (bass.data(), kick.data(), (int)bass.size(),
                                                              sampleRate, settings, PhaseFixEngine::absoluteManualMaxDelayMs);
            aggregated.beforeMatchPercent = before.matchPercent;
            aggregated.afterMatchPercent = after.matchPercent;
        }
        else
        {
            aggregated.beforeMatchPercent = 50.0f;
            aggregated.afterMatchPercent = 50.0f;
        }

        aggregated.predictedAfterMatchPercent = aggregated.afterMatchPercent;
        aggregated.improvementPercent = aggregated.afterMatchPercent - aggregated.beforeMatchPercent;

        PhaseFixEngine::updateDerivedResultFields (aggregated);

        if (aggregated.singleHitAnalysis && aggregated.enoughSignal
            && ! aggregated.message.containsIgnoreCase ("single-hit"))
        {
            aggregated.message << " Single-hit analysis.";
        }

        return aggregated;
    }
}

class KickLockAudioProcessor::PhaseAlignmentEngine
{
public:
    void prepare (double newSampleRate, int samplesPerBlock, int newBaseDelaySamples)
    {
        sampleRate = newSampleRate;
        baseDelaySamples = juce::jmax (0, newBaseDelaySamples);

        const auto maxDelay = juce::jmax (1, baseDelaySamples * 2 + 4);
        delayLine.setMaximumDelayInSamples (maxDelay);
        delayLine.prepare ({ sampleRate, (juce::uint32) juce::jmax (1, samplesPerBlock), 2 });
        delayLine.reset();

        smoothedDelay.reset (sampleRate, 0.020);
        smoothedDelay.setCurrentAndTargetValue ((float) baseDelaySamples);

        smoothedAllpassFreq.reset (sampleRate, 0.030);
        smoothedAllpassFreq.setCurrentAndTargetValue (50.0f);

        // Q is smoothed with the same 30 ms ramp as frequency so that a change
        // to the rotator resonance recomputes coefficients gradually instead of
        // snapping (the same reason frequency is smoothed).
        smoothedAllpassQ.reset (sampleRate, 0.030);
        smoothedAllpassQ.setCurrentAndTargetValue (defaultAllpassQ);

        allpassWet.reset (sampleRate, 0.010);
        allpassWet.setCurrentAndTargetValue (0.0f);

        activeStages = 2;

        initialiseFilters();
        reset();
    }

    void reset()
    {
        delayLine.reset();

        for (auto& channel : allpassFilters)
            for (auto& stage : channel)
                stage.reset();

        smoothedDelay.setCurrentAndTargetValue ((float) baseDelaySamples);
        smoothedAllpassFreq.setCurrentAndTargetValue (50.0f);
        smoothedAllpassQ.setCurrentAndTargetValue (defaultAllpassQ);
        allpassWet.setCurrentAndTargetValue (0.0f);
        lastAppliedAllpassFreq = -1.0f;
        lastAppliedAllpassQ = -1.0f;
    }

    void process (juce::AudioBuffer<float>& mainBuffer,
                  float delayMs,
                  bool polarityInvert,
                  float allpassFreqHz,
                  bool allpassEnabled,
                  float allpassQ,
                  int allpassStages)
    {
        const auto numChannels = juce::jmin (2, mainBuffer.getNumChannels());
        const auto numSamples = mainBuffer.getNumSamples();

        // Match AllpassPhaseRotator's clamp so the audio path and the offline
        // analyzer/verification agree on how many stages actually run.
        const int clampedStages = juce::jlimit (2, 4, allpassStages);
        if (clampedStages != activeStages)
        {
            // A stage-count change alters the cascade topology; the trailing
            // stages' z-state is stale for the new routing, so clear it to avoid
            // a click. Coefficients are re-pushed below via the epsilon guard.
            activeStages = clampedStages;
            for (auto& channel : allpassFilters)
                for (auto& stage : channel)
                    stage.reset();
            lastAppliedAllpassFreq = -1.0f;
            lastAppliedAllpassQ = -1.0f;
        }

        smoothedDelay.setTargetValue (delaySamplesForUserDelayMs (delayMs));
        smoothedAllpassFreq.setTargetValue (juce::jlimit (20.0f, 500.0f, allpassFreqHz));
        smoothedAllpassQ.setTargetValue (juce::jlimit (minAllpassQ, maxAllpassQ, allpassQ));
        allpassWet.setTargetValue (allpassEnabled ? 1.0f : 0.0f);

        for (int i = 0; i < numSamples; ++i)
        {
            const auto delaySamples = juce::jlimit (0.0f,
                                                   (float) delayLine.getMaximumDelayInSamples(),
                                                   smoothedDelay.getNextValue());
            const auto wet = allpassWet.getNextValue();
            const bool runAllpass = wet > 1.0e-5f || allpassWet.isSmoothing();

            if (runAllpass)
                updateAllpassCoefficients (smoothedAllpassFreq.getNextValue(),
                                           smoothedAllpassQ.getNextValue());

            for (int ch = 0; ch < numChannels; ++ch)
            {
                delayLine.pushSample (ch, mainBuffer.getSample (ch, i));
                auto sample = delayLine.popSample (ch, delaySamples);

                if (polarityInvert)
                    sample = -sample;

                if (runAllpass)
                {
                    const auto dry = sample;
                    auto rotated = sample;
                    for (int s = 0; s < activeStages; ++s)
                        rotated = allpassFilters[(size_t) ch][(size_t) s].processSample (rotated);
                    sample = dry + wet * (rotated - dry);
                }

                mainBuffer.setSample (ch, i, sample);
            }

            for (int ch = numChannels; ch < 2; ++ch)
            {
                delayLine.pushSample (ch, 0.0f);
                (void) delayLine.popSample (ch, delaySamples);
            }
        }
    }

    void processBypassed (juce::AudioBuffer<float>& mainBuffer)
    {
        const auto numChannels = juce::jmin (2, mainBuffer.getNumChannels());
        const auto numSamples = mainBuffer.getNumSamples();
        const auto fixedDelay = (float) baseDelaySamples;

        for (int i = 0; i < numSamples; ++i)
        {
            for (int ch = 0; ch < numChannels; ++ch)
            {
                delayLine.pushSample (ch, mainBuffer.getSample (ch, i));
                mainBuffer.setSample (ch, i, delayLine.popSample (ch, fixedDelay));
            }

            for (int ch = numChannels; ch < 2; ++ch)
            {
                delayLine.pushSample (ch, 0.0f);
                (void) delayLine.popSample (ch, fixedDelay);
            }
        }
    }

private:
    void initialiseFilters()
    {
        for (auto& channel : allpassFilters)
        {
            for (auto& stage : channel)
            {
                stage.coefficients = new juce::dsp::IIR::Coefficients<float> (1.0f, 0.0f, 1.0f, 0.0f);
                stage.prepare ({ sampleRate, 1, 1 });
            }
        }

        updateAllpassCoefficients (50.0f, defaultAllpassQ);
    }

    float delaySamplesForUserDelayMs (float delayMs) const noexcept
    {
        const auto clampedMs = juce::jlimit (-20.0f, 20.0f, delayMs);
        return (float) baseDelaySamples + (float) (clampedMs * sampleRate / 1000.0);
    }

    // Recompute the shared allpass coefficients when either the (smoothed)
    // frequency or Q moves past a small epsilon. Both guards matter: the runtime
    // cascade must track the same freq/Q/stage settings the analyzer predicted
    // and applyLatestFix() writes, or the audible result won't match the
    // reported "after" match (the P1 bug). Coefficients are shared across all
    // four stages of both channels; only the first activeStages are run.
    void updateAllpassCoefficients (float frequencyHz, float q)
    {
        const auto limitedFreq = juce::jlimit (20.0f, 500.0f, frequencyHz);
        const auto limitedQ = juce::jlimit (minAllpassQ, maxAllpassQ, q);

        if (std::abs (limitedFreq - lastAppliedAllpassFreq) < 0.01f
            && std::abs (limitedQ - lastAppliedAllpassQ) < 1.0e-4f)
            return;

        const auto coeffs = juce::dsp::IIR::ArrayCoefficients<float>::makeAllPass (sampleRate, limitedFreq, limitedQ);

        for (auto& channel : allpassFilters)
            for (auto& stage : channel)
                *stage.coefficients = coeffs;

        lastAppliedAllpassFreq = limitedFreq;
        lastAppliedAllpassQ = limitedQ;
    }

    static constexpr int maxAllpassStages = 4;
    static constexpr float defaultAllpassQ = 0.70710678f;
    static constexpr float minAllpassQ = 0.1f;
    static constexpr float maxAllpassQ = 10.0f;

    double sampleRate = 44100.0;
    int baseDelaySamples = 0;
    int activeStages = 2;
    float lastAppliedAllpassFreq = -1.0f;
    float lastAppliedAllpassQ = -1.0f;

    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> delayLine;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedDelay;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedAllpassFreq;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedAllpassQ;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> allpassWet;

    // Per-channel cascade of up to 4 allpass stages (matching AllpassPhaseRotator).
    std::array<std::array<juce::dsp::IIR::Filter<float>, (size_t) maxAllpassStages>, 2> allpassFilters;
};

class KickLockAudioProcessor::AutoAlignEngine : public juce::Thread
{
public:
    explicit AutoAlignEngine (KickLockAudioProcessor& p)
        : juce::Thread ("KickLock Auto-Align"), owner (p)
    {
        startThread();
    }

    ~AutoAlignEngine() override
    {
        signalThreadShouldExit();
        notify();
        stopThread (2000);
    }

    void prepare (double newSampleRate)
    {
        sampleRate = newSampleRate;
        captureSamples = juce::jmax (1, (int) std::ceil (sampleRate * 0.050));
        maxLagSamples = juce::jmax (1, (int) std::ceil (sampleRate * 0.020));

        mainCapture.assign ((size_t) captureSamples, 0.0f);
        sideCapture.assign ((size_t) captureSamples, 0.0f);
        captureIndex.store (0, std::memory_order_release);
        state.store (State::Idle, std::memory_order_release);
    }

    void requestCapture() noexcept
    {
        if (state.load (std::memory_order_acquire) == State::Idle)
        {
            captureIndex.store (0, std::memory_order_release);
            state.store (State::Armed, std::memory_order_release);
        }
    }

    void pushSample (float mainSample, float sidechainSample) noexcept
    {
        auto current = state.load (std::memory_order_acquire);

        if (current == State::Armed)
        {
            if (std::abs (sidechainSample) < triggerThreshold)
                return;

            captureIndex.store (0, std::memory_order_release);
            current = State::Capturing;
            state.store (current, std::memory_order_release);
        }

        if (current != State::Capturing)
            return;

        const auto index = captureIndex.load (std::memory_order_relaxed);
        if (index >= captureSamples)
            return;

        mainCapture[(size_t) index] = mainSample;
        sideCapture[(size_t) index] = sidechainSample;

        const auto next = index + 1;
        captureIndex.store (next, std::memory_order_release);

        if (next >= captureSamples)
        {
            state.store (State::Analyzing, std::memory_order_release);
            notify();
        }
    }

    void run() override
    {
        while (! threadShouldExit())
        {
            if (state.load (std::memory_order_acquire) != State::Analyzing)
            {
                wait (10);
                continue;
            }

            const auto result = analyzeCapturedBuffers();
            state.store (State::Idle, std::memory_order_release);

            juce::MessageManager::callAsync ([this, result]
            {
                applyResultToParameters (result);
            });
        }
    }

private:
    enum class State
    {
        Idle,
        Armed,
        Capturing,
        Analyzing
    };

    struct Result
    {
        float delayMs = 0.0f;
        bool invertPolarity = false;
        bool valid = false;
    };

    Result analyzeCapturedBuffers() const
    {
        Result result;
        double bestAbsCorrelation = 0.0;
        double bestSignedCorrelation = 0.0;
        int bestLag = 0;

        for (int lag = -maxLagSamples; lag <= maxLagSamples; ++lag)
        {
            double xy = 0.0;
            double xx = 0.0;
            double yy = 0.0;
            int count = 0;

            for (int i = 0; i < captureSamples; ++i)
            {
                const int mainIndex = i - lag;
                if (mainIndex < 0 || mainIndex >= captureSamples)
                    continue;

                const auto x = (double) mainCapture[(size_t) mainIndex];
                const auto y = (double) sideCapture[(size_t) i];
                xy += x * y;
                xx += x * x;
                yy += y * y;
                ++count;
            }

            if (count < 16)
                continue;

            const auto denom = std::sqrt (xx * yy);
            if (denom <= 1.0e-12)
                continue;

            const auto corr = xy / denom;
            const auto absCorr = std::abs (corr);

            if (absCorr > bestAbsCorrelation)
            {
                bestAbsCorrelation = absCorr;
                bestSignedCorrelation = corr;
                bestLag = lag;
            }
        }

        if (bestAbsCorrelation > 0.05)
        {
            result.valid = true;
            result.delayMs = (float) ((double) bestLag * 1000.0 / sampleRate);
            result.delayMs = juce::jlimit (-20.0f, 20.0f, result.delayMs);
            result.invertPolarity = bestSignedCorrelation < 0.0;
        }

        return result;
    }

    void applyResultToParameters (Result result)
    {
        if (! result.valid)
            return;

        setParameter ("delay_ms", result.delayMs);
        setParameter ("delayMs", result.delayMs);
        setParameter ("polarity_invert", result.invertPolarity ? 1.0f : 0.0f);
        setParameter ("polarityInvert", result.invertPolarity ? 1.0f : 0.0f);
    }

    void setParameter (const char* id, float value)
    {
        if (auto* parameter = owner.apvts.getParameter (id))
        {
            parameter->beginChangeGesture();
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
            parameter->endChangeGesture();
        }
    }

    KickLockAudioProcessor& owner;
    double sampleRate = 44100.0;
    int captureSamples = 1;
    int maxLagSamples = 1;
    static constexpr float triggerThreshold = 0.25118864f; // -12 dBFS

    std::vector<float> mainCapture;
    std::vector<float> sideCapture;
    std::atomic<State> state { State::Idle };
    std::atomic<int> captureIndex { 0 };
};

KickLockAudioProcessor::KickLockAudioProcessor()
    : AudioProcessor (BusesProperties()
                           .withInput ("Main Input", juce::AudioChannelSet::stereo(), true)
                           .withInput ("Sidechain Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                           ),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    delayMsParam = apvts.getRawParameterValue ("delay_ms");
    delayMsLegacyParam = apvts.getRawParameterValue ("delayMs");
    delayInterpParam = apvts.getRawParameterValue ("delayInterp");
    polarityInvertParam = apvts.getRawParameterValue ("polarity_invert");
    polarityInvertLegacyParam = apvts.getRawParameterValue ("polarityInvert");
    phaseFilterEnabledParam = apvts.getRawParameterValue ("allpass_enable");
    phaseFilterEnabledLegacyParam = apvts.getRawParameterValue ("phaseFilterEnabled");
    rotatorFreqParam = apvts.getRawParameterValue ("allpass_freq");
    rotatorFreqLegacyParam = apvts.getRawParameterValue ("rotatorFreq");
    rotatorQParam = apvts.getRawParameterValue ("rotatorQ");
    rotatorStagesParam = apvts.getRawParameterValue ("rotatorStages");
    crossoverFreqParam = apvts.getRawParameterValue ("crossover_freq");
    dynEqFreqParam = apvts.getRawParameterValue ("dyneq_freq");
    dynEqQParam = apvts.getRawParameterValue ("dyneq_q");
    dynEqBoostDbParam = apvts.getRawParameterValue ("dyneq_boost_db");
    dynEqAmountParam = apvts.getRawParameterValue ("dyneq_amount");
    dynEqAttackMsParam = apvts.getRawParameterValue ("dyneq_attack_ms");
    dynEqHoldMsParam = apvts.getRawParameterValue ("dyneq_hold_ms");
    dynEqReleaseMsParam = apvts.getRawParameterValue ("dyneq_release_ms");
    dynEqTriggerRatioParam = apvts.getRawParameterValue ("dyneq_trigger_ratio");

    autoAlignEngine = std::make_unique<AutoAlignEngine> (*this);
}

KickLockAudioProcessor::~KickLockAudioProcessor()
{
    // The background analysis job captures `this`. Remove any queued job and
    // wait for a running one to finish before the members it touches (the
    // capture buffer, the result fields) start tearing down.
    analysisThreadPool.removeAllJobs (true, 2000);
    autoAlignEngine.reset();
}

juce::AudioProcessorValueTreeState::ParameterLayout KickLockAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "delay_ms", 1 },
        "Delay",
        juce::NormalisableRange<float> (-20.0f, 20.0f, 0.01f),
        0.0f));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "polarity_invert", 1 },
        "Polarity Invert",
        false));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "allpass_freq", 1 },
        "Allpass Frequency",
        juce::NormalisableRange<float> (20.0f, 500.0f, 0.0f, 0.35f),
        50.0f));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "allpass_enable", 1 },
        "Allpass Enable",
        false));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "delayMs", 1 },
        "Legacy Audio Bass Delay",
        juce::NormalisableRange<float> (-20.0f, 20.0f, 0.01f),
        0.0f));

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "delayInterp", 1 },
        "Delay Interpolation",
        juce::StringArray { "Linear", "Allpass" },
        0));

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "gridDivision", 1 },
        "Grid Division",
        juce::StringArray { "1/4", "1/8", "1/16", "1/32", "Bar", "ms" },
        5));

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "scopeViewMode", 1 },
        "Scope View",
        juce::StringArray { "Triggered", "Free-run", "Phase Delta", "Overlay", "Separate" },
        0));

    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { "visualOffsetSamples", 1 },
        "Visual Offset Samples",
        -4096, 4096, 0));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "polarityInvert", 1 },
        "Legacy Polarity Invert",
        false));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "phaseFilterEnabled", 1 },
        "Phase Filter",
        false));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "rotatorFreq", 1 },
        "Legacy Rotator Frequency",
        juce::NormalisableRange<float> (20.0f, 500.0f, 0.0f, 0.35f),
        50.0f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "rotatorQ", 1 },
        "Rotator Q",
        juce::NormalisableRange<float> (0.1f, 10.0f, 0.01f),
        0.7f));

    layout.add (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { "rotatorStages", 1 },
        "Rotator Stages",
        juce::StringArray { "2", "3", "4" },
        0));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "crossover_freq", 1 },
        "Crossover Frequency",
        juce::NormalisableRange<float> (40.0f, 500.0f, 0.0f, 0.35f),
        150.0f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "dyneq_freq", 1 },
        "Transient EQ Frequency",
        juce::NormalisableRange<float> (1000.0f, 8000.0f, 0.0f, 0.45f),
        3200.0f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "dyneq_q", 1 },
        "Transient EQ Q",
        juce::NormalisableRange<float> (0.5f, 12.0f, 0.01f),
        4.0f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "dyneq_boost_db", 1 },
        "Transient EQ Max Boost",
        juce::NormalisableRange<float> (0.0f, 18.0f, 0.01f),
        9.0f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "dyneq_amount", 1 },
        "Transient EQ Amount",
        juce::NormalisableRange<float> (0.0f, 1.0f, 0.001f),
        0.0f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "dyneq_attack_ms", 1 },
        "Transient EQ Attack",
        juce::NormalisableRange<float> (0.1f, 50.0f, 0.01f),
        2.0f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "dyneq_hold_ms", 1 },
        "Transient EQ Hold",
        juce::NormalisableRange<float> (0.0f, 250.0f, 0.01f),
        18.0f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "dyneq_release_ms", 1 },
        "Transient EQ Release",
        juce::NormalisableRange<float> (1.0f, 500.0f, 0.01f),
        80.0f));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "dyneq_trigger_ratio", 1 },
        "Transient EQ Trigger Ratio",
        juce::NormalisableRange<float> (1.05f, 20.0f, 0.01f),
        1.6f));

    return layout;
}

void KickLockAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    multibandCore.prepare (sampleRate, samplesPerBlock, juce::jmax (1, getTotalNumOutputChannels()), 20.0f);
    const auto maxDelaySamples = multibandCore.reportLatencySamples();
    setLatencySamples (maxDelaySamples);

    if (autoAlignEngine != nullptr)
        autoAlignEngine->prepare (sampleRate);

    // Live multi-band phase meters (P5). They band-pass internally, so no
    // separate pre-filters are needed. ~0.25 s rolling window per band.
    const int liveWindow = juce::jmax (256, (int) (sampleRate * 0.25));
    dryMultiBandMeter.prepare (sampleRate, liveWindow);
    processedMultiBandMeter.prepare (sampleRate, liveWindow);
    processedMeterSidechainDelay.setMaximumDelayInSamples (maxDelaySamples + 4);
    processedMeterSidechainDelay.prepare ({ sampleRate, (juce::uint32) juce::jmax (1, samplesPerBlock), 1 });
    processedMeterSidechainDelay.reset();
    bypassDelay.setMaximumDelayInSamples (maxDelaySamples + 4);
    bypassDelay.prepare ({ sampleRate, (juce::uint32) juce::jmax (1, samplesPerBlock), 2 });
    bypassDelay.reset();
    rawBassLowpass.setType (juce::dsp::LinkwitzRileyFilterType::lowpass);
    rawKickLowpass.setType (juce::dsp::LinkwitzRileyFilterType::lowpass);
    processedBassLowpass.setType (juce::dsp::LinkwitzRileyFilterType::lowpass);
    processedKickLowpass.setType (juce::dsp::LinkwitzRileyFilterType::lowpass);
    rawBassLowpass.prepare ({ sampleRate, (juce::uint32) juce::jmax (1, samplesPerBlock), 1 });
    rawKickLowpass.prepare ({ sampleRate, (juce::uint32) juce::jmax (1, samplesPerBlock), 1 });
    processedBassLowpass.prepare ({ sampleRate, (juce::uint32) juce::jmax (1, samplesPerBlock), 1 });
    processedKickLowpass.prepare ({ sampleRate, (juce::uint32) juce::jmax (1, samplesPerBlock), 1 });
    rawBassLowpass.reset();
    rawKickLowpass.reset();
    processedBassLowpass.reset();
    processedKickLowpass.reset();
    scopeFifo.prepare (8192);

    // ~2 seconds of raw bass/kick for the Analyze button's cross-correlation.
    rawCapture.prepare ((int) (sampleRate * 2.0));
    transientDetector.prepare (sampleRate);
    transientDetector.setThreshold (0.004f);
    transientDetector.setMinimumEnergyGate (0.0004f);
    transientDetector.setAttackReleaseMs (2.0f, 60.0f);
    // Relative detection: fire when the fast peak envelope pulls this many
    // times ahead of the slow body envelope, so long 808s / noisy tails whose
    // level never returns to silence still re-arm between hits.
    transientDetector.setTriggerRatio (3.0f);
    transientDetector.setHoldoffMs (90.0f);
    hitCapture.prepare (sampleRate, 20.0f, 150.0f);
    // Held-activity trackers for the P3 status. ~500 ms hold so a steady loop
    // never flickers to "no signal" in the gaps between kick transients. The
    // activation floor is low: it only needs to tell "playing" from "silent",
    // not judge phase-read quality (that is materialUsable below).
    kickActivity.prepare (sampleRate, 500.0f, 3.0e-3f);
    bassActivity.prepare (sampleRate, 500.0f, 3.0e-3f);
    kickActiveHeld.store (false);
    bassActiveHeld.store (false);
    analysisSignalUsable.store (false);
    analysisMaterialReady.store (false);

    // Decimate the scope feed so the UI ring can show a multi-beat view
    // without allocating on the audio thread.
    scopeDecimationFactor = juce::jmax (1, (int) (sampleRate / 2048.0));
    scopeDecimationCounter = 0;
    sidechainReferenceAvailable.store (false);
    tempoAvailable.store (false);
    latestBpm.store (0.0f);
    bassSignalRms.store (0.0f);
    kickSignalRms.store (0.0f);
    transientHealthDb.store (0.0f);
    transientPrePeak.store (0.0f);
    transientPostPeak.store (0.0f);
    for (auto& bandMatch : liveBandMatchPercent)
        bandMatch.store (50.0f);
    latestAppliedBeforePercent.store (-1.0f);
    correlationProductLpf = 0.0f;
    correlationMainEnergyLpf = 0.0f;
    correlationSideEnergyLpf = 0.0f;
    realtimeCorrelation.store (0.0f);
    uiSmoothingInitialized.store (false, std::memory_order_relaxed);

    lastInterpChoice = 0;
    lastStageChoice = 0;
    lastRotatorFreq = -1.0f;
    lastRotatorQ = -1.0f;
    lastDelayActive = false;

    {
        const std::lock_guard<std::mutex> lock (resultMutex);
        latestFixResult = {};
        lastAnalyzedBassWindow.clear();
        lastAnalyzedKickWindow.clear();
    }
    revertSnapshotValid.store (false, std::memory_order_release);

    // Reset the Analyze state machine unless a background job is mid-flight
    // (leave a running/analyzing job to resolve itself so its terminal store
    // isn't clobbered here).
    const auto state = analyzeState.load (std::memory_order_acquire);
    if (! analyzeStateIsBusy (state))
        analyzeState.store (AnalyzeState::Idle, std::memory_order_release);
}

void KickLockAudioProcessor::releaseResources()
{
}

void KickLockAudioProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    auto mainBuffer = getBusBuffer (buffer, true, 0);
    const auto numChannels = juce::jmin (2, mainBuffer.getNumChannels());
    const auto numSamples = mainBuffer.getNumSamples();
    const auto fixedDelay = (float) getLatencySamples();

    for (int i = 0; i < numSamples; ++i)
    {
        for (int ch = 0; ch < numChannels; ++ch)
        {
            bypassDelay.pushSample (ch, mainBuffer.getSample (ch, i));
            mainBuffer.setSample (ch, i, bypassDelay.popSample (ch, fixedDelay));
        }

        for (int ch = numChannels; ch < 2; ++ch)
        {
            bypassDelay.pushSample (ch, 0.0f);
            (void) bypassDelay.popSample (ch, fixedDelay);
        }
    }

    for (auto channel = getTotalNumInputChannels(); channel < getTotalNumOutputChannels(); ++channel)
        buffer.clear (channel, 0, buffer.getNumSamples());
}

bool KickLockAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto mainIn = layouts.getMainInputChannelSet();
    const auto mainOut = layouts.getMainOutputChannelSet();
    const auto sidechainIn = layouts.inputBuses.size() > 1 ? layouts.inputBuses[(size_t) 1]
                                                           : juce::AudioChannelSet::disabled();

    if (mainIn.isDisabled() || mainOut.isDisabled())
        return false;

    const auto isMonoOrStereo = [] (const juce::AudioChannelSet& set)
    {
        return set == juce::AudioChannelSet::mono() || set == juce::AudioChannelSet::stereo();
    };

    if (! isMonoOrStereo (mainIn) || ! isMonoOrStereo (mainOut))
        return false;

    if (! sidechainIn.isDisabled() && ! isMonoOrStereo (sidechainIn))
        return false;

    return true;
}

bool KickLockAudioProcessor::isBassProcessingNeutral() const noexcept
{
    constexpr float epsilon = 1.0e-6f;

    const float delayMs = delayMsParam != nullptr ? delayMsParam->load()
                                                  : (delayMsLegacyParam != nullptr ? delayMsLegacyParam->load() : 0.0f);
    const bool polarity = (polarityInvertParam != nullptr && polarityInvertParam->load() > 0.5f)
                       || (polarityInvertLegacyParam != nullptr && polarityInvertLegacyParam->load() > 0.5f);
    const bool phaseFilter = (phaseFilterEnabledParam != nullptr && phaseFilterEnabledParam->load() > 0.5f)
                          || (phaseFilterEnabledLegacyParam != nullptr && phaseFilterEnabledLegacyParam->load() > 0.5f);
    const float dynEqAmount = dynEqAmountParam != nullptr ? dynEqAmountParam->load() : 0.0f;

    return std::abs (delayMs) <= epsilon && ! polarity && ! phaseFilter && dynEqAmount <= epsilon;
}

void KickLockAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    bool bpmDetected = false;
    float bpmValue = 0.0f;

    if (auto* currentPlayHead = getPlayHead())
    {
        if (const auto position = currentPlayHead->getPosition())
        {
            if (const auto bpm = position->getBpm())
            {
                bpmValue = (float) *bpm;
                bpmDetected = bpmValue > 0.0f;
            }
        }
    }

    tempoAvailable.store (bpmDetected);
    latestBpm.store (bpmDetected ? bpmValue : 0.0f);

    const auto totalNumInputChannels = getTotalNumInputChannels();
    const auto totalNumOutputChannels = getTotalNumOutputChannels();

    // Clear any output channels that don't have a corresponding input,
    // since buffers may contain garbage.
    for (auto channel = totalNumInputChannels; channel < totalNumOutputChannels; ++channel)
        buffer.clear (channel, 0, buffer.getNumSamples());

    auto mainBuffer = getBusBuffer (buffer, true, 0);
    auto sidechainBuffer = getBusBuffer (buffer, true, 1);

    const auto* sidechainBus = getBus (true, 1);
    const bool hasSidechain = sidechainBus != nullptr && sidechainBus->isEnabled() && sidechainBuffer.getNumChannels() > 0;
    sidechainReferenceAvailable.store (hasSidechain);

    const int numSamples = mainBuffer.getNumSamples();
    double bassEnergySum = 0.0;
    double kickEnergySum = 0.0;

    const float snakeDelayMs = delayMsParam != nullptr ? delayMsParam->load() : 0.0f;
    const float legacyDelayMs = delayMsLegacyParam != nullptr ? delayMsLegacyParam->load() : 0.0f;
    const float delayMs = std::abs (snakeDelayMs) > 1.0e-6f ? snakeDelayMs : legacyDelayMs;

    const bool polarityInvert = (polarityInvertParam != nullptr && polarityInvertParam->load() > 0.5f)
                             || (polarityInvertLegacyParam != nullptr && polarityInvertLegacyParam->load() > 0.5f);
    const bool phaseFilterEnabled = (phaseFilterEnabledParam != nullptr && phaseFilterEnabledParam->load() > 0.5f)
                                 || (phaseFilterEnabledLegacyParam != nullptr && phaseFilterEnabledLegacyParam->load() > 0.5f);
    const float snakeFreq = rotatorFreqParam != nullptr ? rotatorFreqParam->load() : 50.0f;
    const float legacyFreq = rotatorFreqLegacyParam != nullptr ? rotatorFreqLegacyParam->load() : 50.0f;
    const float allpassFreq = std::abs (snakeFreq - 50.0f) > 1.0e-4f ? snakeFreq : legacyFreq;
    const float allpassQ = rotatorQParam != nullptr ? rotatorQParam->load() : 0.70710678f;
    const int allpassStages = 2 + (rotatorStagesParam != nullptr
                                       ? juce::jlimit (0, 2, (int) std::lround (rotatorStagesParam->load()))
                                       : 0);

    MultibandPhaseCore::Params coreParams;
    coreParams.crossoverHz = crossoverFreqParam != nullptr ? crossoverFreqParam->load() : 150.0f;
    coreParams.userDelayMs = delayMs;
    coreParams.polarityInvert = polarityInvert;
    coreParams.allpassEnabled = phaseFilterEnabled;
    coreParams.allpassFreqHz = allpassFreq;
    coreParams.allpassQ = allpassQ;
    coreParams.allpassStages = allpassStages;
    coreParams.dynEqFreqHz = dynEqFreqParam != nullptr ? dynEqFreqParam->load() : 3200.0f;
    coreParams.dynEqQ = dynEqQParam != nullptr ? dynEqQParam->load() : 4.0f;
    coreParams.dynEqMaxBoostDb = dynEqBoostDbParam != nullptr ? dynEqBoostDbParam->load() : 9.0f;
    coreParams.dynEqAmount = dynEqAmountParam != nullptr ? dynEqAmountParam->load() : 0.0f;
    coreParams.dynEqAttackMs = dynEqAttackMsParam != nullptr ? dynEqAttackMsParam->load() : 2.0f;
    coreParams.dynEqHoldMs = dynEqHoldMsParam != nullptr ? dynEqHoldMsParam->load() : 18.0f;
    coreParams.dynEqReleaseMs = dynEqReleaseMsParam != nullptr ? dynEqReleaseMsParam->load() : 80.0f;
    coreParams.dynEqTriggerRatio = dynEqTriggerRatioParam != nullptr ? dynEqTriggerRatioParam->load() : 1.6f;

    rawBassLowpass.setCutoffFrequency (coreParams.crossoverHz);
    rawKickLowpass.setCutoffFrequency (coreParams.crossoverHz);
    processedBassLowpass.setCutoffFrequency (coreParams.crossoverHz);
    processedKickLowpass.setCutoffFrequency (coreParams.crossoverHz);

    // Capture and meter RAW mono bass/kick before polarity/delay/rotator touch
    // the audible bass buffer. Skipped without a sidechain.
    if (hasSidechain)
    {
        const int mainCh = mainBuffer.getNumChannels();
        const int scCh   = sidechainBuffer.getNumChannels();

        for (int i = 0; i < numSamples; ++i)
        {
            float mSum = 0.0f;
            for (int ch = 0; ch < mainCh; ++ch)
                mSum += mainBuffer.getSample (ch, i);
            const float bassMono = mainCh > 0 ? mSum / (float) mainCh : 0.0f;
            bassEnergySum += (double) bassMono * (double) bassMono;

            float sSum = 0.0f;
            for (int ch = 0; ch < scCh; ++ch)
                sSum += sidechainBuffer.getSample (ch, i);
            const float kickMono = scCh > 0 ? sSum / (float) scCh : 0.0f;
            kickEnergySum += (double) kickMono * (double) kickMono;

            const float bassLow = rawBassLowpass.processSample (0, bassMono);
            const float kickLow = rawKickLowpass.processSample (0, kickMono);

            rawCapture.push (bassLow, kickLow);

            if (autoAlignEngine != nullptr)
                autoAlignEngine->pushSample (bassLow, kickLow);

            dryMultiBandMeter.pushSample (bassLow, kickLow);
        }
    }
    else
    {
        const int mainCh = mainBuffer.getNumChannels();

        for (int i = 0; i < numSamples; ++i)
        {
            float mSum = 0.0f;
            for (int ch = 0; ch < mainCh; ++ch)
                mSum += mainBuffer.getSample (ch, i);

            const float bassMono = mainCh > 0 ? mSum / (float) mainCh : 0.0f;
            bassEnergySum += (double) bassMono * (double) bassMono;
        }
    }

    const double safeCount = (double) juce::jmax (1, numSamples);
    const float blockBassRms = (float) std::sqrt (bassEnergySum / safeCount);
    const float blockKickRms = hasSidechain ? (float) std::sqrt (kickEnergySum / safeCount) : 0.0f;
    bassSignalRms.store (blockBassRms);
    kickSignalRms.store (blockKickRms);

    // Feed the held-activity trackers so status reflects recent hits, not just
    // this instant. Without a sidechain the kick can never be "active".
    bassActivity.pushBlock (blockBassRms, numSamples);
    if (hasSidechain)
        kickActivity.pushBlock (blockKickRms, numSamples);
    else
        kickActivity.pushBlock (0.0f, numSamples);

    kickActiveHeld.store (kickActivity.isActive());
    bassActiveHeld.store (bassActivity.isActive());

    // "Usable" material: both signals cleared a floor loud enough for a phase
    // read within the hold window. Lets status tell "present but far too quiet"
    // (SIGNAL TOO LOW) apart from a genuinely playing loop, without flickering
    // between kick transients.
    constexpr float usableFloorRms = 8.0e-3f;
    analysisSignalUsable.store (hasSidechain
                                && kickActivity.isUsable (usableFloorRms)
                                && bassActivity.isUsable (usableFloorRms));

    // "Enough material" for a meaningful analysis window: the raw capture has
    // accumulated at least ~0.5 s of samples AND both signals have been active
    // recently. Display-only; Apply Fix gates on the analysis result, not this.
    const int capturedSamples = rawCapture.getFilledSamples();
    const int minMaterialSamples = (int) (getSampleRate() * 0.5);
    analysisMaterialReady.store (hasSidechain
                                 && capturedSamples >= minMaterialSamples
                                 && kickActivity.isActive()
                                 && bassActivity.isActive());

    const bool neutral = isBassProcessingNeutral();
    const bool processingNeeded = ! neutral;

    multibandCore.process (mainBuffer, sidechainBuffer, coreParams, numSamples);
    transientHealthDb.store (multibandCore.getHealthMeter().getHealthDb());
    transientPrePeak.store (multibandCore.getHealthMeter().getPrePeak());
    transientPostPeak.store (multibandCore.getHealthMeter().getPostPeak());

    // Feed the correlation meter and scope fifo with mono-summed
    // main/sidechain values. Skipped entirely when there's no sidechain,
    // since a zero-sidechain signal would be meaningless for both.
    if (hasSidechain)
    {
        const int mainChannels = mainBuffer.getNumChannels();
        const int sidechainChannels = sidechainBuffer.getNumChannels();

        for (int i = 0; i < numSamples; ++i)
        {
            float mainSum = 0.0f;
            for (int ch = 0; ch < mainChannels; ++ch)
                mainSum += mainBuffer.getSample (ch, i);
            const float mainMono = mainChannels > 0 ? mainSum / (float) mainChannels : 0.0f;

            float sidechainSum = 0.0f;
            for (int ch = 0; ch < sidechainChannels; ++ch)
                sidechainSum += sidechainBuffer.getSample (ch, i);
            const float sidechainMono = sidechainChannels > 0 ? sidechainSum / (float) sidechainChannels : 0.0f;
            processedMeterSidechainDelay.pushSample (0, sidechainMono);
            const float meteredSidechainMono =
                processedMeterSidechainDelay.popSample (0, (float) getLatencySamples());
            const bool transientDetected = transientDetector.processSample (meteredSidechainMono);

            const float processedBassLow = processedBassLowpass.processSample (0, mainMono);
            const float alignedKickLow = processedKickLowpass.processSample (0, meteredSidechainMono);
            processedMultiBandMeter.pushSample (processedBassLow, alignedKickLow);
            hitCapture.pushSample (mainMono, meteredSidechainMono, transientDetected);

            constexpr float alpha = 0.005f;
            correlationProductLpf += alpha * ((mainMono * meteredSidechainMono) - correlationProductLpf);
            correlationMainEnergyLpf += alpha * ((mainMono * mainMono) - correlationMainEnergyLpf);
            correlationSideEnergyLpf += alpha * ((meteredSidechainMono * meteredSidechainMono) - correlationSideEnergyLpf);

            constexpr float noiseFloorEnergy = 1.0e-8f; // -80 dBFS squared
            const auto denominator = std::sqrt (correlationMainEnergyLpf * correlationSideEnergyLpf);
            const float signedCorrelation = (correlationMainEnergyLpf > noiseFloorEnergy
                                             && correlationSideEnergyLpf > noiseFloorEnergy
                                             && denominator > 1.0e-12f)
                ? juce::jlimit (-1.0f, 1.0f, correlationProductLpf / denominator)
                : 0.0f;
            realtimeCorrelation.store (std::isfinite (signedCorrelation) ? signedCorrelation : 0.0f);

            // Decimate the scope feed only: the meter still sees every sample,
            // but the UI keeps a slower, longer history for the musical grid.
            if (++scopeDecimationCounter >= scopeDecimationFactor)
            {
                scopeDecimationCounter = 0;
                scopeFifo.pushSample (mainMono, meteredSidechainMono);
            }
        }
    }
    else
    {
        realtimeCorrelation.store (0.0f);
    }

    // Overall (low-end-weighted) match from whichever meter reflects what the
    // user is hearing: the processed meter when the bass path is doing anything,
    // otherwise the dry meter.
    const auto& activeMeter = processingNeeded ? processedMultiBandMeter : dryMultiBandMeter;

    const float dryMatch = hasSidechain ? dryMultiBandMeter.getWeightedMatchPercent() : 50.0f;
    const float processedMatch = hasSidechain ? processedMultiBandMeter.getWeightedMatchPercent() : 50.0f;
    const float activeMatch = hasSidechain ? activeMeter.getWeightedMatchPercent() : 50.0f;
    const float activeLowEnd = hasSidechain ? activeMeter.getLowEndMatchPercent() : 50.0f;
    const float activeBroad = hasSidechain ? activeMeter.getBroadbandMatchPercent() : 50.0f;
    std::array<float, PhaseBands::numBands> activeBands {};
    for (int band = 0; band < PhaseBands::numBands; ++band)
        activeBands[(size_t) band] = hasSidechain ? activeMeter.getBandMatchPercent (band) : 50.0f;

    // Smooth the UI values with a slow ~500ms EMA to prevent the numbers from dancing too fast
    const float dt = (float)buffer.getNumSamples() / (float)getSampleRate();
    const float uiAlpha = 1.0f - std::exp(-dt / 0.5f);

    // P3: seed each EMA from the first real reading, then blend. The old
    // "snap whenever |current-50|<0.001" test also snapped whenever a value
    // legitimately passed through 50%, freezing the display on the way past.
    // A one-shot initialized flag seeds cleanly without that artefact.
    const bool seed = ! uiSmoothingInitialized.load (std::memory_order_relaxed);
    auto smoothAtomic = [uiAlpha, seed](std::atomic<float>& target, float newValue) {
        if (seed)
            target.store (newValue);
        else
        {
            const float current = target.load();
            target.store (current + uiAlpha * (newValue - current));
        }
    };

    smoothAtomic(dryInputMatchPercent, dryMatch);
    smoothAtomic(processedMatchPercent, processedMatch);

    // P5/P8 live read-outs: overall multi-band, sub/low only, broad 20-500.
    smoothAtomic(liveMultiBandMatchPercent, activeMatch);
    smoothAtomic(liveLowEndMatchPercent, activeLowEnd);
    smoothAtomic(liveBroadbandMatchPercent, activeBroad);
    smoothAtomic(realtimeLowBandMatchPercent, activeLowEnd); // legacy alias (sub/low)
    for (int band = 0; band < PhaseBands::numBands; ++band)
        smoothAtomic (liveBandMatchPercent[(size_t) band], activeBands[(size_t) band]);

    uiSmoothingInitialized.store (true, std::memory_order_relaxed);

    correlationPercent.store ((realtimeCorrelation.load() + 1.0f) * 50.0f);
}

juce::AudioProcessorEditor* KickLockAudioProcessor::createEditor()
{
    return new KickLockAudioProcessorEditor (*this);
}

bool KickLockAudioProcessor::hasEditor() const
{
    return true;
}

const juce::String KickLockAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool KickLockAudioProcessor::acceptsMidi() const
{
    return false;
}

bool KickLockAudioProcessor::producesMidi() const
{
    return false;
}

bool KickLockAudioProcessor::isMidiEffect() const
{
    return false;
}

double KickLockAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int KickLockAudioProcessor::getNumPrograms()
{
    return 4;
}

int KickLockAudioProcessor::getCurrentProgram()
{
    return currentProgramIndex;
}

void KickLockAudioProcessor::setCurrentProgram (int index)
{
    currentProgramIndex = juce::jlimit (0, getNumPrograms() - 1, index);
    const auto preset = makeFactoryPresetSnapshot (currentProgramIndex);
    restoreParameterSnapshot (preset);

    initialiseCompareSlotsIfNeeded();
    compareSlots[(size_t) activeCompareSlot.load (std::memory_order_acquire)] = preset;
    writeCompareSlotsToState();
}

const juce::String KickLockAudioProcessor::getProgramName (int index)
{
    switch (index)
    {
        case 0:  return "Tight EDM";
        case 1:  return "Deep House Sub";
        case 2:  return "Trap 808";
        case 3:  return "Neutral";
        default: return {};
    }
}

void KickLockAudioProcessor::changeProgramName (int /*index*/, const juce::String& /*newName*/)
{
}

float KickLockAudioProcessor::readParameterValue (const char* id, float fallback) const
{
    if (auto* parameter = apvts.getParameter (id))
        return parameter->convertFrom0to1 (parameter->getValue());

    if (const auto* value = apvts.getRawParameterValue (id))
        return value->load();

    return fallback;
}

void KickLockAudioProcessor::setParameterValueWithGesture (const char* id, float value)
{
    if (auto* parameter = apvts.getParameter (id))
    {
        parameter->beginChangeGesture();
        parameter->setValueNotifyingHost (parameter->convertTo0to1 (value));
        parameter->endChangeGesture();
    }
}

KickLockAudioProcessor::ParameterSnapshot KickLockAudioProcessor::captureCurrentParameterSnapshot() const
{
    ParameterSnapshot snapshot;
    snapshot.delayMs = readParameterValue ("delay_ms", readParameterValue ("delayMs", 0.0f));
    snapshot.polarityInvert = readParameterValue ("polarity_invert", readParameterValue ("polarityInvert", 0.0f)) > 0.5f;
    snapshot.phaseFilterEnabled = readParameterValue ("allpass_enable", readParameterValue ("phaseFilterEnabled", 0.0f)) > 0.5f;
    snapshot.phaseFilterFreqHz = readParameterValue ("allpass_freq", readParameterValue ("rotatorFreq", 50.0f));
    snapshot.phaseFilterQ = readParameterValue ("rotatorQ", 0.7f);
    snapshot.phaseFilterStageIndex = juce::jlimit (0, 2, (int) std::lround (readParameterValue ("rotatorStages", 0.0f)));
    return snapshot;
}

void KickLockAudioProcessor::restoreParameterSnapshot (const ParameterSnapshot& snapshot)
{
    setParameterValueWithGesture ("delay_ms", snapshot.delayMs);
    setParameterValueWithGesture ("delayMs", snapshot.delayMs);
    setParameterValueWithGesture ("polarity_invert", snapshot.polarityInvert ? 1.0f : 0.0f);
    setParameterValueWithGesture ("polarityInvert", snapshot.polarityInvert ? 1.0f : 0.0f);
    setParameterValueWithGesture ("allpass_enable", snapshot.phaseFilterEnabled ? 1.0f : 0.0f);
    setParameterValueWithGesture ("phaseFilterEnabled", snapshot.phaseFilterEnabled ? 1.0f : 0.0f);
    setParameterValueWithGesture ("allpass_freq", snapshot.phaseFilterFreqHz);
    setParameterValueWithGesture ("rotatorFreq", snapshot.phaseFilterFreqHz);
    setParameterValueWithGesture ("rotatorQ", snapshot.phaseFilterQ);
    setParameterValueWithGesture ("rotatorStages", (float) snapshot.phaseFilterStageIndex);
}

KickLockAudioProcessor::ParameterSnapshot KickLockAudioProcessor::makeFactoryPresetSnapshot (int index)
{
    ParameterSnapshot preset;

    switch (index)
    {
        case 0: // Tight EDM
            preset.phaseFilterEnabled = true;
            preset.phaseFilterFreqHz = 95.0f;
            preset.phaseFilterQ = 1.2f;
            preset.phaseFilterStageIndex = 0;
            break;

        case 1: // Deep House Sub
            preset.phaseFilterEnabled = true;
            preset.phaseFilterFreqHz = 55.0f;
            preset.phaseFilterQ = 2.0f;
            preset.phaseFilterStageIndex = 1;
            break;

        case 2: // Trap 808
            preset.phaseFilterEnabled = true;
            preset.phaseFilterFreqHz = 45.0f;
            preset.phaseFilterQ = 1.4f;
            preset.phaseFilterStageIndex = 2;
            break;

        case 3: // Neutral
        default:
            break;
    }

    return preset;
}

void KickLockAudioProcessor::initialiseCompareSlotsIfNeeded()
{
    if (compareSlotsInitialised)
        return;

    if (apvts.state.hasProperty ("compareSlot0DelayMs"))
    {
        loadCompareSlotsFromState();
        return;
    }

    compareSlots[0] = captureCurrentParameterSnapshot();
    compareSlots[1] = compareSlots[0];
    compareSlotsInitialised = true;
    writeCompareSlotsToState();
}

void KickLockAudioProcessor::loadCompareSlotsFromState()
{
    auto readSlot = [this] (int slot, const ParameterSnapshot& fallback)
    {
        const juce::String prefix = "compareSlot" + juce::String (slot);
        ParameterSnapshot snapshot = fallback;
        snapshot.delayMs = (float) apvts.state.getProperty (juce::Identifier (prefix + "DelayMs"), snapshot.delayMs);
        snapshot.polarityInvert = (bool) apvts.state.getProperty (juce::Identifier (prefix + "Polarity"), snapshot.polarityInvert);
        snapshot.phaseFilterEnabled = (bool) apvts.state.getProperty (juce::Identifier (prefix + "PhaseEnabled"), snapshot.phaseFilterEnabled);
        snapshot.phaseFilterFreqHz = (float) apvts.state.getProperty (juce::Identifier (prefix + "FreqHz"), snapshot.phaseFilterFreqHz);
        snapshot.phaseFilterQ = (float) apvts.state.getProperty (juce::Identifier (prefix + "Q"), snapshot.phaseFilterQ);
        snapshot.phaseFilterStageIndex = juce::jlimit (0, 2, (int) apvts.state.getProperty (juce::Identifier (prefix + "StageIndex"), snapshot.phaseFilterStageIndex));
        return snapshot;
    };

    const auto current = captureCurrentParameterSnapshot();
    compareSlots[0] = readSlot (0, current);
    compareSlots[1] = readSlot (1, current);
    activeCompareSlot.store (juce::jlimit (0, 1, (int) apvts.state.getProperty ("compareActiveSlot", 0)),
                             std::memory_order_release);
    currentProgramIndex = juce::jlimit (0, getNumPrograms() - 1,
                                        (int) apvts.state.getProperty ("currentProgramIndex", currentProgramIndex));
    compareSlotsInitialised = true;
}

void KickLockAudioProcessor::writeCompareSlotsToState()
{
    apvts.state.setProperty ("compareActiveSlot", activeCompareSlot.load (std::memory_order_acquire), nullptr);
    apvts.state.setProperty ("currentProgramIndex", currentProgramIndex, nullptr);

    for (int slot = 0; slot < 2; ++slot)
    {
        const auto& snapshot = compareSlots[(size_t) slot];
        const juce::String prefix = "compareSlot" + juce::String (slot);
        apvts.state.setProperty (juce::Identifier (prefix + "DelayMs"), snapshot.delayMs, nullptr);
        apvts.state.setProperty (juce::Identifier (prefix + "Polarity"), snapshot.polarityInvert, nullptr);
        apvts.state.setProperty (juce::Identifier (prefix + "PhaseEnabled"), snapshot.phaseFilterEnabled, nullptr);
        apvts.state.setProperty (juce::Identifier (prefix + "FreqHz"), snapshot.phaseFilterFreqHz, nullptr);
        apvts.state.setProperty (juce::Identifier (prefix + "Q"), snapshot.phaseFilterQ, nullptr);
        apvts.state.setProperty (juce::Identifier (prefix + "StageIndex"), snapshot.phaseFilterStageIndex, nullptr);
    }
}

void KickLockAudioProcessor::selectCompareSlot (int slotIndex)
{
    initialiseCompareSlotsIfNeeded();

    const int target = juce::jlimit (0, 1, slotIndex);
    const int current = activeCompareSlot.load (std::memory_order_acquire);
    if (target == current)
        return;

    compareSlots[(size_t) current] = captureCurrentParameterSnapshot();
    activeCompareSlot.store (target, std::memory_order_release);
    restoreParameterSnapshot (compareSlots[(size_t) target]);
    writeCompareSlotsToState();
}

void KickLockAudioProcessor::copyActiveCompareSlotToOther()
{
    initialiseCompareSlotsIfNeeded();

    const int current = activeCompareSlot.load (std::memory_order_acquire);
    const int other = 1 - current;
    compareSlots[(size_t) current] = captureCurrentParameterSnapshot();
    compareSlots[(size_t) other] = compareSlots[(size_t) current];
    writeCompareSlotsToState();
}

void KickLockAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    initialiseCompareSlotsIfNeeded();
    compareSlots[(size_t) activeCompareSlot.load (std::memory_order_acquire)] = captureCurrentParameterSnapshot();
    writeCompareSlotsToState();

    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void KickLockAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));

    if (xml != nullptr && xml->hasTagName (apvts.state.getType()))
    {
        auto restoredState = juce::ValueTree::fromXml (*xml);
        apvts.replaceState (restoredState);

        auto snapBoolParameter = [this, &restoredState] (const char* id)
        {
            auto* parameter = apvts.getParameter (id);
            if (parameter == nullptr)
                return;

            const auto restoredValue = (float) restoredState.getProperty (id, parameter->getValue());
            const float snappedValue = restoredValue >= 0.5f ? 1.0f : 0.0f;
            parameter->setValueNotifyingHost (parameter->convertTo0to1 (snappedValue));
            apvts.state.setProperty (id, snappedValue, nullptr);
        };

        snapBoolParameter ("polarity_invert");
        snapBoolParameter ("polarityInvert");
        snapBoolParameter ("allpass_enable");
        snapBoolParameter ("phaseFilterEnabled");

        compareSlotsInitialised = false;
        loadCompareSlotsFromState();
    }
}

PhaseFixResult KickLockAudioProcessor::analyzeFix()
{
    std::vector<float> bass, kick;
    const int n = rawCapture.snapshot (bass, kick);
    return computeAndPublishFix (bass, kick, n);
}

PhaseFixResult KickLockAudioProcessor::computeAndPublishFix (const std::vector<float>& bass,
                                                            const std::vector<float>& kick,
                                                            int numSamples)
{
    const auto delayInterpolation = interpolationFromChoice (delayInterpParam != nullptr
                                                                 ? delayInterpParam->load()
                                                                 : 0.0f);

    PhaseFixResult result;
    std::vector<float> analyzedBass, analyzedKick;

    if (numSamples > 32)
    {
        const auto hits = extractRecentHitWindows (kick, getSampleRate(), 8);
        if (! hits.empty())
            appendHitWindows (bass, kick, hits, analyzedBass, analyzedKick);
        else
        {
            analyzedBass = bass;
            analyzedKick = kick;
        }

        result = analyzeAggregatedHits (bass, kick, hits, getSampleRate(), delayInterpolation);
    }
    else
    {
        PhaseFixEngine::updateDerivedResultFields (result);
    }

    // P6: replace the generic "waiting for signal" text with a specific reason
    // (no kick / no bass / not enough material) when there's no usable result.
    refineInsufficientSignalMessage (result, bass, kick, numSamples, getSampleRate());

    // Publish the result and the window it was computed from under the lock so
    // a background worker and the message thread never race on them.
    {
        const std::lock_guard<std::mutex> lock (resultMutex);
        latestFixResult = result;
        lastAnalyzedBassWindow = std::move (analyzedBass);
        lastAnalyzedKickWindow = std::move (analyzedKick);
    }

    latestAnalyzedBeforePercent.store (result.beforeMatchPercent);
    latestAnalyzedAfterPercent.store (result.predictedAfterMatchPercent);
    latestVerifiedAfterPercent.store (result.verifiedAfterMatchPercent);
    latestVerificationDeltaPercent.store (result.verificationDeltaPercent);
    latestFixConfidence.store (result.confidence * 100.0f);

    return result;
}

bool KickLockAudioProcessor::beginBackgroundAnalyze()
{
    // Reject re-entry while an analysis is already in flight.
    if (analyzeStateIsBusy (analyzeState.load (std::memory_order_acquire)))
        return false;

    if (! sidechainReferenceAvailable.load (std::memory_order_acquire)
        || ! analysisSignalUsable.load (std::memory_order_acquire)
        || ! analysisMaterialReady.load (std::memory_order_acquire))
    {
        return false;
    }

    analyzeState.store (AnalyzeState::Preparing, std::memory_order_release);

    // Snapshot on the message thread (allocates, but off the audio thread),
    // then hand the copy to the worker. The worker only ever touches this copy
    // and the lock-guarded result fields, never live/mutable audio buffers.
    auto bass = std::make_shared<std::vector<float>>();
    auto kick = std::make_shared<std::vector<float>>();
    const int n = rawCapture.snapshot (*bass, *kick);

    analysisThreadPool.addJob ([this, bass, kick, n]
    {
        analyzeState.store (AnalyzeState::Analyzing, std::memory_order_release);

        try
        {
            const auto result = computeAndPublishFix (*bass, *kick, n);

            const bool usable = result.enoughSignal;
            analyzeState.store (usable ? AnalyzeState::ResultReady
                                       : AnalyzeState::NotEnoughMaterial,
                                std::memory_order_release);
        }
        catch (...)
        {
            PhaseFixResult failed;
            failed.message = "Analyze failed. Keep the loop playing and try again.";

            {
                const std::lock_guard<std::mutex> lock (resultMutex);
                latestFixResult = failed;
                lastAnalyzedBassWindow.clear();
                lastAnalyzedKickWindow.clear();
            }

            analyzeState.store (AnalyzeState::Failed, std::memory_order_release);
        }
    });

    return true;
}

void KickLockAudioProcessor::acknowledgeAnalyzeState() noexcept
{
    // Called by the UI once it has consumed a resolved state, so a subsequent
    // Analyze can transition cleanly from Idle-like semantics again.
    if (analyzeStateIsResolved (analyzeState.load (std::memory_order_acquire)))
        analyzeState.store (AnalyzeState::Idle, std::memory_order_release);
}

bool KickLockAudioProcessor::applyLatestFix()
{
    // Snapshot the shared result + analysis windows under lock so a background
    // analysis job can't swap them out from under us mid-apply. All the heavy
    // scoring below then runs on these local copies with the lock released.
    PhaseFixResult fix;
    std::vector<float> bassWindow;
    std::vector<float> kickWindow;
    {
        const std::lock_guard<std::mutex> lock (resultMutex);
        fix = latestFixResult;
        bassWindow = lastAnalyzedBassWindow;
        kickWindow = lastAnalyzedKickWindow;
    }

    if (! fix.applyAllowed && ! fix.optionalApplyAllowed)
        return false;

    latestRevertSnapshot = captureCurrentParameterSnapshot();
    revertSnapshotValid.store (true, std::memory_order_release);
    latestAppliedBeforePercent.store (fix.beforeMatchPercent);

    setParameterValueWithGesture ("polarity_invert", fix.bassPolarityInvert ? 1.0f : 0.0f);
    setParameterValueWithGesture ("polarityInvert", fix.bassPolarityInvert ? 1.0f : 0.0f);
    setParameterValueWithGesture ("delay_ms", juce::jlimit (-20.0f, 20.0f, fix.bassDelayMs));
    setParameterValueWithGesture ("delayMs", juce::jlimit (-20.0f, 20.0f, fix.bassDelayMs));
    setParameterValueWithGesture ("allpass_enable", fix.phaseFilterEnabled ? 1.0f : 0.0f);
    setParameterValueWithGesture ("phaseFilterEnabled", fix.phaseFilterEnabled ? 1.0f : 0.0f);

    if (fix.phaseFilterEnabled)
    {
        setParameterValueWithGesture ("allpass_freq", juce::jlimit (20.0f, 500.0f, fix.phaseFilterFreqHz));
        setParameterValueWithGesture ("rotatorFreq", juce::jlimit (20.0f, 500.0f, fix.phaseFilterFreqHz));
        setParameterValueWithGesture ("rotatorQ", fix.phaseFilterQ);
        setParameterValueWithGesture ("rotatorStages", (float) juce::jlimit (0, 2, fix.phaseFilterStages - 2));
    }

    if (! bassWindow.empty() && bassWindow.size() == kickWindow.size())
    {
        PhaseFixRenderSettings settings;
        settings.bassPolarityInvert = fix.bassPolarityInvert;
        settings.bassDelayMs = fix.bassDelayMs;
        settings.phaseFilterEnabled = fix.phaseFilterEnabled;
        settings.phaseFilterFreqHz = fix.phaseFilterFreqHz;
        settings.phaseFilterQ = fix.phaseFilterQ;
        settings.phaseFilterStages = fix.phaseFilterStages;
        settings.delayInterpolation = interpolationFromChoice (delayInterpParam != nullptr
                                                                   ? delayInterpParam->load()
                                                                   : 0.0f);

        const auto verified = PhaseFixEngine::scoreSettings (bassWindow.data(),
                                                             kickWindow.data(),
                                                             (int) bassWindow.size(),
                                                             getSampleRate(),
                                                             settings,
                                                             PhaseFixEngine::absoluteManualMaxDelayMs);

        PhaseFixEngine::applyVerification (fix, verified.matchPercent);

        const std::lock_guard<std::mutex> lock (resultMutex);
        // Only write verification back if the stored result is still the one we
        // applied (a racing re-analyze would have replaced it — leave that one).
        PhaseFixEngine::applyVerification (latestFixResult, verified.matchPercent);
        latestVerifiedAfterPercent.store (latestFixResult.verifiedAfterMatchPercent);
        latestVerificationDeltaPercent.store (latestFixResult.verificationDeltaPercent);
    }

    return true;
}

bool KickLockAudioProcessor::revertLatestFix()
{
    if (! revertSnapshotValid.load (std::memory_order_acquire))
        return false;

    restoreParameterSnapshot (latestRevertSnapshot);
    revertSnapshotValid.store (false, std::memory_order_release);
    latestAppliedBeforePercent.store (-1.0f);
    return true;
}

PhaseFixResult KickLockAudioProcessor::getLatestFixResult() const
{
    const std::lock_guard<std::mutex> lock (resultMutex);
    return latestFixResult;
}

int KickLockAudioProcessor::getScopeDecimationFactor() const noexcept
{
    return scopeDecimationFactor;
}

bool KickLockAudioProcessor::hasSidechainReference() const noexcept
{
    return sidechainReferenceAvailable.load();
}

bool KickLockAudioProcessor::isTempoAvailable() const noexcept
{
    return tempoAvailable.load();
}

float KickLockAudioProcessor::getLatestBpm() const noexcept
{
    return latestBpm.load();
}

float KickLockAudioProcessor::getBassSignalRms() const noexcept
{
    return bassSignalRms.load();
}

float KickLockAudioProcessor::getKickSignalRms() const noexcept
{
    return kickSignalRms.load();
}

void KickLockAudioProcessor::setLatestFixResultForTesting (const PhaseFixResult& result)
{
    const std::lock_guard<std::mutex> lock (resultMutex);
    latestFixResult = result;
}

void KickLockAudioProcessor::requestAutoAlign()
{
    if (autoAlignEngine != nullptr)
        autoAlignEngine->requestCapture();
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new KickLockAudioProcessor();
}
