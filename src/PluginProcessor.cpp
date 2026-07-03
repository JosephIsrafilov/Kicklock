#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>
#include <cmath>
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

        const float threshold = std::max (peak * 0.25f, 1.0e-4f);
        const int holdoffSamples = juce::jmax (1, (int) std::lround (sampleRate * 0.070));
        const int preSamples = juce::jmax (1, (int) std::lround (sampleRate * 0.006));
        const int postSamples = juce::jmax (64, (int) std::lround (sampleRate * 0.110));

        int lastAcceptedPeak = -holdoffSamples;
        for (int i = 1; i < (int) kick.size() - 1; ++i)
        {
            const float v = std::abs (kick[(size_t) i]);
            if (v < threshold)
                continue;

            if (v < std::abs (kick[(size_t) (i - 1)]) || v < std::abs (kick[(size_t) (i + 1)]))
                continue;

            if (i - lastAcceptedPeak < holdoffSamples)
                continue;

            AnalysisHitWindow hit;
            hit.peak = i;
            hit.start = juce::jlimit (0, (int) kick.size() - 1, i - preSamples);
            hit.length = std::min ((int) kick.size() - hit.start, preSamples + postSamples);

            if (hit.length > 96)
            {
                hits.push_back (hit);
                lastAcceptedPeak = i;
            }
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
        perHitResults.reserve (hits.size());
        std::vector<float> allHitBass;
        std::vector<float> allHitKick;
        std::vector<float> scoringBass;
        std::vector<float> scoringKick;
        appendHitWindows (bass, kick, hits, allHitBass, allHitKick);

        for (const auto& hit : hits)
        {
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
                scoringBass.insert (scoringBass.end(),
                                    bass.begin() + hit.start,
                                    bass.begin() + hit.start + hit.length);
                scoringKick.insert (scoringKick.end(),
                                    kick.begin() + hit.start,
                                    kick.begin() + hit.start + hit.length);
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

        aggregated.valid = true;
        aggregated.enoughSignal = true;
        aggregated.contributingHits = (int) perHitResults.size();
        aggregated.singleHitAnalysis = perHitResults.size() == 1;

        int polarityFlips = 0;
        int phaseRecommendations = 0;
        int improvementCount = 0;
        int largeTimingCount = 0;
        int timelineMoveCount = 0;
        std::vector<float> confidences;
        std::vector<float> safeDelays;
        std::vector<float> safeImprovements;
        std::vector<float> largeTimingOffsets;
        std::vector<float> timelineMoves;
        std::vector<float> phaseFreqs;
        std::vector<float> phaseQs;
        std::vector<float> phaseStages;

        for (const auto& hitResult : perHitResults)
        {
            confidences.push_back (hitResult.confidence);

            if (hitResult.bassPolarityInvert)
                ++polarityFlips;

            if (hitResult.improvementPercent >= 5.0f)
                ++improvementCount;

            if (hitResult.largeTimingOffset)
            {
                ++largeTimingCount;
                largeTimingOffsets.push_back (hitResult.detectedTimingOffsetMs);
                continue;
            }

            if (hitResult.requiresTimelineMove)
            {
                ++timelineMoveCount;
                timelineMoves.push_back (hitResult.suggestedKickMoveMs);
                continue;
            }

            safeDelays.push_back (hitResult.bassDelayMs);
            safeImprovements.push_back (hitResult.improvementPercent);

            if (hitResult.phaseFilterEnabled)
            {
                ++phaseRecommendations;
                phaseFreqs.push_back (hitResult.phaseFilterFreqHz);
                phaseQs.push_back (hitResult.phaseFilterQ);
                phaseStages.push_back ((float) hitResult.phaseFilterStages);
            }
        }

        const int validHits = (int) perHitResults.size();
        const float polarityShare = validHits > 0 ? (float) polarityFlips / (float) validHits : 0.0f;
        const bool majorityPolarity = polarityShare >= 0.5f;
        const bool polarityStable = polarityShare >= 0.70f || polarityShare <= 0.30f;

        if (largeTimingCount > validHits / 2)
        {
            aggregated.largeTimingOffset = true;
            aggregated.detectedTimingOffsetMs = medianOf (largeTimingOffsets);
            aggregated.confidence = averageOf (confidences);
            PhaseFixEngine::updateDerivedResultFields (aggregated);
            return aggregated;
        }

        if (timelineMoveCount > validHits / 2)
        {
            aggregated.requiresTimelineMove = true;
            aggregated.suggestedKickMoveMs = medianOf (timelineMoves);
            aggregated.confidence = averageOf (confidences);
            PhaseFixEngine::updateDerivedResultFields (aggregated);
            return aggregated;
        }

        if (safeDelays.empty())
        {
            aggregated.confidence = averageOf (confidences);
            PhaseFixEngine::updateDerivedResultFields (aggregated);
            return aggregated;
        }

        const float medianDelay = medianOf (safeDelays);
        const float delayStdDev = standardDeviationOf (safeDelays, averageOf (safeDelays));
        const float phaseShare = validHits > 0 ? (float) phaseRecommendations / (float) validHits : 0.0f;
        const bool phaseStable = phaseShare >= 0.60f || phaseShare <= 0.40f;
        const bool phaseEnabled = phaseShare >= 0.60f;
        const bool improvementStable = improvementCount >= (int) std::ceil ((float) validHits * 0.60f);

        aggregated.bassPolarityInvert = majorityPolarity;
        aggregated.bassDelayMs = medianDelay;
        aggregated.phaseFilterEnabled = phaseEnabled;
        aggregated.phaseFilterFreqHz = phaseEnabled ? medianOf (phaseFreqs) : 200.0f;
        aggregated.phaseFilterQ = phaseEnabled ? medianOf (phaseQs) : 0.7f;
        aggregated.phaseFilterStages = phaseEnabled
            ? juce::jlimit (2, 4, (int) std::lround (medianOf (phaseStages)))
            : 2;

        PhaseFixRenderSettings settings;
        settings.bassPolarityInvert = aggregated.bassPolarityInvert;
        settings.bassDelayMs = aggregated.bassDelayMs;
        settings.phaseFilterEnabled = aggregated.phaseFilterEnabled;
        settings.phaseFilterFreqHz = aggregated.phaseFilterFreqHz;
        settings.phaseFilterQ = aggregated.phaseFilterQ;
        settings.phaseFilterStages = aggregated.phaseFilterStages;
        settings.delayInterpolation = delayInterpolation;

        const auto* scoreBass = scoringBass.empty() ? bass.data() : scoringBass.data();
        const auto* scoreKick = scoringKick.empty() ? kick.data() : scoringKick.data();
        const int scoreSamples = scoringBass.empty() ? (int) bass.size() : (int) scoringBass.size();

        const auto before = PhaseFixEngine::scoreSettings (scoreBass, scoreKick, scoreSamples,
                                                           sampleRate, {}, PhaseFixEngine::absoluteManualMaxDelayMs);
        const auto after = PhaseFixEngine::scoreSettings (scoreBass, scoreKick, scoreSamples,
                                                          sampleRate, settings, PhaseFixEngine::absoluteManualMaxDelayMs);

        aggregated.beforeMatchPercent = before.matchPercent;
        aggregated.afterMatchPercent = after.matchPercent;
        aggregated.predictedAfterMatchPercent = after.matchPercent;
        aggregated.improvementPercent = aggregated.afterMatchPercent - aggregated.beforeMatchPercent;

        const float averageConfidence = averageOf (confidences);
        const float delayConsistency = medianDelay <= PhaseFixEngine::defaultAutoFixMaxDelayMs
            ? std::clamp (1.0f - (delayStdDev / 1.0f), 0.0f, 1.0f)
            : 0.0f;
        const float polarityConsistency = polarityStable ? 1.0f : 0.35f;
        const float phaseConsistency = phaseStable ? 1.0f : 0.50f;
        const float consistency = delayConsistency * polarityConsistency * phaseConsistency;

        aggregated.confidence = std::clamp (averageConfidence * consistency
                                            * (aggregated.singleHitAnalysis ? 0.85f : 1.0f),
                                            0.0f, 1.0f);

        if (! polarityStable
            || ! phaseStable
            || ! improvementStable
            || (safeDelays.size() >= 2
                && medianDelay <= PhaseFixEngine::defaultAutoFixMaxDelayMs
                && delayStdDev > 1.0f))
        {
            aggregated.unstableRecommendation = true;
        }

        PhaseFixEngine::updateDerivedResultFields (aggregated);

        if (aggregated.singleHitAnalysis && aggregated.enoughSignal
            && ! aggregated.message.containsIgnoreCase ("single-hit"))
        {
            aggregated.message << " Single-hit analysis.";
        }

        return aggregated;
    }
}

KickLockAudioProcessor::KickLockAudioProcessor()
    : AudioProcessor (BusesProperties()
                           .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                           .withOutput ("Output", juce::AudioChannelSet::stereo(), true)
                           .withInput ("Sidechain", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    delayMsParam = apvts.getRawParameterValue ("delayMs");
    delayInterpParam = apvts.getRawParameterValue ("delayInterp");
    polarityInvertParam = apvts.getRawParameterValue ("polarityInvert");
    phaseFilterEnabledParam = apvts.getRawParameterValue ("phaseFilterEnabled");
    rotatorFreqParam = apvts.getRawParameterValue ("rotatorFreq");
    rotatorQParam = apvts.getRawParameterValue ("rotatorQ");
    rotatorStagesParam = apvts.getRawParameterValue ("rotatorStages");
}

juce::AudioProcessorValueTreeState::ParameterLayout KickLockAudioProcessor::createParameterLayout()
{
    juce::AudioProcessorValueTreeState::ParameterLayout layout;

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "delayMs", 1 },
        "Audio Bass Delay",
        juce::NormalisableRange<float> (0.0f, 50.0f, 0.01f),
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
        juce::StringArray { "Phase Delta", "Overlay", "Separate" },
        0));

    layout.add (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { "visualOffsetSamples", 1 },
        "Visual Offset Samples",
        -4096, 4096, 0));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "polarityInvert", 1 },
        "Polarity Invert",
        false));

    layout.add (std::make_unique<juce::AudioParameterBool> (
        juce::ParameterID { "phaseFilterEnabled", 1 },
        "Phase Filter",
        false));

    layout.add (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { "rotatorFreq", 1 },
        "Rotator Frequency",
        juce::NormalisableRange<float> (20.0f, 2000.0f, 0.0f, 0.3f),
        200.0f));

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

    return layout;
}

void KickLockAudioProcessor::prepareToPlay (double sampleRate, int /*samplesPerBlock*/)
{
    for (int ch = 0; ch < 2; ++ch)
    {
        mainDelay[(size_t) ch].prepare (sampleRate, 50.0f);
        rotator[(size_t) ch].prepare (sampleRate, 2);
    }

    dryInputCorrelationMeter.prepare (sampleRate, (int) sampleRate / 20);
    processedCorrelationMeter.prepare (sampleRate, (int) sampleRate / 20);
    scopeFifo.prepare (8192);

    dryMainHighPass.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass (sampleRate, 30.0f);
    dryMainLowPass.coefficients  = juce::dsp::IIR::Coefficients<float>::makeLowPass  (sampleRate, 120.0f);
    dryKickHighPass.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass (sampleRate, 30.0f);
    dryKickLowPass.coefficients  = juce::dsp::IIR::Coefficients<float>::makeLowPass  (sampleRate, 120.0f);
    processedMainHighPass.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass (sampleRate, 30.0f);
    processedMainLowPass.coefficients  = juce::dsp::IIR::Coefficients<float>::makeLowPass  (sampleRate, 120.0f);
    processedKickHighPass.coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass (sampleRate, 30.0f);
    processedKickLowPass.coefficients  = juce::dsp::IIR::Coefficients<float>::makeLowPass  (sampleRate, 120.0f);
    dryMainHighPass.reset();
    dryMainLowPass.reset();
    dryKickHighPass.reset();
    dryKickLowPass.reset();
    processedMainHighPass.reset();
    processedMainLowPass.reset();
    processedKickHighPass.reset();
    processedKickLowPass.reset();
    phaseFilterWet.reset (sampleRate, 0.005);
    phaseFilterWet.setCurrentAndTargetValue (0.0f);

    // ~2 seconds of raw bass/kick for the Analyze button's cross-correlation.
    rawCapture.prepare ((int) (sampleRate * 2.0));
    transientDetector.prepare (sampleRate);
    transientDetector.setThreshold (0.004f);
    transientDetector.setMinimumEnergyGate (0.0004f);
    transientDetector.setAttackReleaseMs (2.0f, 60.0f);
    transientDetector.setHoldoffMs (90.0f);
    hitCapture.prepare (sampleRate, 25.0f, 180.0f);
    hitHistory.reset();

    // Decimate the scope feed so the UI ring can show a multi-beat view
    // without allocating on the audio thread.
    scopeDecimationFactor = juce::jmax (1, (int) (sampleRate / 2048.0));
    scopeDecimationCounter = 0;
    sidechainReferenceAvailable.store (false);
    tempoAvailable.store (false);
    latestBpm.store (0.0f);
    bassSignalRms.store (0.0f);
    kickSignalRms.store (0.0f);

    lastInterpChoice = 0;
    lastStageChoice = 0;
    lastRotatorFreq = -1.0f;
    lastRotatorQ = -1.0f;
    lastDelayActive = false;
    latestFixResult = {};
}

void KickLockAudioProcessor::releaseResources()
{
}

bool KickLockAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto mainIn = layouts.getMainInputChannelSet();
    const auto mainOut = layouts.getMainOutputChannelSet();

    if (mainIn.isDisabled() || mainOut.isDisabled())
        return false;

    if (mainIn != mainOut)
        return false;

    // Main and sidechain buses may each be mono or stereo only.
    for (const auto& bus : layouts.inputBuses)
        if (! bus.isDisabled() && bus != juce::AudioChannelSet::mono() && bus != juce::AudioChannelSet::stereo())
            return false;

    for (const auto& bus : layouts.outputBuses)
        if (! bus.isDisabled() && bus != juce::AudioChannelSet::mono() && bus != juce::AudioChannelSet::stereo())
            return false;

    return true;
}

bool KickLockAudioProcessor::isBassProcessingNeutral() const noexcept
{
    constexpr float epsilon = 1.0e-6f;

    const float delayMs = delayMsParam != nullptr ? delayMsParam->load() : 0.0f;
    const bool polarity = polarityInvertParam != nullptr && polarityInvertParam->load() > 0.5f;
    const bool phaseFilter = phaseFilterEnabledParam != nullptr && phaseFilterEnabledParam->load() > 0.5f;

    return normaliseBassDelayMs (delayMs) <= epsilon && ! polarity && ! phaseFilter;
}

void KickLockAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    bool bpmDetected = false;
    float bpmValue = 0.0f;

    if (auto* playHead = getPlayHead())
    {
        if (const auto position = playHead->getPosition())
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

            rawCapture.push (bassMono, kickMono);
            const bool transientDetected = transientDetector.processSample (kickMono);
            hitCapture.pushSample (bassMono, kickMono, transientDetected);

            const float lowBass = dryMainLowPass.processSample (
                dryMainHighPass.processSample (bassMono));
            const float lowKick = dryKickLowPass.processSample (
                dryKickHighPass.processSample (kickMono));
            dryInputCorrelationMeter.pushSample (lowBass, lowKick);
        }

        rawCapture.publishSnapshot();
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
    bassSignalRms.store ((float) std::sqrt (bassEnergySum / safeCount));
    kickSignalRms.store (hasSidechain ? (float) std::sqrt (kickEnergySum / safeCount) : 0.0f);

    const bool neutral = isBassProcessingNeutral();
    const float delayMs = normaliseBassDelayMs (delayMsParam->load());
    const bool delayActive = delayMs > 1.0e-6f;
    const bool phaseFilterEnabled = phaseFilterEnabledParam->load() > 0.5f;
    const bool phaseFadeOutActive = ! phaseFilterEnabled
        && (phaseFilterWet.isSmoothing() || phaseFilterWet.getCurrentValue() > 1.0e-5f);
    const bool processingNeeded = ! neutral || phaseFadeOutActive;

    if (! delayActive && lastDelayActive)
    {
        for (auto& delay : mainDelay)
            delay.reset();
    }
    lastDelayActive = delayActive;

    if (! processingNeeded)
    {
        phaseFilterWet.setCurrentAndTargetValue (0.0f);
    }
    else
    {
        // 1) Polarity invert on the main bus.
        if (polarityInvertParam->load() > 0.5f)
        {
            for (int ch = 0; ch < mainBuffer.getNumChannels(); ++ch)
                mainBuffer.applyGain (ch, 0, numSamples, -1.0f);
        }

        // 2) Fractional delay on the main/bass bus only. Do not run the delay
        // line at exactly zero delay; the neutral/default path must be a real
        // pass-through, not an assumed-transparent DSP path.
        if (delayActive)
        {
            const double delaySamples = delayMs / 1000.0 * getSampleRate();

            for (int ch = 0; ch < 2; ++ch)
                mainDelay[(size_t) ch].setDelaySamples ((float) delaySamples);

            const int interpChoice = (int) delayInterpParam->load();
            if (interpChoice != lastInterpChoice)
            {
                const auto type = interpChoice == 0 ? InterpolationType::Linear : InterpolationType::Allpass;

                for (int ch = 0; ch < 2; ++ch)
                    mainDelay[(size_t) ch].setInterpolationType (type);

                lastInterpChoice = interpChoice;
            }

            for (int ch = 0; ch < mainBuffer.getNumChannels(); ++ch)
                for (int i = 0; i < numSamples; ++i)
                    mainBuffer.setSample (ch, i, mainDelay[(size_t) ch].processSample (mainBuffer.getSample (ch, i)));
        }

        // 3) Allpass phase rotator on the main bus. Coefficients are updated
        // only on parameter-change edges, not in steady state. This still runs
        // makeAllPass/assignment on the audio thread for those edges; keep it
        // isolated here until the processor owns a non-audio-thread coefficient
        // handoff.
        if (phaseFilterEnabled || phaseFadeOutActive)
        {
            const int stageChoice = (int) rotatorStagesParam->load();
            if (phaseFilterEnabled && stageChoice != lastStageChoice)
            {
                for (int ch = 0; ch < 2; ++ch)
                    rotator[(size_t) ch].prepare (getSampleRate(), stageChoice + 2);

                lastStageChoice = stageChoice;
            }

            const float rotatorFreq = rotatorFreqParam->load();
            const float rotatorQ = rotatorQParam->load();
            if (phaseFilterEnabled && (rotatorFreq != lastRotatorFreq || rotatorQ != lastRotatorQ))
            {
                for (int ch = 0; ch < 2; ++ch)
                    rotator[(size_t) ch].setParameters (rotatorFreq, rotatorQ);

                lastRotatorFreq = rotatorFreq;
                lastRotatorQ = rotatorQ;
            }

            phaseFilterWet.setTargetValue (phaseFilterEnabled ? 1.0f : 0.0f);
            for (int i = 0; i < numSamples; ++i)
            {
                const float mix = phaseFilterWet.getNextValue();
                for (int ch = 0; ch < mainBuffer.getNumChannels(); ++ch)
                {
                    const float dry = mainBuffer.getSample (ch, i);
                    const float wet = rotator[(size_t) ch].processSample (dry);
                    mainBuffer.setSample (ch, i, dry + mix * (wet - dry));
                }
            }

            if (! phaseFilterEnabled
                && ! phaseFilterWet.isSmoothing()
                && phaseFilterWet.getCurrentValue() <= 1.0e-5f)
            {
                phaseFilterWet.setCurrentAndTargetValue (0.0f);
            }
        }
    }

    // 4) Feed the correlation meter and scope fifo with mono-summed
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

            const float lowMain = processedMainLowPass.processSample (
                processedMainHighPass.processSample (mainMono));
            const float lowKick = processedKickLowPass.processSample (
                processedKickHighPass.processSample (sidechainMono));

            processedCorrelationMeter.pushSample (lowMain, lowKick);

            // Decimate the scope feed only: the meter still sees every sample,
            // but the UI keeps a slower, longer history for the musical grid.
            if (++scopeDecimationCounter >= scopeDecimationFactor)
            {
                scopeDecimationCounter = 0;
                scopeFifo.pushSample (mainMono, sidechainMono);
            }
        }
    }

    const float dryMatch = hasSidechain ? dryInputCorrelationMeter.getCorrelation() : 50.0f;
    const float processedMatch = hasSidechain ? processedCorrelationMeter.getCorrelation() : 50.0f;
    const float activeMatch = hasSidechain ? (processingNeeded ? processedMatch : dryMatch) : 50.0f;

    dryInputMatchPercent.store (dryMatch);
    processedMatchPercent.store (processedMatch);
    realtimeLowBandMatchPercent.store (activeMatch);
    correlationPercent.store (activeMatch);
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
    return 1;
}

int KickLockAudioProcessor::getCurrentProgram()
{
    return 0;
}

void KickLockAudioProcessor::setCurrentProgram (int /*index*/)
{
}

const juce::String KickLockAudioProcessor::getProgramName (int /*index*/)
{
    return {};
}

void KickLockAudioProcessor::changeProgramName (int /*index*/, const juce::String& /*newName*/)
{
}

void KickLockAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void KickLockAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::unique_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));

    if (xml != nullptr && xml->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

AnalyzerInstruction KickLockAudioProcessor::analyzeAndApply (AnalyzeMode mode)
{
    const auto fix = analyzeFix();

    AnalyzerInstruction instruction;
    instruction.delayMs = fix.bassDelayMs;
    instruction.flipPolarity = fix.bassPolarityInvert;
    instruction.phaseFilterRecommended = fix.phaseFilterEnabled;
    instruction.phaseFilterFreqHz = fix.phaseFilterFreqHz;
    instruction.phaseFilterQ = fix.phaseFilterQ;
    instruction.phaseFilterStages = fix.phaseFilterStages;
    instruction.beforeMatchPercent = fix.beforeMatchPercent;
    instruction.afterMatchPercent = fix.afterMatchPercent;
    instruction.confidence = fix.confidence;
    instruction.message = fix.message;

    if (! fix.enoughSignal)
        instruction.action = AlignmentAction::NotEnoughSignal;
    else if (fix.largeTimingOffset)
        instruction.action = AlignmentAction::LowConfidence;
    else if (fix.requiresTimelineMove)
    {
        instruction.action = AlignmentAction::RecommendMoveKick;
        instruction.delayMs = -fix.suggestedKickMoveMs;
    }
    else if (fix.unstableRecommendation)
        instruction.action = AlignmentAction::LowConfidence;
    else if (fix.quality == PhaseFixQuality::AlreadyGood && fix.phaseFilterEnabled)
        instruction.action = AlignmentAction::RecommendPhaseFilter;
    else if (fix.quality == PhaseFixQuality::NoUsefulChange)
        instruction.action = AlignmentAction::LowConfidence;
    else if (fix.bassDelayMs > 0.02f)
        instruction.action = AlignmentAction::ApplyBassDelay;
    else if (fix.bassPolarityInvert)
        instruction.action = AlignmentAction::FlipBassPolarity;
    else if (fix.phaseFilterEnabled)
        instruction.action = AlignmentAction::RecommendPhaseFilter;

    if (mode == AnalyzeMode::AutoApplySafe)
        applyLatestFix();

    return instruction;
}

HitAnalysisResult KickLockAudioProcessor::analyzeLatestHit()
{
    std::vector<float> bass, kick;
    const int n = hitCapture.snapshotLatest (bass, kick);

    if (n <= 0)
    {
        HitAnalysisResult result;
        result.instruction.action = AlignmentAction::NotEnoughSignal;
        result.instruction.message = "Waiting for detected kick hit.";
        return result;
    }

    auto result = PerHitAnalyzer::analyzeHit (bass.data(), kick.data(), n,
                                              getSampleRate(), hitCapture.getSequence());

    if (! result.valid)
        return result;

    hitHistory.push (result);

    result.instruction.message << " | hit avg "
                               << juce::String ((int) std::round (hitHistory.getRollingMatchPercent())) << "%"
                               << " over " << juce::String (hitHistory.getCount());

    return result;
}

PhaseFixResult KickLockAudioProcessor::analyzeFix()
{
    std::vector<float> bass, kick;
    const int n = rawCapture.snapshot (bass, kick);
    const auto delayInterpolation = interpolationFromChoice (delayInterpParam != nullptr
                                                                 ? delayInterpParam->load()
                                                                 : 0.0f);

    if (n > 32)
    {
        const auto hits = extractRecentHitWindows (kick, getSampleRate(), 8);
        if (! hits.empty())
            appendHitWindows (bass, kick, hits, lastAnalyzedBassWindow, lastAnalyzedKickWindow);
        else
        {
            lastAnalyzedBassWindow = bass;
            lastAnalyzedKickWindow = kick;
        }

        latestFixResult = analyzeAggregatedHits (bass, kick, hits, getSampleRate(), delayInterpolation);
    }
    else
    {
        lastAnalyzedBassWindow.clear();
        lastAnalyzedKickWindow.clear();
        latestFixResult = {};
        PhaseFixEngine::updateDerivedResultFields (latestFixResult);
    }

    latestAnalyzedBeforePercent.store (latestFixResult.beforeMatchPercent);
    latestAnalyzedAfterPercent.store (latestFixResult.predictedAfterMatchPercent);
    latestVerifiedAfterPercent.store (latestFixResult.verifiedAfterMatchPercent);
    latestVerificationDeltaPercent.store (latestFixResult.verificationDeltaPercent);
    latestFixConfidence.store (latestFixResult.confidence * 100.0f);

    return latestFixResult;
}

bool KickLockAudioProcessor::applyLatestFix()
{
    if (! latestFixResult.applyAllowed
        && ! latestFixResult.optionalApplyAllowed)
    {
        return false;
    }

    if (auto* polParam = apvts.getParameter ("polarityInvert"))
        polParam->setValueNotifyingHost (latestFixResult.bassPolarityInvert ? 1.0f : 0.0f);

    if (auto* delayParam = apvts.getParameter ("delayMs"))
        delayParam->setValueNotifyingHost (
            delayParam->convertTo0to1 (normaliseBassDelayMs (latestFixResult.bassDelayMs)));

    if (auto* enabledParam = apvts.getParameter ("phaseFilterEnabled"))
        enabledParam->setValueNotifyingHost (latestFixResult.phaseFilterEnabled ? 1.0f : 0.0f);

    if (latestFixResult.phaseFilterEnabled)
    {
        if (auto* freqParam = apvts.getParameter ("rotatorFreq"))
            freqParam->setValueNotifyingHost (
                freqParam->convertTo0to1 (latestFixResult.phaseFilterFreqHz));

        if (auto* qParam = apvts.getParameter ("rotatorQ"))
            qParam->setValueNotifyingHost (
                qParam->convertTo0to1 (latestFixResult.phaseFilterQ));

        if (auto* stagesParam = apvts.getParameter ("rotatorStages"))
        {
            const int stageIndex = juce::jlimit (0, 2, latestFixResult.phaseFilterStages - 2);
            stagesParam->setValueNotifyingHost (stagesParam->convertTo0to1 ((float) stageIndex));
        }
    }

    if (! lastAnalyzedBassWindow.empty()
        && lastAnalyzedBassWindow.size() == lastAnalyzedKickWindow.size())
    {
        PhaseFixRenderSettings settings;
        settings.bassPolarityInvert = latestFixResult.bassPolarityInvert;
        settings.bassDelayMs = latestFixResult.bassDelayMs;
        settings.phaseFilterEnabled = latestFixResult.phaseFilterEnabled;
        settings.phaseFilterFreqHz = latestFixResult.phaseFilterFreqHz;
        settings.phaseFilterQ = latestFixResult.phaseFilterQ;
        settings.phaseFilterStages = latestFixResult.phaseFilterStages;
        settings.delayInterpolation = interpolationFromChoice (delayInterpParam != nullptr
                                                                   ? delayInterpParam->load()
                                                                   : 0.0f);

        const auto verified = PhaseFixEngine::scoreSettings (lastAnalyzedBassWindow.data(),
                                                             lastAnalyzedKickWindow.data(),
                                                             (int) lastAnalyzedBassWindow.size(),
                                                             getSampleRate(),
                                                             settings,
                                                             PhaseFixEngine::absoluteManualMaxDelayMs);

        PhaseFixEngine::applyVerification (latestFixResult, verified.matchPercent);
        latestVerifiedAfterPercent.store (latestFixResult.verifiedAfterMatchPercent);
        latestVerificationDeltaPercent.store (latestFixResult.verificationDeltaPercent);
    }

    return true;
}

PhaseFixResult KickLockAudioProcessor::getLatestFixResult() const
{
    return latestFixResult;
}

int KickLockAudioProcessor::getLatestHitSequence() const noexcept
{
    return hitCapture.getSequence();
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
    latestFixResult = result;
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new KickLockAudioProcessor();
}
