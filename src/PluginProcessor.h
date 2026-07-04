#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include "util/ScopeFifo.h"
#include "util/RawCaptureBuffer.h"
#include "util/HitCaptureBuffer.h"
#include "dsp/CorrelationMeter.h"
#include "dsp/RealtimeMultiBandMeter.h"
#include "dsp/FractionalDelayLine.h"
#include "dsp/AllpassPhaseRotator.h"
#include "dsp/AlignmentAnalyzer.h"
#include "dsp/AnalyzerInstruction.h"
#include "dsp/AnalyzeState.h"
#include "dsp/TransientDetector.h"
#include "dsp/SignalActivityTracker.h"
#include "dsp/PerHitAnalyzer.h"
#include "dsp/PhaseFixEngine.h"
#include "ui/ScopeVisuals.h"
#include "ui/UiFormatters.h"

class KickLockAudioProcessor : public juce::AudioProcessor
{
public:
    KickLockAudioProcessor();
    ~KickLockAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBassProcessingNeutral() const noexcept;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    std::atomic<float> correlationPercent { 0.0f };
    std::atomic<float> realtimeCorrelation { 0.0f };
    std::atomic<float> realtimeLowBandMatchPercent { 0.0f };
    std::atomic<float> dryInputMatchPercent { 0.0f };
    std::atomic<float> processedMatchPercent { 0.0f };

    // Live multi-band phase-match read-outs (P5/P8), updated per block from the
    // realtime meter over the currently-monitored (dry or processed) low end.
    std::atomic<float> liveMultiBandMatchPercent { 50.0f };  // low-end-weighted overall
    std::atomic<float> liveLowEndMatchPercent { 50.0f };     // SUB + LOW only
    std::atomic<float> liveBroadbandMatchPercent { 50.0f };  // even 20 Hz-2 kHz blend

    std::atomic<float> latestAnalyzedBeforePercent { 50.0f };
    std::atomic<float> latestAnalyzedAfterPercent { 50.0f };
    std::atomic<float> latestVerifiedAfterPercent { -1.0f };
    std::atomic<float> latestVerificationDeltaPercent { 0.0f };
    std::atomic<float> latestFixConfidence { 0.0f };
    ScopeFifo scopeFifo;

    // Runs cross-correlation over the captured raw bass/kick. Default behavior
    // is recommend-only; AutoApplySafe may change bass-path parameters, but
    // never delays or otherwise processes the sidechain/kick reference.
    // Message thread only (allocates).
    AnalyzerInstruction analyzeAndApply (AnalyzeMode mode = AnalyzeMode::RecommendOnly);
    HitAnalysisResult analyzeLatestHit();
    int getLatestHitSequence() const noexcept;

    // Synchronous analysis. Snapshots the raw capture, runs the offline
    // PhaseFixEngine search, and publishes the result. Message-thread callers
    // (and the unit tests) can use this directly; it can also be called from a
    // background worker via beginBackgroundAnalyze().
    PhaseFixResult analyzeFix();

    // Non-blocking analysis for the UI. Snapshots on the message thread, then
    // runs the heavy search on a background worker so the click returns
    // immediately and audio never sees the grid search. Poll getAnalyzeState()
    // for the result. Returns false if an analysis is already in flight.
    bool beginBackgroundAnalyze();
    AnalyzeState getAnalyzeState() const noexcept { return analyzeState.load (std::memory_order_acquire); }
    void acknowledgeAnalyzeState() noexcept;

    bool applyLatestFix();
    PhaseFixResult getLatestFixResult() const;
    int getScopeDecimationFactor() const noexcept;
    bool hasSidechainReference() const noexcept;
    bool isTempoAvailable() const noexcept;
    float getLatestBpm() const noexcept;
    float getBassSignalRms() const noexcept;
    float getKickSignalRms() const noexcept;

    // Musically-aware activity flags for P3 status. These hold "active" for a
    // window after the last transient/level crossing, so a normal beat does not
    // flicker to SIGNAL TOO LOW in the gaps between kicks. Read on the UI thread.
    bool isKickActive() const noexcept { return kickActiveHeld.load(); }
    bool isBassActive() const noexcept { return bassActiveHeld.load(); }
    bool isAnalysisSignalUsable() const noexcept { return analysisSignalUsable.load(); }
    bool hasEnoughMaterialForAnalysis() const noexcept { return analysisMaterialReady.load(); }
    void setLatestFixResultForTesting (const PhaseFixResult&);
    void requestAutoAlign();

private:
    class PhaseAlignmentEngine;
    class AutoAlignEngine;

    std::atomic<float>* delayMsParam = nullptr;
    std::atomic<float>* delayMsLegacyParam = nullptr;
    std::atomic<float>* delayInterpParam = nullptr;
    std::atomic<float>* polarityInvertParam = nullptr;
    std::atomic<float>* polarityInvertLegacyParam = nullptr;
    std::atomic<float>* phaseFilterEnabledParam = nullptr;
    std::atomic<float>* phaseFilterEnabledLegacyParam = nullptr;
    std::atomic<float>* rotatorFreqParam = nullptr;
    std::atomic<float>* rotatorFreqLegacyParam = nullptr;
    std::atomic<float>* rotatorQParam = nullptr;
    std::atomic<float>* rotatorStagesParam = nullptr;

    std::unique_ptr<PhaseAlignmentEngine> phaseAlignmentEngine;
    std::unique_ptr<AutoAlignEngine> autoAlignEngine;

    // Live phase-match meters (P5). Multi-band across 20 Hz-2 kHz, low-end
    // weighted, so the kick click can't swing the reading. dry reads the raw
    // pre-processing relationship; processed reads the post-correction one. Both
    // filter internally, so no separate pre-filter members are needed.
    RealtimeMultiBandMeter dryMultiBandMeter;
    RealtimeMultiBandMeter processedMultiBandMeter;

    // Rolling capture of raw (pre-processing) mono bass/kick for the Analyze
    // button. Sized in prepareToPlay (~2 s). Written on the audio thread,
    // then copied into a published buffer that the message thread snapshots.
    RawCaptureBuffer rawCapture;
    TransientDetector transientDetector;
    HitCaptureBuffer hitCapture;
    HitAnalysisHistory hitHistory;
    PhaseFixResult latestFixResult;
    std::vector<float> lastAnalyzedBassWindow;
    std::vector<float> lastAnalyzedKickWindow;

    // Background Analyze. The heavy PhaseFixEngine grid search runs on this
    // single-thread pool so the UI click returns immediately and the audio
    // thread is never touched. resultMutex guards latestFixResult and the
    // analyzed window vectors, which the worker publishes and the message
    // thread reads under lock (never locked on the audio thread).
    juce::ThreadPool analysisThreadPool { 1 };
    std::atomic<AnalyzeState> analyzeState { AnalyzeState::Idle };
    mutable std::mutex resultMutex;

    // Computes a fix from an already-captured bass/kick window. Shared by the
    // synchronous analyzeFix() and the background worker; publishes the result
    // and window under resultMutex.
    PhaseFixResult computeAndPublishFix (const std::vector<float>& bass,
                                         const std::vector<float>& kick,
                                         int numSamples);

    int lastInterpChoice = 0;
    int lastStageChoice = 0;
    float lastRotatorFreq = -1.0f;
    float lastRotatorQ = -1.0f;
    bool lastDelayActive = false;

    // Scope decimation: only push 1 in N samples to the scope fifo so the
    // display shows ~1 second of audio and scrolls slowly enough to read.
    // The correlation meter still receives every sample. Set in prepareToPlay
    // from the sample rate.
    int scopeDecimationFactor = 1;
    int scopeDecimationCounter = 0;
    std::atomic<bool> sidechainReferenceAvailable { false };
    std::atomic<bool> tempoAvailable { false };
    std::atomic<float> latestBpm { 0.0f };
    std::atomic<float> bassSignalRms { 0.0f };
    std::atomic<float> kickSignalRms { 0.0f };
    float correlationProductLpf = 0.0f;
    float correlationMainEnergyLpf = 0.0f;
    float correlationSideEnergyLpf = 0.0f;

    // Musically-aware activity detection. Instant per-block RMS flickers to
    // "no signal" in every gap between kick transients; these trackers HOLD an
    // active verdict for ~500 ms after the last time the level crossed a small
    // threshold, so a steady loop reads continuously active. Updated once per
    // block; the resulting held-activity flags are published as atomics for the
    // editor's status classifier (kick/bass detected recently + enough material
    // captured), which is the primary status source rather than instant RMS.
    SignalActivityTracker kickActivity;
    SignalActivityTracker bassActivity;
    std::atomic<bool> kickActiveHeld { false };
    std::atomic<bool> bassActiveHeld { false };
    std::atomic<bool> analysisSignalUsable { false };
    std::atomic<bool> analysisMaterialReady { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KickLockAudioProcessor)
};
