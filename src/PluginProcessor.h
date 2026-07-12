#pragma once

#include <JuceHeader.h>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "util/ScopeFifo.h"
#include "util/SpectrumFifo.h"
#include "util/RawCaptureBuffer.h"
#include "util/HitCaptureBuffer.h"
#include "util/LearnHitQueue.h"
#include "util/NoteMapUpdateQueue.h"
#include "dsp/CorrelationMeter.h"
#include "dsp/RealtimeMultiBandMeter.h"
#include "dsp/FractionalDelayLine.h"
#include "dsp/AllpassPhaseRotator.h"
#include "dsp/AlignmentAnalyzer.h"
#include "dsp/AnalyzeState.h"
#include "dsp/LearnState.h"
#include "dsp/TransientDetector.h"
#include "dsp/SignalActivityTracker.h"
#include "dsp/PitchTracker.h"
#include "dsp/PhaseFixEngine.h"
#include "dsp/MultibandPhaseCore.h"
#include "dsp/NotePhaseMap.h"
#include "dsp/DynamicRuntimeSelector.h"
#include "dsp/TransientPunchMeter.h"
#include "ui/ScopeVisuals.h"
#include "ui/UiFormatters.h"

class KickLockAudioProcessor : public juce::AudioProcessor,
                               private juce::AudioProcessorValueTreeState::Listener,
                               private juce::Timer
{
public:
    struct CallbackPauseControlForTesting
    {
        void pause();
        void release();
        bool waitUntilEntered (int timeoutMs);

    private:
        std::mutex mutex;
        std::condition_variable condition;
        bool paused = false;
        bool entered = false;

        friend class KickLockAudioProcessor;
    };

    struct LearnWorkerPauseControlForTesting
    {
        void pause (LearnState state, bool ignoreCancellation);
        void release();
        bool waitUntilEntered (int timeoutMs);

    private:
        std::mutex mutex;
        std::condition_variable condition;
        LearnState pausedState = LearnState::Idle;
        bool ignoreCancellation = false;
        bool entered = false;

        friend class KickLockAudioProcessor;
    };

    KickLockAudioProcessor();
    ~KickLockAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBassProcessingNeutral() const noexcept;
    // True whenever a sidechain bus is present, enabled, and has channels —
    // independent of whether processBlock() or processBlockBypassed() is the
    // one calling it, so the "sidechain routed" UI state never depends on
    // host bypass state.
    bool isSidechainBusActive (const juce::AudioBuffer<float>& sidechainBuffer) const noexcept;

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
    // liveMatchValid is false while the meter has nothing meaningful to
    // measure (no sidechain or silence), so the UI can show "no signal"
    // instead of a misleading neutral 50%.
    std::atomic<bool> liveMatchValid { false };
    std::atomic<float> liveMultiBandMatchPercent { 50.0f };  // low-end-weighted overall
    std::atomic<float> liveLowEndMatchPercent { 50.0f };     // SUB + LOW only
    std::atomic<float> liveBroadbandMatchPercent { 50.0f };  // even 20 Hz-500 Hz blend

    // Live sub/low-end loss in dB versus the best achievable alignment (see
    // SubLossMeter.h) — the same SUB+LOW relationship as liveLowEndMatchPercent,
    // reframed as an honest "how many dB you're throwing away" headline instead
    // of an abstract percentage.
    std::atomic<float> liveLowEndSubLossDb { 0.0f };
    std::array<std::atomic<float>, PhaseBands::numBands> liveBandMatchPercent {};

    // P3: one-shot seed flag for the UI-value EMAs so the first real reading is
    // taken verbatim and later readings blend, without snapping when a value
    // passes through 50%.
    std::atomic<bool> uiSmoothingInitialized { false };

    // Detected bass fundamental (Hz, 0 = not tracking), published every block
    // for the UI readout. When the Pitch Follow parameter is on and the phase
    // filter is enabled, the allpass centre frequency follows this value so
    // the phase correction stays on the note as the bassline moves — the
    // dynamic behaviour a static filter setup can't provide.
    std::atomic<float> trackedBassHz { 0.0f };
    std::atomic<bool> dynamicFallbackActive { false };
    std::atomic<bool> dynamicMapStale { false };
    std::atomic<int> activeMidiNote { -1 };

    std::atomic<float> latestAnalyzedBeforePercent { 50.0f };
    std::atomic<float> latestAnalyzedAfterPercent { 50.0f };
    std::atomic<float> latestVerifiedAfterPercent { -1.0f };
    std::atomic<float> latestVerificationDeltaPercent { 0.0f };
    std::atomic<float> latestFixConfidence { 0.0f };
    ScopeFifo scopeFifo;
    SpectrumFifo spectrumFifo;
    juce::dsp::DelayLine<float> spectrumSidechainDelay;
    ScopeFifo rawScopeFifo;

    // UI sets this when the Spectrum view is visible so the audio thread can
    // skip full-rate spectrum FIFO traffic (and the dual FFT) the rest of the time.
    void setSpectrumCaptureEnabled (bool enabled) noexcept
    {
        spectrumCaptureEnabled.store (enabled, std::memory_order_relaxed);
    }

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
    bool beginLearn();
    bool stopLearn();
    void cancelLearn();
    LearnState getLearnState() const noexcept;
    LearnProgressSnapshot getLearnProgress() const;
    LearnFinalizeResult getPendingLearnResult() const;
    bool hasPendingLearnResult() const noexcept;
    bool canApplyLatestLearnResult() const;
    juce::String getLearnApplyBlockedReason() const;
    bool applyLatestLearnResult();
    bool discardLatestLearnResult();
    bool clearNoteMap();
    bool hasValidNoteMap() const noexcept;
    NotePhaseMapSnapshot getNoteMapSnapshot() const;
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
    // Non-const: the editor's oscilloscope drains the capture's progressive
    // sweep stream (a ReVision-style per-hit redraw feed) on the UI thread.
    HitCaptureBuffer& getTriggeredHitCapture() noexcept { return hitCapture; }
    const HitCaptureBuffer& getTriggeredHitCapture() const noexcept { return hitCapture; }

    // Kick-punch transient integrity meter. Reads live from the processed
    // bass/kick low end so it moves the instant Delay/Polarity/Phase Filter
    // change. The reference feature stores a snapshot for before/after A/B.
    float getTransientPunchDb() const noexcept { return transientPunchMeter.getPunchDb(); }
    bool isTransientPunchValid() const noexcept { return transientPunchMeter.isValid(); }
    // Per-hit finalized low-end peaks: kick-alone vs combined (kick+bass). Real,
    // stable inputs for the transient-health bars (updated once per kick, so
    // they don't jitter per block).
    float getTransientKickPeak() const noexcept { return transientPunchMeter.getKickPunch(); }
    float getTransientSumPeak() const noexcept { return transientPunchMeter.getSumPunch(); }
    void setTransientPunchReference() noexcept
    {
        transientPunchReferenceDb.store (transientPunchMeter.getPunchDb(), std::memory_order_release);
        transientPunchReferenceSet.store (true, std::memory_order_release);
    }
    void clearTransientPunchReference() noexcept
    {
        transientPunchReferenceSet.store (false, std::memory_order_release);
    }
    bool isTransientPunchReferenceSet() const noexcept { return transientPunchReferenceSet.load (std::memory_order_acquire); }
    float getTransientPunchReferenceDb() const noexcept { return transientPunchReferenceDb.load (std::memory_order_acquire); }

    // Musically-aware activity flags for P3 status. These hold "active" for a
    // window after the last transient/level crossing, so a normal beat does not
    // flicker to SIGNAL TOO LOW in the gaps between kicks. Read on the UI thread.
    bool isKickActive() const noexcept { return kickActiveHeld.load(); }
    bool isBassActive() const noexcept { return bassActiveHeld.load(); }
    bool isAnalysisSignalUsable() const noexcept { return analysisSignalUsable.load(); }
    bool hasEnoughMaterialForAnalysis() const noexcept { return analysisMaterialReady.load(); }
    void setLatestFixResultForTesting (const PhaseFixResult&);
    void setPendingLearnResultForTesting (const LearnFinalizeResult&, const LearnSessionContext&);
    // Test-only hook: exercises the rollback-bundle capture in isolation from the
    // async Analyze worker (mirrors setLatestFixResultForTesting).
    void ensureRevertBundleCapturedForTesting() { ensureRevertBundleCaptured(); }
    void requestAutoAlign();

    // Phase 2 Learn-transport diagnostics (read by the worker/UI in later
    // phases). All are approximate lifetime counters, safe to read from any
    // thread.
    int getLearnAcceptedHits()    const noexcept { return learnHitQueue.getAcceptedHitCount(); }
    int getLearnIgnoredOverlaps() const noexcept { return learnHitQueue.getIgnoredOverlapCount(); }
    int getLearnDroppedHits()     const noexcept { return learnHitQueue.getDroppedHitCount(); }
    int getPendingMapUpdates()    const noexcept { return noteMapUpdateQueue.getPendingUpdateCount(); }
    int getDroppedMapUpdates()    const noexcept { return noteMapUpdateQueue.getDroppedUpdateCount(); }

    // Test-only hooks for the RT-safe transport (no Learn state machine exists
    // yet). setLearnActiveForTesting flips the capture gate; the map helpers
    // simulate the message-thread publish and read the audio-owned active map.
    void setLearnActiveForTesting (bool active) noexcept
    {
        learnActive.store (active, std::memory_order_release);
    }
    bool publishNoteMapForTesting (const NotePhaseMapSnapshot& snapshot) noexcept
    {
        {
            const std::lock_guard<std::mutex> lock (mapMutex);
            messageOwnedNoteMap = snapshot;
        }
        return noteMapUpdateQueue.push (snapshot);
    }
    const NotePhaseMapSnapshot& getActiveNoteMapForTesting() const noexcept
    {
        return activeNoteMap;
    }
    int getDynamicLastMidiForTesting() const noexcept { return dynamicNoteState.lastMidi; }
    NotePhaseMapSnapshot getMessageOwnedNoteMapForTesting() const;
    uint64_t getLearnSessionIdForTesting() const noexcept
    {
        return activeLearnSessionId.load (std::memory_order_acquire);
    }
    bool serviceMapPublicationRetryForTesting();
    void requestMapPublicationForTesting (const NotePhaseMapSnapshot& map) { requestMapPublication (map); }
    bool isMapPublicationRetryScheduledForTesting() const noexcept { return isTimerRunning(); }
    void setMapPublicationRetryObserverForTesting (std::shared_ptr<std::atomic<int>> observer)
    {
        mapPublicationRetryObserver = std::move (observer);
    }
    void setResolvedLearnStateForTesting (LearnState state);
    std::shared_ptr<CallbackPauseControlForTesting> getMapTimerCallbackPauseControlForTesting() const noexcept
    {
        return mapTimerCallbackPauseControlForTesting;
    }
    std::shared_ptr<LearnWorkerPauseControlForTesting> getLearnWorkerPauseControlForTesting() const noexcept
    {
        return learnWorkerPauseControlForTesting;
    }
    void setLearnWorkerPauseStateForTesting (LearnState state, bool ignoreCancellation = false)
    {
        learnWorkerPauseControlForTesting->pause (state, ignoreCancellation);
    }

private:
    class AutoAlignEngine;

    struct ParameterSnapshot
    {
        float delayMs = 0.0f;
        bool polarityInvert = false;
        bool phaseFilterEnabled = false;
        float phaseFilterFreqHz = 50.0f;
        float phaseFilterQ = 0.7f;
        int phaseFilterStageIndex = 0;
        bool crossoverEnabled = false;
        float crossoverFreqHz = 150.0f;
        int delayInterpolationIndex = 0;
        bool pitchTrack = false;
        int correctionModeIndex = 0;
        float dynamicStrength = 1.0f;
    };

    // Rollback storage for every operation that can change the audible state
    // (Analyze/Apply today; Learn/Apply-Learn later). Captured once by
    // ensureRevertBundleCaptured() before the first such operation and consumed
    // by Revert, so repeated Analyze/Apply cycles never move the rollback point.
    //
    // Validity is intentionally NOT stored in this struct: the atomic
    // revertSnapshotValid (below) is the single cross-thread validity gate.
    // This storage is only ever read or written on the message thread, so it
    // needs no synchronization of its own.
    struct RevertBundle
    {
        ParameterSnapshot parameters;
        NotePhaseMapSnapshot noteMap;
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
    std::atomic<float>* crossoverEnableParamRaw = nullptr;
    std::atomic<float>* pitchTrackParam = nullptr;
    std::atomic<float>* correctionModeParam = nullptr;
    std::atomic<float>* dynamicStrengthParam = nullptr;


    void parameterChanged (const juce::String& parameterID, float newValue) override;
    void timerCallback() override;
    void markRestoredParameterSources (bool hasDelayMs,
                                       bool hasLegacyDelayMs,
                                       bool hasPolarityInvert,
                                       bool hasLegacyPolarityInvert,
                                       bool hasAllpassEnable,
                                       bool hasLegacyPhaseFilterEnabled,
                                       bool hasAllpassFreq,
                                       bool hasLegacyRotatorFreq) noexcept;
    float getEffectiveDelayMs() const noexcept;
    bool getEffectivePolarityInvert() const noexcept;
    bool getEffectivePhaseFilterEnabled() const noexcept;
    float getEffectiveAllpassFreqHz() const noexcept;

    std::atomic<uint32_t> parameterChangeCounter { 1 };
    std::atomic<uint32_t> delayCanonicalChange { 1 };
    std::atomic<uint32_t> delayLegacyChange { 0 };
    std::atomic<uint32_t> polarityCanonicalChange { 1 };
    std::atomic<uint32_t> polarityLegacyChange { 0 };
    std::atomic<uint32_t> phaseCanonicalChange { 1 };
    std::atomic<uint32_t> phaseLegacyChange { 0 };
    std::atomic<uint32_t> allpassFreqCanonicalChange { 1 };
    std::atomic<uint32_t> allpassFreqLegacyChange { 0 };

    MultibandPhaseCore multibandCore;
    TransientPunchMeter transientPunchMeter;
    std::atomic<float> transientPunchReferenceDb { 0.0f };
    std::atomic<bool> transientPunchReferenceSet { false };
    std::unique_ptr<AutoAlignEngine> autoAlignEngine;
    juce::AudioBuffer<float> sidechainMonoScratch;

    // Live phase-match meters (P5). Multi-band across 20 Hz-500 Hz, low-end
    // weighted, so the kick click can't swing the reading. dry reads the raw
    // pre-processing relationship; processed reads the post-correction one. Both
    // filter internally, so no separate pre-filter members are needed.
    RealtimeMultiBandMeter dryMultiBandMeter;
    RealtimeMultiBandMeter processedMultiBandMeter;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> processedMeterSidechainDelay;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> bypassDelay;
    juce::AudioBuffer<float> analysisBuffer;
    juce::dsp::LinkwitzRileyFilter<float> rawBassLowpass;
    juce::dsp::LinkwitzRileyFilter<float> rawKickLowpass;
    juce::dsp::LinkwitzRileyFilter<float> processedBassLowpass;
    juce::dsp::LinkwitzRileyFilter<float> processedKickLowpass;
    juce::dsp::LinkwitzRileyFilter<float> analysisBassCrossoverSim;

    // Rolling capture of raw (pre-processing) mono bass/kick for the Analyze
    // button. Sized in prepareToPlay (~2 s). Written on the audio thread,
    // then copied into a published buffer that the message thread snapshots.
    RawCaptureBuffer rawCapture;
    TransientDetector transientDetector;
    HitCaptureBuffer hitCapture;
    PitchTracker pitchTracker;
    PhaseFixResult latestFixResult;
    std::vector<float> lastAnalyzedBassWindow;
    std::vector<float> lastAnalyzedKickWindow;
    // The exact pair used for the offline recommendation.  Apply verification
    // must use this same hit-concatenated material, not a fresh capture or the
    // full rolling buffer.
    float lastAnalyzedCrossoverHz = 150.0f;
    double lastAnalyzedSampleRate = 0.0;
    InterpolationType lastAnalyzedDelayInterpolation = InterpolationType::Linear;
    bool lastAnalyzedCrossoverEnabled = false;
    // revertBundle storage is message-thread-only: it is never touched from
    // prepareToPlay() or the audio thread. revertSnapshotValid is the sole
    // atomic validity gate, so the UI thread (hasRevertSnapshot) and a
    // re-prepare can flip validity without racing the (message-thread) storage.
    RevertBundle revertBundle;
    std::atomic<bool> revertSnapshotValid { false };
    std::array<ParameterSnapshot, 2> compareSlots {};
    bool compareSlotsInitialised = false;
    std::atomic<int> activeCompareSlot { 0 };

    // Phase 2 Learn transport (RT-safe SPSC infrastructure; inert until a later
    // phase sets learnActive). learnHitQueue: audio-thread producer / worker
    // consumer. learnTransientDetector: dedicated Learn trigger detector, kept
    // separate from the scope/punch detector so their states never interfere.
    // noteMapUpdateQueue: message-thread producer / audio-thread consumer.
    // activeNoteMap is owned by the audio thread; nothing reads it for
    // correction in this phase.
    LearnHitQueue learnHitQueue;
    TransientDetector learnTransientDetector;
    NoteMapUpdateQueue noteMapUpdateQueue;
    NotePhaseMapSnapshot activeNoteMap;
    std::atomic<bool> learnActive { false };
    std::atomic<LearnState> learnState { LearnState::Idle };
    std::atomic<uint64_t> learnSessionCounter { 0 };
    std::atomic<uint64_t> activeLearnSessionId { 0 };
    std::atomic<bool> learnStartRequested { false };
    std::atomic<bool> learnStopRequested { false };
    std::atomic<bool> learnCancelRequested { false };
    std::atomic<bool> learnAudioCaptureAcknowledged { false };
    std::atomic<bool> shuttingDown { false };
    std::thread learnWorker;
    std::mutex learnControlMutex;
    std::mutex learnWorkerCompletionMutex;
    std::condition_variable learnWorkerCompletionCondition;
    bool learnWorkerFinished = true;
    std::shared_ptr<LearnWorkerPauseControlForTesting> learnWorkerPauseControlForTesting =
        std::make_shared<LearnWorkerPauseControlForTesting>();
    mutable std::mutex learnMutex;
    mutable std::mutex learnProgressMutex;
    PendingLearnCandidate pendingLearnCandidate;
    LearnProgressSnapshot learnProgress;
    mutable std::mutex mapMutex;
    NotePhaseMapSnapshot messageOwnedNoteMap = NoteMap::makeEmptyNoteMap();
    std::mutex mapTimerCallbackMutex;
    std::mutex mapPublicationMutex;
    NotePhaseMapSnapshot pendingMapPublication = NoteMap::makeEmptyNoteMap();
    bool hasPendingMapPublication = false;
    std::shared_ptr<std::atomic<int>> mapPublicationRetryObserver;
    std::shared_ptr<CallbackPauseControlForTesting> mapTimerCallbackPauseControlForTesting =
        std::make_shared<CallbackPauseControlForTesting>();
    DynamicNoteState dynamicNoteState;
    int dynamicSilenceResetSamples = 12000;

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

    // Shared observation path (used by both processBlock() and
    // processBlockBypassed()) so diagnostic metering — Analyze capture, the
    // held-activity trackers, the transient detector, HitCaptureBuffer, the
    // Kick Punch meter, and the scope feed — keeps working whenever a
    // sidechain is routed, even while the corrective DSP itself is bypassed or
    // neutral. Neither allocates, locks, or throws; both are audio-thread-safe.
    //
    // Per-block observation statistics accumulated by processObservationCapture
    // for the activity trackers. Both energy (RMS) and PEAK are tracked: block
    // RMS of a short kick tick dilutes with the host buffer size (~10 dB going
    // 512 -> 4096 samples), so a peak term keeps activity detection
    // buffer-size-independent.
    struct BlockObservationStats
    {
        float bassEnergySum = 0.0f;
        float kickEnergySum = 0.0f;
        float bassPeak = 0.0f;
        float kickPeak = 0.0f;
    };

    // Captures raw (pre-processing) mono bass/kick, low-passed, into rawCapture
    // / the auto-align engine / the dry multi-band meter, and accumulates the
    // block's bass/kick energy and peak for the activity trackers.
    void processObservationCapture (const juce::AudioBuffer<float>& mainBuffer,
                                    const juce::AudioBuffer<float>& sidechainBuffer,
                                    bool hasSidechain,
                                    int numSamples,
                                    BlockObservationStats& statsOut) noexcept;

    // Publishes block-level RMS and the musically-held kick/bass activity and
    // analysis-readiness flags from the stats processObservationCapture()
    // accumulated.
    void updateActivityAndSignalState (bool hasSidechain,
                                       const BlockObservationStats& stats,
                                       int numSamples) noexcept;

    // Per-sample: aligns the sidechain to the current latency, runs the
    // transient detector, feeds HitCaptureBuffer and the Kick Punch meter, the
    // processed multi-band meter, the realtime correlation LPFs, and the
    // (decimated) scope fifo. `mainMono` is whatever the main bus currently
    // sounds like (post-correction in processBlock, post-fixed-delay-only in
    // processBlockBypassed); `sidechainMonoRaw` is the raw, not-yet-latency-
    // aligned sidechain mono sample.
    void pushMetersScopeAndTransientState (float mainMono, float sidechainMonoRaw) noexcept;
    ParameterSnapshot captureCurrentParameterSnapshot() const;
    void restoreParameterSnapshot (const ParameterSnapshot&);
    // Captures the rollback bundle exactly once (no-op if already valid), so the
    // first audible-state change stores the pre-change state and later
    // Analyze/Apply operations leave the rollback point untouched.
    void ensureRevertBundleCaptured();
    void serviceLearnAudioCommands() noexcept;
    void drainPendingMapUpdates() noexcept;
    void runLearnWorker (uint64_t sessionId, LearnSessionContext context);
    void requestMapPublication (const NotePhaseMapSnapshot& map);
    bool retryMapPublication();
    bool waitForLearnWorker (int timeoutMs);
    void waitForLearnWorkerUntilFinished();
    void signalLearnWorkerFinished();
    bool pauseLearnWorkerForTesting (LearnState state, uint64_t sessionId);
    void clearPendingLearnCandidate();
    juce::String getLearnApplyBlockedReason (const PendingLearnCandidate&) const;
    void invalidateLearnSession();
    void resetResolvedLearnStateToIdle();
    bool learnStateIsActivelyMutating() const noexcept;
    float readParameterValue (const char* id, float fallback) const;
    void setParameterValueWithGesture (const char* id, float value);
    void initialiseCompareSlotsIfNeeded();
    void loadCompareSlotsFromState();
    void writeCompareSlotsToState();
    static ParameterSnapshot makeFactoryPresetSnapshot (int index);
    RuntimeBaseSettings readCurrentRuntimeBaseSettings (float delayMs,
                                                        bool polarityInvert,
                                                        bool crossoverEnabled,
                                                        float crossoverHz,
                                                        int allpassStages,
                                                        int delayInterpolationIndex) const noexcept;

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
    int rawScopeDecimationCounter = 0;

    std::atomic<bool> spectrumCaptureEnabled { false };

    // Live multi-band meters update every Nth sample (EMA window scaled to match).
    static constexpr int meterDecimationFactor = 2;
    int dryMeterDecimationCounter = 0;
    int processedMeterDecimationCounter = 0;

    // Avoid recomputing Linkwitz-Riley coefficients every block when the
    // crossover frequency hasn't moved.
    float lastPublishedCrossoverHz = -1.0f;

    std::atomic<bool> sidechainReferenceAvailable { false };
    std::atomic<bool> tempoAvailable { false };
    std::atomic<float> latestBpm { 0.0f };
    std::atomic<float> bassSignalRms { 0.0f };
    std::atomic<float> kickSignalRms { 0.0f };

    // Musically-aware activity detection. Instant per-block RMS flickers to
    // "no signal" in every gap between kick transients; these trackers HOLD an
    // active verdict for ~500 ms after the last time the level crossed a small
    // threshold, so a steady loop reads continuously active. Updated once per
    // block; the resulting held-activity flags are published as atomics for the
    // editor's status classifier (kick/bass detected recently + enough material
    // captured), which is the primary status source rather than instant RMS.
    SignalActivityTracker kickActivity;
    SignalActivityTracker bassActivity;

    // Grace window keeping analysisMaterialReady true for a while after the
    // held activity lapses (e.g. transport stopped), so the user can still
    // click Analyze on the audio they just played. Counted down in samples on
    // the audio thread; bounded so hours-old material eventually reads stale.
    int materialReadyHoldSamples = 0;

    std::atomic<bool> kickActiveHeld { false };
    std::atomic<bool> bassActiveHeld { false };
    std::atomic<bool> analysisSignalUsable { false };
    std::atomic<bool> analysisMaterialReady { false };
    int currentProgramIndex = 3;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KickLockAudioProcessor)
};
