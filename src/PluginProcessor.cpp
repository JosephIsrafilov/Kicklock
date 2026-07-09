#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "dsp/FrequencyDomainPhaseRefiner.h"
#include "dsp/HitConsensus.h"
#include "dsp/MultiBandCorrelation.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace {
struct AnalysisHitWindow {
  int start = 0;
  int length = 0;
  int peak = 0;
};

float medianOf(std::vector<float> values) {
  if (values.empty())
    return 0.0f;

  std::sort(values.begin(), values.end());
  const auto mid = values.size() / 2;
  return values.size() % 2 == 0 ? 0.5f * (values[mid - 1] + values[mid])
                                : values[mid];
}

float averageOf(const std::vector<float> &values) {
  if (values.empty())
    return 0.0f;

  return std::accumulate(values.begin(), values.end(), 0.0f) /
         (float)values.size();
}

float standardDeviationOf(const std::vector<float> &values, float mean) {
  if (values.size() < 2)
    return 0.0f;

  float accum = 0.0f;
  for (auto value : values) {
    const float delta = value - mean;
    accum += delta * delta;
  }

  return std::sqrt(accum / (float)values.size());
}

InterpolationType interpolationFromChoice(float choiceValue) noexcept {
  return (int)std::lround(choiceValue) == 0 ? InterpolationType::Linear
                                            : InterpolationType::Allpass;
}

float rmsOf(const std::vector<float> &x) noexcept {
  if (x.empty())
    return 0.0f;

  double sum = 0.0;
  for (auto v : x)
    sum += (double)v * (double)v;

  return (float)std::sqrt(sum / (double)x.size());
}

void applyCrossoverPhaseSimulation(std::vector<float> &bass, double sampleRate,
                                   float crossoverHz) {
  if (bass.empty() || sampleRate <= 0.0)
    return;

  juce::dsp::LinkwitzRileyFilter<float> crossoverSim;
  crossoverSim.setType(juce::dsp::LinkwitzRileyFilterType::allpass);
  crossoverSim.prepare({sampleRate, (juce::uint32)juce::jmax(1, (int)bass.size()), 1});
  crossoverSim.setCutoffFrequency(crossoverHz);
  crossoverSim.reset();

  for (auto &sample : bass)
    sample = crossoverSim.processSample(0, sample);
}

// P6: when analysis can't produce a usable fix, say specifically what's
// missing rather than a generic "waiting for signal". Inspects the captured
// window so the message names the actual absent input (no kick / no bass /
// not enough material) instead of guessing.
void refineInsufficientSignalMessage(PhaseFixResult &result,
                                     const std::vector<float> &bass,
                                     const std::vector<float> &kick,
                                     int numSamples, double sampleRate) {
  if (result.enoughSignal)
    return;

  const int minMaterial = (int)(sampleRate * 0.3);
  if (numSamples < minMaterial) {
    result.message = "Not enough material captured yet. Keep the loop playing, "
                     "then analyze again.";
    return;
  }

  constexpr float presenceFloor = 1.5e-3f;
  const bool hasKick = rmsOf(kick) >= presenceFloor;
  const bool hasBass = rmsOf(bass) >= presenceFloor;

  if (!hasKick && !hasBass)
    result.message = "No kick or bass detected. Feed a kick into the sidechain "
                     "and bass into the main input.";
  else if (!hasKick)
    result.message = "No kick detected on the sidechain. Route the kick to the "
                     "sidechain input.";
  else if (!hasBass)
    result.message = "No bass detected on the main input. Route the bass "
                     "through the plugin.";
  else
    result.message = "Kick and bass are present but too quiet for a reliable "
                     "low-end phase read.";
}

std::vector<AnalysisHitWindow>
extractRecentHitWindows(const std::vector<float> &kick, double sampleRate,
                        int maxHits = 8) {
  std::vector<AnalysisHitWindow> hits;
  if (kick.size() < 128 || sampleRate <= 0.0)
    return hits;

  float peak = 0.0f;
  for (auto sample : kick)
    peak = std::max(peak, std::abs(sample));

  if (peak < 1.0e-4f)
    return hits;

  const auto timeMsToCoeff = [sampleRate](float ms) noexcept {
    const double samples = std::max(1.0, sampleRate * (double)ms / 1000.0);
    return (float)std::exp(-1.0 / samples);
  };

  const float peakEnergy = peak * peak;
  const float threshold = std::max(peakEnergy * 0.08f, 1.0e-8f);
  const float minimumEnergyGate = std::max(peakEnergy * 0.02f, 1.0e-10f);
  const float attackCoeff = timeMsToCoeff(2.0f);
  const float releaseCoeff = timeMsToCoeff(60.0f);
  const int holdoffSamples =
      juce::jmax(1, (int)std::lround(sampleRate * 0.140));
  const int peakSearch = juce::jmax(1, (int)std::lround(sampleRate * 0.008));
  const int preSamples = juce::jmax(1, (int)std::lround(sampleRate * 0.006));
  const int postSamples = juce::jmax(64, (int)std::lround(sampleRate * 0.110));

  float envelope = 0.0f;
  bool wasAbove = false;
  int holdoffRemaining = 0;

  for (int i = 0; i < (int)kick.size(); ++i) {
    const float energy = kick[(size_t)i] * kick[(size_t)i];
    const float coeff = energy > envelope ? attackCoeff : releaseCoeff;
    envelope = coeff * envelope + (1.0f - coeff) * energy;

    if (holdoffRemaining > 0)
      --holdoffRemaining;

    const bool above = envelope >= threshold && envelope >= minimumEnergyGate;
    const bool detected = above && !wasAbove && holdoffRemaining <= 0;
    wasAbove = above;

    if (!detected)
      continue;

    holdoffRemaining = holdoffSamples;

    int localPeak = i;
    float localEnergy = energy;
    const int searchEnd = juce::jmin((int)kick.size(), i + peakSearch);
    for (int j = i + 1; j < searchEnd; ++j) {
      const float e = kick[(size_t)j] * kick[(size_t)j];
      if (e > localEnergy) {
        localEnergy = e;
        localPeak = j;
      }
    }

    AnalysisHitWindow hit;
    hit.peak = localPeak;
    hit.start = juce::jlimit(0, (int)kick.size() - 1, localPeak - preSamples);
    hit.length =
        std::min((int)kick.size() - hit.start, preSamples + postSamples);

    if (hit.length > 96 && localEnergy >= minimumEnergyGate)
      hits.push_back(hit);
  }

  if ((int)hits.size() > maxHits)
    hits.erase(hits.begin(), hits.end() - maxHits);

  return hits;
}

void appendHitWindows(const std::vector<float> &bass,
                      const std::vector<float> &kick,
                      const std::vector<AnalysisHitWindow> &hits,
                      std::vector<float> &bassOut,
                      std::vector<float> &kickOut) {
  bassOut.clear();
  kickOut.clear();

  size_t totalSamples = 0;
  for (const auto &hit : hits)
    if (hit.start >= 0 && hit.length > 0 &&
        hit.start + hit.length <= (int)bass.size() &&
        hit.start + hit.length <= (int)kick.size()) {
      totalSamples += (size_t)hit.length;
    }

  bassOut.reserve(totalSamples);
  kickOut.reserve(totalSamples);

  for (const auto &hit : hits) {
    if (hit.start < 0 || hit.length <= 0 ||
        hit.start + hit.length > (int)bass.size() ||
        hit.start + hit.length > (int)kick.size()) {
      continue;
    }

    bassOut.insert(bassOut.end(), bass.begin() + hit.start,
                   bass.begin() + hit.start + hit.length);
    kickOut.insert(kickOut.end(), kick.begin() + hit.start,
                   kick.begin() + hit.start + hit.length);
  }
}

PhaseFixResult analyzeAggregatedHits(const std::vector<float> &bass,
                                     const std::vector<float> &kick,
                                     const std::vector<AnalysisHitWindow> &hits,
                                     double sampleRate,
                                     InterpolationType delayInterpolation) {
  PhaseFixResult aggregated;

  if (bass.empty() || kick.empty()) {
    PhaseFixEngine::updateDerivedResultFields(aggregated);
    return aggregated;
  }

  if (hits.empty()) {
    aggregated = PhaseFixEngine::analyze(
        bass.data(), kick.data(), (int)bass.size(), sampleRate,
        PhaseFixEngine::defaultAutoFixMaxDelayMs, delayInterpolation);
    aggregated.contributingHits = aggregated.enoughSignal ? 1 : 0;
    aggregated.singleHitAnalysis = true;
    aggregated.confidence *= 0.85f;
    PhaseFixEngine::updateDerivedResultFields(aggregated);
    if (aggregated.enoughSignal &&
        !aggregated.message.containsIgnoreCase("single-hit"))
      aggregated.message << " Single-hit analysis.";
    return aggregated;
  }

  std::vector<PhaseFixResult> perHitResults;
  std::vector<HitObservation> hitObservations;
  std::vector<FrequencyDomainPhaseRefiner::Hit> refinementHits;
  perHitResults.reserve(hits.size());
  hitObservations.reserve(hits.size());
  refinementHits.reserve(hits.size());

  std::vector<float> allHitBass;
  std::vector<float> allHitKick;
  appendHitWindows(bass, kick, hits, allHitBass, allHitKick);

  for (int i = 0; i < (int)hits.size(); ++i) {
    const auto &hit = hits[(size_t)i];
    if (hit.start < 0 || hit.length <= 0 ||
        hit.start + hit.length > (int)bass.size() ||
        hit.start + hit.length > (int)kick.size()) {
      continue;
    }

    auto hitResult = PhaseFixEngine::analyze(
        bass.data() + hit.start, kick.data() + hit.start, hit.length,
        sampleRate, PhaseFixEngine::defaultAutoFixMaxDelayMs,
        delayInterpolation);

    if (hitResult.enoughSignal) {
      perHitResults.push_back(hitResult);
      refinementHits.push_back(
          {bass.data() + hit.start, kick.data() + hit.start, hit.length,
           hitResult.bassDelayMs * (float)sampleRate / 1000.0f,
           hitResult.bassPolarityInvert});

      auto multiBandResult = MultiBandCorrelation::analyze(
          bass.data() + hit.start, kick.data() + hit.start, hit.length,
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
      obs.energy =
          PhaseBands::table[0].weight * multiBandResult.bands[0].kickEnergy +
          PhaseBands::table[1].weight * multiBandResult.bands[1].kickEnergy +
          PhaseBands::table[2].weight * multiBandResult.bands[2].kickEnergy +
          PhaseBands::table[3].weight * multiBandResult.bands[3].kickEnergy;

      float bestBandEnergy = -1.0f;
      for (int band = 0; band < PhaseBands::lowEndBandCount; ++band) {
        const auto &bandDef = PhaseBands::table[(size_t)band];
        const float bandEnergy = multiBandResult.bands[(size_t)band].kickEnergy;
        if (bandEnergy > bestBandEnergy) {
          bestBandEnergy = bandEnergy;
          obs.dominantFrequencyHz = 0.5f * (bandDef.lowHz + bandDef.highHz);
        }
      }

      hitObservations.push_back(obs);
    }
  }

  if (perHitResults.empty()) {
    const auto *fallbackBass =
        allHitBass.empty() ? bass.data() : allHitBass.data();
    const auto *fallbackKick =
        allHitKick.empty() ? kick.data() : allHitKick.data();
    const int fallbackSamples =
        allHitBass.empty() ? (int)bass.size() : (int)allHitBass.size();

    aggregated = PhaseFixEngine::analyze(
        fallbackBass, fallbackKick, fallbackSamples, sampleRate,
        PhaseFixEngine::defaultAutoFixMaxDelayMs, delayInterpolation);
    aggregated.contributingHits = 0;
    PhaseFixEngine::updateDerivedResultFields(aggregated);
    return aggregated;
  }

  const auto refined =
      FrequencyDomainPhaseRefiner::refine(refinementHits, sampleRate);
  if (refined.valid && refined.delaySamples.size() == perHitResults.size()) {
    for (size_t i = 0; i < refined.delaySamples.size(); ++i) {
      const float delayMs =
          refined.delaySamples[i] * 1000.0f / (float)sampleRate;
      perHitResults[i].bassDelayMs = delayMs;
      hitObservations[i].delayMs = delayMs;
    }
  }

  // --- NEW CLUSTERING CONSENSUS ---
  auto consensus = HitConsensus::analyze(hitObservations);

  aggregated.valid = true;
  aggregated.enoughSignal = true;
  aggregated.contributingHits = (int)perHitResults.size();
  aggregated.singleHitAnalysis = perHitResults.size() == 1;

  if (consensus.hasConsensus) {
    const auto &domCluster =
        consensus.clusters[(size_t)consensus.dominantClusterIndex];
    aggregated.unstableRecommendation =
        consensus.dominantClusterShare <= 0.50f ||
        (perHitResults.size() < 4 && consensus.dominantClusterShare < 0.70f);

    aggregated.bassPolarityInvert = domCluster.centroidPolarity;
    aggregated.bassDelayMs = domCluster.centroidDelayMs;
    aggregated.phaseFilterEnabled = domCluster.centroidPhaseEnabled;
    aggregated.phaseFilterFreqHz = domCluster.centroidPhaseEnabled
                                       ? domCluster.centroidPhaseFreqHz
                                       : 200.0f;
    aggregated.phaseFilterQ = 0.7f;
    aggregated.phaseFilterStages = 2; // Keep simple for now
    aggregated.confidence = consensus.consensusConfidence;
  } else {
    aggregated.unstableRecommendation = perHitResults.size() > 1;

    // Priority 2: Highest energy single hit
    int bestIdx = 0;
    float maxE = -1.0f;
    for (size_t i = 0; i < hitObservations.size(); ++i) {
      if (hitObservations[i].energy > maxE) {
        maxE = hitObservations[i].energy;
        bestIdx = (int)i;
      }
    }
    aggregated.bassPolarityInvert =
        hitObservations[(size_t)bestIdx].polarityInvert;
    aggregated.bassDelayMs = hitObservations[(size_t)bestIdx].delayMs;
    aggregated.phaseFilterEnabled =
        hitObservations[(size_t)bestIdx].phaseFilterEnabled;
    aggregated.phaseFilterFreqHz =
        hitObservations[(size_t)bestIdx].phaseFilterFreqHz;
    aggregated.phaseFilterQ = 0.7f;
    aggregated.phaseFilterStages = 2;
    aggregated.confidence = hitObservations[(size_t)bestIdx].signalConfidence *
                            0.4f; // Penalized fallback
  }

  // Clamp the delay to the fixed-PDC budget, but keep track if we exceeded it.
  if (std::abs(aggregated.bassDelayMs) >
      PhaseFixEngine::defaultAutoFixMaxDelayMs + 0.25f) {
    aggregated.largeTimingOffset = true;
    aggregated.detectedTimingOffsetMs = std::abs(aggregated.bassDelayMs);
    aggregated.bassDelayMs = juce::jlimit(
        -PhaseFixEngine::defaultAutoFixMaxDelayMs,
        PhaseFixEngine::defaultAutoFixMaxDelayMs, aggregated.bassDelayMs);
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

  for (const auto &hit : hits) {
    if (hit.start < 0 || hit.length <= 0 ||
        hit.start + hit.length > (int)bass.size() ||
        hit.start + hit.length > (int)kick.size()) {
      continue;
    }

    const auto before = PhaseFixEngine::scoreSettings(
        bass.data() + hit.start, kick.data() + hit.start, hit.length,
        sampleRate, {}, PhaseFixEngine::absoluteManualMaxDelayMs);
    const auto after = PhaseFixEngine::scoreSettings(
        bass.data() + hit.start, kick.data() + hit.start, hit.length,
        sampleRate, settings, PhaseFixEngine::absoluteManualMaxDelayMs);

    sumBeforeMatch += before.matchPercent;
    sumAfterMatch += after.matchPercent;
    numScores++;
  }

  if (numScores > 0) {
    aggregated.beforeMatchPercent = sumBeforeMatch / (float)numScores;
    aggregated.afterMatchPercent = sumAfterMatch / (float)numScores;
  } else if (!bass.empty() && !kick.empty()) {
    const auto before = PhaseFixEngine::scoreSettings(
        bass.data(), kick.data(), (int)bass.size(), sampleRate, {},
        PhaseFixEngine::absoluteManualMaxDelayMs);
    const auto after = PhaseFixEngine::scoreSettings(
        bass.data(), kick.data(), (int)bass.size(), sampleRate, settings,
        PhaseFixEngine::absoluteManualMaxDelayMs);
    aggregated.beforeMatchPercent = before.matchPercent;
    aggregated.afterMatchPercent = after.matchPercent;
  } else {
    aggregated.beforeMatchPercent = 0.0f;
    aggregated.afterMatchPercent = 0.0f;
  }

  aggregated.predictedAfterMatchPercent = aggregated.afterMatchPercent;

  const auto beforeFull = PhaseFixEngine::scoreSettings(
      bass.data(), kick.data(), (int)bass.size(), sampleRate, {},
      PhaseFixEngine::absoluteManualMaxDelayMs);
  const auto afterFull = PhaseFixEngine::scoreSettings(
      bass.data(), kick.data(), (int)bass.size(), sampleRate, settings,
      PhaseFixEngine::absoluteManualMaxDelayMs);

  aggregated.displayBeforeMatchPercent = beforeFull.matchPercent;
  aggregated.displayAfterMatchPercent = afterFull.matchPercent;

  aggregated.improvementPercent =
      aggregated.afterMatchPercent - aggregated.beforeMatchPercent;

  PhaseFixEngine::updateDerivedResultFields(aggregated);

  if (aggregated.singleHitAnalysis && aggregated.enoughSignal &&
      !aggregated.message.containsIgnoreCase("single-hit")) {
    aggregated.message << " Single-hit analysis.";
  }

  return aggregated;
}
} // namespace

class KickLockAudioProcessor::AutoAlignEngine : public juce::Thread {
public:
  explicit AutoAlignEngine(KickLockAudioProcessor &p)
      : juce::Thread("KickLock Auto-Align"), owner(p) {
    startThread();
  }

  ~AutoAlignEngine() override {
    // Invalidate the liveness token FIRST: a result lambda already queued
    // on the message loop would otherwise dereference the owner processor
    // after this destructor chain (which runs on that same message thread)
    // has finished tearing it down.
    alive->store(false);
    signalThreadShouldExit();
    notify();
    stopThread(2000);
  }

  void prepare(double newSampleRate) {
    sampleRate = newSampleRate;
    captureSamples = juce::jmax(1, (int)std::ceil(sampleRate * 0.050));
    maxLagSamples = juce::jmax(1, (int)std::ceil(sampleRate * 0.020));

    mainCapture.assign((size_t)captureSamples, 0.0f);
    sideCapture.assign((size_t)captureSamples, 0.0f);
    captureIndex.store(0, std::memory_order_release);
    state.store(State::Idle, std::memory_order_release);
  }

  void requestCapture() noexcept {
    if (state.load(std::memory_order_acquire) == State::Idle) {
      captureIndex.store(0, std::memory_order_release);
      state.store(State::Armed, std::memory_order_release);
    }
  }

  void pushSample(float mainSample, float sidechainSample) noexcept {
    auto current = state.load(std::memory_order_acquire);

    if (current == State::Armed) {
      if (std::abs(sidechainSample) < triggerThreshold)
        return;

      captureIndex.store(0, std::memory_order_release);
      current = State::Capturing;
      state.store(current, std::memory_order_release);
    }

    if (current != State::Capturing)
      return;

    const auto index = captureIndex.load(std::memory_order_relaxed);
    if (index >= captureSamples)
      return;

    mainCapture[(size_t)index] = mainSample;
    sideCapture[(size_t)index] = sidechainSample;

    const auto next = index + 1;
    captureIndex.store(next, std::memory_order_release);

    if (next >= captureSamples) {
      state.store(State::Analyzing, std::memory_order_release);
      notify();
    }
  }

  void run() override {
    while (!threadShouldExit()) {
      if (state.load(std::memory_order_acquire) != State::Analyzing) {
        wait(10);
        continue;
      }

      const auto result = analyzeCapturedBuffers();
      state.store(State::Idle, std::memory_order_release);

      // The async lambda may fire after the processor (and this engine)
      // is destroyed — the destructor runs on the same message thread the
      // lambda is queued on, so `this` alone would dangle. The shared
      // liveness token outlives the engine and gates the dereference.
      juce::MessageManager::callAsync([this, result, aliveToken = alive] {
        if (aliveToken->load())
          applyResultToParameters(result);
      });
    }
  }

private:
  enum class State { Idle, Armed, Capturing, Analyzing };

  struct Result {
    float delayMs = 0.0f;
    bool invertPolarity = false;
    bool valid = false;
  };

  Result analyzeCapturedBuffers() const {
    Result result;
    double bestAbsCorrelation = 0.0;
    double bestSignedCorrelation = 0.0;
    int bestLag = 0;

    for (int lag = -maxLagSamples; lag <= maxLagSamples; ++lag) {
      double xy = 0.0;
      double xx = 0.0;
      double yy = 0.0;
      int count = 0;

      for (int i = 0; i < captureSamples; ++i) {
        const int mainIndex = i - lag;
        if (mainIndex < 0 || mainIndex >= captureSamples)
          continue;

        const auto x = (double)mainCapture[(size_t)mainIndex];
        const auto y = (double)sideCapture[(size_t)i];
        xy += x * y;
        xx += x * x;
        yy += y * y;
        ++count;
      }

      if (count < 16)
        continue;

      const auto denom = std::sqrt(xx * yy);
      if (denom <= 1.0e-12)
        continue;

      const auto corr = xy / denom;
      const auto absCorr = std::abs(corr);

      if (absCorr > bestAbsCorrelation) {
        bestAbsCorrelation = absCorr;
        bestSignedCorrelation = corr;
        bestLag = lag;
      }
    }

    if (bestAbsCorrelation > 0.05) {
      result.valid = true;
      result.delayMs = (float)((double)bestLag * 1000.0 / sampleRate);
      result.delayMs = juce::jlimit(-20.0f, 20.0f, result.delayMs);
      result.invertPolarity = bestSignedCorrelation < 0.0;
    }

    return result;
  }

  void applyResultToParameters(Result result) {
    if (!result.valid)
      return;

    setParameter("delay_ms", result.delayMs);
    setParameter("delayMs", result.delayMs);
    setParameter("polarity_invert", result.invertPolarity ? 1.0f : 0.0f);
    setParameter("polarityInvert", result.invertPolarity ? 1.0f : 0.0f);
  }

  void setParameter(const char *id, float value) {
    if (auto *parameter = owner.apvts.getParameter(id)) {
      parameter->beginChangeGesture();
      parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
      parameter->endChangeGesture();
    }
  }

  KickLockAudioProcessor &owner;

  // Liveness token shared with any pending message-thread result lambdas;
  // flipped false in the destructor so a late lambda never touches `owner`.
  std::shared_ptr<std::atomic<bool>> alive =
      std::make_shared<std::atomic<bool>>(true);
  double sampleRate = 44100.0;
  int captureSamples = 1;
  int maxLagSamples = 1;
  static constexpr float triggerThreshold = 0.25118864f; // -12 dBFS

  std::vector<float> mainCapture;
  std::vector<float> sideCapture;
  std::atomic<State> state{State::Idle};
  std::atomic<int> captureIndex{0};
};

KickLockAudioProcessor::KickLockAudioProcessor()
    : AudioProcessor(
          BusesProperties()
              .withInput("Main Input", juce::AudioChannelSet::stereo(), true)
              .withInput("Sidechain Input", juce::AudioChannelSet::stereo(),
                         true)
              .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      apvts(*this, nullptr, "PARAMETERS", createParameterLayout()) {
  delayMsParam = apvts.getRawParameterValue("delay_ms");
  delayMsLegacyParam = apvts.getRawParameterValue("delayMs");
  delayInterpParam = apvts.getRawParameterValue("delayInterp");
  polarityInvertParam = apvts.getRawParameterValue("polarity_invert");
  polarityInvertLegacyParam = apvts.getRawParameterValue("polarityInvert");
  phaseFilterEnabledParam = apvts.getRawParameterValue("allpass_enable");
  phaseFilterEnabledLegacyParam =
      apvts.getRawParameterValue("phaseFilterEnabled");
  rotatorFreqParam = apvts.getRawParameterValue("allpass_freq");
  rotatorFreqLegacyParam = apvts.getRawParameterValue("rotatorFreq");
  rotatorQParam = apvts.getRawParameterValue("rotatorQ");
  rotatorStagesParam = apvts.getRawParameterValue("rotatorStages");
  crossoverFreqParam = apvts.getRawParameterValue("crossover_freq");
  crossoverEnableParamRaw = apvts.getRawParameterValue("crossover_enable");
  pitchTrackParam = apvts.getRawParameterValue("pitch_track");

  for (const auto *id :
       {"delay_ms", "delayMs", "polarity_invert", "polarityInvert",
        "allpass_enable", "phaseFilterEnabled", "allpass_freq", "rotatorFreq"})
    apvts.addParameterListener(id, this);

  autoAlignEngine = std::make_unique<AutoAlignEngine>(*this);
}

KickLockAudioProcessor::~KickLockAudioProcessor() {
  for (const auto *id :
       {"delay_ms", "delayMs", "polarity_invert", "polarityInvert",
        "allpass_enable", "phaseFilterEnabled", "allpass_freq", "rotatorFreq"})
    apvts.removeParameterListener(id, this);

  // The background analysis job captures `this`. Remove any queued job and
  // wait for a running one to finish before the members it touches (the
  // capture buffer, the result fields) start tearing down.
  analysisThreadPool.removeAllJobs(true, 2000);
  autoAlignEngine.reset();
}

void KickLockAudioProcessor::parameterChanged(const juce::String &parameterID,
                                              float) {
  const auto stamp =
      parameterChangeCounter.fetch_add(1, std::memory_order_relaxed) + 1;

  if (parameterID == "delay_ms") {
    delayCanonicalChange.store(stamp, std::memory_order_release);
    return;
  }
  if (parameterID == "delayMs") {
    delayLegacyChange.store(stamp, std::memory_order_release);
    return;
  }
  if (parameterID == "polarity_invert") {
    polarityCanonicalChange.store(stamp, std::memory_order_release);
    return;
  }
  if (parameterID == "polarityInvert") {
    polarityLegacyChange.store(stamp, std::memory_order_release);
    return;
  }
  if (parameterID == "allpass_enable") {
    phaseCanonicalChange.store(stamp, std::memory_order_release);
    return;
  }
  if (parameterID == "phaseFilterEnabled") {
    phaseLegacyChange.store(stamp, std::memory_order_release);
    return;
  }
  if (parameterID == "allpass_freq") {
    allpassFreqCanonicalChange.store(stamp, std::memory_order_release);
    return;
  }
  if (parameterID == "rotatorFreq") {
    allpassFreqLegacyChange.store(stamp, std::memory_order_release);
    return;
  }
}

void KickLockAudioProcessor::markRestoredParameterSources(
    bool hasDelayMs, bool hasLegacyDelayMs, bool hasPolarityInvert,
    bool hasLegacyPolarityInvert, bool hasAllpassEnable,
    bool hasLegacyPhaseFilterEnabled, bool hasAllpassFreq,
    bool hasLegacyRotatorFreq) noexcept {
  auto markPair = [this](bool hasCanonical, bool hasLegacy,
                         std::atomic<uint32_t> &canonicalStamp,
                         std::atomic<uint32_t> &legacyStamp) noexcept {
    const auto base =
        parameterChangeCounter.fetch_add(2, std::memory_order_relaxed) + 1;

    if (!hasCanonical && hasLegacy) {
      canonicalStamp.store(base, std::memory_order_release);
      legacyStamp.store(base + 1, std::memory_order_release);
      return;
    }

    legacyStamp.store(base, std::memory_order_release);
    canonicalStamp.store(base + 1, std::memory_order_release);
  };

  markPair(hasDelayMs, hasLegacyDelayMs, delayCanonicalChange,
           delayLegacyChange);
  markPair(hasPolarityInvert, hasLegacyPolarityInvert, polarityCanonicalChange,
           polarityLegacyChange);
  markPair(hasAllpassEnable, hasLegacyPhaseFilterEnabled, phaseCanonicalChange,
           phaseLegacyChange);
  markPair(hasAllpassFreq, hasLegacyRotatorFreq, allpassFreqCanonicalChange,
           allpassFreqLegacyChange);
}

float KickLockAudioProcessor::getEffectiveDelayMs() const noexcept {
  const bool useLegacy = delayLegacyChange.load(std::memory_order_acquire) >
                         delayCanonicalChange.load(std::memory_order_acquire);

  if (useLegacy && delayMsLegacyParam != nullptr)
    return delayMsLegacyParam->load();

  if (delayMsParam != nullptr)
    return delayMsParam->load();

  return delayMsLegacyParam != nullptr ? delayMsLegacyParam->load() : 0.0f;
}

bool KickLockAudioProcessor::getEffectivePolarityInvert() const noexcept {
  const bool useLegacy =
      polarityLegacyChange.load(std::memory_order_acquire) >
      polarityCanonicalChange.load(std::memory_order_acquire);

  if (useLegacy && polarityInvertLegacyParam != nullptr)
    return polarityInvertLegacyParam->load() > 0.5f;

  if (polarityInvertParam != nullptr)
    return polarityInvertParam->load() > 0.5f;

  return polarityInvertLegacyParam != nullptr &&
         polarityInvertLegacyParam->load() > 0.5f;
}

bool KickLockAudioProcessor::getEffectivePhaseFilterEnabled() const noexcept {
  const bool useLegacy = phaseLegacyChange.load(std::memory_order_acquire) >
                         phaseCanonicalChange.load(std::memory_order_acquire);

  if (useLegacy && phaseFilterEnabledLegacyParam != nullptr)
    return phaseFilterEnabledLegacyParam->load() > 0.5f;

  if (phaseFilterEnabledParam != nullptr)
    return phaseFilterEnabledParam->load() > 0.5f;

  return phaseFilterEnabledLegacyParam != nullptr &&
         phaseFilterEnabledLegacyParam->load() > 0.5f;
}

float KickLockAudioProcessor::getEffectiveAllpassFreqHz() const noexcept {
  const bool useLegacy =
      allpassFreqLegacyChange.load(std::memory_order_acquire) >
      allpassFreqCanonicalChange.load(std::memory_order_acquire);

  if (useLegacy && rotatorFreqLegacyParam != nullptr)
    return rotatorFreqLegacyParam->load();

  if (rotatorFreqParam != nullptr)
    return rotatorFreqParam->load();

  return rotatorFreqLegacyParam != nullptr ? rotatorFreqLegacyParam->load()
                                           : 50.0f;
}

juce::AudioProcessorValueTreeState::ParameterLayout
KickLockAudioProcessor::createParameterLayout() {
  juce::AudioProcessorValueTreeState::ParameterLayout layout;

  // Human-readable value text. The slider attachments install each
  // parameter's text conversion as the slider's textFromValueFunction, so
  // without these the knob textboxes (and host automation lanes) show raw
  // float precision like "236.1548004".
  const auto signedMsText = [](float value, int) {
    return formatSignedDelayMs(value);
  };
  const auto wholeHzText = [](float value, int) {
    return juce::String((int)std::lround(value)) + " Hz";
  };
  const auto qText = [](float value, int) { return juce::String(value, 2); };

  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"crossover_enable", 1}, "Crossover Enable", false));

  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"delay_ms", 1}, "Delay",
      juce::NormalisableRange<float>(-20.0f, 20.0f, 0.01f), 0.0f,
      juce::AudioParameterFloatAttributes().withStringFromValueFunction(
          signedMsText)));

  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"polarity_invert", 1}, "Polarity Invert", false));

  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"allpass_freq", 1}, "Allpass Frequency",
      juce::NormalisableRange<float>(20.0f, 500.0f, 0.0f, 0.35f), 50.0f,
      juce::AudioParameterFloatAttributes().withStringFromValueFunction(
          wholeHzText)));

  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"allpass_enable", 1}, "Allpass Enable", false));

  // Dynamic pitch follow: when on (and the phase filter is enabled), the
  // allpass centre frequency continuously follows the bass's detected
  // fundamental instead of the static Phase Freq knob, so the phase
  // correction stays on the note as the bassline moves.
  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"pitch_track", 1}, "Pitch Follow", false));

  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"delayMs", 1}, "Legacy Audio Bass Delay",
      juce::NormalisableRange<float>(-20.0f, 20.0f, 0.01f), 0.0f,
      juce::AudioParameterFloatAttributes().withStringFromValueFunction(
          signedMsText)));

  layout.add(std::make_unique<juce::AudioParameterChoice>(
      juce::ParameterID{"delayInterp", 1}, "Delay Interpolation",
      juce::StringArray{"Linear", "Allpass"}, 0));

  // Default to a beat-synced 1/4 grid (ReVision-style): with host tempo the
  // scrolling views then show one quarter-note per screen — a stationary,
  // readable window for a looping pattern — instead of the entire multi-
  // second history squashed into the width. "ms" mode remains selectable.
  layout.add(std::make_unique<juce::AudioParameterChoice>(
      juce::ParameterID{"gridDivision", 1}, "Grid Division",
      juce::StringArray{"1/4", "1/2", "1", "4", "Bar", "ms"}, 0));

  layout.add(std::make_unique<juce::AudioParameterChoice>(
      juce::ParameterID{"scopeViewMode", 1}, "Scope View",
      juce::StringArray{"Triggered", "Free-run", "Phase Delta", "Spectrum",
                        "Separate"},
      0));

  layout.add(std::make_unique<juce::AudioParameterInt>(
      juce::ParameterID{"visualOffsetSamples", 1}, "Visual Offset Samples",
      -4096, 4096, 0));

  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"polarityInvert", 1}, "Legacy Polarity Invert", false));

  layout.add(std::make_unique<juce::AudioParameterBool>(
      juce::ParameterID{"phaseFilterEnabled", 1}, "Phase Filter", false));

  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"rotatorFreq", 1}, "Legacy Rotator Frequency",
      juce::NormalisableRange<float>(20.0f, 500.0f, 0.0f, 0.35f), 50.0f,
      juce::AudioParameterFloatAttributes().withStringFromValueFunction(
          wholeHzText)));

  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"rotatorQ", 1}, "Rotator Q",
      juce::NormalisableRange<float>(0.1f, 10.0f, 0.01f), 0.7f,
      juce::AudioParameterFloatAttributes().withStringFromValueFunction(
          qText)));

  layout.add(std::make_unique<juce::AudioParameterChoice>(
      juce::ParameterID{"rotatorStages", 1}, "Rotator Stages",
      juce::StringArray{"2", "3", "4"}, 0));

  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"crossover_freq", 1}, "Crossover Frequency",
      juce::NormalisableRange<float>(40.0f, 500.0f, 0.0f, 0.35f), 150.0f,
      juce::AudioParameterFloatAttributes().withStringFromValueFunction(
          wholeHzText)));

  return layout;
}

void KickLockAudioProcessor::prepareToPlay(double sampleRate,
                                           int samplesPerBlock) {
  const int numChannels = juce::jmax(1, getTotalNumOutputChannels());
  analysisBuffer.setSize(numChannels, samplesPerBlock);
  sidechainMonoScratch.setSize(1, samplesPerBlock, false, false, true);

  multibandCore.prepare(sampleRate, samplesPerBlock, numChannels, 20.0f);
  const auto maxDelaySamples = multibandCore.reportLatencySamples();
  setLatencySamples(maxDelaySamples);

  if (autoAlignEngine != nullptr)
    autoAlignEngine->prepare(sampleRate);

  // Live multi-band phase meters (P5). They band-pass internally, so no
  // separate pre-filters are needed. ~0.25 s rolling window per band.
  // Meters are fed every meterDecimationFactor samples, so scale the window
  // (and EMA) to keep the same real-time time constant.
  const int liveWindow = juce::jmax(128, (int)(sampleRate * 0.25) / meterDecimationFactor);
  dryMultiBandMeter.prepare(sampleRate / (double) meterDecimationFactor, liveWindow);
  processedMultiBandMeter.prepare(sampleRate / (double) meterDecimationFactor, liveWindow);
  processedMeterSidechainDelay.setMaximumDelayInSamples(maxDelaySamples + 4);
  processedMeterSidechainDelay.prepare(
      {sampleRate, (juce::uint32)juce::jmax(1, samplesPerBlock), 1});
  processedMeterSidechainDelay.reset();
  bypassDelay.setMaximumDelayInSamples(maxDelaySamples + 4);
  bypassDelay.prepare(
      {sampleRate, (juce::uint32)juce::jmax(1, samplesPerBlock), 2});
  bypassDelay.reset();
  rawBassLowpass.setType(juce::dsp::LinkwitzRileyFilterType::lowpass);
  rawKickLowpass.setType(juce::dsp::LinkwitzRileyFilterType::lowpass);
  processedBassLowpass.setType(juce::dsp::LinkwitzRileyFilterType::lowpass);
  processedKickLowpass.setType(juce::dsp::LinkwitzRileyFilterType::lowpass);
  rawBassLowpass.prepare(
      {sampleRate, (juce::uint32)juce::jmax(1, samplesPerBlock), 1});
  rawKickLowpass.prepare(
      {sampleRate, (juce::uint32)juce::jmax(1, samplesPerBlock), 1});
  processedBassLowpass.prepare(
      {sampleRate, (juce::uint32)juce::jmax(1, samplesPerBlock), 1});
  processedKickLowpass.prepare(
      {sampleRate, (juce::uint32)juce::jmax(1, samplesPerBlock), 1});
  analysisBassCrossoverSim.setType(juce::dsp::LinkwitzRileyFilterType::allpass);
  analysisBassCrossoverSim.prepare(
      {sampleRate, (juce::uint32)juce::jmax(1, samplesPerBlock), 1});
  rawBassLowpass.reset();
  rawKickLowpass.reset();
  processedBassLowpass.reset();
  processedKickLowpass.reset();
  analysisBassCrossoverSim.reset();
  scopeFifo.prepare(8192);
  rawScopeFifo.prepare(8192);
  spectrumFifo.prepare(16384);

  // ~2 seconds of raw bass/kick for the Analyze button's cross-correlation.
  rawCapture.prepare((int)(sampleRate * 2.0));
  transientDetector.prepare(sampleRate);
  transientDetector.setThreshold(1.0e-7f);
  transientDetector.setMinimumEnergyGate(1.0e-8f);
  transientDetector.setAttackReleaseMs(2.0f, 60.0f);
  transientDetector.setTriggerRatio(1.35f);
  transientDetector.setHoldoffMs(90.0f);

  // Triggered oscilloscope capture: keep 20 ms pre-roll internally for a
  // stable trigger, but display the post-trigger window from 0..500 ms so
  // long kicks/808s are visible instead of being cut at ~150 ms.
  hitCapture.prepare(sampleRate, 20.0f, 500.0f);
  // Bass fundamental tracker for the Pitch Follow mode; fed the low-passed
  // bass in processObservationCapture. 25-300 Hz covers the playable bass
  // range while rejecting octave-up garbage.
  pitchTracker.prepare(sampleRate, 25.0f, 300.0f);
  trackedBassHz.store(0.0f);
  transientPunchMeter.prepare(sampleRate);
  transientPunchReferenceDb.store(0.0f, std::memory_order_release);
  transientPunchReferenceSet.store(false, std::memory_order_release);
  // Held-activity trackers for the P3 status. ~1.5 s hold so a steady loop
  // never flickers to "no signal" in the gaps between kick transients — a
  // 500 ms hold lapsed between hits at or below ~120 BPM (and on any
  // half-time / sparse pattern), which is what made the Analyze button read
  // "waiting for kick" and never enable. 1.5 s covers 4-on-floor down to
  // ~40 BPM while still going inactive quickly once the transport stops. The
  // activation floor (-60 dBFS) is deliberately low: it only needs to tell
  // "playing" from "silent" — the digital noise floor sits far below it —
  // not judge phase-read quality (that is materialUsable below).
  kickActivity.prepare(sampleRate, 1500.0f, 1.0e-3f);
  bassActivity.prepare(sampleRate, 1500.0f, 1.0e-3f);
  materialReadyHoldSamples = 0;
  kickActiveHeld.store(false);
  bassActiveHeld.store(false);
  analysisSignalUsable.store(false);
  analysisMaterialReady.store(false);

  // Decimate the scope feed so the UI ring can show a multi-beat view
  // without allocating on the audio thread.
  scopeDecimationFactor = juce::jmax(1, (int)(sampleRate / 2048.0));
  scopeDecimationCounter = 0;
  rawScopeDecimationCounter = 0;
  // Spectrum: No spectrum decimation
  dryMeterDecimationCounter = 0;
  processedMeterDecimationCounter = 0;
  lastPublishedCrossoverHz = -1.0f;
  sidechainReferenceAvailable.store(false);
  tempoAvailable.store(false);
  latestBpm.store(0.0f);
  bassSignalRms.store(0.0f);
  kickSignalRms.store(0.0f);
  for (auto &bandMatch : liveBandMatchPercent)
    bandMatch.store(0.0f);
  latestAppliedBeforePercent.store(-1.0f);
  realtimeCorrelation.store(0.0f);
  liveMatchValid.store(false);
  uiSmoothingInitialized.store(false, std::memory_order_relaxed);

  lastInterpChoice = 0;
  lastStageChoice = 0;
  lastRotatorFreq = -1.0f;
  lastRotatorQ = -1.0f;
  lastDelayActive = false;

  {
    const std::lock_guard<std::mutex> lock(resultMutex);
    latestFixResult = {};
    lastAnalyzedBassWindow.clear();
    lastAnalyzedKickWindow.clear();
    lastAnalyzedCrossoverHz =
        crossoverFreqParam != nullptr ? crossoverFreqParam->load() : 150.0f;
  }
  revertSnapshotValid.store(false, std::memory_order_release);

  // Reset the Analyze state machine unless a background job is mid-flight
  // (leave a running/analyzing job to resolve itself so its terminal store
  // isn't clobbered here).
  const auto state = analyzeState.load(std::memory_order_acquire);
  if (!analyzeStateIsBusy(state))
    analyzeState.store(AnalyzeState::Idle, std::memory_order_release);
}

void KickLockAudioProcessor::releaseResources() {}

bool KickLockAudioProcessor::isSidechainBusActive(
    const juce::AudioBuffer<float> &sidechainBuffer) const noexcept {
  const auto *sidechainBus = getBus(true, 1);
  return sidechainBus != nullptr && sidechainBus->isEnabled() &&
         sidechainBuffer.getNumChannels() > 0;
}

void KickLockAudioProcessor::processObservationCapture(
    const juce::AudioBuffer<float> &mainBuffer,
    const juce::AudioBuffer<float> &sidechainBuffer, bool hasSidechain,
    int numSamples, BlockObservationStats &statsOut) noexcept {
  statsOut = {};

  const int mainCh = mainBuffer.getNumChannels();

  // Capture and meter RAW mono bass/kick before polarity/delay/rotator touch
  // the audible bass buffer. Skipped without a sidechain. This runs
  // identically whether the corrective DSP that follows is live or bypassed,
  // so Analyze capture and the dry multi-band meter never go stale.
  if (hasSidechain) {
    const int scCh = sidechainBuffer.getNumChannels();
    const float mainNorm = mainCh > 0 ? 1.0f / (float)mainCh : 0.0f;
    const float scNorm = scCh > 0 ? 1.0f / (float)scCh : 0.0f;
    const bool crossoverEnable = crossoverEnableParamRaw == nullptr ||
                                 crossoverEnableParamRaw->load() > 0.5f;
    // Channel pointers avoid per-sample getSample() bounds checks in the hot path.
    const float* mainPtrs[2] = {
        mainCh > 0 ? mainBuffer.getReadPointer(0) : nullptr,
        mainCh > 1 ? mainBuffer.getReadPointer(1) : nullptr};
    const float* scPtrs[2] = {
        scCh > 0 ? sidechainBuffer.getReadPointer(0) : nullptr,
        scCh > 1 ? sidechainBuffer.getReadPointer(1) : nullptr};

    for (int i = 0; i < numSamples; ++i) {
      float mSum = 0.0f;
      if (mainPtrs[0] != nullptr)
        mSum += mainPtrs[0][i];
      if (mainPtrs[1] != nullptr)
        mSum += mainPtrs[1][i];
      // Fall back for >2 channel layouts (rare for this plugin).
      for (int ch = 2; ch < mainCh; ++ch)
        mSum += mainBuffer.getSample(ch, i);
      const float bassMono = mSum * mainNorm;
      statsOut.bassEnergySum += (float)(bassMono * bassMono);
      statsOut.bassPeak = juce::jmax(statsOut.bassPeak, std::abs(bassMono));

      float sSum = 0.0f;
      if (scPtrs[0] != nullptr)
        sSum += scPtrs[0][i];
      if (scPtrs[1] != nullptr)
        sSum += scPtrs[1][i];
      for (int ch = 2; ch < scCh; ++ch)
        sSum += sidechainBuffer.getSample(ch, i);
      const float kickMono = sSum * scNorm;
      statsOut.kickEnergySum += (float)(kickMono * kickMono);
      statsOut.kickPeak = juce::jmax(statsOut.kickPeak, std::abs(kickMono));

      const float rawBassLow = rawBassLowpass.processSample(0, bassMono);
      const float kickLow = rawKickLowpass.processSample(0, kickMono);

      // The live pitch/auto-align path follows the crossover phase shift, while
      // the stored analyze snapshot and dry meter stay raw so old captures can
      // be re-analyzed consistently when Analyze forces crossover on.
      const float bassLow = crossoverEnable
                                 ? analysisBassCrossoverSim.processSample(0, rawBassLow)
                                 : rawBassLow;

      rawCapture.push(rawBassLow, kickLow);
      pitchTracker.pushSample(bassLow);

      if (autoAlignEngine != nullptr)
        autoAlignEngine->pushSample(bassLow, kickLow);

      if (++dryMeterDecimationCounter >= meterDecimationFactor) {
        dryMeterDecimationCounter = 0;
        dryMultiBandMeter.pushSample(rawBassLow, kickLow);
      }
    }
  } else {
    const float mainNorm = mainCh > 0 ? 1.0f / (float)mainCh : 0.0f;
    const float* mainPtrs[2] = {
        mainCh > 0 ? mainBuffer.getReadPointer(0) : nullptr,
        mainCh > 1 ? mainBuffer.getReadPointer(1) : nullptr};

    for (int i = 0; i < numSamples; ++i) {
      float mSum = 0.0f;
      if (mainPtrs[0] != nullptr)
        mSum += mainPtrs[0][i];
      if (mainPtrs[1] != nullptr)
        mSum += mainPtrs[1][i];
      for (int ch = 2; ch < mainCh; ++ch)
        mSum += mainBuffer.getSample(ch, i);

      const float bassMono = mSum * mainNorm;
      statsOut.bassEnergySum += (float)(bassMono * bassMono);
      statsOut.bassPeak = juce::jmax(statsOut.bassPeak, std::abs(bassMono));
    }
  }
}

void KickLockAudioProcessor::updateActivityAndSignalState(
    bool hasSidechain, const BlockObservationStats &stats,
    int numSamples) noexcept {
  const float safeCount = (float)juce::jmax(1, numSamples);
  const float blockBassRms = std::sqrt(stats.bassEnergySum / safeCount);
  const float blockKickRms =
      hasSidechain ? std::sqrt(stats.kickEnergySum / safeCount) : 0.0f;
  bassSignalRms.store(blockBassRms);
  kickSignalRms.store(blockKickRms);

  // Activity level = max(block RMS, block peak * 0.25). Block RMS of a short
  // kick tick dilutes with the host buffer size (~10 dB from 512 to 4096
  // samples), so RMS alone made detection buffer-size-dependent; the scaled
  // peak term restores it. The 0.25 factor keeps a lone full-scale click from
  // reading louder than a sustained tone of the same audibility.
  const float bassLevel = juce::jmax(blockBassRms, stats.bassPeak * 0.25f);
  const float kickLevel =
      hasSidechain ? juce::jmax(blockKickRms, stats.kickPeak * 0.25f) : 0.0f;

  // Feed the held-activity trackers so status reflects recent hits, not just
  // this instant. Without a sidechain the kick can never be "active".
  bassActivity.pushBlock(bassLevel, numSamples);
  kickActivity.pushBlock(kickLevel, numSamples);

  kickActiveHeld.store(kickActivity.isActive());
  bassActiveHeld.store(bassActivity.isActive());

  // "Usable" material: both signals cleared a floor loud enough for a phase
  // read within the hold window. Lets status tell "present but far too quiet"
  // (SIGNAL TOO LOW) apart from a genuinely playing loop, without flickering
  // between kick transients. -48 dBFS: the offline engine's own presence
  // floor is 1.5e-3 RMS, so gating the UI much stricter than the engine it
  // gates just refused material the analysis handles fine.
  constexpr float usableFloorRms = 4.0e-3f;
  analysisSignalUsable.store(hasSidechain &&
                             kickActivity.isUsable(usableFloorRms) &&
                             bassActivity.isUsable(usableFloorRms));

  // "Enough material" for a meaningful analysis window: the raw capture has
  // accumulated at least ~0.5 s of samples AND both signals were active
  // recently. "Recently" includes a ~30 s grace window after the activity
  // holds lapse, so stopping the transport doesn't instantly disarm Analyze
  // on audio that is still sitting in the capture ring — while hours-old
  // material still eventually reads stale. Display-only; Apply Fix gates on
  // the analysis result, not this.
  const int capturedSamples = rawCapture.getFilledSamples();
  const int minMaterialSamples = (int)(getSampleRate() * 0.5);
  const bool readyNow = hasSidechain && capturedSamples >= minMaterialSamples &&
                        kickActivity.isActive() && bassActivity.isActive();

  if (readyNow)
    materialReadyHoldSamples = (int)(getSampleRate() * 30.0);
  else
    materialReadyHoldSamples =
        juce::jmax(0, materialReadyHoldSamples - juce::jmax(0, numSamples));

  analysisMaterialReady.store(hasSidechain &&
                              (readyNow || materialReadyHoldSamples > 0));
}

void KickLockAudioProcessor::pushMetersScopeAndTransientState(
    float mainMono, float sidechainMonoRaw) noexcept {
  processedMeterSidechainDelay.pushSample(0, sidechainMonoRaw);
  const float meteredSidechainMono =
      processedMeterSidechainDelay.popSample(0, (float)getLatencySamples());
  const float processedBassLow =
      processedBassLowpass.processSample(0, mainMono);
  const float alignedKickLow =
      processedKickLowpass.processSample(0, meteredSidechainMono);

  // Trigger mostly from the aligned low/body lane, with a small full-band
  // sidechain component so sharp kick attacks can re-arm on top of long 808
  // tails. The captured waveform remains full-band for visual truth.
  const float triggerKick =
      0.80f * alignedKickLow + 0.20f * meteredSidechainMono;
  const bool transientDetected = transientDetector.processSample(triggerKick);

  if (++processedMeterDecimationCounter >= meterDecimationFactor) {
    processedMeterDecimationCounter = 0;
    processedMultiBandMeter.pushSample(processedBassLow, alignedKickLow);
  }
  hitCapture.pushSample(mainMono, meteredSidechainMono, transientDetected);
  transientPunchMeter.pushSample(alignedKickLow, processedBassLow,
                                 transientDetected);

  // Decimate the scope feed only: the meter still sees every sample, but the
  // UI keeps a slower, longer history for the musical grid. The triggered
  // view no longer needs trigger markers here — it assembles the
  // HitCaptureBuffer's full-rate progressive sweep stream instead, which is
  // sample-accurate by construction.
  if (++scopeDecimationCounter >= scopeDecimationFactor) {
    scopeDecimationCounter = 0;
    scopeFifo.pushSample(mainMono, meteredSidechainMono);
  }

  if (spectrumCaptureEnabled.load(std::memory_order_relaxed)) {
    spectrumFifo.pushSample(mainMono, meteredSidechainMono);
  }
}

void KickLockAudioProcessor::processBlockBypassed(
    juce::AudioBuffer<float> &buffer, juce::MidiBuffer &) {
  juce::ScopedNoDenormals noDenormals;

  auto mainBuffer = getBusBuffer(buffer, true, 0);
  auto sidechainBuffer = getBusBuffer(buffer, true, 1);

  // Bypass must not kill diagnostic observation: whenever the host still
  // routes a sidechain, keep the same capture/metering/transient/Kick-Punch
  // path alive that processBlock() runs, even though the corrective DSP
  // below is skipped (a fixed-delay passthrough only, to preserve PDC).
  const bool hasSidechain = isSidechainBusActive(sidechainBuffer);
  sidechainReferenceAvailable.store(hasSidechain);

  const int numSamples = mainBuffer.getNumSamples();
  BlockObservationStats observationStats;

  // Track the crossover cutoff here too: processObservationCapture() low-passes
  // the raw bass/kick it hands to the Analyze capture with rawBassLowpass /
  // rawKickLowpass, but only processBlock() ever set their cutoff. Under host
  // bypass the Analyze capture was therefore filtered at whatever cutoff was
  // last active (the JUCE default on a fresh instance), skewing the analysis.
  const float crossoverHz =
      crossoverFreqParam != nullptr ? crossoverFreqParam->load() : 150.0f;
  if (std::abs(crossoverHz - lastPublishedCrossoverHz) > 0.01f) {
    rawBassLowpass.setCutoffFrequency(crossoverHz);
    rawKickLowpass.setCutoffFrequency(crossoverHz);
    analysisBassCrossoverSim.setCutoffFrequency(crossoverHz);
    lastPublishedCrossoverHz = crossoverHz;
  }

  processObservationCapture(mainBuffer, sidechainBuffer, hasSidechain,
                            numSamples, observationStats);
  updateActivityAndSignalState(hasSidechain, observationStats, numSamples);

  // Under bypass the dry relationship is what the user hears.
  liveMatchValid.store(hasSidechain && dryMultiBandMeter.hasSignal());

  const auto numChannels = juce::jmin(2, mainBuffer.getNumChannels());
  const auto fixedDelay = (float)getLatencySamples();
  const int sidechainChannels = sidechainBuffer.getNumChannels();

  float* mainWrite[2] = {
      numChannels > 0 ? mainBuffer.getWritePointer(0) : nullptr,
      numChannels > 1 ? mainBuffer.getWritePointer(1) : nullptr
  };
  const float* scRead[2] = {
      sidechainChannels > 0 ? sidechainBuffer.getReadPointer(0) : nullptr,
      sidechainChannels > 1 ? sidechainBuffer.getReadPointer(1) : nullptr
  };

  for (int i = 0; i < numSamples; ++i) {
    float mainSum = 0.0f;
    for (int ch = 0; ch < numChannels; ++ch) {
      bypassDelay.pushSample(ch, mainWrite[ch][i]);
      const float out = bypassDelay.popSample(ch, fixedDelay);
      mainWrite[ch][i] = out;
      mainSum += out;
    }

    for (int ch = numChannels; ch < 2; ++ch) {
      bypassDelay.pushSample(ch, 0.0f);
      (void)bypassDelay.popSample(ch, fixedDelay);
    }

    if (hasSidechain) {
      const float mainMono =
          numChannels > 0 ? mainSum / (float)numChannels : 0.0f;

      float sidechainSum = 0.0f;
      if (scRead[0] != nullptr) sidechainSum += scRead[0][i];
      if (scRead[1] != nullptr) sidechainSum += scRead[1][i];
      for (int ch = 2; ch < sidechainChannels; ++ch)
        sidechainSum += sidechainBuffer.getSample(ch, i);
      const float sidechainMono = sidechainChannels > 0
                                      ? sidechainSum / (float)sidechainChannels
                                      : 0.0f;

      pushMetersScopeAndTransientState(mainMono, sidechainMono);
    }
  }

  if (!hasSidechain)
    realtimeCorrelation.store(0.0f);

  for (auto channel = getTotalNumInputChannels();
       channel < getTotalNumOutputChannels(); ++channel)
    buffer.clear(channel, 0, buffer.getNumSamples());
}

bool KickLockAudioProcessor::isBusesLayoutSupported(
    const BusesLayout &layouts) const {
  const auto mainIn = layouts.getMainInputChannelSet();
  const auto mainOut = layouts.getMainOutputChannelSet();
  const auto sidechainIn = layouts.inputBuses.size() > 1
                               ? layouts.inputBuses[(size_t)1]
                               : juce::AudioChannelSet::disabled();

  if (mainIn.isDisabled() || mainOut.isDisabled())
    return false;

  const auto isMonoOrStereo = [](const juce::AudioChannelSet &set) {
    return set == juce::AudioChannelSet::mono() ||
           set == juce::AudioChannelSet::stereo();
  };

  if (!isMonoOrStereo(mainIn) || !isMonoOrStereo(mainOut))
    return false;

  if (!sidechainIn.isDisabled() && !isMonoOrStereo(sidechainIn))
    return false;

  return true;
}

bool KickLockAudioProcessor::isBassProcessingNeutral() const noexcept {
  constexpr float epsilon = 1.0e-6f;

  return std::abs(getEffectiveDelayMs()) <= epsilon &&
         !getEffectivePolarityInvert() && !getEffectivePhaseFilterEnabled();
}

void KickLockAudioProcessor::processBlock(juce::AudioBuffer<float> &buffer,
                                          juce::MidiBuffer &) {
  juce::ScopedNoDenormals noDenormals;

  bool bpmDetected = false;
  float bpmValue = 0.0f;

  if (auto *currentPlayHead = getPlayHead()) {
    if (const auto position = currentPlayHead->getPosition()) {
      if (const auto bpm = position->getBpm()) {
        bpmValue = (float)*bpm;
        bpmDetected = bpmValue > 0.0f;
      }
    }
  }

  tempoAvailable.store(bpmDetected);
  latestBpm.store(bpmDetected ? bpmValue : 0.0f);

  const auto totalNumInputChannels = getTotalNumInputChannels();
  const auto totalNumOutputChannels = getTotalNumOutputChannels();

  // Clear any output channels that don't have a corresponding input,
  // since buffers may contain garbage.
  for (auto channel = totalNumInputChannels; channel < totalNumOutputChannels;
       ++channel)
    buffer.clear(channel, 0, buffer.getNumSamples());

  auto mainBuffer = getBusBuffer(buffer, true, 0);
  auto sidechainBuffer = getBusBuffer(buffer, true, 1);

  const bool hasSidechain = isSidechainBusActive(sidechainBuffer);
  sidechainReferenceAvailable.store(hasSidechain);

  const int numSamples = mainBuffer.getNumSamples();

  // ==========================================
  // CABLE 1: Raw UI Feed (Decoupled Visuals)
  // ==========================================
  // Push raw, un-filtered, un-shifted audio directly to the UI scope
  if (hasSidechain) {
    const int mainChannels = mainBuffer.getNumChannels();
    const int sidechainChannels = sidechainBuffer.getNumChannels();
    const float mainNorm = mainChannels > 0 ? 1.0f / (float)mainChannels : 0.0f;
    const float scNorm =
        sidechainChannels > 0 ? 1.0f / (float)sidechainChannels : 0.0f;
    const float* mainPtrs[2] = {
        mainChannels > 0 ? mainBuffer.getReadPointer(0) : nullptr,
        mainChannels > 1 ? mainBuffer.getReadPointer(1) : nullptr};
    const float* scPtrs[2] = {
        sidechainChannels > 0 ? sidechainBuffer.getReadPointer(0) : nullptr,
        sidechainChannels > 1 ? sidechainBuffer.getReadPointer(1) : nullptr};
        
    float* scMonoWrite = sidechainMonoScratch.getWritePointer(0);

    for (int i = 0; i < numSamples; ++i) {
      float mSum = 0.0f;
      if (mainPtrs[0] != nullptr)
        mSum += mainPtrs[0][i];
      if (mainPtrs[1] != nullptr)
        mSum += mainPtrs[1][i];
      for (int ch = 2; ch < mainChannels; ++ch)
        mSum += mainBuffer.getSample(ch, i);
      const float mainMono = mSum * mainNorm;

      float sSum = 0.0f;
      if (scPtrs[0] != nullptr)
        sSum += scPtrs[0][i];
      if (scPtrs[1] != nullptr)
        sSum += scPtrs[1][i];
      for (int ch = 2; ch < sidechainChannels; ++ch)
        sSum += sidechainBuffer.getSample(ch, i);
      const float sidechainMono = sSum * scNorm;
      scMonoWrite[i] = sidechainMono;

      if (++rawScopeDecimationCounter >= scopeDecimationFactor) {
        rawScopeDecimationCounter = 0;
        rawScopeFifo.pushSample(mainMono, sidechainMono);
      }
    }
  }

  // ==========================================
  // CABLE 2: The Analysis Feed (Smoothed)
  // ==========================================
  const int numCh = juce::jmin((int)analysisBuffer.getNumChannels(),
                               mainBuffer.getNumChannels());
  for (int ch = 0; ch < numCh; ++ch)
    analysisBuffer.copyFrom(ch, 0, mainBuffer, ch, 0, numSamples);

  BlockObservationStats observationStats;

  const float delayMs = getEffectiveDelayMs();
  const bool polarityInvert = getEffectivePolarityInvert();
  const bool phaseFilterEnabled = getEffectivePhaseFilterEnabled();
  float allpassFreq = getEffectiveAllpassFreqHz();
  const float allpassQ =
      rotatorQParam != nullptr ? rotatorQParam->load() : 0.70710678f;
  const int allpassStages =
      2 +
      (rotatorStagesParam != nullptr
           ? juce::jlimit(0, 2, (int)std::lround(rotatorStagesParam->load()))
           : 0);

  // Dynamic pitch follow: retarget the allpass centre to the tracked bass
  // fundamental so the phase rotation stays on the note as the bassline
  // moves. The DSP's own 30 ms frequency smoothing + throttled coefficient
  // updates make the retune click-free. The Phase Freq PARAMETER is never
  // written from here (audio thread must not push parameter changes); it
  // simply stops being the source while follow is active, and the UI shows
  // the live tracked value instead.
  const float trackedHz = pitchTracker.getFrequencyHz();
  trackedBassHz.store(trackedHz);

  const bool pitchFollowActive = pitchTrackParam != nullptr &&
                                 pitchTrackParam->load() > 0.5f &&
                                 phaseFilterEnabled && trackedHz > 0.0f;
  if (pitchFollowActive)
    allpassFreq = juce::jlimit(20.0f, 500.0f, trackedHz);

  MultibandPhaseCore::Params coreParams;

  // Raw parameter pointer cached in the constructor — the previous
  // apvts.getParameter() call here was a per-block string-keyed lookup on
  // the audio thread.
  const bool crossoverEnable = crossoverEnableParamRaw == nullptr ||
                               crossoverEnableParamRaw->load() > 0.5f;

  coreParams.crossoverEnabled = crossoverEnable;
  coreParams.crossoverHz =
      crossoverFreqParam != nullptr ? crossoverFreqParam->load() : 150.0f;
  coreParams.userDelayMs = delayMs;
  coreParams.polarityInvert = polarityInvert;
  coreParams.allpassEnabled = phaseFilterEnabled;
  coreParams.allpassFreqHz = allpassFreq;
  coreParams.allpassQ = allpassQ;
  coreParams.allpassStages = allpassStages;

  if (std::abs(coreParams.crossoverHz - lastPublishedCrossoverHz) > 0.01f) {
    rawBassLowpass.setCutoffFrequency(coreParams.crossoverHz);
    rawKickLowpass.setCutoffFrequency(coreParams.crossoverHz);
    processedBassLowpass.setCutoffFrequency(coreParams.crossoverHz);
    processedKickLowpass.setCutoffFrequency(coreParams.crossoverHz);
    analysisBassCrossoverSim.setCutoffFrequency(coreParams.crossoverHz);
    lastPublishedCrossoverHz = coreParams.crossoverHz;
  }

  // Feed analysis with analysisBuffer instead of mainBuffer
  processObservationCapture(analysisBuffer, sidechainBuffer, hasSidechain,
                            numSamples, observationStats);
  updateActivityAndSignalState(hasSidechain, observationStats, numSamples);

  const bool neutral = isBassProcessingNeutral();
  const bool processingNeeded = !neutral;

  multibandCore.process(mainBuffer, sidechainBuffer, coreParams, numSamples);

  if (hasSidechain) {
    const int mainChannels = mainBuffer.getNumChannels();
    const float mainNorm = mainChannels > 0 ? 1.0f / (float)mainChannels : 0.0f;
    const float latencySamplesF = (float)getLatencySamples();
    const bool captureSpectrum =
        spectrumCaptureEnabled.load(std::memory_order_relaxed);
    const float* mainPtrs[2] = {
        mainChannels > 0 ? mainBuffer.getReadPointer(0) : nullptr,
        mainChannels > 1 ? mainBuffer.getReadPointer(1) : nullptr};
    const float* scMonoRead = sidechainMonoScratch.getReadPointer(0);

    for (int i = 0; i < numSamples; ++i) {
      float mainSum = 0.0f;
      if (mainPtrs[0] != nullptr)
        mainSum += mainPtrs[0][i];
      if (mainPtrs[1] != nullptr)
        mainSum += mainPtrs[1][i];
      for (int ch = 2; ch < mainChannels; ++ch)
        mainSum += mainBuffer.getSample(ch, i);
      const float mainMono = mainSum * mainNorm;

      const float sidechainMono = scMonoRead[i];

      processedMeterSidechainDelay.pushSample(0, sidechainMono);
      const float meteredSidechainMono =
          processedMeterSidechainDelay.popSample(0, latencySamplesF);

      const float processedBassLow =
          processedBassLowpass.processSample(0, mainMono);
      const float alignedKickLow =
          processedKickLowpass.processSample(0, meteredSidechainMono);

      if (++processedMeterDecimationCounter >= meterDecimationFactor) {
        processedMeterDecimationCounter = 0;
        processedMultiBandMeter.pushSample(processedBassLow, alignedKickLow);
      }

      // Drive hitCapture and transientPunchMeter from the filtered/processed
      // sidechain so broadband noise does not cause false retriggers (808 tail
      // test).
      const float triggerKick =
          0.80f * alignedKickLow + 0.20f * meteredSidechainMono;
      const bool transientDetectedPunch =
          transientDetector.processSample(triggerKick);
      hitCapture.pushSample(mainMono, meteredSidechainMono,
                            transientDetectedPunch);
      transientPunchMeter.pushSample(alignedKickLow, processedBassLow,
                                     transientDetectedPunch);

      if (++scopeDecimationCounter >= scopeDecimationFactor) {
        scopeDecimationCounter = 0;
        scopeFifo.pushSample(mainMono, meteredSidechainMono);
      }

      if (captureSpectrum) {
        spectrumFifo.pushSample(mainMono, meteredSidechainMono);
      }
    }
  }

  else {
    realtimeCorrelation.store(0.0f);
  }

  // Overall (low-end-weighted) match from whichever meter reflects what the
  // user is hearing: the processed meter when the bass path is doing anything,
  // otherwise the dry meter.
  const auto &activeMeter =
      processingNeeded ? processedMultiBandMeter : dryMultiBandMeter;

  liveMatchValid.store(hasSidechain && activeMeter.hasSignal());

  const float dryMatch =
      hasSidechain ? dryMultiBandMeter.getWeightedMatchPercent() : 50.0f;
  const float processedMatch =
      hasSidechain ? processedMultiBandMeter.getWeightedMatchPercent() : 50.0f;
  const float activeMatch =
      hasSidechain ? activeMeter.getWeightedMatchPercent() : 50.0f;
  const float activeLowEnd =
      hasSidechain ? activeMeter.getLowEndMatchPercent() : 50.0f;
  const float activeBroad =
      hasSidechain ? activeMeter.getBroadbandMatchPercent() : 50.0f;
  const float activeSubLossDb =
      hasSidechain ? activeMeter.getLowEndSubLossDb() : 0.0f; // Sub loss is 0.0 for no loss
  std::array<float, PhaseBands::numBands> activeBands{};
  for (int band = 0; band < PhaseBands::numBands; ++band)
    activeBands[(size_t)band] =
        hasSidechain ? activeMeter.getBandMatchPercent(band) : 50.0f;

  // Smooth the UI values with a slow ~500ms EMA to prevent the numbers from
  // dancing too fast
  const float dt = (float)buffer.getNumSamples() / (float)getSampleRate();
  const float uiAlpha = 1.0f - std::exp(-dt / 0.5f);

  // P3: seed each EMA from the first real reading, then blend. The old
  // "snap whenever |current-50|<0.001" test also snapped whenever a value
  // legitimately passed through 50%, freezing the display on the way past.
  // A one-shot initialized flag seeds cleanly without that artefact.
  const bool seed = !uiSmoothingInitialized.load(std::memory_order_relaxed);
  auto smoothAtomic = [uiAlpha, seed](std::atomic<float> &target,
                                      float newValue) {
    if (seed)
      target.store(newValue);
    else {
      const float current = target.load();
      target.store(current + uiAlpha * (newValue - current));
    }
  };

  smoothAtomic(dryInputMatchPercent, dryMatch);
  smoothAtomic(processedMatchPercent, processedMatch);

  // P5/P8 live read-outs: overall multi-band, sub/low only, broad 20-500.
  smoothAtomic(liveMultiBandMatchPercent, activeMatch);
  smoothAtomic(liveLowEndMatchPercent, activeLowEnd);
  smoothAtomic(liveBroadbandMatchPercent, activeBroad);
  smoothAtomic(liveLowEndSubLossDb, activeSubLossDb);
  smoothAtomic(realtimeLowBandMatchPercent,
               activeLowEnd); // legacy alias (sub/low)
  smoothAtomic(
      correlationPercent,
      activeMatch); // legacy headline alias: low-end-weighted, not broadband
  for (int band = 0; band < PhaseBands::numBands; ++band)
    smoothAtomic(liveBandMatchPercent[(size_t)band], activeBands[(size_t)band]);

  uiSmoothingInitialized.store(true, std::memory_order_relaxed);

  realtimeCorrelation.store(
      juce::jlimit(-1.0f, 1.0f, activeMatch / 100.0f));
}

juce::AudioProcessorEditor *KickLockAudioProcessor::createEditor() {
  return new KickLockAudioProcessorEditor(*this);
}

bool KickLockAudioProcessor::hasEditor() const { return true; }

const juce::String KickLockAudioProcessor::getName() const {
  return JucePlugin_Name;
}

bool KickLockAudioProcessor::acceptsMidi() const { return false; }

bool KickLockAudioProcessor::producesMidi() const { return false; }

bool KickLockAudioProcessor::isMidiEffect() const { return false; }

double KickLockAudioProcessor::getTailLengthSeconds() const { return 0.0; }

int KickLockAudioProcessor::getNumPrograms() { return 4; }

int KickLockAudioProcessor::getCurrentProgram() { return currentProgramIndex; }

void KickLockAudioProcessor::setCurrentProgram(int index) {
  currentProgramIndex = juce::jlimit(0, getNumPrograms() - 1, index);
  const auto preset = makeFactoryPresetSnapshot(currentProgramIndex);
  restoreParameterSnapshot(preset);

  initialiseCompareSlotsIfNeeded();
  compareSlots[(size_t)activeCompareSlot.load(std::memory_order_acquire)] =
      preset;
  writeCompareSlotsToState();
}

const juce::String KickLockAudioProcessor::getProgramName(int index) {
  switch (index) {
  case 0:
    return "Tight EDM";
  case 1:
    return "Deep House Sub";
  case 2:
    return "Trap 808";
  case 3:
    return "Neutral";
  default:
    return {};
  }
}

void KickLockAudioProcessor::changeProgramName(
    int /*index*/, const juce::String & /*newName*/) {}

float KickLockAudioProcessor::readParameterValue(const char *id,
                                                 float fallback) const {
  if (auto *parameter = apvts.getParameter(id))
    return parameter->convertFrom0to1(parameter->getValue());

  if (const auto *value = apvts.getRawParameterValue(id))
    return value->load();

  return fallback;
}

void KickLockAudioProcessor::setParameterValueWithGesture(const char *id,
                                                          float value) {
  if (auto *parameter = apvts.getParameter(id)) {
    parameter->beginChangeGesture();
    parameter->setValueNotifyingHost(parameter->convertTo0to1(value));
    parameter->endChangeGesture();
  }
}

KickLockAudioProcessor::ParameterSnapshot
KickLockAudioProcessor::captureCurrentParameterSnapshot() const {
  ParameterSnapshot snapshot;
  snapshot.delayMs = getEffectiveDelayMs();
  snapshot.polarityInvert = getEffectivePolarityInvert();
  snapshot.phaseFilterEnabled = getEffectivePhaseFilterEnabled();
  snapshot.phaseFilterFreqHz = getEffectiveAllpassFreqHz();
  snapshot.phaseFilterQ = readParameterValue("rotatorQ", 0.7f);
  snapshot.phaseFilterStageIndex = juce::jlimit(
      0, 2, (int)std::lround(readParameterValue("rotatorStages", 0.0f)));
  snapshot.crossoverEnabled = crossoverEnableParamRaw
                                  ? (crossoverEnableParamRaw->load() > 0.5f)
                                  : false;
  snapshot.crossoverFreqHz =
      crossoverFreqParam ? crossoverFreqParam->load() : 150.0f;
  return snapshot;
}

void KickLockAudioProcessor::restoreParameterSnapshot(
    const ParameterSnapshot &snapshot) {
  setParameterValueWithGesture("delay_ms", snapshot.delayMs);
  setParameterValueWithGesture("delayMs", snapshot.delayMs);
  setParameterValueWithGesture("polarity_invert",
                               snapshot.polarityInvert ? 1.0f : 0.0f);
  setParameterValueWithGesture("polarityInvert",
                               snapshot.polarityInvert ? 1.0f : 0.0f);
  setParameterValueWithGesture("allpass_enable",
                               snapshot.phaseFilterEnabled ? 1.0f : 0.0f);
  setParameterValueWithGesture("phaseFilterEnabled",
                               snapshot.phaseFilterEnabled ? 1.0f : 0.0f);
  setParameterValueWithGesture("allpass_freq", snapshot.phaseFilterFreqHz);
  setParameterValueWithGesture("rotatorFreq", snapshot.phaseFilterFreqHz);
  setParameterValueWithGesture("rotatorQ", snapshot.phaseFilterQ);
  setParameterValueWithGesture("rotatorStages",
                               (float)snapshot.phaseFilterStageIndex);
  setParameterValueWithGesture("crossover_enable",
                               snapshot.crossoverEnabled ? 1.0f : 0.0f);
  setParameterValueWithGesture("crossover_freq", snapshot.crossoverFreqHz);
}

KickLockAudioProcessor::ParameterSnapshot
KickLockAudioProcessor::makeFactoryPresetSnapshot(int index) {
  ParameterSnapshot preset;

  switch (index) {
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

void KickLockAudioProcessor::initialiseCompareSlotsIfNeeded() {
  if (compareSlotsInitialised)
    return;

  if (apvts.state.hasProperty("compareSlot0DelayMs")) {
    loadCompareSlotsFromState();
    return;
  }

  compareSlots[0] = captureCurrentParameterSnapshot();
  compareSlots[1] = compareSlots[0];
  compareSlotsInitialised = true;
  writeCompareSlotsToState();
}

void KickLockAudioProcessor::loadCompareSlotsFromState() {
  auto readSlot = [this](int slot, const ParameterSnapshot &fallback) {
    const juce::String prefix = "compareSlot" + juce::String(slot);
    ParameterSnapshot snapshot = fallback;
    snapshot.delayMs = (float)apvts.state.getProperty(
        juce::Identifier(prefix + "DelayMs"), snapshot.delayMs);
    snapshot.polarityInvert = (bool)apvts.state.getProperty(
        juce::Identifier(prefix + "Polarity"), snapshot.polarityInvert);
    snapshot.phaseFilterEnabled = (bool)apvts.state.getProperty(
        juce::Identifier(prefix + "PhaseEnabled"), snapshot.phaseFilterEnabled);
    snapshot.phaseFilterFreqHz = (float)apvts.state.getProperty(
        juce::Identifier(prefix + "FreqHz"), snapshot.phaseFilterFreqHz);
    snapshot.phaseFilterQ = (float)apvts.state.getProperty(
        juce::Identifier(prefix + "Q"), snapshot.phaseFilterQ);
    snapshot.phaseFilterStageIndex = juce::jlimit(
        0, 2,
        (int)apvts.state.getProperty(juce::Identifier(prefix + "StageIndex"),
                                     snapshot.phaseFilterStageIndex));
    return snapshot;
  };

  const auto current = captureCurrentParameterSnapshot();
  compareSlots[0] = readSlot(0, current);
  compareSlots[1] = readSlot(1, current);
  activeCompareSlot.store(
      juce::jlimit(0, 1, (int)apvts.state.getProperty("compareActiveSlot", 0)),
      std::memory_order_release);
  currentProgramIndex = juce::jlimit(
      0, getNumPrograms() - 1,
      (int)apvts.state.getProperty("currentProgramIndex", currentProgramIndex));
  compareSlotsInitialised = true;
}

void KickLockAudioProcessor::writeCompareSlotsToState() {
  apvts.state.setProperty("compareActiveSlot",
                          activeCompareSlot.load(std::memory_order_acquire),
                          nullptr);
  apvts.state.setProperty("currentProgramIndex", currentProgramIndex, nullptr);

  for (int slot = 0; slot < 2; ++slot) {
    const auto &snapshot = compareSlots[(size_t)slot];
    const juce::String prefix = "compareSlot" + juce::String(slot);
    apvts.state.setProperty(juce::Identifier(prefix + "DelayMs"),
                            snapshot.delayMs, nullptr);
    apvts.state.setProperty(juce::Identifier(prefix + "Polarity"),
                            snapshot.polarityInvert, nullptr);
    apvts.state.setProperty(juce::Identifier(prefix + "PhaseEnabled"),
                            snapshot.phaseFilterEnabled, nullptr);
    apvts.state.setProperty(juce::Identifier(prefix + "FreqHz"),
                            snapshot.phaseFilterFreqHz, nullptr);
    apvts.state.setProperty(juce::Identifier(prefix + "Q"),
                            snapshot.phaseFilterQ, nullptr);
    apvts.state.setProperty(juce::Identifier(prefix + "StageIndex"),
                            snapshot.phaseFilterStageIndex, nullptr);
  }
}

void KickLockAudioProcessor::selectCompareSlot(int slotIndex) {
  initialiseCompareSlotsIfNeeded();

  const int target = juce::jlimit(0, 1, slotIndex);
  const int current = activeCompareSlot.load(std::memory_order_acquire);
  if (target == current)
    return;

  compareSlots[(size_t)current] = captureCurrentParameterSnapshot();
  activeCompareSlot.store(target, std::memory_order_release);
  restoreParameterSnapshot(compareSlots[(size_t)target]);
  writeCompareSlotsToState();
}

void KickLockAudioProcessor::copyActiveCompareSlotToOther() {
  initialiseCompareSlotsIfNeeded();

  const int current = activeCompareSlot.load(std::memory_order_acquire);
  const int other = 1 - current;
  compareSlots[(size_t)current] = captureCurrentParameterSnapshot();
  compareSlots[(size_t)other] = compareSlots[(size_t)current];
  writeCompareSlotsToState();
}

void KickLockAudioProcessor::getStateInformation(juce::MemoryBlock &destData) {
  initialiseCompareSlotsIfNeeded();
  compareSlots[(size_t)activeCompareSlot.load(std::memory_order_acquire)] =
      captureCurrentParameterSnapshot();
  writeCompareSlotsToState();

  auto state = apvts.copyState();
  std::unique_ptr<juce::XmlElement> xml(state.createXml());
  copyXmlToBinary(*xml, destData);
}

void KickLockAudioProcessor::setStateInformation(const void *data,
                                                 int sizeInBytes) {
  std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));

  if (xml != nullptr && xml->hasTagName(apvts.state.getType())) {
    auto restoredState = juce::ValueTree::fromXml(*xml);
    const bool hasDelayMs =
        restoredState.hasProperty(juce::Identifier("delay_ms"));
    const bool hasLegacyDelayMs =
        restoredState.hasProperty(juce::Identifier("delayMs"));
    const bool hasPolarityInvert =
        restoredState.hasProperty(juce::Identifier("polarity_invert"));
    const bool hasLegacyPolarityInvert =
        restoredState.hasProperty(juce::Identifier("polarityInvert"));
    const bool hasAllpassEnable =
        restoredState.hasProperty(juce::Identifier("allpass_enable"));
    const bool hasLegacyPhaseFilterEnabled =
        restoredState.hasProperty(juce::Identifier("phaseFilterEnabled"));
    const bool hasAllpassFreq =
        restoredState.hasProperty(juce::Identifier("allpass_freq"));
    const bool hasLegacyRotatorFreq =
        restoredState.hasProperty(juce::Identifier("rotatorFreq"));

    auto migrateLegacyToCanonical = [&restoredState](
                                        bool hasCanonical, bool hasLegacy,
                                        const char *canonicalId,
                                        const char *legacyId) {
      if (hasCanonical || !hasLegacy)
        return;

      const auto value = restoredState.getProperty(juce::Identifier(legacyId));
      restoredState.setProperty(juce::Identifier(canonicalId), value, nullptr);
    };

    migrateLegacyToCanonical(hasDelayMs, hasLegacyDelayMs, "delay_ms",
                             "delayMs");
    migrateLegacyToCanonical(hasPolarityInvert, hasLegacyPolarityInvert,
                             "polarity_invert", "polarityInvert");
    migrateLegacyToCanonical(hasAllpassEnable, hasLegacyPhaseFilterEnabled,
                             "allpass_enable", "phaseFilterEnabled");
    migrateLegacyToCanonical(hasAllpassFreq, hasLegacyRotatorFreq,
                             "allpass_freq", "rotatorFreq");

    auto snapBoolParameter = [&restoredState](const char *id) {
      if (restoredState.hasProperty(id)) {
        const float restoredValue = (float)restoredState.getProperty(id);
        const float snappedValue = restoredValue >= 0.5f ? 1.0f : 0.0f;
        restoredState.setProperty(id, snappedValue, nullptr);
      }
    };

    snapBoolParameter("polarity_invert");
    snapBoolParameter("polarityInvert");
    snapBoolParameter("allpass_enable");
    snapBoolParameter("phaseFilterEnabled");

    apvts.replaceState(restoredState);

    markRestoredParameterSources(
        hasDelayMs || hasLegacyDelayMs, hasLegacyDelayMs,
        hasPolarityInvert || hasLegacyPolarityInvert, hasLegacyPolarityInvert,
        hasAllpassEnable || hasLegacyPhaseFilterEnabled,
        hasLegacyPhaseFilterEnabled, hasAllpassFreq || hasLegacyRotatorFreq,
        hasLegacyRotatorFreq);

    compareSlotsInitialised = false;
    loadCompareSlotsFromState();
  }
}

PhaseFixResult KickLockAudioProcessor::analyzeFix() {
  std::vector<float> bass, kick;
  const int n = rawCapture.snapshot(bass, kick);
  return computeAndPublishFix(bass, kick, n);
}

PhaseFixResult
KickLockAudioProcessor::computeAndPublishFix(const std::vector<float> &bass,
                                             const std::vector<float> &kick,
                                             int numSamples) {
  const auto delayInterpolation = interpolationFromChoice(
      delayInterpParam != nullptr ? delayInterpParam->load() : 0.0f);
  const bool analyzeWithCrossover = crossoverEnableParamRaw == nullptr ||
                                    crossoverEnableParamRaw->load() > 0.5f;
  const float analysisCrossoverHz =
      crossoverFreqParam != nullptr ? crossoverFreqParam->load() : 150.0f;

  PhaseFixResult result;
  std::vector<float> analyzedBass, analyzedKick;
  std::vector<float> crossoverBass;
  const std::vector<float>* bassForAnalysis = &bass;

  if (analyzeWithCrossover && numSamples > 0) {
    crossoverBass = bass;
    applyCrossoverPhaseSimulation(crossoverBass, getSampleRate(),
                                  analysisCrossoverHz);
    bassForAnalysis = &crossoverBass;
  }

  if (numSamples > 32) {
    const auto hits = extractRecentHitWindows(kick, getSampleRate(), 8);
    if (!hits.empty())
      appendHitWindows(*bassForAnalysis, kick, hits, analyzedBass, analyzedKick);
    else {
      analyzedBass = *bassForAnalysis;
      analyzedKick = kick;
    }

    result = analyzeAggregatedHits(*bassForAnalysis, kick, hits, getSampleRate(),
                                   delayInterpolation);
  } else {
    PhaseFixEngine::updateDerivedResultFields(result);
  }

  // P6: replace the generic "waiting for signal" text with a specific reason
  // (no kick / no bass / not enough material) when there's no usable result.
  refineInsufficientSignalMessage(result, *bassForAnalysis, kick, numSamples,
                                  getSampleRate());

  // Publish the result and the window it was computed from under the lock so
  // a background worker and the message thread never race on them.
  {
    const std::lock_guard<std::mutex> lock(resultMutex);
    
    // Map the internal offline scores to the live meter scale for display
    result.displayBeforeMatchPercent = liveLowEndMatchPercent.load();
    const float improvement = result.predictedAfterMatchPercent - result.beforeMatchPercent;
    result.displayAfterMatchPercent = juce::jlimit(0.0f, 100.0f, result.displayBeforeMatchPercent + improvement);
    
    latestFixResult = result;
    lastAnalyzedBassWindow = std::move(analyzedBass);
    lastAnalyzedKickWindow = std::move(analyzedKick);
    lastAnalyzedCrossoverHz = analysisCrossoverHz;
  }

  latestAnalyzedBeforePercent.store(result.beforeMatchPercent);
  latestAnalyzedAfterPercent.store(result.predictedAfterMatchPercent);
  latestVerifiedAfterPercent.store(result.verifiedAfterMatchPercent);
  latestVerificationDeltaPercent.store(result.verificationDeltaPercent);
  latestFixConfidence.store(result.confidence * 100.0f);

  return result;
}

bool KickLockAudioProcessor::beginBackgroundAnalyze() {
  // Reject re-entry while an analysis is already in flight.
  if (analyzeStateIsBusy(analyzeState.load(std::memory_order_acquire)))
    return false;

  // Gate the click on the sidechain being routed and on there being enough
  // captured material to analyse — NOT on the instantaneous held-activity
  // flags. Those flags legitimately lapse in the gap between kick transients;
  // re-checking them here (in addition to the editor already gating the
  // button on them) only added a race where a click landing mid-gap was
  // silently dropped with no feedback. computeAndPublishFix() itself reports
  // "not enough material" cleanly if the captured window turns out unusable,
  // and analysing what was just playing right after the transport stops is
  // the correct behaviour.
  const int minMaterialSamples = (int)(getSampleRate() * 0.5);
  if (!sidechainReferenceAvailable.load(std::memory_order_acquire) ||
      rawCapture.getFilledSamples() < minMaterialSamples) {
    return false;
  }

  setParameterValueWithGesture("crossover_enable", 1.0f);

  analyzeState.store(AnalyzeState::Preparing, std::memory_order_release);

  // Snapshot on the message thread (allocates, but off the audio thread),
  // then hand the copy to the worker. The worker only ever touches this copy
  // and the lock-guarded result fields, never live/mutable audio buffers.
  auto bass = std::make_shared<std::vector<float>>();
  auto kick = std::make_shared<std::vector<float>>();
  const int n = rawCapture.snapshot(*bass, *kick);

  analysisThreadPool.addJob([this, bass, kick, n] {
    juce::ScopedNoDenormals noDenormals;
    analyzeState.store(AnalyzeState::Analyzing, std::memory_order_release);

    try {
      const auto result = computeAndPublishFix(*bass, *kick, n);

      const bool usable = result.enoughSignal;
      analyzeState.store(usable ? AnalyzeState::ResultReady
                                : AnalyzeState::NotEnoughMaterial,
                         std::memory_order_release);
    } catch (...) {
      PhaseFixResult failed;
      failed.message = "Analyze failed. Keep the loop playing and try again.";

      {
        const std::lock_guard<std::mutex> lock(resultMutex);
        latestFixResult = failed;
        lastAnalyzedBassWindow.clear();
        lastAnalyzedKickWindow.clear();
      }

      analyzeState.store(AnalyzeState::Failed, std::memory_order_release);
    }
  });

  return true;
}

void KickLockAudioProcessor::acknowledgeAnalyzeState() noexcept {
  // Called by the UI once it has consumed a resolved state, so a subsequent
  // Analyze can transition cleanly from Idle-like semantics again.
  if (analyzeStateIsResolved(analyzeState.load(std::memory_order_acquire)))
    analyzeState.store(AnalyzeState::Idle, std::memory_order_release);
}

bool KickLockAudioProcessor::applyLatestFix() {
  // Snapshot the shared result + analysis windows under lock so a background
  // analysis job can't swap them out from under us mid-apply. All the heavy
  // scoring below then runs on these local copies with the lock released.
  PhaseFixResult fix;
  std::vector<float> bassWindow;
  std::vector<float> kickWindow;
  float analyzedCrossoverHz = 150.0f;
  {
    const std::lock_guard<std::mutex> lock(resultMutex);
    fix = latestFixResult;
    bassWindow = lastAnalyzedBassWindow;
    kickWindow = lastAnalyzedKickWindow;
    analyzedCrossoverHz = lastAnalyzedCrossoverHz;
  }

  if (!fix.applyAllowed && !fix.optionalApplyAllowed)
    return false;

  const float currentCrossoverHz =
      crossoverFreqParam != nullptr ? crossoverFreqParam->load() : 150.0f;
  if (std::abs(currentCrossoverHz - analyzedCrossoverHz) > 1.0f) {
    const std::lock_guard<std::mutex> lock(resultMutex);
    latestFixResult.verificationWarning = true;
    latestFixResult.message =
        "Crossover changed after analysis. Analyze again before applying.";
    return false;
  }

  latestRevertSnapshot = captureCurrentParameterSnapshot();
  revertSnapshotValid.store(true, std::memory_order_release);
  latestAppliedBeforePercent.store(fix.beforeMatchPercent);

  setParameterValueWithGesture("polarity_invert",
                               fix.bassPolarityInvert ? 1.0f : 0.0f);
  setParameterValueWithGesture("polarityInvert",
                               fix.bassPolarityInvert ? 1.0f : 0.0f);
  setParameterValueWithGesture("delay_ms",
                               juce::jlimit(-20.0f, 20.0f, fix.bassDelayMs));
  setParameterValueWithGesture("delayMs",
                               juce::jlimit(-20.0f, 20.0f, fix.bassDelayMs));

  if (fix.phaseFilterEnabled) {
    setParameterValueWithGesture("allpass_enable", 1.0f);
    setParameterValueWithGesture("phaseFilterEnabled", 1.0f);
    setParameterValueWithGesture(
        "allpass_freq", juce::jlimit(20.0f, 500.0f, fix.phaseFilterFreqHz));
    setParameterValueWithGesture(
        "rotatorFreq", juce::jlimit(20.0f, 500.0f, fix.phaseFilterFreqHz));
    setParameterValueWithGesture("rotatorQ", fix.phaseFilterQ);
    setParameterValueWithGesture(
        "rotatorStages", (float)juce::jlimit(0, 2, fix.phaseFilterStages - 2));
  }

  setParameterValueWithGesture("crossover_enable", 1.0f);

  {
    const std::lock_guard<std::mutex> lock(resultMutex);
    latestFixResult.applyAllowed = false;
    latestFixResult.optionalApplyAllowed = false;
  }

  if (!bassWindow.empty() && bassWindow.size() == kickWindow.size()) {
    PhaseFixRenderSettings settings;
    const auto appliedSnapshot = captureCurrentParameterSnapshot();
    settings.bassPolarityInvert = appliedSnapshot.polarityInvert;
    settings.bassDelayMs = appliedSnapshot.delayMs;
    settings.phaseFilterEnabled = appliedSnapshot.phaseFilterEnabled;
    settings.phaseFilterFreqHz = appliedSnapshot.phaseFilterFreqHz;
    settings.phaseFilterQ = appliedSnapshot.phaseFilterQ;
    settings.phaseFilterStages = 2 + appliedSnapshot.phaseFilterStageIndex;
    settings.delayInterpolation = interpolationFromChoice(
        delayInterpParam != nullptr ? delayInterpParam->load() : 0.0f);

    // Verify on the analysis pool, not the message thread: scoring renders
    // the whole analyzed window through the delay + rotator and can take
    // tens of milliseconds — enough to visibly hitch the UI on click. The
    // windows are moved into shared_ptrs because ThreadPool jobs must be
    // copyable. Same publish-under-resultMutex pattern as the analyze job;
    // the editor picks the verification numbers up on its poll timer.
    auto verifyBass =
        std::make_shared<std::vector<float>>(std::move(bassWindow));
    auto verifyKick =
        std::make_shared<std::vector<float>>(std::move(kickWindow));
    const double verifySampleRate = getSampleRate();

    analysisThreadPool.addJob([this, verifyBass, verifyKick, settings,
                               verifySampleRate] {
      juce::ScopedNoDenormals noDenormals;

      const auto verified = PhaseFixEngine::scoreSettings(
          verifyBass->data(), verifyKick->data(), (int)verifyBass->size(),
          verifySampleRate, settings, PhaseFixEngine::absoluteManualMaxDelayMs);

      const std::lock_guard<std::mutex> lock(resultMutex);
      // Note: if a re-analyze raced us, this stamps verification onto the
      // newer result — same (accepted) behaviour as the old synchronous
      // path, which also wrote to whatever latestFixResult held by then.
      PhaseFixEngine::applyVerification(latestFixResult, verified.matchPercent);
      latestVerifiedAfterPercent.store(
          latestFixResult.verifiedAfterMatchPercent);
      latestVerificationDeltaPercent.store(
          latestFixResult.verificationDeltaPercent);
    });
  }

  return true;
}

bool KickLockAudioProcessor::revertLatestFix() {
  if (!revertSnapshotValid.load(std::memory_order_acquire))
    return false;

  restoreParameterSnapshot(latestRevertSnapshot);
  revertSnapshotValid.store(false, std::memory_order_release);
  latestAppliedBeforePercent.store(-1.0f);
  return true;
}

PhaseFixResult KickLockAudioProcessor::getLatestFixResult() const {
  const std::lock_guard<std::mutex> lock(resultMutex);
  return latestFixResult;
}

int KickLockAudioProcessor::getScopeDecimationFactor() const noexcept {
  return scopeDecimationFactor;
}

bool KickLockAudioProcessor::hasSidechainReference() const noexcept {
  return sidechainReferenceAvailable.load();
}

bool KickLockAudioProcessor::isTempoAvailable() const noexcept {
  return tempoAvailable.load();
}

float KickLockAudioProcessor::getLatestBpm() const noexcept {
  return latestBpm.load();
}

float KickLockAudioProcessor::getBassSignalRms() const noexcept {
  return bassSignalRms.load();
}

float KickLockAudioProcessor::getKickSignalRms() const noexcept {
  return kickSignalRms.load();
}

void KickLockAudioProcessor::setLatestFixResultForTesting(
    const PhaseFixResult &result) {
  const std::lock_guard<std::mutex> lock(resultMutex);
  latestFixResult = result;
}

void KickLockAudioProcessor::requestAutoAlign() {
  if (autoAlignEngine != nullptr)
    autoAlignEngine->requestCapture();
}

juce::AudioProcessor *JUCE_CALLTYPE createPluginFilter() {
  return new KickLockAudioProcessor();
}
