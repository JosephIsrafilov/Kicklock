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
#include "dsp/AnalyzeState.h"
#include "dsp/TransientDetector.h"
#include "dsp/SignalActivityTracker.h"
#include "dsp/PhaseFixEngine.h"
#include "dsp/MultibandPhaseCore.h"
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
    std::atomic<float> transientHealthDb { 0.0f };
    std::atomic<float> transientPrePeak { 0.0f };
    std::atomic<float> transientPostPeak { 0.0f };

    // Live multi-band phase-match read-outs (P5/P8), updated per block from the
    // realtime meter over the currently-monitored (dry or processed) low end.
    std::atomic<float> liveMultiBandMatchPercent { 50.0f };  // low-end-weighted overall
    std::atomic<float> liveLowEndMatchPercent { 50.0f };     // SUB + LOW only
    std::atomic<float> liveBroadbandMatchPercent { 50.0f };  // even 20 Hz-500 Hz blend
    std::array<std::atomic<float>, PhaseBands::numBands> liveBandMatchPercent {};
    std::atomic<float> latestAppliedBeforePercent { -1.0f };

    // P3: one-shot seed flag for the UI-value EMAs so the first real reading is
    // taken verbatim and later readings blend, without snapping when a value
    // passes through 50%.
    std::atomic<bool> uiSmoothingInitialized { false };

    std::atomic<float> latestAnalyzedBeforePercent { 50.0f };
    std::atomic<float> latestAnalyzedAfterPercent { 50.0f };
    std::atomic<float> latestVerifiedAfterPercent { -1.0f };
    std::atomic<float> latestVerificationDeltaPercent { 0.0f };
    std::atomic<float> latestFixConfidence { 0.0f };
    ScopeFifo scopeFifo;

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
    bool revertLatestFix();
    bool hasRevertSnapshot() const noexcept { return revertSnapshotValid.load (std::memory_order_acquire); }
    void selectCompareSlot (int slotIndex);
    void copyActiveCompareSlotToOther();
    int getActiveCompareSlot() const noexcept { return activeCompareSlot.load (std::memory_order_acquire); }
    PhaseFixResult getLatestFixResult() const;
    int getScopeDecimationFactor() const noexcept;
    bool hasSidechainReference() const noexcept;
    bool isTempoAvailable() const noexcept;
    float getLatestBpm() const noexcept;
    float getBassSignalRms() const noexcept;
    float getKickSignalRms() const noexcept;
    const HitCaptureBuffer& getTriggeredHitCapture() const noexcept { return hitCapture; }
    float getTransientPrePeak() const noexcept { return transientPrePeak.load(); }
    float getTransientPostPeak() const noexcept { return transientPostPeak.load(); }
    float getTransientHealthDb() const noexcept { return transientHealthDb.load(); }

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

    struct ParameterSnapshot
    {
        float delayMs = 0.0f;
        bool polarityInvert = false;
        bool phaseFilterEnabled = false;
        float phaseFilterFreqHz = 50.0f;
        float phaseFilterQ = 0.7f;
        int phaseFilterStageIndex = 0;
    };

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
    std::atomic<float>* crossoverFreqParam = nullptr;
    std::atomic<float>* dynEqFreqParam = nullptr;
    std::atomic<float>* dynEqQParam = nullptr;
    std::atomic<float>* dynEqBoostDbParam = nullptr;
    std::atomic<float>* dynEqAmountParam = nullptr;
    std::atomic<float>* dynEqAttackMsParam = nullptr;
    std::atomic<float>* dynEqHoldMsParam = nullptr;
    std::atomic<float>* dynEqReleaseMsParam = nullptr;
    std::atomic<float>* dynEqTriggerRatioParam = nullptr;

    MultibandPhaseCore multibandCore;
    std::unique_ptr<AutoAlignEngine> autoAlignEngine;

    // Live phase-match meters (P5). Multi-band across 20 Hz-500 Hz, low-end
    // weighted, so the kick click can't swing the reading. dry reads the raw
    // pre-processing relationship; processed reads the post-correction one. Both
    // filter internally, so no separate pre-filter members are needed.
    RealtimeMultiBandMeter dryMultiBandMeter;
    RealtimeMultiBandMeter processedMultiBandMeter;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> processedMeterSidechainDelay;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> bypassDelay;
    juce::dsp::LinkwitzRileyFilter<float> rawBassLowpass;
    juce::dsp::LinkwitzRileyFilter<float> rawKickLowpass;
    juce::dsp::LinkwitzRileyFilter<float> processedBassLowpass;
    juce::dsp::LinkwitzRileyFilter<float> processedKickLowpass;

    // Rolling capture of raw (pre-processing) mono bass/kick for the Analyze
    // button. Sized in prepareToPlay (~2 s). Written on the audio thread,
    // then copied into a published buffer that the message thread snapshots.
    RawCaptureBuffer rawCapture;
    TransientDetector transientDetector;
    HitCaptureBuffer hitCapture;
    PhaseFixResult latestFixResult;
    std::vector<float> lastAnalyzedBassWindow;
    std::vector<float> lastAnalyzedKickWindow;
    ParameterSnapshot latestRevertSnapshot;
    std::atomic<bool> revertSnapshotValid { false };
    std::array<ParameterSnapshot, 2> compareSlots {};
    bool compareSlotsInitialised = false;
    std::atomic<int> activeCompareSlot { 0 };

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
    ParameterSnapshot captureCurrentParameterSnapshot() const;
    void restoreParameterSnapshot (const ParameterSnapshot&);
    float readParameterValue (const char* id, float fallback) const;
    void setParameterValueWithGesture (const char* id, float value);
    void initialiseCompareSlotsIfNeeded();
    void loadCompareSlotsFromState();
    void writeCompareSlotsToState();
    static ParameterSnapshot makeFactoryPresetSnapshot (int index);

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
    int currentProgramIndex = 3;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KickLockAudioProcessor)
};
