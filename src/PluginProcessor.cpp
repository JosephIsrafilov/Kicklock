#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "dsp/FrequencyDomainPhaseRefiner.h"
#include "dsp/HitConsensus.h"
#include "dsp/LearnPipelineCore.h"
#include "dsp/MultiBandCorrelation.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <exception>
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
                                      InterpolationType delayInterpolation,
                                      const std::function<bool()> &shouldCancel = {}) {
  PhaseFixResult aggregated;

  if (bass.empty() || kick.empty()) {
    PhaseFixEngine::updateDerivedResultFields(aggregated);
    return aggregated;
  }

  if (hits.empty()) {
    if (shouldCancel && shouldCancel())
      return aggregated;
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
    if (shouldCancel && shouldCancel())
      return {};
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
    if (shouldCancel && shouldCancel())
      return {};

    if (hitResult.enoughSignal) {
      perHitResults.push_back(hitResult);
      refinementHits.push_back(
          {bass.data() + hit.start, kick.data() + hit.start, hit.length,
           hitResult.bassDelayMs * (float)sampleRate / 1000.0f,
           hitResult.bassPolarityInvert});

      auto multiBandResult = MultiBandCorrelation::analyze(
          bass.data() + hit.start, kick.data() + hit.start, hit.length,
          sampleRate);
      if (shouldCancel && shouldCancel())
        return {};

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
  if (shouldCancel && shouldCancel())
    return {};
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

  // Canonical evaluation policy: once the recommendation settings are chosen,
  // score both sides on the exact concatenation of the selected hit windows.
  // This pair is also stored by computeAndPublishFix() for Apply verification;
  // never mix this with the full rolling capture or a live meter reading.
  const auto *evaluationBass = allHitBass.empty() ? bass.data() : allHitBass.data();
  const auto *evaluationKick = allHitKick.empty() ? kick.data() : allHitKick.data();
  const int evaluationSamples = allHitBass.empty() ? (int) bass.size()
                                                    : (int) allHitBass.size();

  if (evaluationBass != nullptr && evaluationKick != nullptr && evaluationSamples > 0)
  {
    if (shouldCancel && shouldCancel())
      return {};
    const auto before = PhaseFixEngine::scoreSettings (
        evaluationBass, evaluationKick, evaluationSamples, sampleRate, {},
        PhaseFixEngine::absoluteManualMaxDelayMs);
    if (shouldCancel && shouldCancel())
      return {};
    const auto after = PhaseFixEngine::scoreSettings (
        evaluationBass, evaluationKick, evaluationSamples, sampleRate, settings,
        PhaseFixEngine::absoluteManualMaxDelayMs);
    if (shouldCancel && shouldCancel())
      return {};

    aggregated.beforeMatchPercent = before.matchPercent;
    aggregated.afterMatchPercent = after.matchPercent;
    aggregated.predictedAfterMatchPercent = after.matchPercent;
    aggregated.displayBeforeMatchPercent = before.matchPercent;
    aggregated.displayAfterMatchPercent = after.matchPercent;
  }
  else
  {
    aggregated.beforeMatchPercent = 0.0f;
    aggregated.afterMatchPercent = 0.0f;
    aggregated.predictedAfterMatchPercent = 0.0f;
    aggregated.displayBeforeMatchPercent = 0.0f;
    aggregated.displayAfterMatchPercent = 0.0f;
  }

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
     const bool stopped = waitForThreadToExit (KickLockAudioProcessor::cooperativeTeardownBoundMs);
     if (! stopped)
       waitForThreadToExit (-1);
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
      if (threadShouldExit())
        break;
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

  Result analyzeCapturedBuffers() {
    Result result;
    double bestAbsCorrelation = 0.0;
    double bestSignedCorrelation = 0.0;
    int bestLag = 0;

    for (int lag = -maxLagSamples; lag <= maxLagSamples; ++lag) {
      if (threadShouldExit())
        return {};
      double xy = 0.0;
      double xx = 0.0;
      double yy = 0.0;
      int count = 0;

      for (int i = 0; i < captureSamples; ++i) {
        if ((i & 255) == 0 && threadShouldExit())
          return {};
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

class KickLockAudioProcessor::LearnWorker : public juce::Thread
{
public:
  explicit LearnWorker (KickLockAudioProcessor& p)
      : juce::Thread ("KickLock Learn"), owner (p) {}

  void run() override
  {
    while (! threadShouldExit())
    {
      owner.prepareLearnQueueIfSafe();
      owner.resetLearnQueueIfSafe();

      std::shared_ptr<const LearnSessionContext> context;
      {
        const std::lock_guard<std::mutex> lock (owner.learnWorkerSessionMutex);
        context = owner.learnWorkerSession;
      }
      const auto sessionId = owner.activeLearnSessionId.load (std::memory_order_acquire);
      if (context != nullptr && context->sessionId == sessionId
          && learnStateIsBusy (owner.learnState.load (std::memory_order_acquire)))
      {
        owner.runLearnWorker (sessionId, *context);
        continue;
      }

      wait (10);
    }
  }

  void waitForWork (int timeoutMs) { wait (timeoutMs); }

private:
  KickLockAudioProcessor& owner;
};

// Phase 9: dedicated measurement worker. Pops one completed capture from the
// bounded audio -> worker queue, scores it with the pure canonical scorer
// (worker allocation is allowed; this thread is never the audio thread), and
// pushes the small result back through the bounded worker -> audio queue.
// Never touches the DynamicStateMap or APVTS.
class KickLockAudioProcessor::DynamicMeasurementWorker : public juce::Thread
{
public:
    explicit DynamicMeasurementWorker (KickLockAudioProcessor& p)
        : juce::Thread ("KickLock Dynamic Measurement"), owner (p) {}

    void run() override
    {
        while (! threadShouldExit())
        {
            if (owner.serviceDynamicMeasurementWorkerStep())
                continue;
            wait (10);
        }
    }

private:
    KickLockAudioProcessor& owner;
};

bool KickLockAudioProcessor::serviceDynamicMeasurementWorkerStep()
{
    DynamicRuntimeMeasurementCaptureResult capture;
    if (! dynamicMeasurementCaptureQueue.pop (capture))
        return false;

    DynamicMeasurementScoredCapture scored;
    scored.mapGeneration = capture.mapGeneration;
    scored.stableStateId = capture.stableStateId;
    scored.branchKind = capture.branchKind;
    scored.triggerSample = capture.triggerSample;
    scored.score = DynamicMeasurementScorer::scoreCapturedPair (
        capture.beforeBass.data(), capture.beforeKick.data(),
        capture.afterBass.data(), capture.afterKick.data(),
        capture.windowSamples, capture.sampleRate);
    scored.valid = capture.valid && scored.score.valid;

    dynamicMeasurementScoreQueue.push (scored);
    return true;
}

void KickLockAudioProcessor::CallbackPauseControlForTesting::pause()
{
  const std::lock_guard<std::mutex> lock (mutex);
  paused = true;
  entered = false;
}

void KickLockAudioProcessor::CallbackPauseControlForTesting::release()
{
  {
    const std::lock_guard<std::mutex> lock (mutex);
    paused = false;
  }
  condition.notify_all();
}

bool KickLockAudioProcessor::CallbackPauseControlForTesting::waitUntilEntered (int timeoutMs)
{
  std::unique_lock<std::mutex> lock (mutex);
  return condition.wait_for (lock, std::chrono::milliseconds (std::max (0, timeoutMs)),
                             [this] { return entered; });
}

void KickLockAudioProcessor::LearnWorkerPauseControlForTesting::pause (
    LearnState state, bool shouldIgnoreCancellation)
{
  const std::lock_guard<std::mutex> lock (mutex);
  pausedState = state;
  ignoreCancellation = shouldIgnoreCancellation;
  entered = false;
}

void KickLockAudioProcessor::LearnWorkerPauseControlForTesting::release()
{
  {
    const std::lock_guard<std::mutex> lock (mutex);
    pausedState = LearnState::Idle;
    ignoreCancellation = false;
  }
  condition.notify_all();
}

bool KickLockAudioProcessor::LearnWorkerPauseControlForTesting::waitUntilEntered (int timeoutMs)
{
  std::unique_lock<std::mutex> lock (mutex);
  return condition.wait_for (lock, std::chrono::milliseconds (std::max (0, timeoutMs)),
                             [this] { return entered; });
}

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
  correctionModeParam = apvts.getRawParameterValue("correction_mode");
  dynamicStrengthParam = apvts.getRawParameterValue("dynamic_strength");

  for (const auto *id :
       {"delay_ms", "delayMs", "polarity_invert", "polarityInvert",
        "allpass_enable", "phaseFilterEnabled", "allpass_freq", "rotatorFreq"})
    apvts.addParameterListener(id, this);

   spectrumFifo.prepare (16384);
   autoAlignEngine = std::make_unique<AutoAlignEngine>(*this);
   learnWorker = std::make_unique<LearnWorker> (*this);
   learnWorker->startThread();
   dynamicMeasurementWorker = std::make_unique<DynamicMeasurementWorker> (*this);
   dynamicMeasurementWorker->startThread();
}

KickLockAudioProcessor::~KickLockAudioProcessor() {
  stopTimer();
  {
    const std::lock_guard<std::mutex> lock (mapTimerCallbackMutex);
  }
  shuttingDown.store(true, std::memory_order_release);
  if (dynamicMeasurementWorker != nullptr)
  {
    dynamicMeasurementWorker->signalThreadShouldExit();
    dynamicMeasurementWorker->notify();
    const bool stoppedMeasurementWorker = dynamicMeasurementWorker->waitForThreadToExit (cooperativeTeardownBoundMs);
    if (! stoppedMeasurementWorker)
      dynamicMeasurementWorker->waitForThreadToExit (-1);
    dynamicMeasurementWorker.reset();
  }
  invalidateLearnSession();
  learnStartRequested.store(false, std::memory_order_release);
  learnStopRequested.store(false, std::memory_order_release);
   learnCancelRequested.store(true, std::memory_order_release);
   learnActive.store(false, std::memory_order_release);
   learnQueueReady.store (false, std::memory_order_release);
     if (learnWorker != nullptr)
     {
        learnWorker->signalThreadShouldExit();
        learnWorker->notify();
        const bool stopped = learnWorker->waitForThreadToExit (cooperativeTeardownBoundMs);
        if (! stopped)
          learnWorker->waitForThreadToExit (-1);
       learnWorker.reset();
     }
   {
     const std::lock_guard<std::mutex> lock (learnWorkerSessionMutex);
     learnWorkerSession.reset();
   }

  for (const auto *id :
       {"delay_ms", "delayMs", "polarity_invert", "polarityInvert",
        "allpass_enable", "phaseFilterEnabled", "allpass_freq", "rotatorFreq"})
    apvts.removeParameterListener(id, this);

    // Running analysis jobs observe ThreadPoolJob::shouldExit() between bounded
    // analysis units, so this is a bounded cooperative wait rather than a kill.
    const bool analysisStopped = analysisThreadPool.removeAllJobs (true, cooperativeTeardownBoundMs);
    if (! analysisStopped)
      analysisThreadPool.removeAllJobs (true, -1);
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

  layout.add(std::make_unique<juce::AudioParameterChoice>(
      juce::ParameterID{"correction_mode", 1}, "Correction Mode",
      juce::StringArray{"Static", "Dynamic"}, 0));

  layout.add(std::make_unique<juce::AudioParameterFloat>(
      juce::ParameterID{"dynamic_strength", 1}, "Dynamic Strength",
      juce::NormalisableRange<float>(0.0f, 1.0f, 0.01f), 1.0f,
      juce::AudioParameterFloatAttributes().withStringFromValueFunction(
          [] (float value, int) { return juce::String (std::round (value * 100.0f)) + "%"; })));

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
   const std::lock_guard<std::mutex> learnLifecycleLock (learnControlMutex);
   if (learnStateIsActivelyMutating())
     cancelLearnLocked();

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

  spectrumSidechainDelay.setMaximumDelayInSamples(maxDelaySamples + 4);
  spectrumSidechainDelay.prepare({sampleRate, (juce::uint32)juce::jmax(1, samplesPerBlock), 2});
  spectrumSidechainDelay.reset();
  rawScopeFifo.prepare(8192);
  spectrumFifo.prepare(16384);

  // ~2 seconds of raw bass/kick for the Analyze button's cross-correlation.
  rawCapture.prepare((int)(sampleRate * 2.0));
  // ~20 s covers long multi-bar Learn loops without sharing the 2 s Analyze ring.
  learnLoopCapture.prepare((int)(sampleRate * 20.0), false);
  transientDetector.prepare(sampleRate);
  transientDetector.setThreshold(1.0e-7f);
  transientDetector.setMinimumEnergyGate(1.0e-8f);
  transientDetector.setAttackReleaseMs(2.0f, 60.0f);
  transientDetector.setTriggerRatio(1.35f);
  transientDetector.setHoldoffMs(90.0f);

   // Learn queue storage is rebuilt only after both its audio producer and
   // persistent worker consumer have observed this lifecycle boundary.
  configureDynamicFingerprintTrigger (learnTransientDetector, sampleRate);
  runtimeFingerprintCapture.prepare(sampleRate);
   learnActive.store (false, std::memory_order_release);
    learnAudioCaptureAcknowledged.store (false, std::memory_order_release);
    learnQueueReady.store (false, std::memory_order_release);
    learnQueueSampleRate.store (sampleRate, std::memory_order_release);
    learnQueueResetRequested.store (false, std::memory_order_release);
    learnQueueRequestedGeneration.store (0, std::memory_order_release);
    learnQueuePrepareRequested.store (true, std::memory_order_release);
   if (learnWorker != nullptr)
     learnWorker->notify();
   stopTimer();
  {
    const std::lock_guard<std::mutex> lock (mapPublicationMutex);
    noteMapUpdateQueue.prepare();
    hasPendingMapPublication = false;
  }
  {
    const std::lock_guard<std::mutex> lock (mapMutex);
    activeNoteMap = messageOwnedNoteMap;
  }

  // Phase 7: New DynamicStateMap production runtime. Prepared with the same
  // sample rate / max block / channel count as the Static path, so its reported
  // 20 ms PDC latency matches exactly. The audio-owned active map is seeded from
  // the message-owned map without locking on the audio thread later.
  dynamicRuntimeChannels = juce::jlimit (1, 2, numChannels);
  dynamicMapUpdateQueue.prepare();
  {
    const std::lock_guard<std::mutex> lock (mapMutex);
    activeDynamicStateMap = messageOwnedDynamicStateMap;
  }
  dynamicRuntime.prepare (sampleRate, juce::jmax (1, samplesPerBlock), dynamicRuntimeChannels);
  dynamicRuntime.activateMap (activeDynamicStateMap);
  dynamicRawBassMono.setSize (1, juce::jmax (1, samplesPerBlock), false, false, true);
  dynamicRawKickMono.setSize (1, juce::jmax (1, samplesPerBlock), false, false, true);
  dynamicRuntimeOutput.setSize (dynamicRuntimeChannels, juce::jmax (1, samplesPerBlock), false, false, true);
  activeDynamicMapSource.store (DynamicMapSource::None, std::memory_order_release);
  dynamicHadSidechain = false;
  lastBlockUsedNewDynamic = false;
  dynamicHasLastPlayheadPosition = false;
  dynamicLastPlayheadEndSample = 0;
  dynamicWasPlaying = false;

  dynamicNoteState.reset();
  dynamicSilenceResetSamples = juce::jmax (1, (int) std::round (sampleRate * 0.25));
  dynamicFallbackActive.store (false, std::memory_order_release);
  dynamicMapStale.store (false, std::memory_order_release);

  // Phase 9: measurement queues/aggregation/snapshot are rebuilt every
  // prepareToPlay(), same as the map queue above, so no runtime verification
  // history or in-flight capture survives a sample-rate or block-size change.
  // The measurement worker thread pops dynamicMeasurementCaptureQueue and
  // pushes dynamicMeasurementScoreQueue concurrently with this method
  // (prepareToPlay() is a message-thread/host lifecycle call, never
  // synchronized with any other processor thread by the host), so it must be
  // fully stopped before either queue is reallocated below and only started
  // again once every Phase-9 container it touches is back in a consistent
  // state - otherwise the worker could pop/push mid-reallocation. This
  // mirrors the destructor's own signalThreadShouldExit()/notify()/
  // waitForThreadToExit() teardown idiom exactly, just as a bounded
  // stop-then-restart instead of a final stop.
  if (dynamicMeasurementWorker != nullptr)
  {
    dynamicMeasurementWorker->signalThreadShouldExit();
    dynamicMeasurementWorker->notify();
    if (! dynamicMeasurementWorker->waitForThreadToExit (cooperativeTeardownBoundMs))
      dynamicMeasurementWorker->waitForThreadToExit (-1);
    dynamicMeasurementWorker.reset();
  }

  // The capture queue's slots are pre-sized to this session's actual window
  // length up front (allocation happens here, off the audio thread, never
  // again afterward).
  {
    DynamicRuntimeMeasurementCaptureResult captureQueueTemplate;
    captureQueueTemplate.resizeWindows (DynamicRuntimeMeasurementCaptureContract::windowSamplesFor (sampleRate));
    dynamicMeasurementCaptureQueue.prepare (DynamicMeasurementQueueContract::kCaptureQueueCapacity, captureQueueTemplate);
    dynamicFocusedTraceQueue.prepare (DynamicMeasurementQueueContract::kCaptureQueueCapacity, captureQueueTemplate);
    dynamicMeasurementDrainScratch.resizeWindows (dynamicRuntime.getMeasurementWindowSamples());
  }
  dynamicMeasurementScoreQueue.prepare (DynamicMeasurementQueueContract::kScoreQueueCapacity);
  recentUnknownQueue.prepare (DynamicRecentUnknownContract::kMaxRecentUnknownClusters);
  recentUnknownLog.clear();
  dynamicPredictedMeasurementQueue.prepare (4);
  dynamicVerifiedAggregation.reset();
  dynamicVerifiedAggregation.reconcile (activeDynamicStateMap, dynamicRuntime.getMapGeneration());
  dynamicSnapshotPublisher.prepare();
  dynamicSnapshotBlockCounter = 0;
  {
    const std::lock_guard<std::mutex> lock (mapMutex);
    activeDynamicPredictedMeasurements = messageOwnedDynamicPredictedMeasurements;
  }

  // Every Phase-9 container the worker touches is consistent again; safe to
  // resume servicing the queues.
  dynamicMeasurementWorker = std::make_unique<DynamicMeasurementWorker> (*this);
  dynamicMeasurementWorker->startThread();
  activeMidiNote.store (-1, std::memory_order_release);

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
  // Only flip the atomic validity gate here. The RevertBundle storage is
  // message-thread-owned and must not be mutated from prepareToPlay(), which
  // the host may call off the message thread.
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

    const bool usingLearnQueue = learnActive.load (std::memory_order_relaxed)
        && learnQueueReady.load (std::memory_order_acquire) && enterLearnQueue();

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
      runtimeFingerprintCapture.pushSample(rawBassLow, kickLow);

      // Learn capture (Phase 2 plumbing only; inert until a later phase sets
      // learnActive). Uses the dedicated Learn transient detector and feeds the
      // RT-safe hit queue with the raw low-band pair and the tracked pitch. It
      // performs no analysis and never writes the audio output.
        if (usingLearnQueue && learnQueueReady.load (std::memory_order_acquire))
        {
          // Dynamic Learn uses the same canonical raw kick source and detector
          // configuration as the Phase-7 runtime. The queued analysis window
          // remains low-band for legacy timing/rotator work only.
          const bool learnTrigger = learnTransientDetector.processSample(kickMono);
          const int fingerprintSamples = DynamicFingerprintWindow::forSampleRate (getSampleRate()).windowSamples;
          const bool rawTimelineHasHeadroom = learnLoopCapture.getFilledSamples() + fingerprintSamples
              <= learnLoopCapture.getCapacity();
          const bool acceptTrigger = learnActive.load (std::memory_order_relaxed)
              && ! learnStopRequested.load (std::memory_order_acquire) && rawTimelineHasHeadroom;
          learnHitQueue.pushSample(rawBassLow, kickLow, learnTrigger,
                                   pitchTracker.getFrequencyHz(), acceptTrigger);
          // Same sample index timeline as LearnHitQueue::absoluteSampleAtTrigger.
          learnLoopCapture.push(bassMono, kickMono);
        }

      if (autoAlignEngine != nullptr)
        autoAlignEngine->pushSample(bassLow, kickLow);

      if (++dryMeterDecimationCounter >= meterDecimationFactor) {
        dryMeterDecimationCounter = 0;
        dryMultiBandMeter.pushSample(rawBassLow, kickLow);
      }
    }
    if (usingLearnQueue)
      leaveLearnQueue();
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
}

void KickLockAudioProcessor::processBlockBypassed(
    juce::AudioBuffer<float> &buffer, juce::MidiBuffer &) {
  juce::ScopedNoDenormals noDenormals;
  serviceLearnAudioCommands();
  drainPendingMapUpdates();
  drainDynamicMapUpdates();
  dynamicNoteState.reset();
  dynamicFallbackActive.store(false, std::memory_order_release);
  dynamicMapStale.store(false, std::memory_order_release);
  activeMidiNote.store(-1, std::memory_order_release);

  auto mainBuffer = getBusBuffer(buffer, true, 0);
  auto sidechainBuffer = getBusBuffer(buffer, true, 1);

  // Bypass must not kill diagnostic observation: whenever the host still
  // routes a sidechain, keep the same capture/metering/transient/Kick-Punch
  // path alive that processBlock() runs, even though the corrective DSP
  // below is skipped (a fixed-delay passthrough only, to preserve PDC).
  const bool hasSidechain = isSidechainBusActive(sidechainBuffer);
  sidechainReferenceAvailable.store(hasSidechain);
  scopeFifo.setChannelCounts (mainBuffer.getNumChannels(),
                              hasSidechain ? sidechainBuffer.getNumChannels() : 0);
  rawScopeFifo.setChannelCounts (mainBuffer.getNumChannels(),
                                 hasSidechain ? sidechainBuffer.getNumChannels() : 0);

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

  // Bypass shadow policy: when New Dynamic would be the active source, advance
  // the runtime (capture, matcher, scheduler, hot branches) with the real input
  // but DISCARD its corrective output, so hot-branch and timestamp continuity is
  // preserved and unbypass never replays stale pre-bypass history. The audible
  // output remains the fixed 20 ms delayed dry path below, and the New selection
  // diagnostics stay at their bypass defaults (published only when audible).
  {
    const auto bypassCorrectionMode =
        correctionModeParam != nullptr && correctionModeParam->load() > 0.5f
            ? CorrectionMode::Dynamic : CorrectionMode::Static;
    DynamicMapSource bypassSource = DynamicMapSource::None;
    if (bypassCorrectionMode == CorrectionMode::Dynamic)
      bypassSource = resolveDynamicMapSource(activeDynamicStateMap, activeNoteMap);
    const bool shadowNew = bypassCorrectionMode == CorrectionMode::Dynamic
        && bypassSource == DynamicMapSource::NewDynamicStateMap
        && dynamicRuntime.isPrepared()
        && mainBuffer.getNumChannels() >= dynamicRuntimeChannels;
    activeDynamicMapSource.store(bypassCorrectionMode == CorrectionMode::Dynamic
                                     ? bypassSource : DynamicMapSource::None,
                                 std::memory_order_release);
    if (shadowNew)
      renderNewDynamicRuntime(mainBuffer, sidechainBuffer, hasSidechain, numSamples,
                              /*shadowOnly=*/true);
    lastBlockUsedNewDynamic = shadowNew;
  }

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
      if (spectrumCaptureEnabled.load(std::memory_order_relaxed)) {
        const float rawBassL = numChannels > 0 ? mainWrite[0][i] : 0.0f;
        const float rawBassR = numChannels > 1 ? mainWrite[1][i] : rawBassL;
        const float sideRawL = scRead[0] != nullptr ? scRead[0][i] : 0.0f;
        const float sideRawR = scRead[1] != nullptr ? scRead[1][i] : sideRawL;

        spectrumSidechainDelay.pushSample (0, sideRawL);
        spectrumSidechainDelay.pushSample (1, sideRawR);
        const float alignedKickL = spectrumSidechainDelay.popSample (0, (float) getLatencySamples());
        const float alignedKickR = spectrumSidechainDelay.popSample (1, (float) getLatencySamples());

        spectrumFifo.pushSample (rawBassL, rawBassR, alignedKickL, alignedKickR,
                                 juce::jlimit (0, 2, numChannels),
                                 juce::jlimit (0, 2, sidechainChannels));
      }
    }
  }

  if (!hasSidechain)
    realtimeCorrelation.store(0.0f);

  for (auto channel = getTotalNumInputChannels();
       channel < getTotalNumOutputChannels(); ++channel)
    buffer.clear(channel, 0, buffer.getNumSamples());

  // Phase 9 / Section 19 bypass policy: shadow rendering may have completed
  // captures, but the audible output stays the fixed dry delay, so those
  // captures must never be published as verified correction - drain and
  // discard them (never overwrite an in-flight slot, never block audio) and
  // publish the snapshot with bypassActive=true instead.
  {
    DynamicRuntimeMeasurementCaptureResult discardedCapture;
    while (dynamicRuntime.takeCompletedMeasurementCapture(discardedCapture)) {}
  }
  drainDynamicMeasurementScores();
  publishDynamicRuntimeSnapshot(hasSidechain, /*bypassActive=*/true);
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
  serviceLearnAudioCommands();
  drainPendingMapUpdates();
  drainDynamicMapUpdates();

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
  scopeFifo.setChannelCounts (mainBuffer.getNumChannels(),
                              hasSidechain ? sidechainBuffer.getNumChannels() : 0);
  rawScopeFifo.setChannelCounts (mainBuffer.getNumChannels(),
                                 hasSidechain ? sidechainBuffer.getNumChannels() : 0);

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
  float runtimeDelayMs = delayMs;
  bool runtimePolarityInvert = polarityInvert;
  const bool phaseFilterEnabled = getEffectivePhaseFilterEnabled();
  const float manualAllpassFreq = getEffectiveAllpassFreqHz();
  float allpassFreq = manualAllpassFreq;
  float allpassQ =
      rotatorQParam != nullptr ? rotatorQParam->load() : 0.70710678f;
  int allpassStages =
      2 +
      (rotatorStagesParam != nullptr
           ? juce::jlimit(0, 2, (int)std::lround(rotatorStagesParam->load()))
           : 0);
  const bool crossoverEnable = crossoverEnableParamRaw == nullptr ||
                               crossoverEnableParamRaw->load() > 0.5f;
  const float crossoverHz = crossoverFreqParam != nullptr ? crossoverFreqParam->load() : 150.0f;
  const int delayInterpolationIndex = delayInterpParam != nullptr
                                        ? juce::jlimit(0, 1, (int) std::lround(delayInterpParam->load()))
                                        : 0;
  const auto correctionMode = correctionModeParam != nullptr && correctionModeParam->load() > 0.5f
                                ? CorrectionMode::Dynamic : CorrectionMode::Static;

  // Dynamic mode uses the tracker only for note selection. Static mode keeps
  // the established Pitch Follow frequency override. Neither path writes an
  // APVTS parameter from the audio thread.
  const float trackedHz = pitchTracker.getFrequencyHz();
  trackedBassHz.store(trackedHz);

  // Phase 7 source arbitration. Static bypasses Dynamic arbitration entirely.
  // In Dynamic, priority is New DynamicStateMap > Legacy KLNoteMap > None, via
  // resolveDynamicMapSource(). The New runtime path owns its own global base and
  // selection and does not run the Static/Legacy MultibandPhaseCore correction.
  DynamicMapSource dynamicSource = DynamicMapSource::None;
  if (correctionMode == CorrectionMode::Dynamic)
    dynamicSource = resolveDynamicMapSource(activeDynamicStateMap, activeNoteMap);
  const bool useNewDynamic = correctionMode == CorrectionMode::Dynamic
      && dynamicSource == DynamicMapSource::NewDynamicStateMap
      && dynamicRuntime.isPrepared()
      && mainBuffer.getNumChannels() >= dynamicRuntimeChannels;
  activeDynamicMapSource.store(correctionMode == CorrectionMode::Dynamic
                                   ? dynamicSource : DynamicMapSource::None,
                               std::memory_order_release);

  float allpassSmoothingSeconds = 0.030f;
  if (useNewDynamic) {
    // The New Dynamic runtime owns global base + selection; it is rendered
    // below in place of the Static/Legacy MultibandPhaseCore path, and it
    // publishes its own dynamicFallbackActive/activeMidiNote diagnostics.
  } else if (correctionMode == CorrectionMode::Static) {
    const bool pitchFollowActive = pitchTrackParam != nullptr &&
                                   pitchTrackParam->load() > 0.5f &&
                                   phaseFilterEnabled && trackedHz > 0.0f;
    if (pitchFollowActive)
      allpassFreq = juce::jlimit(20.0f, 500.0f, trackedHz);
    dynamicNoteState.reset();
    dynamicFallbackActive.store(false, std::memory_order_release);
    dynamicMapStale.store(false, std::memory_order_release);
    activeMidiNote.store(-1, std::memory_order_release);
  } else if (!phaseFilterEnabled) {
    dynamicNoteState.reset();
    dynamicFallbackActive.store(false, std::memory_order_release);
    dynamicMapStale.store(false, std::memory_order_release);
    activeMidiNote.store(-1, std::memory_order_release);
  } else {
    const auto base = readCurrentRuntimeBaseSettings(delayMs, polarityInvert, crossoverEnable,
                                                      crossoverHz, allpassStages, delayInterpolationIndex);
    ConflictFingerprint freshFingerprint;
    const auto* fingerprint = runtimeFingerprintCapture.takeCompleted (freshFingerprint)
                                ? &freshFingerprint : nullptr;
    const auto selected = selectDynamicRuntime(
        activeNoteMap, base, manualAllpassFreq, allpassQ, trackedHz,
        dynamicStrengthParam != nullptr ? dynamicStrengthParam->load() : 1.0f,
        numSamples, dynamicSilenceResetSamples, dynamicNoteState, fingerprint);
    allpassFreq = selected.targetFreqHz;
    allpassQ = selected.targetQ;
    runtimeDelayMs = selected.targetDelayMs;
    runtimePolarityInvert = selected.targetPolarityInvert;
    allpassStages = selected.targetStages;
    allpassSmoothingSeconds = 0.070f;
    dynamicFallbackActive.store(selected.fallbackActive, std::memory_order_release);
    dynamicMapStale.store(selected.mapStale, std::memory_order_release);
    activeMidiNote.store(selected.selectedMidi, std::memory_order_release);
  }

  MultibandPhaseCore::Params coreParams;

  // Raw parameter pointer cached in the constructor — the previous
  // apvts.getParameter() call here was a per-block string-keyed lookup on
  // the audio thread.
  coreParams.crossoverEnabled = crossoverEnable;
  coreParams.crossoverHz = crossoverHz;
  coreParams.userDelayMs = runtimeDelayMs;
  coreParams.polarityInvert = runtimePolarityInvert;
  coreParams.allpassEnabled = phaseFilterEnabled;
  coreParams.allpassFreqHz = allpassFreq;
  coreParams.allpassQ = allpassQ;
  coreParams.allpassStages = allpassStages;
  coreParams.allpassSmoothingSeconds = allpassSmoothingSeconds;

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
  // The New Dynamic runtime always applies the map's correction, so the
  // processed meter reflects what is heard even when the Static parameters are
  // neutral.
  const bool processingNeeded = !neutral || useNewDynamic;

  if (useNewDynamic)
    renderNewDynamicRuntime(mainBuffer, sidechainBuffer, hasSidechain, numSamples,
                            /*shadowOnly=*/false);
  else
    multibandCore.process(mainBuffer, sidechainBuffer, coreParams, numSamples);

  lastBlockUsedNewDynamic = useNewDynamic;

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

      if (captureSpectrum)
      {
        const float bassL = mainChannels > 0 ? mainBuffer.getReadPointer (0)[i] : 0.0f;
        const float bassR = mainChannels > 1 ? mainBuffer.getReadPointer (1)[i] : bassL;
        const float kickL = sidechainBuffer.getNumChannels() > 0 ? sidechainBuffer.getReadPointer (0)[i] : 0.0f;
        const float kickR = sidechainBuffer.getNumChannels() > 1 ? sidechainBuffer.getReadPointer (1)[i] : kickL;

        spectrumSidechainDelay.pushSample (0, kickL);
        spectrumSidechainDelay.pushSample (1, kickR);
        const float alignedKickL = spectrumSidechainDelay.popSample (0, latencySamplesF);
        const float alignedKickR = spectrumSidechainDelay.popSample (1, latencySamplesF);

        spectrumFifo.pushSample (bassL, bassR, alignedKickL, alignedKickR,
                                 juce::jlimit (0, 2, mainChannels),
                                 juce::jlimit (0, 2, sidechainBuffer.getNumChannels()));
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

  // Phase 9: once per process() call, never once per sample.
  drainDynamicMeasurementCaptures();
  drainDynamicMeasurementScores();
  drainRecentUnknownEvents();
  publishDynamicRuntimeSnapshot(hasSidechain, /*bypassActive=*/false);
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
  snapshot.delayInterpolationIndex = juce::jlimit(
      0, 1, (int)std::lround(readParameterValue("delayInterp", 0.0f)));
  snapshot.pitchTrack = pitchTrackParam ? (pitchTrackParam->load() > 0.5f)
                                      : false;
  snapshot.correctionModeIndex = correctionModeParam
                                     ? juce::jlimit (0, 1, (int) std::lround (correctionModeParam->load()))
                                     : 0;
  snapshot.dynamicStrength = dynamicStrengthParam ? dynamicStrengthParam->load() : 1.0f;
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
  setParameterValueWithGesture("delayInterp",
                               (float)snapshot.delayInterpolationIndex);
  setParameterValueWithGesture("pitch_track", snapshot.pitchTrack ? 1.0f : 0.0f);
  setParameterValueWithGesture("correction_mode", (float) snapshot.correctionModeIndex);
  setParameterValueWithGesture("dynamic_strength", snapshot.dynamicStrength);
}

RuntimeBaseSettings KickLockAudioProcessor::readCurrentRuntimeBaseSettings(
    float delayMs, bool polarityInvert, bool crossoverEnabled, float crossoverHz,
    int allpassStages, int delayInterpolationIndex) const noexcept {
  RuntimeBaseSettings settings;
  settings.delayMs = delayMs;
  settings.polarityInvert = polarityInvert;
  settings.crossoverEnabled = crossoverEnabled;
  settings.crossoverHz = crossoverHz;
  settings.allpassStages = allpassStages;
  settings.delayInterpolationIndex = delayInterpolationIndex;
  return settings;
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
    snapshot.crossoverEnabled = (bool)apvts.state.getProperty(
        juce::Identifier(prefix + "CrossoverEnabled"), snapshot.crossoverEnabled);
    snapshot.crossoverFreqHz = (float)apvts.state.getProperty(
        juce::Identifier(prefix + "CrossoverHz"), snapshot.crossoverFreqHz);
    snapshot.delayInterpolationIndex = juce::jlimit(
        0, 1,
        (int)apvts.state.getProperty(juce::Identifier(prefix + "DelayInterp"),
                                     snapshot.delayInterpolationIndex));
    snapshot.pitchTrack = (bool)apvts.state.getProperty(
        juce::Identifier(prefix + "PitchTrack"), snapshot.pitchTrack);
    snapshot.correctionModeIndex = juce::jlimit(
        0, 1, (int) apvts.state.getProperty(juce::Identifier(prefix + "CorrectionMode"),
                                             snapshot.correctionModeIndex));
    snapshot.dynamicStrength = (float) apvts.state.getProperty(
        juce::Identifier(prefix + "DynamicStrength"), snapshot.dynamicStrength);
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
    apvts.state.setProperty(juce::Identifier(prefix + "CrossoverEnabled"),
                            snapshot.crossoverEnabled, nullptr);
    apvts.state.setProperty(juce::Identifier(prefix + "CrossoverHz"),
                            snapshot.crossoverFreqHz, nullptr);
    apvts.state.setProperty(juce::Identifier(prefix + "DelayInterp"),
                            snapshot.delayInterpolationIndex, nullptr);
    apvts.state.setProperty(juce::Identifier(prefix + "PitchTrack"),
                            snapshot.pitchTrack, nullptr);
    apvts.state.setProperty(juce::Identifier(prefix + "CorrectionMode"),
                            snapshot.correctionModeIndex, nullptr);
    apvts.state.setProperty(juce::Identifier(prefix + "DynamicStrength"),
                            snapshot.dynamicStrength, nullptr);
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
  for (int i = state.getNumChildren() - 1; i >= 0; --i)
  {
    const auto child = state.getChild(i);
    if (child.hasType(juce::Identifier(NoteMapKeys::tree))
        || child.hasType(juce::Identifier(DynamicStateMapKeys::tree)))
      state.removeChild(i, nullptr);
  }

  NotePhaseMapSnapshot appliedMap;
  DynamicStateMap appliedDynamicMap;
  {
    const std::lock_guard<std::mutex> lock(mapMutex);
    appliedMap = messageOwnedNoteMap;
    appliedDynamicMap = messageOwnedDynamicStateMap;
  }
  // The legacy KLNoteMap and the new KLDynamicStateMap are persisted as two
  // independent children so old projects keep working and both coexist.
  state.appendChild(noteMapToValueTree(appliedMap), nullptr);
  state.appendChild(dynamicStateMapToValueTree(appliedDynamicMap), nullptr);
  std::unique_ptr<juce::XmlElement> xml(state.createXml());
  copyXmlToBinary(*xml, destData);
}

void KickLockAudioProcessor::setStateInformation(const void *data,
                                                    int sizeInBytes) {
   const std::lock_guard<std::mutex> learnLifecycleLock (learnControlMutex);
   if (learnStateIsActivelyMutating())
     cancelLearnLocked();

  std::unique_ptr<juce::XmlElement> xml(getXmlFromBinary(data, sizeInBytes));

  if (xml != nullptr && xml->hasTagName(apvts.state.getType())) {
    auto restoredState = juce::ValueTree::fromXml(*xml);
    NotePhaseMapSnapshot restoredMap = NoteMap::makeEmptyNoteMap();
    // The new KLDynamicStateMap is located independently of the legacy KLNoteMap.
    // A missing child restores an empty map (never a previous project's data);
    // a malformed child is rejected by dynamicStateMapFromValueTree() and also
    // restores empty. A legacy map is never converted into a DynamicStateMap.
    DynamicStateMap restoredDynamicMap = makeEmptyDynamicStateMap();
    for (int i = 0; i < restoredState.getNumChildren(); ++i)
    {
      const auto child = restoredState.getChild(i);
      if (child.hasType(juce::Identifier(NoteMapKeys::tree)))
        restoredMap = noteMapFromValueTree(child);
      else if (child.hasType(juce::Identifier(DynamicStateMapKeys::tree)))
        restoredDynamicMap = dynamicStateMapFromValueTree(child);
    }
    auto findParameterState = [&restoredState](const char* id) {
      for (const auto& child : restoredState) {
        if (child.hasType("PARAM")
            && child.getProperty("id").toString() == id)
          return child;
      }

      return juce::ValueTree{};
    };

    auto hasParameterState = [&restoredState, &findParameterState](const char* id) {
      return findParameterState(id).isValid()
             || restoredState.hasProperty(juce::Identifier(id));
    };

    auto getParameterStateValue = [&restoredState, &findParameterState](const char* id) {
      auto parameterState = findParameterState(id);
      return parameterState.isValid()
                 ? parameterState.getProperty("value")
                 : restoredState.getProperty(juce::Identifier(id));
    };

    auto setParameterStateValue = [&restoredState, &findParameterState](const char* id,
                                                                          const juce::var& value) {
      auto parameterState = findParameterState(id);
      if (parameterState.isValid()) {
        parameterState.setProperty("value", value, nullptr);
        return;
      }

      auto newParameterState = juce::ValueTree("PARAM");
      newParameterState.setProperty("id", id, nullptr);
      newParameterState.setProperty("value", value, nullptr);
      restoredState.appendChild(newParameterState, nullptr);
    };

    const bool hasDelayMs = hasParameterState("delay_ms");
    const bool hasLegacyDelayMs = hasParameterState("delayMs");
    const bool hasPolarityInvert = hasParameterState("polarity_invert");
    const bool hasLegacyPolarityInvert = hasParameterState("polarityInvert");
    const bool hasAllpassEnable = hasParameterState("allpass_enable");
    const bool hasLegacyPhaseFilterEnabled = hasParameterState("phaseFilterEnabled");
    const bool hasAllpassFreq = hasParameterState("allpass_freq");
    const bool hasLegacyRotatorFreq = hasParameterState("rotatorFreq");
    const bool hasCorrectionMode = hasParameterState("correction_mode");
    const bool hasDynamicStrength = hasParameterState("dynamic_strength");

    auto migrateLegacyToCanonical = [&getParameterStateValue, &setParameterStateValue](
                                        bool hasCanonical, bool hasLegacy,
                                        const char* canonicalId,
                                        const char* legacyId) {
      if (hasCanonical || !hasLegacy)
        return;

      setParameterStateValue(canonicalId, getParameterStateValue(legacyId));
    };

    migrateLegacyToCanonical(hasDelayMs, hasLegacyDelayMs, "delay_ms",
                             "delayMs");
    migrateLegacyToCanonical(hasPolarityInvert, hasLegacyPolarityInvert,
                             "polarity_invert", "polarityInvert");
    migrateLegacyToCanonical(hasAllpassEnable, hasLegacyPhaseFilterEnabled,
                             "allpass_enable", "phaseFilterEnabled");
    migrateLegacyToCanonical(hasAllpassFreq, hasLegacyRotatorFreq,
                             "allpass_freq", "rotatorFreq");

    // Phase 4 compatibility: old state payloads predate these parameters.
    // Explicitly add their factory values before replaceState(), rather than
    // retaining values from whatever project was loaded previously.
    if (!hasCorrectionMode)
      setParameterStateValue("correction_mode", 0.0f);
    if (!hasDynamicStrength)
      setParameterStateValue("dynamic_strength", 1.0f);

    apvts.replaceState(restoredState);

    // APVTS can skip a discrete parameter setter when its denormalised value
    // is unchanged. Toggle it once so state restoration is still broadcast to
    // the VST3 parameter cache before applying the stored value.
    for (const auto* id : { "crossover_enable", "polarity_invert",
                            "allpass_enable", "pitch_track", "correction_mode", "polarityInvert",
                            "phaseFilterEnabled" }) {
      if (hasParameterState(id)) {
        if (auto* parameter = apvts.getParameter(id)) {
          const float value = (float)getParameterStateValue(id);
          const float normalised = parameter->convertTo0to1(value);
          parameter->setValueNotifyingHost(normalised >= 0.5f ? 0.0f : 1.0f);
          parameter->setValueNotifyingHost(normalised);
        }
      }
    }

    markRestoredParameterSources(
        hasDelayMs || hasLegacyDelayMs, hasLegacyDelayMs,
        hasPolarityInvert || hasLegacyPolarityInvert, hasLegacyPolarityInvert,
        hasAllpassEnable || hasLegacyPhaseFilterEnabled,
        hasLegacyPhaseFilterEnabled, hasAllpassFreq || hasLegacyRotatorFreq,
        hasLegacyRotatorFreq);

    compareSlotsInitialised = false;
    loadCompareSlotsFromState();

    {
      const std::lock_guard<std::mutex> lock(mapMutex);
      messageOwnedNoteMap = restoredMap;
      messageOwnedDynamicStateMap = restoredDynamicMap;
      // Section 11/K: predicted/verified measurements are never serialized,
      // so a restored project always starts Unavailable rather than
      // replaying a previous session's numbers.
      messageOwnedDynamicPredictedMeasurements = {};
    }
    resetResolvedLearnStateToIdle();
    requestMapPublication(restoredMap);
    // Publish the restored DynamicStateMap through the RT-safe SPSC queue. If
    // the queue is transiently full the message-owned copy is retained and
    // prepareToPlay() re-seeds the audio-owned map from it.
    dynamicMapUpdateQueue.push(restoredDynamicMap);
    dynamicPredictedMeasurementQueue.push ({});
  }
  else
  {
    const auto empty = NoteMap::makeEmptyNoteMap();
    const auto emptyDynamic = makeEmptyDynamicStateMap();
    {
      const std::lock_guard<std::mutex> lock(mapMutex);
      messageOwnedNoteMap = empty;
      messageOwnedDynamicStateMap = emptyDynamic;
      messageOwnedDynamicPredictedMeasurements = {};
    }
    resetResolvedLearnStateToIdle();
    requestMapPublication(empty);
    dynamicMapUpdateQueue.push(emptyDynamic);
    dynamicPredictedMeasurementQueue.push ({});
  }
}

PhaseFixResult KickLockAudioProcessor::analyzeFix() {
  const std::lock_guard<std::mutex> controlLock (learnControlMutex);
  if (learnStateIsActivelyMutating()) {
    PhaseFixResult blocked;
    blocked.message = "Learn is active. Stop or cancel Learn before Analyze.";
    return blocked;
  }
  std::vector<float> bass, kick;
  const int n = rawCapture.snapshot(bass, kick);
  return computeAndPublishFix(bass, kick, n);
}

PhaseFixResult
KickLockAudioProcessor::computeAndPublishFix(const std::vector<float> &bass,
                                             const std::vector<float> &kick,
                                             int numSamples,
                                             const std::function<bool()>& shouldCancel) {
  const auto cancelled = [&shouldCancel]
  {
    return shouldCancel && shouldCancel();
  };
  if (cancelled())
    return {};
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
    if (cancelled())
      return {};
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
                                   delayInterpolation, shouldCancel);
    if (cancelled())
      return {};
  } else {
    PhaseFixEngine::updateDerivedResultFields(result);
  }

  // P6: replace the generic "waiting for signal" text with a specific reason
  // (no kick / no bass / not enough material) when there's no usable result.
  refineInsufficientSignalMessage(result, *bassForAnalysis, kick, numSamples,
                                  getSampleRate());
  if (cancelled())
    return {};

  // Publish the result and the window it was computed from under the lock so
  // a background worker and the message thread never race on them.
  {
    const std::lock_guard<std::mutex> lock(resultMutex);

    latestFixResult = result;
    lastAnalyzedBassWindow = std::move(analyzedBass);
    lastAnalyzedKickWindow = std::move(analyzedKick);
    lastAnalyzedCrossoverHz = analysisCrossoverHz;
    lastAnalyzedSampleRate = getSampleRate();
    lastAnalyzedDelayInterpolation = delayInterpolation;
    lastAnalyzedCrossoverEnabled = analyzeWithCrossover;
  }

  latestAnalyzedBeforePercent.store(result.beforeMatchPercent);
  latestAnalyzedAfterPercent.store(result.predictedAfterMatchPercent);
  latestVerifiedAfterPercent.store(result.verifiedAfterMatchPercent);
  latestVerificationDeltaPercent.store(result.verificationDeltaPercent);
  latestFixConfidence.store(result.confidence * 100.0f);

  return result;
}

bool KickLockAudioProcessor::beginBackgroundAnalyze() {
  const std::lock_guard<std::mutex> controlLock (learnControlMutex);
  if (learnStateIsActivelyMutating())
    return false;

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

  // Analyze enables the crossover for its fixed-window scoring. Preserve the
  // complete audible state first, and keep this original bundle until Revert
  // so later Analyze/Apply cycles cannot move the rollback point.
  ensureRevertBundleCaptured();
  setParameterValueWithGesture("crossover_enable", 1.0f);

  analyzeState.store(AnalyzeState::Preparing, std::memory_order_release);

  // Snapshot on the message thread (allocates, but off the audio thread),
  // then hand the copy to the worker. The worker only ever touches this copy
  // and the lock-guarded result fields, never live/mutable audio buffers.
  auto bass = std::make_shared<std::vector<float>>();
  auto kick = std::make_shared<std::vector<float>>();
  const int n = rawCapture.snapshot(*bass, *kick);

  analysisThreadPool.addJob (std::function<juce::ThreadPoolJob::JobStatus()> ([this, bass, kick, n] {
    const auto cancelled = [this]
    {
      const auto* job = juce::ThreadPoolJob::getCurrentThreadPoolJob();
      return shuttingDown.load (std::memory_order_acquire)
          || (job != nullptr && job->shouldExit());
    };
    if (cancelled())
      return juce::ThreadPoolJob::jobHasFinished;
    juce::ScopedNoDenormals noDenormals;
    analyzeState.store(AnalyzeState::Analyzing, std::memory_order_release);

    try {
      const auto result = computeAndPublishFix(*bass, *kick, n, cancelled);
      if (cancelled())
        return juce::ThreadPoolJob::jobHasFinished;

      const bool usable = result.enoughSignal;
      analyzeState.store(usable ? AnalyzeState::ResultReady
                                : AnalyzeState::NotEnoughMaterial,
                          std::memory_order_release);
    } catch (...) {
      if (cancelled())
        return juce::ThreadPoolJob::jobHasFinished;
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
    return juce::ThreadPoolJob::jobHasFinished;
  }));

  return true;
}

void KickLockAudioProcessor::acknowledgeAnalyzeState() noexcept {
  // Called by the UI once it has consumed a resolved state, so a subsequent
  // Analyze can transition cleanly from Idle-like semantics again.
  if (analyzeStateIsResolved(analyzeState.load(std::memory_order_acquire)))
    analyzeState.store(AnalyzeState::Idle, std::memory_order_release);
}

bool KickLockAudioProcessor::applyLatestFix() {
  const std::lock_guard<std::mutex> controlLock (learnControlMutex);
  if (learnStateIsActivelyMutating())
    return false;

  // Snapshot the shared result + analysis windows under lock so a background
  // analysis job can't swap them out from under us mid-apply. All the heavy
  // scoring below then runs on these local copies with the lock released.
  PhaseFixResult fix;
  std::vector<float> bassWindow;
  std::vector<float> kickWindow;
  float analyzedCrossoverHz = 150.0f;
  double analyzedSampleRate = 0.0;
  InterpolationType analyzedDelayInterpolation = InterpolationType::Linear;
  bool analyzedCrossoverEnabled = false;
  {
    const std::lock_guard<std::mutex> lock(resultMutex);
    fix = latestFixResult;
    bassWindow = lastAnalyzedBassWindow;
    kickWindow = lastAnalyzedKickWindow;
    analyzedCrossoverHz = lastAnalyzedCrossoverHz;
    analyzedSampleRate = lastAnalyzedSampleRate;
    analyzedDelayInterpolation = lastAnalyzedDelayInterpolation;
    analyzedCrossoverEnabled = lastAnalyzedCrossoverEnabled;
  }

  if (!fix.applyAllowed && !fix.optionalApplyAllowed)
    return false;

  const float currentCrossoverHz =
      crossoverFreqParam != nullptr ? crossoverFreqParam->load() : 150.0f;
  const auto currentInterpolation = interpolationFromChoice (
      delayInterpParam != nullptr ? delayInterpParam->load() : 0.0f);
  const bool hasStoredEvaluation = analyzedSampleRate > 0.0 && ! bassWindow.empty()
      && bassWindow.size() == kickWindow.size();
  if ((hasStoredEvaluation && std::abs(currentCrossoverHz - analyzedCrossoverHz) > 1.0f)
      || (hasStoredEvaluation && std::abs (getSampleRate() - analyzedSampleRate) > 0.01)
      || (hasStoredEvaluation && currentInterpolation != analyzedDelayInterpolation)) {
    const std::lock_guard<std::mutex> lock(resultMutex);
    latestFixResult.verificationWarning = true;
    latestFixResult.message =
        "Analysis settings changed. Analyze again before applying.";
    return false;
  }

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

  setParameterValueWithGesture ("crossover_enable",
                                hasStoredEvaluation ? (analyzedCrossoverEnabled ? 1.0f : 0.0f)
                                                    : 1.0f);

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
    settings.delayInterpolation = analyzedDelayInterpolation;

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

    analysisThreadPool.addJob (std::function<juce::ThreadPoolJob::JobStatus()> ([this, verifyBass, verifyKick, settings,
                                                                                  verifySampleRate] {
      const auto cancelled = [this]
      {
        const auto* job = juce::ThreadPoolJob::getCurrentThreadPoolJob();
        return shuttingDown.load (std::memory_order_acquire)
            || (job != nullptr && job->shouldExit());
      };
      if (cancelled())
        return juce::ThreadPoolJob::jobHasFinished;
      juce::ScopedNoDenormals noDenormals;

      const auto verified = PhaseFixEngine::scoreSettings(
          verifyBass->data(), verifyKick->data(), (int)verifyBass->size(),
          verifySampleRate, settings, PhaseFixEngine::absoluteManualMaxDelayMs);
      if (cancelled())
        return juce::ThreadPoolJob::jobHasFinished;

      const std::lock_guard<std::mutex> lock(resultMutex);
      // Note: if a re-analyze raced us, this stamps verification onto the
      // newer result — same (accepted) behaviour as the old synchronous
      // path, which also wrote to whatever latestFixResult held by then.
      PhaseFixEngine::applyVerification(latestFixResult, verified.matchPercent);
      latestVerifiedAfterPercent.store(
          latestFixResult.verifiedAfterMatchPercent);
      latestVerificationDeltaPercent.store(
          latestFixResult.verificationDeltaPercent);
      return juce::ThreadPoolJob::jobHasFinished;
    }));
  }

  return true;
}

void KickLockAudioProcessor::ensureRevertBundleCaptured() {
  // Message thread only. revertSnapshotValid is the single validity gate, so a
  // concurrent prepareToPlay() that clears it can never race the storage below.
  if (revertSnapshotValid.load(std::memory_order_acquire))
    return;

  revertBundle.parameters = captureCurrentParameterSnapshot();
  {
    const std::lock_guard<std::mutex> lock(mapMutex);
    revertBundle.noteMap = messageOwnedNoteMap;
    revertBundle.dynamicStateMap = messageOwnedDynamicStateMap;
    revertBundle.dynamicPredictedMeasurements = messageOwnedDynamicPredictedMeasurements;
  }
  revertSnapshotValid.store(true, std::memory_order_release);
}

bool KickLockAudioProcessor::revertLatestFix() {
  // Message thread only.
  const std::lock_guard<std::mutex> controlLock (learnControlMutex);
  if (learnStateIsActivelyMutating()
      || !revertSnapshotValid.load(std::memory_order_acquire))
    return false;

  restoreParameterSnapshot(revertBundle.parameters);
  {
    const std::lock_guard<std::mutex> lock(mapMutex);
    messageOwnedNoteMap = revertBundle.noteMap;
    messageOwnedDynamicStateMap = revertBundle.dynamicStateMap;
    messageOwnedDynamicPredictedMeasurements = revertBundle.dynamicPredictedMeasurements;
  }
  resetResolvedLearnStateToIdle();
  requestMapPublication(revertBundle.noteMap);
  requestDynamicMapPublication(revertBundle.dynamicStateMap);
  requestDynamicPredictedMeasurementPublication(revertBundle.dynamicPredictedMeasurements);
  revertSnapshotValid.store(false, std::memory_order_release);
  return true;
}

bool KickLockAudioProcessor::learnStateIsActivelyMutating() const noexcept
{
  return learnStateIsBusy (learnState.load (std::memory_order_acquire));
}

void KickLockAudioProcessor::clearPendingLearnCandidate()
{
  const std::lock_guard<std::mutex> lock (learnMutex);
  pendingLearnCandidate = {};
  pendingDynamicMeasurementSessionId = 0;
  pendingDynamicPredictedMeasurements = {};
}

void KickLockAudioProcessor::invalidateLearnSession()
{
  activeLearnSessionId.fetch_add (1, std::memory_order_acq_rel);
}

void KickLockAudioProcessor::resetResolvedLearnStateToIdle()
{
  const auto state = learnState.load (std::memory_order_acquire);
  if (state == LearnState::ResultReady || state == LearnState::NotEnoughMaterial
      || state == LearnState::Failed)
    invalidateLearnSession();
  clearPendingLearnCandidate();
  learnState.store (LearnState::Idle, std::memory_order_release);
  const std::lock_guard<std::mutex> lock (learnProgressMutex);
  learnProgress = {};
  learnProgress.state = LearnState::Idle;
  learnProgress.stopRequested = false;
}

bool KickLockAudioProcessor::prepareLearnQueueIfSafe()
{
  if (! learnQueuePrepareRequested.load (std::memory_order_acquire))
    return true;

  if (! beginLearnQueueMutation())
    return false;

  try
  {
    learnHitQueue.prepare (learnQueueSampleRate.load (std::memory_order_acquire));
  }
  catch (...)
  {
    endLearnQueueMutation();
    throw;
  }
  learnQueuePrepareRequested.store (false, std::memory_order_release);
  learnQueueReady.store (true, std::memory_order_release);
  learnQueuePreparedGeneration.fetch_add (1, std::memory_order_acq_rel);
  endLearnQueueMutation();
  return true;
}

bool KickLockAudioProcessor::resetLearnQueueIfSafe()
{
  if (! learnQueueResetRequested.load (std::memory_order_acquire)
      || learnActive.load (std::memory_order_acquire))
    return true;

  if (! beginLearnQueueMutation())
    return false;

  // The producer and consumer are quiescent while reconfiguring is held. This
  // is deliberately the only place a new Learn generation clears pre-roll.
  learnHitQueue.reset();
  learnLoopCapture.reset();
  learnTransientDetector.reset();
  learnQueueReady.store (true, std::memory_order_release);
  learnQueuePreparedGeneration.fetch_add (1, std::memory_order_acq_rel);
  learnQueueResetRequested.store (false, std::memory_order_release);
  endLearnQueueMutation();
  return true;
}

bool KickLockAudioProcessor::enterLearnQueue() noexcept
{
  if (learnQueueReconfiguring.load (std::memory_order_acquire))
    return false;

  learnQueueUsers.fetch_add (1, std::memory_order_acq_rel);
  if (! learnQueueReconfiguring.load (std::memory_order_acquire))
    return true;

  learnQueueUsers.fetch_sub (1, std::memory_order_acq_rel);
  return false;
}

void KickLockAudioProcessor::leaveLearnQueue() noexcept
{
  learnQueueUsers.fetch_sub (1, std::memory_order_release);
}

bool KickLockAudioProcessor::beginLearnQueueMutation() noexcept
{
  bool expected = false;
  if (! learnQueueReconfiguring.compare_exchange_strong (expected, true,
                                                          std::memory_order_acq_rel,
                                                          std::memory_order_acquire))
    return false;
  if (learnQueueUsers.load (std::memory_order_acquire) == 0)
    return true;

  learnQueueReconfiguring.store (false, std::memory_order_release);
  return false;
}

void KickLockAudioProcessor::endLearnQueueMutation() noexcept
{
  learnQueueReconfiguring.store (false, std::memory_order_release);
}

bool KickLockAudioProcessor::pauseLearnWorkerForTesting (LearnState state,
                                                          uint64_t sessionId)
{
  const auto control = learnWorkerPauseControlForTesting;
  std::unique_lock<std::mutex> lock (control->mutex);
  if (control->pausedState != state)
    return true;

  control->entered = true;
  control->condition.notify_all();
  while (control->pausedState == state)
  {
    if (shuttingDown.load (std::memory_order_acquire)
        || (! control->ignoreCancellation
            && (activeLearnSessionId.load (std::memory_order_acquire) != sessionId
                || learnState.load (std::memory_order_acquire) == LearnState::Cancelling)))
      return false;
    control->condition.wait_for (lock, std::chrono::milliseconds (1));
  }
  return true;
}

void KickLockAudioProcessor::serviceLearnAudioCommands() noexcept
{
  if (learnCancelRequested.exchange (false, std::memory_order_acq_rel))
  {
    learnActive.store (false, std::memory_order_release);
    learnHitQueue.stopAcceptingNewHits();
    learnAudioCaptureAcknowledged.store (false, std::memory_order_release);
  }

  if (learnStartRequested.load (std::memory_order_acquire))
  {
    const auto requestedGeneration = learnQueueRequestedGeneration.load (std::memory_order_acquire);
    const auto preparedGeneration = learnQueuePreparedGeneration.load (std::memory_order_acquire);
    if (! learnQueueReady.load (std::memory_order_acquire)
        || requestedGeneration == 0 || preparedGeneration < requestedGeneration)
      return;

    if (learnStartRequested.exchange (false, std::memory_order_acq_rel)
        && learnQueueReady.load (std::memory_order_acquire))
    {
      // Audio only activates a prepared generation. The worker reset its FIFO,
      // counters, and pre-roll arrays before publishing this generation.
      learnQueueActiveGeneration.store (preparedGeneration, std::memory_order_release);
      auto expected = LearnState::Preparing;
      if (learnState.compare_exchange_strong (expected, LearnState::Capturing,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire))
      {
        learnActive.store (true, std::memory_order_release);
        learnAudioCaptureAcknowledged.store (true, std::memory_order_release);
      }
      else
      {
        learnHitQueue.stopAcceptingNewHits();
      }
    }
  }

  if (learnStopRequested.load (std::memory_order_acquire))
  {
    learnHitQueue.stopAcceptingNewHits();
    if (! learnHitQueue.hasInProgressCapture())
    {
      learnActive.store (false, std::memory_order_release);
      learnAudioCaptureAcknowledged.store (false, std::memory_order_release);
    }
  }
}

void KickLockAudioProcessor::drainPendingMapUpdates() noexcept
{
  // Audio thread owns activeNoteMap. A replacement resets only Dynamic note
  // hysteresis; the DSP smoothers retain state and move to the new target.
  bool replaced = false;
  NotePhaseMapSnapshot pendingMap;
  while (noteMapUpdateQueue.pop (pendingMap))
  {
    activeNoteMap = pendingMap;
    replaced = true;
  }
  if (replaced)
    dynamicNoteState.reset();
}

void KickLockAudioProcessor::drainDynamicMapUpdates() noexcept
{
  // Audio thread owns activeDynamicStateMap. Drain the RT-safe SPSC queue; the
  // newest complete update wins. A malformed/empty map arrives as a complete
  // empty map (the deserializer never produces a partial one), so it activates
  // as empty rather than partially. The runtime is only re-activated when a new
  // update was actually drained; its package cache additionally skips redundant
  // branch reconfiguration for identical generation/strength/rate.
  bool replaced = false;
  DynamicStateMap pending;
  while (dynamicMapUpdateQueue.pop(pending))
  {
    activeDynamicStateMap = pending;
    replaced = true;
  }

  // Phase 9: the predicted-measurement sidecar is queued in lockstep with
  // the map at the Apply call site, so drain it in the SAME pass; the newest
  // complete bundle wins, matching the map's own newest-wins policy.
  std::array<DynamicMeasurementSummary, DynamicMeasurementContract::kMaxRetainedStates> pendingPredicted;
  bool predictedReplaced = false;
  while (dynamicPredictedMeasurementQueue.pop (pendingPredicted))
  {
    activeDynamicPredictedMeasurements = pendingPredicted;
    predictedReplaced = true;
  }

  if (replaced)
  {
    dynamicRuntime.activateMap(activeDynamicStateMap);
    dynamicVerifiedAggregation.reconcile (activeDynamicStateMap, dynamicRuntime.getMapGeneration());
    if (! predictedReplaced)
      activeDynamicPredictedMeasurements = {};
  }
}

void KickLockAudioProcessor::drainDynamicMeasurementCaptures() noexcept
{
  // Audio thread: pop every capture DynamicProductionRuntime has finished and
  // confirmed eligible this block, and publish it to the bounded audio ->
  // worker queue. A full queue drops the capture (fixed diagnostic on the
  // queue itself) rather than blocking or overwriting an in-flight publish.
  // dynamicMeasurementDrainScratch is sized once in prepareToPlay(), so
  // neither the hand-off nor the queue push allocates here.
  while (dynamicRuntime.takeCompletedMeasurementCapture (dynamicMeasurementDrainScratch))
  {
    dynamicMeasurementCaptureQueue.push (dynamicMeasurementDrainScratch);
    // Phase 12: mirror the identical, already-settled-gated capture to the
    // Focus trace queue when it belongs to the currently focused State.
    // Never a second, independently-computed capture - same data, same
    // eligibility gate DynamicProductionRuntime already applied.
    if (dynamicMeasurementDrainScratch.stableStateId != 0
        && dynamicMeasurementDrainScratch.stableStateId == focusedStableStateId.load (std::memory_order_relaxed))
      dynamicFocusedTraceQueue.push (dynamicMeasurementDrainScratch);
  }
}

void KickLockAudioProcessor::drainRecentUnknownEvents() noexcept
{
  // Audio thread: same-thread pop from the runtime's tiny staging ring
  // (filled during this same processChunk()), then a normal SPSC push -
  // matches the two-stage shape of drainDynamicMeasurementCaptures() above.
  DynamicRecentUnknownRawEvent event;
  while (dynamicRuntime.takeRecentUnknownEvent (event))
    recentUnknownQueue.push (event);
}

void KickLockAudioProcessor::drainDynamicMeasurementScores() noexcept
{
  // Audio thread: fold every worker-scored result into the fixed rolling
  // verified aggregate. addResult() itself rejects a stale
  // stableStateId/mapGeneration pairing, so a result computed against a map
  // generation that has since been replaced can never update the wrong slot.
  DynamicMeasurementScoredCapture scored;
  while (dynamicMeasurementScoreQueue.pop (scored))
    dynamicVerifiedAggregation.addResult (scored);
}

void KickLockAudioProcessor::publishDynamicRuntimeSnapshot (bool sidechainPresent, bool bypassActive) noexcept
{
  // Audio thread, once per process()/processBlockBypassed() call - never
  // once per sample. Builds a complete, self-contained value from
  // audio-owned state only (activeDynamicStateMap, the runtime's own
  // diagnostics, activeDynamicPredictedMeasurements, and
  // dynamicVerifiedAggregation), then publishes it through the tear-free
  // N-way buffer.
  ++dynamicSnapshotBlockCounter;

  DynamicRuntimeSnapshot snapshot;
  snapshot.mapGeneration = dynamicRuntime.getMapGeneration();
  snapshot.source = activeDynamicMapSource.load (std::memory_order_relaxed);
  snapshot.mapValid = activeDynamicStateMap.valid;
  snapshot.stateCount = getOccupiedDynamicStateCount (activeDynamicStateMap);

  const auto& selectorDiag = dynamicRuntime.getSelectorDiagnostics();
  // A selected semantic State can intentionally route through Global (Candidate,
  // recognized-no-correction, or fallback). Only a settled State/Service branch
  // carries an audible active State identity for UI presentation.
  snapshot.activeSemanticStateId = ! selectorDiag.fadeActive
      && selectorDiag.selectedBranchKind != DynamicSelectorBranchKind::Global
      ? selectorDiag.selectedSemanticStateId : 0;
  snapshot.selectedSemanticStateId = selectorDiag.selectedSemanticStateId;
  snapshot.activeBranchKind = selectorDiag.selectedBranchKind;
  snapshot.selectorDiagnostic = selectorDiag.lastDecision;
  snapshot.holdActive = selectorDiag.holdEventCount > 0;
  snapshot.fallbackActive = dynamicRuntime.isFallbackActive();
  snapshot.sidechainPresent = sidechainPresent;
  snapshot.bypassActive = bypassActive;
  snapshot.captureExhaustedCount = dynamicRuntime.getMeasurementCaptureExhaustedCount();
  snapshot.captureDroppedForTransportCount = dynamicRuntime.getMeasurementCaptureDroppedForTransportCount();
  snapshot.measurementCaptureQueueDroppedCount = (uint64_t) dynamicMeasurementCaptureQueue.getDroppedCount();
  snapshot.measurementScoreQueueDroppedCount = (uint64_t) dynamicMeasurementScoreQueue.getDroppedCount();
  snapshot.timestampSample = dynamicRuntime.getRuntimeSamplePosition();

  for (int slot = 0; slot < DynamicMeasurementContract::kMaxRetainedStates; ++slot)
  {
    const auto& state = activeDynamicStateMap.states[(size_t) slot];
    auto& card = snapshot.states[(size_t) slot];
    card = DynamicStateCard {};
    card.occupied = state.occupied;
    if (! state.occupied)
      continue;

    card.stableStateId = state.stableStateId;
    card.slot = slot;
    card.origin = state.origin;
    card.evidence = state.evidence;
    card.enabled = state.enabled;
    card.bypassed = state.bypassed;
    card.hitCount = state.hitCount;
    card.repeatability = state.repeatability;
    card.ambiguity = state.ambiguity;
    card.hasCorrection = (state.origin == DynamicStateOrigin::Auto
                          && state.evidence == DynamicStateEvidence::Stable
                          && state.hasLearnedPackage)
        || (state.origin == DynamicStateOrigin::Manual && state.hasManualBasePackage);
    card.hasLikelyMidi = state.hasLikelyMidi;
    card.likelyMidi = state.hasLikelyMidi ? state.likelyMidi : -1;
    card.hasLikelyPitchHz = state.hasLikelyPitchHz;
    card.likelyPitchHz = state.likelyPitchHz;
    card.correctionPolicy = state.correctionPolicy;
    card.policyRejectionReason = state.policyRejectionReason;

    card.selected = selectorDiag.selectedSemanticStateId == state.stableStateId;
    card.active = snapshot.activeSemanticStateId == state.stableStateId;
    card.activeBranchKind = card.active ? selectorDiag.selectedBranchKind : DynamicSelectorBranchKind::Global;

    card.predicted = activeDynamicPredictedMeasurements[(size_t) slot];
    card.verified = dynamicVerifiedAggregation.summaryFor (slot);

    if (! state.enabled)
      card.assessment = DynamicCorrectionAssessment::Disabled;
    else if (state.bypassed)
      card.assessment = DynamicCorrectionAssessment::Bypassed;
    else if (card.verified.availability == DynamicMeasurementAvailability::Available
             || card.verified.availability == DynamicMeasurementAvailability::Collecting)
      card.assessment = card.verified.assessment;
    else
      card.assessment = card.predicted.assessment;
    card.rejectionReason = card.predicted.rejectionReason;
  }

  dynamicSnapshotPublisher.publishIfDue (snapshot, dynamicSnapshotBlockCounter);
}

void KickLockAudioProcessor::classifyDynamicTransportForNewRuntime(int numSamples) noexcept
{
  // The New runtime's capture bank and scheduler run on an internal monotonic
  // timeline; the host position is used ONLY to classify transport changes and
  // is never the scheduler's absolute time. A missing playhead / missing sample
  // position must not cause repeated resets.
  bool hasPosition = false;
  bool isPlaying = false;
  bool isLooping = false;
  int64_t startSample = 0;

  if (auto* playHead = getPlayHead())
  {
    if (const auto position = playHead->getPosition())
    {
      isPlaying = position->getIsPlaying();
      isLooping = position->getIsLooping();
      if (const auto timeInSamples = position->getTimeInSamples())
      {
        hasPosition = true;
        startSample = *timeInSamples;
      }
    }
  }

  // Stop -> start: restart from a deterministic Global state; never reuse a
  // pre-stop fingerprint result.
  if (isPlaying && ! dynamicWasPlaying && dynamicHasLastPlayheadPosition)
    dynamicRuntime.notifyTransportReset(DynamicProductionTransportReason::StopStart);

  if (hasPosition && dynamicHasLastPlayheadPosition && isPlaying && dynamicWasPlaying)
  {
    const int64_t expected = dynamicLastPlayheadEndSample;
    if (startSample != expected)
    {
      const bool backward = startSample < expected;
      if (backward && isLooping)
      {
        // Valid loop wrap while the host keeps playing: preserve runtime
        // continuity. The internal monotonic timeline is unaffected, so the
        // lower host position can never become an int64 scheduler rewind.
        dynamicRuntime.notifyTransportReset(DynamicProductionTransportReason::LoopWrap);
      }
      else
      {
        // Any other non-contiguous jump is a seek: clear captures, events,
        // Hold, Service binding, and stale delay/history so old delayed audio
        // cannot leak into the new position.
        dynamicRuntime.notifyTransportReset(DynamicProductionTransportReason::Seek);
      }
    }
  }

  dynamicWasPlaying = isPlaying;
  if (hasPosition)
  {
    dynamicHasLastPlayheadPosition = true;
    dynamicLastPlayheadEndSample = startSample + (int64_t) juce::jmax(0, numSamples);
  }
}

void KickLockAudioProcessor::fillRawDynamicFingerprintInputs(
    const juce::AudioBuffer<float>& mainBuffer,
    const juce::AudioBuffer<float>& sidechainBuffer, bool hasSidechain,
    int numSamples) noexcept
{
  float* bassMono = dynamicRawBassMono.getWritePointer(0);
  float* kickMono = dynamicRawKickMono.getWritePointer(0);

  const int mainCh = mainBuffer.getNumChannels();
  const float mainNorm = mainCh > 0 ? 1.0f / (float)mainCh : 0.0f;
  for (int i = 0; i < numSamples; ++i)
  {
    float sum = 0.0f;
    for (int ch = 0; ch < mainCh; ++ch)
      sum += mainBuffer.getSample(ch, i);
    bassMono[i] = sum * mainNorm;
  }

  if (hasSidechain)
  {
    const int scCh = sidechainBuffer.getNumChannels();
    const float scNorm = scCh > 0 ? 1.0f / (float)scCh : 0.0f;
    for (int i = 0; i < numSamples; ++i)
    {
      float sum = 0.0f;
      for (int ch = 0; ch < scCh; ++ch)
        sum += sidechainBuffer.getSample(ch, i);
      kickMono[i] = sum * scNorm;
    }
  }
  else
  {
    for (int i = 0; i < numSamples; ++i)
      kickMono[i] = 0.0f;
  }
}

void KickLockAudioProcessor::renderNewDynamicRuntime(
    juce::AudioBuffer<float>& mainBuffer,
    const juce::AudioBuffer<float>& sidechainBuffer, bool hasSidechain,
    int numSamples, bool shadowOnly) noexcept
{
  // Entering the New source (from Static / Legacy / None / a previous non-New
  // block): clear stale runtime state so no old State identity is routed. Both
  // paths report and produce exactly 20 ms latency, so no second latency layer
  // is introduced.
  if (! lastBlockUsedNewDynamic)
  {
    dynamicRuntime.notifyTransportReset(DynamicProductionTransportReason::HostReset);
    dynamicHasLastPlayheadPosition = false;
    dynamicWasPlaying = false;
    dynamicHadSidechain = hasSidechain;
  }
  else
  {
    classifyDynamicTransportForNewRuntime(numSamples);
  }

  // Sidechain-loss transition (once, on the edge): return to Global, clear Hold,
  // captures and the Service binding; keep the bass path latency-correct.
  if (dynamicHadSidechain && ! hasSidechain)
    dynamicRuntime.notifySidechainLost();
  dynamicHadSidechain = hasSidechain;

  fillRawDynamicFingerprintInputs(mainBuffer, sidechainBuffer, hasSidechain, numSamples);

  const double strength =
      dynamicStrengthParam != nullptr ? (double)dynamicStrengthParam->load() : 1.0;

  const bool ok = dynamicRuntime.process(
      mainBuffer, dynamicRawBassMono.getReadPointer(0),
      hasSidechain ? dynamicRawKickMono.getReadPointer(0) : nullptr, hasSidechain,
      strength, dynamicRuntimeOutput, numSamples);

  if (! shadowOnly)
  {
    if (ok)
      for (int ch = 0; ch < dynamicRuntimeChannels && ch < mainBuffer.getNumChannels(); ++ch)
        mainBuffer.copyFrom(ch, 0, dynamicRuntimeOutput, ch, 0, numSamples);

    // Publish New Dynamic diagnostics: fallback active whenever selection is not
    // a confidently corrected State/Service branch; the map is never stale on
    // this path (an ineligible map would not have arbitrated to New).
    dynamicFallbackActive.store(dynamicRuntime.isFallbackActive(), std::memory_order_release);
    dynamicMapStale.store(false, std::memory_order_release);
    activeMidiNote.store(dynamicRuntime.getSelectedLikelyMidi(), std::memory_order_release);
  }
}

bool KickLockAudioProcessor::beginLearn()
{
  const std::lock_guard<std::mutex> controlLock (learnControlMutex);
  const auto currentState = learnState.load (std::memory_order_acquire);
  if (learnStateIsBusy (currentState)
      || analyzeStateIsBusy (analyzeState.load (std::memory_order_acquire))
      || shuttingDown.load (std::memory_order_acquire))
    return false;

  resetResolvedLearnStateToIdle();

  const uint64_t sessionId = learnSessionCounter.fetch_add (1, std::memory_order_acq_rel) + 1;
  activeLearnSessionId.store (sessionId, std::memory_order_release);

  LearnSessionContext context;
  context.sessionId = sessionId;
  context.sampleRate = getSampleRate();
  context.delayInterpolation = interpolationFromChoice (
      delayInterpParam != nullptr ? delayInterpParam->load() : 0.0f);
  context.crossoverEnabled = crossoverEnableParamRaw == nullptr
      || crossoverEnableParamRaw->load() > 0.5f;
  context.crossoverHz = crossoverFreqParam != nullptr ? crossoverFreqParam->load() : 150.0f;
  context.preLearnAllpassFreqHz = getEffectiveAllpassFreqHz();
  context.preLearnAllpassQ = rotatorQParam != nullptr ? rotatorQParam->load() : 0.7f;
  context.preLearnAllpassEnabled = getEffectivePhaseFilterEnabled();
  context.preLearnStages = 2 + (rotatorStagesParam != nullptr
      ? juce::jlimit (0, 2, (int) std::lround (rotatorStagesParam->load())) : 0);
  // The audio-thread start command resets all per-session queue counters before
  // acknowledging capture, so each frozen baseline is zero by construction.
  context.acceptedHitsBaseline = 0;
  context.droppedHitsBaseline = 0;
  context.ignoredOverlapsBaseline = 0;

  {
    const std::lock_guard<std::mutex> lock (learnProgressMutex);
    learnProgress = {};
    learnProgress.sessionId = sessionId;
    learnProgress.state = LearnState::Preparing;
  }

  learnStopRequested.store (false, std::memory_order_release);
  learnCancelRequested.store (false, std::memory_order_release);
  learnAudioCaptureAcknowledged.store (false, std::memory_order_release);
  learnState.store (LearnState::Preparing, std::memory_order_release);
  {
    const std::lock_guard<std::mutex> lock (learnWorkerSessionMutex);
    learnWorkerSession = std::make_shared<LearnSessionContext> (context);
  }
  learnQueueRequestedGeneration.store (
      learnQueuePreparedGeneration.load (std::memory_order_acquire) + 1,
      std::memory_order_release);
  learnQueueResetRequested.store (true, std::memory_order_release);
  learnStartRequested.store (true, std::memory_order_release);
  if (learnWorker != nullptr)
    learnWorker->notify();
  return true;
}

bool KickLockAudioProcessor::stopLearn()
{
  const std::lock_guard<std::mutex> controlLock (learnControlMutex);
  const auto state = learnState.load (std::memory_order_acquire);
  if (state != LearnState::Preparing && state != LearnState::Capturing)
    return false;

  learnStopRequested.store (true, std::memory_order_release);
  learnHitQueue.stopAcceptingNewHits();
  auto expected = state;
  if (! learnState.compare_exchange_strong (expected, LearnState::Stopping,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire))
  {
    learnStopRequested.store (false, std::memory_order_release);
    return false;
  }
  {
    const std::lock_guard<std::mutex> lock (learnProgressMutex);
    learnProgress.state = LearnState::Stopping;
    learnProgress.stopRequested = true;
  }
  if (learnWorker != nullptr)
    learnWorker->notify();
  return true;
}

void KickLockAudioProcessor::cancelLearn()
{
  const std::lock_guard<std::mutex> controlLock (learnControlMutex);
  cancelLearnLocked();
}

void KickLockAudioProcessor::cancelLearnLocked()
{
  const auto state = learnState.load (std::memory_order_acquire);
  if (! learnStateIsBusy (state))
    return;

  auto expected = state;
  bool transitioned = false;
  while (learnStateIsBusy (expected))
  {
    if (learnState.compare_exchange_weak (expected, LearnState::Cancelling,
                                          std::memory_order_acq_rel,
                                          std::memory_order_acquire))
    {
      transitioned = true;
      break;
    }
  }
  if (! transitioned)
    return;

  invalidateLearnSession();
  learnStartRequested.store (false, std::memory_order_release);
  learnStopRequested.store (true, std::memory_order_release);
  learnCancelRequested.store (true, std::memory_order_release);
  learnActive.store (false, std::memory_order_release);
  learnHitQueue.stopAcceptingNewHits();

  clearPendingLearnCandidate();
  expected = LearnState::Cancelling;
  learnState.compare_exchange_strong (expected, LearnState::Idle,
                                      std::memory_order_acq_rel,
                                      std::memory_order_acquire);
  {
    const std::lock_guard<std::mutex> lock (learnProgressMutex);
    learnProgress.state = LearnState::Idle;
    learnProgress.stopRequested = false;
  }
  if (learnWorker != nullptr)
    learnWorker->notify();
}

void KickLockAudioProcessor::runLearnWorker (uint64_t sessionId,
                                              LearnSessionContext context)
{
  const auto cancelled = [this, sessionId]
  {
    return shuttingDown.load (std::memory_order_acquire)
        || activeLearnSessionId.load (std::memory_order_acquire) != sessionId
        || learnState.load (std::memory_order_acquire) == LearnState::Cancelling;
  };

  try
  {
  std::vector<LearnHitWindow> windows;
  windows.reserve (24);
  int lastSequence = -1;
  int droppedHits = 0;
  int ignoredOverlaps = 0;

  for (;;)
  {
    if (cancelled())
      return;

    const auto currentState = learnState.load (std::memory_order_acquire);
    if (! learnAudioCaptureAcknowledged.load (std::memory_order_acquire))
    {
      if (currentState == LearnState::Stopping)
      {
        auto expected = LearnState::Stopping;
        if (! learnState.compare_exchange_strong (expected, LearnState::Draining,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire))
          return;
        if (! pauseLearnWorkerForTesting (LearnState::Draining, sessionId))
          return;
        break;
      }
      if (learnWorker != nullptr)
        learnWorker->waitForWork (2);
      continue;
    }

    bool drainedAny = false;
    bool captureInProgress = false;
    int pendingHits = 0;
    if (! enterLearnQueue())
    {
      if (cancelled())
        return;
      if (learnWorker != nullptr)
        learnWorker->waitForWork (2);
      continue;
    }
    if (cancelled() || ! learnQueueReady.load (std::memory_order_acquire))
    {
      leaveLearnQueue();
      return;
    }

    int accepted = 0;
    try
    {
      LearnHitWindow window;
      while (learnHitQueue.pop (window))
      {
        drainedAny = true;
        if (window.sequence <= lastSequence)
          continue;
        lastSequence = window.sequence;
        windows.push_back (std::move (window));
      }
      accepted = learnHitQueue.getAcceptedHitCount() - context.acceptedHitsBaseline;
      droppedHits = std::max (0, learnHitQueue.getDroppedHitCount() - context.droppedHitsBaseline);
      ignoredOverlaps = std::max (0, learnHitQueue.getIgnoredOverlapCount()
                                     - context.ignoredOverlapsBaseline);
      pendingHits = learnHitQueue.getPendingHitCount();
      captureInProgress = learnHitQueue.hasInProgressCapture();
    }
    catch (...)
    {
      leaveLearnQueue();
      throw;
    }
    leaveLearnQueue();
    const bool audioQueueInUse = learnQueueUsers.load (std::memory_order_acquire) != 0;

    if (drainedAny)
    {
      const std::lock_guard<std::mutex> lock (learnProgressMutex);
      if (cancelled())
        return;
      learnProgress.state = learnState.load (std::memory_order_acquire);
      learnProgress.capturedHits = std::max (0, accepted);
      learnProgress.drainedHits = (int) windows.size();
      learnProgress.pendingQueueHits = pendingHits;
      learnProgress.droppedQueueHits = droppedHits;
      learnProgress.ignoredOverlappingTriggers = ignoredOverlaps;
    }

    const auto state = learnState.load (std::memory_order_acquire);
    const bool stopping = state == LearnState::Stopping;
    if (stopping && ! audioQueueInUse && ! captureInProgress && pendingHits == 0)
    {
      auto expected = LearnState::Stopping;
      if (! learnState.compare_exchange_strong (expected, LearnState::Draining,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire))
        return;
      if (! pauseLearnWorkerForTesting (LearnState::Draining, sessionId))
        return;
      break;
    }

    if (! drainedAny)
      if (learnWorker != nullptr)
        learnWorker->waitForWork (2);
  }

  if (cancelled())
    return;

  auto expectedState = LearnState::Draining;
  if (! learnState.compare_exchange_strong (expectedState, LearnState::Finalizing,
                                             std::memory_order_acq_rel,
                                             std::memory_order_acquire))
    return;
  if (! pauseLearnWorkerForTesting (LearnState::Finalizing, sessionId))
    return;
  {
    const std::lock_guard<std::mutex> lock (learnProgressMutex);
    if (cancelled())
      return;
    learnProgress.state = LearnState::Finalizing;
    learnProgress.pendingQueueHits = 0;
  }

  LearnPipelineConfig config;
  config.sampleRate = context.sampleRate;
  config.delayInterpolation = context.delayInterpolation;
  config.crossoverEnabled = context.crossoverEnabled;
  config.crossoverHz = context.crossoverHz;
  config.preLearnAllpassFreqHz = context.preLearnAllpassFreqHz;
  config.preLearnAllpassQ = context.preLearnAllpassQ;
  config.preLearnAllpassEnabled = context.preLearnAllpassEnabled;
  config.preLearnAllpassStages = context.preLearnStages;

  LearnDiagnostics diagnostics;
  diagnostics.droppedQueueHits = droppedHits;
  diagnostics.ignoredOverlappingTriggers = ignoredOverlaps;

  try
  {
    std::vector<float> loopBass, loopKick;
    const int loopN = learnLoopCapture.snapshot (loopBass, loopKick);
    DynamicStateMap previousDynamicMap;
    {
      const std::lock_guard<std::mutex> lock (mapMutex);
      previousDynamicMap = messageOwnedDynamicStateMap;
    }
    std::array<DynamicMeasurementSummary, DynamicMeasurementContract::kMaxRetainedStates> predictedMeasurements {};
    auto result = LearnPipelineCore::finalizeDynamic (
        windows, config, previousDynamicMap,
        loopN > 0 ? loopBass.data() : nullptr, loopN > 0 ? loopKick.data() : nullptr, loopN,
        diagnostics, cancelled, &predictedMeasurements);
    if (cancelled())
      return;

    const bool candidateValid = result.valid && result.hasDynamicStateMap
        && isStructurallyValidDynamicStateMap (result.dynamicMap);
    const auto resolvedState = candidateValid ? LearnState::ResultReady
                                              : LearnState::NotEnoughMaterial;
    LearnProgressSnapshot completedProgress;
    completedProgress.state = resolvedState;
    completedProgress.capturedHits = result.diagnostics.capturedHits;
    completedProgress.drainedHits = (int) windows.size();
    completedProgress.rejectedPitchHits = result.diagnostics.rejectedPitchHits;
    completedProgress.timingUsableHits = result.diagnostics.analyzedHits;
    completedProgress.unusableSignalHits = result.diagnostics.unusableSignalHits;
    completedProgress.droppedQueueHits = result.diagnostics.droppedQueueHits;
    completedProgress.ignoredOverlappingTriggers = result.diagnostics.ignoredOverlappingTriggers;
    for (const auto& hit : result.hitAnalyses)
    {
      if (! hit.pitchAccepted)
        continue;
      ++completedProgress.pitchAcceptedHits;
      const int index = NotePhaseMapSnapshot::indexForMidi (
          NoteQuantizer::hzToMidi (hit.trackedFundamentalHz));
      if (index >= 0)
        ++completedProgress.trackedNoteHitCounts[(size_t) index];
    }
    completedProgress.noteReports = result.noteReports;

    {
      const std::lock_guard<std::mutex> lock (learnMutex);
      if (shuttingDown.load (std::memory_order_acquire)
          || activeLearnSessionId.load (std::memory_order_acquire) != sessionId)
        return;
      pendingLearnCandidate = {};
      pendingLearnCandidate.sessionId = sessionId;
      pendingLearnCandidate.context = context;
      pendingLearnCandidate.result = std::move (result);
      pendingLearnCandidate.present = candidateValid;
      // Phase 9: the predicted-measurement sidecar is kept coherent with the
      // pending candidate by tracking the same sessionId; Apply only installs
      // it once the map it describes has actually been published.
      pendingDynamicMeasurementSessionId = sessionId;
      pendingDynamicPredictedMeasurements = predictedMeasurements;
    }
    auto expected = LearnState::Finalizing;
    if (! learnState.compare_exchange_strong (expected, resolvedState,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire))
      return;
    if (cancelled() || learnState.load (std::memory_order_acquire) != resolvedState)
      return;
    const std::lock_guard<std::mutex> lock (learnProgressMutex);
    if (cancelled() || learnState.load (std::memory_order_acquire) != resolvedState)
      return;
    learnProgress.state = resolvedState;
    learnProgress.capturedHits = completedProgress.capturedHits;
    learnProgress.drainedHits = completedProgress.drainedHits;
    learnProgress.pitchAcceptedHits = completedProgress.pitchAcceptedHits;
    learnProgress.rejectedPitchHits = completedProgress.rejectedPitchHits;
    learnProgress.timingUsableHits = completedProgress.timingUsableHits;
    learnProgress.unusableSignalHits = completedProgress.unusableSignalHits;
    learnProgress.droppedQueueHits = completedProgress.droppedQueueHits;
    learnProgress.ignoredOverlappingTriggers = completedProgress.ignoredOverlappingTriggers;
    learnProgress.trackedNoteHitCounts = completedProgress.trackedNoteHitCounts;
    learnProgress.noteReports = completedProgress.noteReports;
  }
  catch (...)
  {
    if (cancelled())
      return;
    {
      const std::lock_guard<std::mutex> lock (learnMutex);
      if (cancelled())
        return;
      pendingLearnCandidate = {};
      pendingLearnCandidate.sessionId = sessionId;
      pendingLearnCandidate.context = context;
      pendingLearnCandidate.result.message = "Learn failed. Try again with more material.";
    }
    auto expected = LearnState::Finalizing;
    if (! learnState.compare_exchange_strong (expected, LearnState::Failed,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire))
      return;
    const std::lock_guard<std::mutex> lock (learnProgressMutex);
    if (cancelled() || learnState.load (std::memory_order_acquire) != LearnState::Failed)
      return;
    learnProgress.state = LearnState::Failed;
  }
  }
  catch (...)
  {
    if (cancelled())
      return;
    clearPendingLearnCandidate();
    auto expected = learnState.load (std::memory_order_acquire);
    if (! learnStateIsBusy (expected)
        || ! learnState.compare_exchange_strong (expected, LearnState::Failed,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire))
      return;
    const std::lock_guard<std::mutex> lock (learnProgressMutex);
    learnProgress.state = LearnState::Failed;
  }
}

LearnState KickLockAudioProcessor::getLearnState() const noexcept
{
  return learnState.load (std::memory_order_acquire);
}

LearnProgressSnapshot KickLockAudioProcessor::getLearnProgress() const
{
  const std::lock_guard<std::mutex> lock (learnProgressMutex);
  auto copy = learnProgress;
  copy.state = learnState.load (std::memory_order_acquire);
  return copy;
}

LearnFinalizeResult KickLockAudioProcessor::getPendingLearnResult() const
{
  const std::lock_guard<std::mutex> lock (learnMutex);
  return pendingLearnCandidate.result;
}

bool KickLockAudioProcessor::hasPendingLearnResult() const noexcept
{
  const std::lock_guard<std::mutex> lock (learnMutex);
  return pendingLearnCandidate.present;
}

juce::String KickLockAudioProcessor::getLearnApplyBlockedReason() const
{
  PendingLearnCandidate candidate;
  const std::lock_guard<std::mutex> lock (learnMutex);
  candidate = pendingLearnCandidate;
  if (! candidate.present)
    return candidate.applyBlockedReason;
  return getLearnApplyBlockedReason (candidate);
}

KickLockAudioProcessor::PendingDynamicLearnPreviewForUi
KickLockAudioProcessor::getPendingDynamicLearnPreviewForUi() const
{
  PendingDynamicLearnPreviewForUi preview;
  PendingLearnCandidate candidate;
  uint64_t predictedSessionId = 0;
  {
    const std::lock_guard<std::mutex> lock (learnMutex);
    candidate = pendingLearnCandidate;
    predictedSessionId = pendingDynamicMeasurementSessionId;
    preview.predicted = pendingDynamicPredictedMeasurements;
  }

  preview.sessionId = candidate.sessionId;
  preview.valid = learnState.load (std::memory_order_acquire) == LearnState::ResultReady
      && candidate.present
      && candidate.result.hasDynamicStateMap
      && isStructurallyValidDynamicStateMap (candidate.result.dynamicMap);
  if (! preview.valid)
  {
    preview.predicted = {};
    return preview;
  }

  preview.map = candidate.result.dynamicMap;
  if (predictedSessionId != candidate.sessionId)
    preview.predicted = {};
  preview.applyBlockedReason = getLearnApplyBlockedReason (candidate);
  preview.applyBlocked = preview.applyBlockedReason.isNotEmpty();
  preview.applyAvailable = true;
  return preview;
}

bool KickLockAudioProcessor::canApplyLatestLearnResult() const
{
  PendingLearnCandidate candidate;
  {
    const std::lock_guard<std::mutex> lock (learnMutex);
    candidate = pendingLearnCandidate;
  }
  return candidate.present && getLearnApplyBlockedReason (candidate).isEmpty();
}

juce::String KickLockAudioProcessor::getLearnApplyBlockedReason (const PendingLearnCandidate& candidate) const
{
  if (learnState.load (std::memory_order_acquire) != LearnState::ResultReady)
    return "Learn has no ready result.";
  if (! candidate.present || ! candidate.result.valid
      || (! (candidate.result.hasDynamicStateMap
              && isStructurallyValidDynamicStateMap (candidate.result.dynamicMap))
          && ! NoteMap::isValidNoteMap (candidate.result.map)))
    return "The pending Learn result is invalid.";
  if (candidate.sessionId != activeLearnSessionId.load (std::memory_order_acquire))
    return "The pending Learn result is stale.";
  if (analyzeStateIsBusy (analyzeState.load (std::memory_order_acquire)))
    return "Analyze is still running.";

  const double currentRate = getSampleRate();
  if (candidate.result.hasDynamicStateMap)
  {
    if (std::abs (currentRate - candidate.context.sampleRate) > 0.01)
      return "Sample rate changed; start Learn again.";
    return {};
  }
  const bool currentCrossover = crossoverEnableParamRaw == nullptr
      || crossoverEnableParamRaw->load() > 0.5f;
  const float currentCrossoverHz = crossoverFreqParam != nullptr ? crossoverFreqParam->load() : 150.0f;
  const auto currentInterpolation = interpolationFromChoice (
      delayInterpParam != nullptr ? delayInterpParam->load() : 0.0f);
  if (std::abs (currentRate - candidate.context.sampleRate) > 0.01)
    return "Sample rate changed; start Learn again.";
  if (currentCrossover != candidate.context.crossoverEnabled)
    return "Crossover enable changed; start Learn again.";
  if (std::abs (currentCrossoverHz - candidate.context.crossoverHz) > 1.0001f)
    return "Crossover frequency changed beyond the 1 Hz tolerance.";
  if (currentInterpolation != candidate.context.delayInterpolation)
    return "Delay interpolation changed; start Learn again.";

  return {};
}

void KickLockAudioProcessor::setPendingLearnResultForTesting (const LearnFinalizeResult& result,
                                                              const LearnSessionContext& suppliedContext)
{
  LearnSessionContext context = suppliedContext;
  if (context.sessionId == 0)
  {
    context.sessionId = learnSessionCounter.fetch_add (1, std::memory_order_acq_rel) + 1;
    activeLearnSessionId.store (context.sessionId, std::memory_order_release);
  }
  {
    const std::lock_guard<std::mutex> lock (learnMutex);
    pendingLearnCandidate.present = result.valid
        && ((result.hasDynamicStateMap && isStructurallyValidDynamicStateMap (result.dynamicMap))
            || NoteMap::isValidNoteMap (result.map));
    pendingLearnCandidate.sessionId = context.sessionId;
    pendingLearnCandidate.context = context;
    pendingLearnCandidate.result = result;
    pendingLearnCandidate.applyBlockedReason.clear();
  }
  learnState.store (LearnState::ResultReady, std::memory_order_release);
  const std::lock_guard<std::mutex> progressLock (learnProgressMutex);
  learnProgress.state = LearnState::ResultReady;
}

void KickLockAudioProcessor::setResolvedLearnStateForTesting (LearnState state)
{
  jassert (state == LearnState::ResultReady || state == LearnState::NotEnoughMaterial
           || state == LearnState::Failed);
  {
    const std::lock_guard<std::mutex> lock (learnMutex);
    pendingLearnCandidate.applyBlockedReason = "stale";
  }
  learnState.store (state, std::memory_order_release);
  const std::lock_guard<std::mutex> lock (learnProgressMutex);
  learnProgress.state = state;
  learnProgress.stopRequested = true;
  learnProgress.capturedHits = 9;
}

bool KickLockAudioProcessor::applyLatestLearnResult()
{
  const std::lock_guard<std::mutex> controlLock (learnControlMutex);
  PendingLearnCandidate candidate;
  {
    const std::lock_guard<std::mutex> lock (learnMutex);
    candidate = pendingLearnCandidate;
  }

  auto reject = [this] (const juce::String& reason)
  {
    const std::lock_guard<std::mutex> lock (learnMutex);
    pendingLearnCandidate.applyBlockedReason = reason;
    return false;
  };

  if (const auto blockedReason = getLearnApplyBlockedReason (candidate); blockedReason.isNotEmpty())
    return reject (blockedReason);

  if (candidate.result.hasDynamicStateMap)
  {
    const DynamicStateMap appliedMap = candidate.result.dynamicMap;
    if (! isStructurallyValidDynamicStateMap (appliedMap))
      return reject ("The pending Dynamic Learn map is invalid.");

    std::array<DynamicMeasurementSummary, DynamicMeasurementContract::kMaxRetainedStates> appliedPredicted {};
    {
      const std::lock_guard<std::mutex> lock (learnMutex);
      if (pendingDynamicMeasurementSessionId == candidate.sessionId)
        appliedPredicted = pendingDynamicPredictedMeasurements;
    }

    ensureRevertBundleCaptured();
    // Do not alter APVTS correction fields for New Dynamic. The map owns the
    // complete Global Base; only the message thread selects Dynamic mode.
    // Queue first while the message-owned value is still recoverable, so a full
    // SPSC queue leaves the pending result intact for an explicit retry.
    {
      const std::lock_guard<std::mutex> publicationLock (mapPublicationMutex);
      const std::lock_guard<std::mutex> lock (mapMutex);
      const auto previous = messageOwnedDynamicStateMap;
      const auto previousPredicted = messageOwnedDynamicPredictedMeasurements;
      hasPendingDynamicMapPublication = false;
      messageOwnedDynamicStateMap = appliedMap;
      if (! dynamicMapUpdateQueue.push (appliedMap))
      {
        messageOwnedDynamicStateMap = previous;
        return reject ("Dynamic map publication is temporarily busy; try Apply again.");
      }
      // Section 11: install the predicted-measurement bundle only after the
      // map itself published successfully. If this second, small, generously
      // sized queue is somehow still full, the map remains correctly applied
      // and the sidecar simply stays Unavailable until the next Apply - it
      // never shows a stale or mismatched measurement.
      messageOwnedDynamicPredictedMeasurements = appliedPredicted;
      if (! dynamicPredictedMeasurementQueue.push (appliedPredicted))
        messageOwnedDynamicPredictedMeasurements = previousPredicted;
    }

    setParameterValueWithGesture ("correction_mode", 1.0f);
    resetResolvedLearnStateToIdle();
    return true;
  }

  const auto globalFix = candidate.result.globalFix;
  const float delay = juce::jlimit (-20.0f, 20.0f, globalFix.bassDelayMs);
  const bool polarity = globalFix.bassPolarityInvert;
  const int stages = juce::jlimit (2, 4, candidate.result.map.base.allpassStages);
  const float frequency = juce::jlimit (20.0f, 500.0f, candidate.result.map.global.allpassFreqHz);
  const float q = juce::jlimit (0.1f, 10.0f, candidate.result.map.global.allpassQ);
  const bool rotatorRequired = candidate.result.map.global.rotatorHelps
      || std::any_of (candidate.result.map.notes.begin(), candidate.result.map.notes.end(),
                      [] (const NoteEntry& entry)
                      {
                        return NoteMap::isValidNoteEntry (entry) && entry.rotatorHelps
                            && entry.confidence >= NoteMap::kMinRuntimeConfidence;
                      });

  NotePhaseMapSnapshot appliedMap = candidate.result.map;
  appliedMap.valid = true;
  appliedMap.schemaVersion = NoteMap::kSchemaVersion;
  appliedMap.base.delayMs = delay;
  appliedMap.base.polarityInvert = polarity;
  appliedMap.base.crossoverEnabled = candidate.context.crossoverEnabled;
  appliedMap.base.crossoverHz = juce::jlimit (20.0f, 500.0f, candidate.context.crossoverHz);
  appliedMap.base.allpassStages = stages;
  appliedMap.base.delayInterpolationIndex = candidate.context.delayInterpolation == InterpolationType::Linear ? 0 : 1;
  appliedMap.base.learnedSampleRate = candidate.context.sampleRate;
  appliedMap.global.allpassFreqHz = frequency;
  appliedMap.global.allpassQ = q;
  if (! NoteMap::isValidNoteMap (appliedMap))
    return reject ("The final applied map failed validation.");

  ensureRevertBundleCaptured();
  const bool preserveAllpassEnabled = getEffectivePhaseFilterEnabled();
  setParameterValueWithGesture ("delay_ms", delay);
  setParameterValueWithGesture ("delayMs", delay);
  setParameterValueWithGesture ("polarity_invert", polarity ? 1.0f : 0.0f);
  setParameterValueWithGesture ("polarityInvert", polarity ? 1.0f : 0.0f);
  setParameterValueWithGesture ("crossover_enable", candidate.context.crossoverEnabled ? 1.0f : 0.0f);
  setParameterValueWithGesture ("crossover_freq", candidate.context.crossoverHz);
  setParameterValueWithGesture ("delayInterp", (float) appliedMap.base.delayInterpolationIndex);
  setParameterValueWithGesture ("rotatorStages", (float) (stages - 2));
  setParameterValueWithGesture ("allpass_freq", frequency);
  setParameterValueWithGesture ("rotatorFreq", frequency);
  setParameterValueWithGesture ("rotatorQ", q);
  setParameterValueWithGesture ("allpass_enable", rotatorRequired || preserveAllpassEnabled ? 1.0f : 0.0f);
  setParameterValueWithGesture ("phaseFilterEnabled", rotatorRequired || preserveAllpassEnabled ? 1.0f : 0.0f);

  const auto actual = captureCurrentParameterSnapshot();
  appliedMap.base.delayMs = actual.delayMs;
  appliedMap.base.polarityInvert = actual.polarityInvert;
  appliedMap.base.crossoverEnabled = actual.crossoverEnabled;
  appliedMap.base.crossoverHz = actual.crossoverFreqHz;
  appliedMap.base.allpassStages = 2 + actual.phaseFilterStageIndex;
  appliedMap.base.delayInterpolationIndex = actual.delayInterpolationIndex;
  appliedMap.global.allpassFreqHz = actual.phaseFilterFreqHz;
  appliedMap.global.allpassQ = actual.phaseFilterQ;
  if (! NoteMap::isValidNoteMap (appliedMap))
    return reject ("The values written by Apply did not form a valid map.");

  {
    const std::lock_guard<std::mutex> lock (mapMutex);
    messageOwnedNoteMap = appliedMap;
  }
  requestMapPublication (appliedMap);
  resetResolvedLearnStateToIdle();
  return true;
}

bool KickLockAudioProcessor::discardLatestLearnResult()
{
  const std::lock_guard<std::mutex> controlLock (learnControlMutex);
  const auto state = learnState.load (std::memory_order_acquire);
  if (state != LearnState::ResultReady && state != LearnState::NotEnoughMaterial
      && state != LearnState::Failed)
    return false;
  resetResolvedLearnStateToIdle();
  return true;
}

bool KickLockAudioProcessor::clearNoteMap()
{
  const std::lock_guard<std::mutex> controlLock (learnControlMutex);
  if (learnStateIsActivelyMutating())
    return false;

  const auto empty = NoteMap::makeEmptyNoteMap();
  if (hasLearnedDynamicData())
    ensureRevertBundleCaptured();
  {
    const std::lock_guard<std::mutex> lock (mapMutex);
    messageOwnedNoteMap = empty;
    messageOwnedDynamicStateMap = makeEmptyDynamicStateMap();
    messageOwnedDynamicPredictedMeasurements = {};
  }
  resetResolvedLearnStateToIdle();
  requestMapPublication (empty);
  requestDynamicMapPublication (makeEmptyDynamicStateMap());
  requestDynamicPredictedMeasurementPublication ({});
  return true;
}

bool KickLockAudioProcessor::clearLearnedDynamicData()
{
  // clearNoteMap() already atomically clears the New map, its measurement
  // sidecar, and legacy compatibility data through the established queues.
  return clearNoteMap();
}

KickLockAudioProcessor::DynamicStateEditOutcome KickLockAudioProcessor::applyDynamicStateEdit (
    const DynamicStateEditRequest& request)
{
  DynamicStateEditOutcome outcome;
  outcome.stableStateId = request.stableStateId;

  const auto source = activeDynamicMapSource.load (std::memory_order_acquire);
  if (source == DynamicMapSource::LegacyDynamicCompatibility)
  {
    outcome.reason = DynamicStateEditRejectionReason::LegacyMapReadOnly;
    return outcome;
  }
  if (source == DynamicMapSource::None)
  {
    outcome.reason = DynamicStateEditRejectionReason::NoMapApplied;
    return outcome;
  }

  DynamicStateMap current;
  {
    const std::lock_guard<std::mutex> lock (mapMutex);
    current = messageOwnedDynamicStateMap;
  }

  DynamicStateEditResult result;
  switch (request.kind)
  {
    case DynamicStateEditKind::SetManualTrim:
      result = setManualTrim (current, request.stableStateId, request.trim);
      break;
    case DynamicStateEditKind::ResetManualTrim:
      result = resetManualTrim (current, request.stableStateId);
      break;
    case DynamicStateEditKind::ResetToLearned:
      result = resetToLearned (current, request.stableStateId);
      break;
    case DynamicStateEditKind::ResetToGlobal:
      result = resetToGlobal (current, request.stableStateId);
      break;
    case DynamicStateEditKind::SetEnabled:
      result = setEnabled (current, request.stableStateId, request.boolValue);
      break;
    case DynamicStateEditKind::SetBypassed:
      result = setBypassed (current, request.stableStateId, request.boolValue);
      break;
    case DynamicStateEditKind::PromoteToManual:
      result = promoteToManual (current, request.stableStateId);
      break;
    case DynamicStateEditKind::RemoveManualState:
      result = removeManualState (current, request.stableStateId);
      break;
    case DynamicStateEditKind::CreateManualState:
      result = createManualState (current, request.creation);
      break;
  }

  if (! result.success)
  {
    outcome.reason = result.reason;
    return outcome;
  }

  if (request.kind == DynamicStateEditKind::CreateManualState)
  {
    // The new id is whatever nextStateId was before the transaction advanced
    // it (createManualState() always allocates exactly one id and increments
    // by exactly one; current.nextStateId is still the pre-mutation value
    // here because `current` was never reassigned).
    outcome.stableStateId = current.nextStateId;
  }

  ensureRevertBundleCaptured();

  // Same atomic accept/reject publish sequence as applyLatestLearnResult():
  // stage under both locks, try the RT-safe push, and roll back the
  // message-owned map on failure so a busy queue never leaves a half-applied
  // edit in place.
  {
    const std::lock_guard<std::mutex> publicationLock (mapPublicationMutex);
    const std::lock_guard<std::mutex> lock (mapMutex);
    const auto previous = messageOwnedDynamicStateMap;
    hasPendingDynamicMapPublication = false;
    messageOwnedDynamicStateMap = result.map;
    if (! dynamicMapUpdateQueue.push (result.map))
    {
      messageOwnedDynamicStateMap = previous;
      outcome.reason = DynamicStateEditRejectionReason::PublicationBusy;
      return outcome;
    }
  }

  outcome.success = true;
  outcome.reason = DynamicStateEditRejectionReason::None;
  return outcome;
}

DynamicState KickLockAudioProcessor::getDynamicStateForUi (uint64_t stableStateId) const
{
  if (activeDynamicMapSource.load (std::memory_order_acquire) != DynamicMapSource::NewDynamicStateMap)
    return {};

  const std::lock_guard<std::mutex> lock (mapMutex);
  const int slot = DynamicStateEditDetail::findOccupiedSlotByStableId (messageOwnedDynamicStateMap, stableStateId);
  if (slot < 0)
    return {};
  return messageOwnedDynamicStateMap.states[(size_t) slot];
}

bool KickLockAudioProcessor::hasValidNoteMap() const noexcept
{
  const std::lock_guard<std::mutex> lock (mapMutex);
  return NoteMap::isValidNoteMap (messageOwnedNoteMap);
}

bool KickLockAudioProcessor::hasLearnedDynamicData() const noexcept
{
  const std::lock_guard<std::mutex> lock (mapMutex);
  return isStructurallyValidDynamicStateMap (messageOwnedDynamicStateMap)
      || NoteMap::isValidNoteMap (messageOwnedNoteMap);
}

NotePhaseMapSnapshot KickLockAudioProcessor::getNoteMapSnapshot() const
{
  const std::lock_guard<std::mutex> lock (mapMutex);
  return messageOwnedNoteMap;
}

NotePhaseMapSnapshot KickLockAudioProcessor::getMessageOwnedNoteMapForTesting() const
{
  const std::lock_guard<std::mutex> lock (mapMutex);
  return messageOwnedNoteMap;
}

void KickLockAudioProcessor::requestMapPublication (const NotePhaseMapSnapshot& map)
{
  {
    const std::lock_guard<std::mutex> lock (mapPublicationMutex);
    pendingMapPublication = map;
    hasPendingMapPublication = true;
  }
  if (retryMapPublication())
  {
    stopTimer();
    return;
  }
  startTimer (10);
}

void KickLockAudioProcessor::timerCallback()
{
  const std::lock_guard<std::mutex> timerCallbackGuard (mapTimerCallbackMutex);
  const auto pauseControl = mapTimerCallbackPauseControlForTesting;
  {
    std::unique_lock<std::mutex> pauseLock (pauseControl->mutex);
    pauseControl->entered = true;
    pauseControl->condition.notify_all();
    pauseControl->condition.wait (pauseLock, [&pauseControl] { return ! pauseControl->paused; });
  }
  if (mapPublicationRetryObserver != nullptr)
    mapPublicationRetryObserver->fetch_add (1, std::memory_order_relaxed);
  if (retryMapPublication() && retryDynamicMapPublication() && retryDynamicPredictedMeasurementPublication())
    stopTimer();
}

bool KickLockAudioProcessor::retryMapPublication()
{
  std::lock_guard<std::mutex> lock (mapPublicationMutex);
  if (! hasPendingMapPublication)
    return true;
  if (! noteMapUpdateQueue.push (pendingMapPublication))
    return false;
  hasPendingMapPublication = false;
  return true;
}

void KickLockAudioProcessor::requestDynamicMapPublication (const DynamicStateMap& map)
{
  {
    const std::lock_guard<std::mutex> lock (mapPublicationMutex);
    pendingDynamicMapPublication = map;
    hasPendingDynamicMapPublication = true;
  }
  if (retryDynamicMapPublication())
  {
    return;
  }
  startTimer (10);
}

bool KickLockAudioProcessor::retryDynamicMapPublication()
{
  std::lock_guard<std::mutex> lock (mapPublicationMutex);
  if (! hasPendingDynamicMapPublication)
    return true;
  if (! dynamicMapUpdateQueue.push (pendingDynamicMapPublication))
    return false;
  hasPendingDynamicMapPublication = false;
  return true;
}

void KickLockAudioProcessor::requestDynamicPredictedMeasurementPublication (
    const std::array<DynamicMeasurementSummary, DynamicMeasurementContract::kMaxRetainedStates>& predicted)
{
  {
    const std::lock_guard<std::mutex> lock (mapPublicationMutex);
    pendingDynamicPredictedMeasurementPublication = predicted;
    hasPendingDynamicPredictedMeasurementPublication = true;
  }
  if (retryDynamicPredictedMeasurementPublication())
    return;
  startTimer (10);
}

bool KickLockAudioProcessor::retryDynamicPredictedMeasurementPublication()
{
  std::lock_guard<std::mutex> lock (mapPublicationMutex);
  if (! hasPendingDynamicPredictedMeasurementPublication)
    return true;
  if (! dynamicPredictedMeasurementQueue.push (pendingDynamicPredictedMeasurementPublication))
    return false;
  hasPendingDynamicPredictedMeasurementPublication = false;
  return true;
}

bool KickLockAudioProcessor::serviceMapPublicationRetryForTesting()
{
  const bool published = retryMapPublication();
  if (published)
    stopTimer();
  return published;
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
