#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

#include <JuceHeader.h>

#include "DynamicStateMap.h"
#include "DynamicFingerprintExtractor.h"
#include "DynamicFingerprintTrigger.h"
#include "DynamicFingerprintMatcher.h"
#include "DynamicPackageMorpher.h"
#include "DynamicHotBranchEngine.h"
#include "DynamicSelectorTypes.h"
#include "DynamicSelectorScheduler.h"
#include "DynamicContinuityMixer.h"
#include "TransientDetector.h"
#include "DynamicRuntimeMeasurementCapture.h"
#include "DynamicRecentUnknownEvents.h"

// =============================================================================
// Phase 7: production runtime coordinator.
//
// DynamicProductionRuntime wires the frozen Phase 1-6 components into a single
// allocation-free audio-thread engine: the audio-owned DynamicStateMap snapshot,
// a dedicated kick trigger detector, the Phase-3 fingerprint capture bank and
// matcher, the Phase-4 package resolver, the Phase-5 hot-branch engine, and the
// Phase-6 sample-accurate selector scheduler + continuity mixer, plus the
// semantic Service branch binding.
//
// It owns an internal monotonic sample timeline shared by the capture bank and
// scheduler so block partitioning cannot change results and a looping host
// timeline is never used as scheduler absolute time. It never allocates,
// locks, serializes, or touches message-thread APIs after prepare().
//
// It does NOT resolve the DynamicStateMap-vs-KLNoteMap source priority, own the
// SPSC publication queue, persist state, or drive meters/scope. Those belong to
// PluginProcessor. This runtime only renders the New Dynamic correction once a
// runtime-eligible map has been activated.
// =============================================================================

enum class DynamicProductionTransportReason : uint8_t
{
    Seek = 0,
    LoopWrap,
    StopStart,
    HostReset
};

struct DynamicProductionDiagnostics
{
    // Fixed capture/matcher/event counters (no runtime strings).
    uint64_t acceptedCaptures = 0;
    uint64_t exhaustedCaptureRequests = 0;
    uint64_t rejectedCaptures = 0;
    uint64_t completedObservations = 0;
    uint64_t invalidFingerprints = 0;
    uint64_t eventQueueExhaustion = 0;
    uint64_t serviceBindings = 0;
    uint64_t servicePrimes = 0;
    uint64_t configurationFailures = 0;
    uint64_t sourceTransitions = 0;
    uint64_t recentUnknownStagingDropped = 0;

    // Mirrors of the selection outcome for the processor's public atomics.
    DynamicSelectorDiagnostic lastSelectorDecision = DynamicSelectorDiagnostic::None;
    uint64_t selectedStableStateId = 0;
    DynamicSelectorBranchKind selectedBranchKind = DynamicSelectorBranchKind::Global;
    bool fallbackActive = true;
};

class DynamicProductionRuntime
{
public:
    bool prepare (double newSampleRate, int maxBlockSize, int channels) noexcept
    {
        prepared = false;
        if (! DynamicFingerprintContract::isSupportedSampleRate (newSampleRate)
            || maxBlockSize <= 0
            || channels < 1 || channels > DynamicHotBranchContract::kMaxChannels)
            return false;

        if (! engine.prepare (newSampleRate, maxBlockSize, channels))
            return false;
        if (! scheduler.prepare (newSampleRate))
            return false;
        if (! mixer.prepare (channels))
            return false;

        captureBank.prepare (newSampleRate);
        configureKickTrigger (newSampleRate);
        if (! measurementCapture.prepare (newSampleRate))
            return false;
        measurementHandoffScratch.resizeWindows (measurementCapture.getWindowSamples());

        sampleRate = newSampleRate;
        maxBlock = maxBlockSize;
        numChannels = channels;
        reportedLatencySamples = engine.getReportedLatencySamples();

        chunkInput.setSize (numChannels, maxBlock, false, true, true);
        chunkOutput.setSize (numChannels, maxBlock, false, true, true);

        juce::dsp::ProcessSpec spec {
            sampleRate,
            (juce::uint32) maxBlock,
            (juce::uint32) numChannels
        };
        fallbackDelay.setMaximumDelayInSamples (reportedLatencySamples + 8);
        fallbackDelay.prepare (spec);

        juce::dsp::ProcessSpec monoSpec { sampleRate, (juce::uint32) maxBlock, (juce::uint32) 1 };
        measurementKickAlignDelay.setMaximumDelayInSamples (reportedLatencySamples + 8);
        measurementKickAlignDelay.prepare (monoSpec);

        activeMap = makeEmptyDynamicStateMap();
        mapGeneration = 0;

        stateEditAudition.fill (StateEditAudition {});
        forceHiddenSlot.fill (false);
        lastAttemptedStatePackage.fill (DynamicPackageResolution {});
        hasLastAttemptedStatePackage.fill (false);
        serviceOwnedByEditAuditionForId = 0;

        prepared = true;
        reset();
        return true;
    }

    void reset() noexcept
    {
        if (! prepared)
            return;

        engine.reset();
        mixer.reset();
        fallbackDelay.reset();
        measurementKickAlignDelay.reset();
        resetTimeline();
        invalidateConfigurationCache();
        diagnostics = DynamicProductionDiagnostics {};
    }

    bool isPrepared() const noexcept { return prepared; }
    int getReportedLatencySamples() const noexcept { return reportedLatencySamples; }
    double getSampleRate() const noexcept { return sampleRate; }
    int64_t getRuntimeSamplePosition() const noexcept { return runtimeSamplePos; }

    const DynamicStateMap& getActiveMap() const noexcept { return activeMap; }
    uint64_t getMapGeneration() const noexcept { return mapGeneration; }

    // Activates a new audio-thread-owned map snapshot (from the drained SPSC
    // queue). Light on purpose: the fingerprint capture timeline and scheduler
    // stay continuous (a fingerprint is a map-independent raw-audio descriptor,
    // and the scheduler's own stale-reference check collapses to Global if a
    // currently-selected identity disappeared). The Service binding is cleared
    // because it was configured from one state's coefficient-derived package,
    // and package reconfiguration is forced for the next process() call.
    void activateMap (const DynamicStateMap& map) noexcept
    {
        activeMap = map;
        ++mapGeneration;
        clearServiceBinding();
        // Phase 12: abandon any in-flight edit audition rather than letting it
        // act on a now-superseded package. configurePackagesIfNeeded() will
        // re-detect exactly what (if anything) actually changed against the
        // new map on the very next process() call, by comparing each slot's
        // freshly-resolved package to lastAttemptedStatePackage[] - a
        // content comparison, not a generation counter, so an unrelated
        // State whose resolved package didn't change is correctly left
        // alone even though the map as a whole was just republished.
        cancelAllStateEditAuditions();
        invalidateConfigurationCache();
    }

    // Transport-discontinuity contract. A valid loop wrap preserves runtime
    // continuity (the host callback audio itself is contiguous). Any other
    // discontinuity clears queued events, Hold, the active fade, the Service
    // binding and the pending captures, snaps selection to Global, and clears
    // stale branch history so old delayed audio cannot leak into the new
    // position. PDC is preserved (the engine keeps its configured branches).
    void notifyTransportReset (DynamicProductionTransportReason reason) noexcept
    {
        if (! prepared || reason == DynamicProductionTransportReason::LoopWrap)
            return;
        ++diagnostics.sourceTransitions;
        engine.reset();
        mixer.reset();
        fallbackDelay.reset();
        measurementKickAlignDelay.reset();
        resetTimeline();
    }

    // Sidechain-loss transition (call once when the sidechain disappears while
    // New Dynamic is active). Clears pending captures, Hold and the Service
    // binding, returns selection to Global and publishes no active State, but
    // keeps the persistent hot branches configured and the bass path
    // latency-correct (engine history is preserved).
    void notifySidechainLost() noexcept
    {
        if (! prepared)
            return;
        resetTimeline (DynamicCaptureRejectCategory::SidechainLost);
    }

    // Renders the New Dynamic correction. `bassInput` is the pre-correction main
    // bus; `rawBassMono`/`rawKickMono` are the canonical raw mono-compatible
    // fingerprint pair (length >= numSamples), fed to the Phase-3 extractor
    // without any pre-filtering. `output` receives the continuity-mixed
    // full-band result (common high added exactly once).
    bool process (const juce::AudioBuffer<float>& bassInput,
                  const float* rawBassMono,
                  const float* rawKickMono,
                  bool hasSidechain,
                  double dynamicStrength,
                  juce::AudioBuffer<float>& output,
                  int numSamples) noexcept
    {
        if (! prepared || numSamples <= 0
            || bassInput.getNumChannels() < numChannels
            || bassInput.getNumSamples() < numSamples
            || output.getNumChannels() < numChannels
            || output.getNumSamples() < numSamples)
            return false;

        const double effectiveStrength = std::isfinite (dynamicStrength)
            ? juce::jlimit (0.0, 1.0, dynamicStrength) : 0.0;

        const bool engineUsable = configurePackagesIfNeeded (effectiveStrength);
        if (! engineUsable)
        {
            ++diagnostics.configurationFailures;
            writeDryFallback (bassInput, output, 0, 0, numSamples);
            updateSelectionDiagnostics();
            return true;
        }

        int offset = 0;
        while (offset < numSamples)
        {
            const int chunk = juce::jmin (maxBlock, numSamples - offset);
            if (! processChunk (bassInput, rawBassMono, rawKickMono, hasSidechain,
                                offset, chunk, output))
            {
                // Fail safe for the remaining samples: finite, latency-correct.
                ++diagnostics.configurationFailures;
                writeDryFallback (bassInput, output, offset, offset, numSamples - offset);
                break;
            }
            offset += chunk;
        }

        updateSelectionDiagnostics();
        return true;
    }

    uint64_t getSelectedStableStateId() const noexcept
    {
        return scheduler.getDiagnostics().selectedSemanticStateId;
    }

    // Optional metadata only: the likelyMidi of the currently-selected semantic
    // State, or -1. Never used as State identity.
    int getSelectedLikelyMidi() const noexcept
    {
        const uint64_t id = getSelectedStableStateId();
        if (id == 0)
            return -1;
        const DynamicState* state = findDynamicStateByStableId (activeMap, id);
        if (state != nullptr && state->hasLikelyMidi)
            return state->likelyMidi;
        return -1;
    }

    bool isFallbackActive() const noexcept
    {
        return scheduler.getDiagnostics().selectedBranchKind == DynamicSelectorBranchKind::Global;
    }

    const DynamicProductionDiagnostics& getDiagnostics() const noexcept { return diagnostics; }
    const DynamicSelectorDiagnostics& getSelectorDiagnostics() const noexcept { return scheduler.getDiagnostics(); }
    bool getServiceBindingValid() const noexcept { return serviceBindingValid; }
    uint64_t getServiceBoundStableStateId() const noexcept { return serviceBoundStableStateId; }

    // Pops the oldest completed measurement capture, stamping the branch
    // that is ACTUALLY audible for this identity right now (never the branch
    // guessed at capture start) and vetoing the hand-off entirely when the
    // target correction is not confidently settled at completion time (Hold,
    // mid-fade, stale reference, or a different identity now selected) -
    // Section 14's "no unfinished fade as if it were settled" rule.
    //
    // `out` must already be sized via out.resizeWindows(getMeasurementWindowSamples())
    // (PluginProcessor does this once in prepareToPlay()); this function only
    // ever does element-wise copies into already-sized vectors, so it never
    // allocates on the audio thread.
    bool takeCompletedMeasurementCapture (DynamicRuntimeMeasurementCaptureResult& out) noexcept
    {
        if (! measurementCapture.takeCompleted (measurementHandoffScratch))
            return false;

        if (measurementHandoffScratch.mapGeneration != mapGeneration)
            return false; // stale generation; never hand off

        const auto& selectorDiag = scheduler.getDiagnostics();
        const bool settled = ! selectorDiag.fadeActive
            && selectorDiag.selectedSemanticStateId == measurementHandoffScratch.stableStateId
            && selectorDiag.selectedBranchKind != DynamicSelectorBranchKind::Global;
        if (! settled)
            return false;

        measurementHandoffScratch.branchKind = selectorDiag.selectedBranchKind;
        out.copyFrom (measurementHandoffScratch);
        return true;
    }

    int getMeasurementWindowSamples() const noexcept { return measurementCapture.getWindowSamples(); }
    uint64_t getMeasurementCaptureExhaustedCount() const noexcept { return measurementCapture.getExhaustedRequestCount(); }
    uint64_t getMeasurementCaptureDroppedForTransportCount() const noexcept { return measurementCapture.getDroppedForTransportCount(); }

    // Section 15: pops the oldest staged Unknown/Ambiguous match event, if
    // any. Same-thread hand-off (audio thread produces via processChunk() and
    // consumes via this call, both within the same processBlock) - the
    // caller (PluginProcessor) is responsible for then publishing it across
    // to the message thread through its own SPSC queue.
    bool takeRecentUnknownEvent (DynamicRecentUnknownRawEvent& out) noexcept
    {
        return recentUnknownStaging.pop (out);
    }
    uint64_t getRecentUnknownStagingDroppedCount() const noexcept { return diagnostics.recentUnknownStagingDropped; }

    // ---- test-only accessors (never used by production UI) ----
    DynamicHotBranchEngine& getEngineForTesting() noexcept { return engine; }
    DynamicSelectorScheduler& getSchedulerForTesting() noexcept { return scheduler; }
    DynamicFingerprintCaptureBank& getCaptureBankForTesting() noexcept { return captureBank; }

private:
    void configureKickTrigger (double newSampleRate) noexcept
    {
        configureDynamicFingerprintTrigger (kickTrigger, newSampleRate);
    }

    void resetTimeline (DynamicCaptureRejectCategory measurementDiscardReason = DynamicCaptureRejectCategory::TransportInvalidated) noexcept
    {
        captureBank.reset();
        scheduler.reset (0);
        kickTrigger.reset();
        runtimeSamplePos = 0;
        clearServiceBinding();
        cancelAllStateEditAuditions();
        measurementCapture.discardInFlight (measurementDiscardReason);
    }

    void clearServiceBinding() noexcept
    {
        if (serviceBindingValid || serviceConfigured)
            engine.clearService();
        serviceBindingValid = false;
        serviceBoundStableStateId = 0;
        serviceConfigured = false;
        serviceOwnedByEditAuditionForId = 0;
    }

    void cancelAllStateEditAuditions() noexcept
    {
        for (int slot = 0; slot < DynamicHotBranchContract::kStateSlots; ++slot)
            cancelStateEditAudition (slot);
    }

    void invalidateConfigurationCache() noexcept
    {
        configuredGeneration = 0;
        configuredStrength = std::numeric_limits<double>::quiet_NaN();
        configuredSampleRate = 0.0;
        hasConfigured = false;
    }

    // Resolves and applies the Global + eight persistent-State packages when the
    // map generation, Dynamic Strength or sample rate changed since the last
    // application. Identical inputs skip reconfiguration entirely (the engine's
    // own configsEqual guard additionally protects branch identity and shared
    // delay history against any redundant same-identity update).
    //
    // Section 12 safety: DynamicHotBranchEngine applies a reconfigured
    // package's coefficients and delay tap immediately against whatever filter
    // memory a branch already has (see AllpassStageState::process()/
    // FractionalInterpolator::process() - there is no ramp). Silently calling
    // configureStateSlot() for a slot whose *content* changed while that exact
    // identity is currently audible (DynamicSelectorScheduler::
    // isSemanticStateReferenced()) would therefore click. A brand-new
    // occupant (identity change) or a state that isn't correction-eligible
    // right now is still reconfigured/cleared directly - the engine's own
    // resetRuntime() on identity change, or simply not being selectable,
    // already makes those cases safe exactly as before this change. Only a
    // genuine same-identity content edit while referenced is routed through
    // beginOrRefreshEditAudition() instead of engine.configureFromPackage().
    bool configurePackagesIfNeeded (double strength) noexcept
    {
        if (hasConfigured
            && configuredGeneration == mapGeneration
            && configuredStrength == strength
            && configuredSampleRate == sampleRate)
            return true;

        const auto globalPackage = resolveDynamicPackage (activeMap, 0, strength, sampleRate);
        if (! globalPackage.valid
            || ! engine.configureFromPackage (DynamicHotBranchKind::Global, -1, globalPackage))
            return false;

        for (int slot = 0; slot < DynamicHotBranchContract::kStateSlots; ++slot)
        {
            const DynamicState& state = activeMap.states[(size_t) slot];
            if (! state.occupied)
            {
                engine.clearStateSlot (slot);
                cancelStateEditAudition (slot);
                hasLastAttemptedStatePackage[(size_t) slot] = false;
                continue;
            }

            const auto package = resolveDynamicPackage (activeMap, state.stableStateId, strength, sampleRate);
            if (! package.valid || package.decision != DynamicPackageDecision::StateCorrection)
            {
                // Disabled / Auto Candidate / recognized-no-correction / bypassed
                // / invalid-for-runtime: no persistent correction branch. The
                // matcher still receives the whole map so the state is
                // recognizable and correctly routes to Global.
                engine.clearStateSlot (slot);
                cancelStateEditAudition (slot);
                hasLastAttemptedStatePackage[(size_t) slot] = false;
                continue;
            }

            const bool contentUnchanged = hasLastAttemptedStatePackage[(size_t) slot]
                && dspPackagesEqual (lastAttemptedStatePackage[(size_t) slot], package);
            if (contentUnchanged)
                continue; // engine.configureFromPackage would itself no-op; nothing to do

            if (! scheduler.isSemanticStateReferenced (state.stableStateId))
            {
                // Silent right now (or a brand-new/identity-changed occupant):
                // safe to commit directly, exactly as before this change.
                if (! engine.configureFromPackage (DynamicHotBranchKind::State, slot, package))
                    engine.clearStateSlot (slot);
                lastAttemptedStatePackage[(size_t) slot] = package;
                hasLastAttemptedStatePackage[(size_t) slot] = true;
                cancelStateEditAudition (slot);
                continue;
            }

            // A genuine edit to the package of a State that is currently
            // audible. Route through the Service-mediated safe retune instead
            // of touching the live branch.
            beginOrRefreshEditAudition (slot, state.stableStateId, package);
        }

        configuredGeneration = mapGeneration;
        configuredStrength = strength;
        configuredSampleRate = sampleRate;
        hasConfigured = true;
        return true;
    }

    enum class EditAuditionPhase : uint8_t
    {
        ClaimingService = 0,
        WaitingForSilence,
        Settling
    };

    struct StateEditAudition
    {
        bool active = false;
        uint64_t stableStateId = 0;
        DynamicPackageResolution pendingPackage;
        EditAuditionPhase phase = EditAuditionPhase::ClaimingService;
        int64_t settleSamplesRemaining = 0;
    };

    static bool dspPackagesEqual (const DynamicPackageResolution& a, const DynamicPackageResolution& b) noexcept
    {
        return a.selectedStableStateId == b.selectedStableStateId
            && a.effectiveAbsoluteDelayMs == b.effectiveAbsoluteDelayMs
            && a.polarityInvert == b.polarityInvert
            && a.allpassStages == b.allpassStages
            && a.crossoverEnabled == b.crossoverEnabled
            && a.crossoverHz == b.crossoverHz
            && a.allpassEnabled == b.allpassEnabled
            && a.delayInterpolationIndex == b.delayInterpolationIndex
            && a.allpassCoefficients.b0 == b.allpassCoefficients.b0
            && a.allpassCoefficients.b1 == b.allpassCoefficients.b1
            && a.allpassCoefficients.b2 == b.allpassCoefficients.b2
            && a.allpassCoefficients.a1 == b.allpassCoefficients.a1
            && a.allpassCoefficients.a2 == b.allpassCoefficients.a2;
    }

    void beginOrRefreshEditAudition (int slot, uint64_t stableStateId,
                                     const DynamicPackageResolution& package) noexcept
    {
        auto& audition = stateEditAudition[(size_t) slot];
        audition.active = true;
        audition.stableStateId = stableStateId;
        audition.pendingPackage = package;
        audition.phase = EditAuditionPhase::ClaimingService;
        audition.settleSamplesRemaining = 0;
    }

    void cancelStateEditAudition (int slot) noexcept
    {
        auto& audition = stateEditAudition[(size_t) slot];
        if (! audition.active)
            return;
        if (serviceOwnedByEditAuditionForId == audition.stableStateId)
            serviceOwnedByEditAuditionForId = 0;
        forceHiddenSlot[(size_t) slot] = false;
        audition = StateEditAudition {};
    }

    // Progresses every in-flight edit audition by exactly one chunk. Called
    // unconditionally every process() call (unlike configurePackagesIfNeeded(),
    // which can skip whole chunks when nothing changed) because "has the
    // referenced identity gone silent yet" must be re-checked every chunk.
    void advanceStateEditAuditions (int numSamplesThisChunk) noexcept
    {
        for (int slot = 0; slot < DynamicHotBranchContract::kStateSlots; ++slot)
        {
            auto& audition = stateEditAudition[(size_t) slot];
            if (! audition.active)
                continue;

            switch (audition.phase)
            {
                case EditAuditionPhase::ClaimingService:
                {
                    const bool serviceLiveForAnotherId = serviceBindingValid && serviceBoundStableStateId != 0
                        && serviceBoundStableStateId != audition.stableStateId
                        && scheduler.isSemanticStateReferenced (serviceBoundStableStateId);
                    const bool serviceLiveForThisIdAlready = serviceBindingValid
                        && serviceBoundStableStateId == audition.stableStateId
                        && scheduler.isSemanticStateReferenced (audition.stableStateId);
                    if (serviceLiveForAnotherId || serviceLiveForThisIdAlready)
                        break; // Service is busy and audible; retry next chunk

                    if (! engine.configureFromPackage (DynamicHotBranchKind::Service, -1, audition.pendingPackage))
                        break; // retry next chunk

                    const int primeSamples = (int) std::ceil (sampleRate
                        * DynamicHotBranchContract::kServiceWarmupMs / 1000.0);
                    engine.primeService (primeSamples);
                    ++diagnostics.servicePrimes;
                    serviceConfigured = true;
                    serviceBoundStableStateId = audition.stableStateId;
                    serviceBindingValid = true;
                    ++diagnostics.serviceBindings;
                    serviceOwnedByEditAuditionForId = audition.stableStateId;
                    audition.phase = EditAuditionPhase::WaitingForSilence;
                    break;
                }
                case EditAuditionPhase::WaitingForSilence:
                {
                    if (scheduler.isSemanticStateReferenced (audition.stableStateId))
                        break; // still audible (old package via State, or already via Service); wait

                    if (! engine.configureFromPackage (DynamicHotBranchKind::State, slot, audition.pendingPackage))
                        engine.clearStateSlot (slot);
                    lastAttemptedStatePackage[(size_t) slot] = audition.pendingPackage;
                    hasLastAttemptedStatePackage[(size_t) slot] = true;
                    forceHiddenSlot[(size_t) slot] = true;

                    const double physicalTap = (double) reportedLatencySamples
                        + audition.pendingPackage.effectiveAbsoluteDelayMs * sampleRate / 1000.0;
                    audition.settleSamplesRemaining = DynamicHotBranchDetail::warmFramesRequired (physicalTap);
                    audition.phase = EditAuditionPhase::Settling;
                    break;
                }
                case EditAuditionPhase::Settling:
                {
                    audition.settleSamplesRemaining -= numSamplesThisChunk;
                    if (audition.settleSamplesRemaining <= 0)
                    {
                        // Un-hide; the existing, unmodified updateServiceBinding()
                        // auto-drop-back logic (via the now-corrected
                        // warmPersistentSlotFor()) hands ownership back from
                        // Service to this now-fully-settled persistent slot on
                        // its own, exactly as it already does for ordinary
                        // cold-start Service usage.
                        forceHiddenSlot[(size_t) slot] = false;
                        if (serviceOwnedByEditAuditionForId == audition.stableStateId)
                            serviceOwnedByEditAuditionForId = 0;
                        audition = StateEditAudition {};
                    }
                    break;
                }
            }
        }
    }

    bool processChunk (const juce::AudioBuffer<float>& bassInput,
                       const float* rawBassMono,
                       const float* rawKickMono,
                       bool hasSidechain,
                       int offset,
                       int n,
                       juce::AudioBuffer<float>& output) noexcept
    {
        for (int ch = 0; ch < numChannels; ++ch)
            chunkInput.copyFrom (ch, 0, bassInput, ch, offset, n);

        const auto engineResult = engine.process (chunkInput, n);
        if (! engineResult.valid)
            return false;

        uint64_t pendingConfidentId = 0;
        for (int i = 0; i < n; ++i)
        {
            const float rawBass = rawBassMono != nullptr ? rawBassMono[offset + i] : 0.0f;
            const float rawKick = rawKickMono != nullptr ? rawKickMono[offset + i] : 0.0f;

            if (hasSidechain && rawKickMono != nullptr)
            {
                const bool triggered = kickTrigger.processSample (rawKick);
                if (triggered)
                {
                    const int64_t triggerSample = captureBank.nextSample();
                    const auto request = captureBank.requestCapture (triggerSample);
                    if (request == DynamicCaptureRequest::Accepted)
                        ++diagnostics.acceptedCaptures;
                    else if (request == DynamicCaptureRequest::Exhausted)
                        ++diagnostics.exhaustedCaptureRequests;
                    else
                        ++diagnostics.rejectedCaptures;
                }
            }
            else
            {
                // Keep the trigger detector state coherent, but never request a
                // capture without a sidechain kick reference.
                kickTrigger.processSample (rawKick);
            }

            captureBank.pushSample (rawBass, rawKick);
            measurementCapture.pushRawSample (rawBass, rawKick);

            DynamicFingerprintObservation observation;
            while (captureBank.takeCompleted (observation))
            {
                ++diagnostics.completedObservations;
                if (! observation.fingerprint.isValid())
                {
                    ++diagnostics.invalidFingerprints;
                    continue;
                }

                const DynamicMatchResult match = matchDynamicFingerprint (observation, activeMap);

                if (match.decision == DynamicMatchDecision::Unknown
                    || match.decision == DynamicMatchDecision::Ambiguous)
                {
                    DynamicRecentUnknownRawEvent unknownEvent;
                    unknownEvent.valid = true;
                    unknownEvent.fingerprint = observation.fingerprint.toPrototype();
                    unknownEvent.outcome = match.decision == DynamicMatchDecision::Ambiguous
                        ? DynamicRecentUnknownOutcome::Ambiguous : DynamicRecentUnknownOutcome::Unknown;
                    unknownEvent.nearestDistance = match.nearestDistance;
                    unknownEvent.triggerSample = observation.triggerSample;
                    unknownEvent.mapGeneration = mapGeneration;
                    if (! recentUnknownStaging.push (unknownEvent))
                        ++diagnostics.recentUnknownStagingDropped;
                }

                DynamicSelectorEvent event;
                event.triggerSample = observation.triggerSample;
                event.readySample = observation.readySample;
                event.match = match;
                if (! scheduler.submitEvent (event))
                {
                    if (scheduler.getDiagnostics().lastDecision == DynamicSelectorDiagnostic::QueueExhausted)
                        ++diagnostics.eventQueueExhaustion;
                }

                if (match.decision == DynamicMatchDecision::Matched
                    && match.correctionAvailable && ! match.selectedBypassed)
                {
                    pendingConfidentId = match.selectedStableStateId;

                    // Phase 9: a runtime event eligible for State verification
                    // (Section 14) - confidently matched, correction actually
                    // available, not bypassed, sidechain present. Whether the
                    // audible branch really settles on this identity by the
                    // time the after-window completes is re-checked at
                    // hand-off (takeCompletedMeasurementCapture), never
                    // assumed here.
                    if (hasSidechain)
                    {
                        const auto package = resolveDynamicPackage (
                            activeMap, match.selectedStableStateId, configuredStrength, sampleRate);
                        if (package.valid && package.decision == DynamicPackageDecision::StateCorrection)
                        {
                            const double tap = (double) reportedLatencySamples
                                + package.effectiveAbsoluteDelayMs * sampleRate / 1000.0;
                            measurementCapture.beginCapture (mapGeneration, match.selectedStableStateId,
                                                             DynamicSelectorBranchKind::Global,
                                                             observation.triggerSample, tap, sampleRate);
                        }
                    }
                }
            }
        }

        advanceStateEditAuditions (n);
        updateServiceBinding (pendingConfidentId);

        DynamicSelectorBranchRoster roster;
        buildRoster (roster);

        DynamicContinuityMixerInputs inputs;
        inputs.globalLow = &engine.getGlobalLowOutput();
        for (int slot = 0; slot < DynamicSelectorContract::kStateSlotCount; ++slot)
            inputs.stateLow[(size_t) slot] = &engine.getStateLowOutput (slot);
        inputs.serviceLow = &engine.getServiceLowOutput();
        inputs.commonHigh = &engine.getHighOutput();

        if (! mixer.renderBlock (scheduler, roster, inputs, n, chunkOutput))
            return false;

        for (int ch = 0; ch < numChannels; ++ch)
            output.copyFrom (ch, offset, chunkOutput, ch, 0, n);

        // Phase 9: feed the actual audible processed bass and the fixed
        // 20 ms-aligned raw kick reference (the same common-latency shift
        // every branch shares) into any in-flight measurement captures at
        // this same absolute stream position, then promote any capture that
        // just finished both windows.
        for (int i = 0; i < n; ++i)
        {
            const float rawKick = rawKickMono != nullptr ? rawKickMono[offset + i] : 0.0f;
            measurementKickAlignDelay.pushSample (0, std::isfinite (rawKick) ? rawKick : 0.0f);
            const float alignedKick = measurementKickAlignDelay.popSample (0, (float) reportedLatencySamples);
            const float processedBass = chunkOutput.getSample (0, i);
            measurementCapture.pushOutputSample (processedBass, alignedKick);
        }
        measurementCapture.serviceCompletions();

        runtimeSamplePos = captureBank.nextSample();
        return true;
    }

    // Bounded semantic Service policy (see DYNAMIC_DESIGN_FREEZE.md). Priming and
    // package resolution run only here, at the chunk boundary, never inside the
    // ordinary per-sample loop.
    void updateServiceBinding (uint64_t confidentId) noexcept
    {
        // Phase 12: an in-flight edit audition owns Service exclusively until
        // it finishes (bounded by Settling's warm-frame countdown, or by
        // WaitingForSilence resolving) - never steal it out from under a
        // safe retune in progress.
        if (serviceOwnedByEditAuditionForId != 0)
            return;

        // A warm persistent branch always wins: drop a Service binding whose
        // identity is now available as a warm persistent State branch.
        if (serviceBindingValid && warmPersistentSlotFor (serviceBoundStableStateId) >= 0)
            clearServiceBinding();

        if (confidentId == 0)
            return;
        if (warmPersistentSlotFor (confidentId) >= 0)
            return; // persistent branch already warm; no Service needed
        if (serviceBindingValid && serviceBoundStableStateId == confidentId)
            return; // already serving this identity

        const auto package = resolveDynamicPackage (activeMap, confidentId, configuredStrength, sampleRate);
        if (! package.valid || package.decision != DynamicPackageDecision::StateCorrection)
            return;

        if (! engine.configureFromPackage (DynamicHotBranchKind::Service, -1, package))
            return;

        // Explicitly prime Service from shared history so it can become warm
        // for selection at a bounded integration boundary.
        const int primeSamples = (int) std::ceil (sampleRate
            * DynamicHotBranchContract::kServiceWarmupMs / 1000.0);
        engine.primeService (primeSamples);
        ++diagnostics.servicePrimes;

        serviceConfigured = true;
        serviceBoundStableStateId = confidentId;
        serviceBindingValid = true;
        ++diagnostics.serviceBindings;
    }

    int warmPersistentSlotFor (uint64_t stableStateId) const noexcept
    {
        if (stableStateId == 0)
            return -1;
        for (int slot = 0; slot < DynamicHotBranchContract::kStateSlots; ++slot)
        {
            // Phase 12: a slot mid edit-audition settle is deliberately not
            // considered "available" even though the engine itself still
            // reports it active+warm - its recursive filter/interpolator
            // state has not settled onto the newly-committed package yet.
            if (forceHiddenSlot[(size_t) slot])
                continue;
            const auto info = engine.getStateInfo (slot);
            if (info.active && info.warm && info.stableStateId == stableStateId)
                return slot;
        }
        return -1;
    }

    void buildRoster (DynamicSelectorBranchRoster& roster) const noexcept
    {
        roster.global = engine.getGlobalInfo();
        for (int slot = 0; slot < DynamicSelectorContract::kStateSlotCount; ++slot)
        {
            roster.states[(size_t) slot] = engine.getStateInfo (slot);
            // Phase 12: hide a slot mid edit-audition settle from the
            // scheduler's selection entirely, for the same reason as above.
            if (forceHiddenSlot[(size_t) slot])
                roster.states[(size_t) slot].active = false;
        }
        roster.service = engine.getServiceInfo();
        roster.serviceBoundStableStateId = serviceBoundStableStateId;
        roster.serviceBindingValid = serviceBindingValid;
    }

    void updateSelectionDiagnostics() noexcept
    {
        const auto& selectorDiag = scheduler.getDiagnostics();
        diagnostics.lastSelectorDecision = selectorDiag.lastDecision;
        diagnostics.selectedStableStateId = selectorDiag.selectedSemanticStateId;
        diagnostics.selectedBranchKind = selectorDiag.selectedBranchKind;
        diagnostics.fallbackActive = selectorDiag.selectedBranchKind == DynamicSelectorBranchKind::Global;
    }

    // Finite, exact-latency safe fallback: a dry 20 ms delay of the input,
    // written straight into `output`. Used only when package configuration
    // fails (never for a runtime-eligible map). The scheduler/capture timeline
    // is meaningless in the failure state, so it is reset to start recovery
    // clean without stale timestamps.
    void writeDryFallback (const juce::AudioBuffer<float>& input,
                           juce::AudioBuffer<float>& output,
                           int inOffset,
                           int outOffset,
                           int numSamples) noexcept
    {
        resetTimeline();
        for (int i = 0; i < numSamples; ++i)
        {
            for (int ch = 0; ch < numChannels; ++ch)
            {
                const float in = input.getSample (ch, inOffset + i);
                fallbackDelay.pushSample (ch, std::isfinite (in) ? in : 0.0f);
                output.setSample (ch, outOffset + i, fallbackDelay.popSample (ch, (float) reportedLatencySamples));
            }
        }
    }

    bool prepared = false;
    double sampleRate = 0.0;
    int maxBlock = 0;
    int numChannels = 0;
    int reportedLatencySamples = 0;

    DynamicStateMap activeMap = makeEmptyDynamicStateMap();
    uint64_t mapGeneration = 0;

    DynamicFingerprintCaptureBank captureBank;
    TransientDetector kickTrigger;
    DynamicHotBranchEngine engine;
    DynamicSelectorScheduler scheduler;
    DynamicContinuityMixer mixer;

    bool serviceBindingValid = false;
    bool serviceConfigured = false;
    uint64_t serviceBoundStableStateId = 0;

    // Phase 12: safe per-state edit retune bookkeeping. All audio-thread-only,
    // non-persistent, fixed-size (no allocation). See configurePackagesIfNeeded()/
    // advanceStateEditAuditions() for the state machine this drives.
    std::array<StateEditAudition, DynamicHotBranchContract::kStateSlots> stateEditAudition {};
    std::array<bool, DynamicHotBranchContract::kStateSlots> forceHiddenSlot {};
    std::array<DynamicPackageResolution, DynamicHotBranchContract::kStateSlots> lastAttemptedStatePackage {};
    std::array<bool, DynamicHotBranchContract::kStateSlots> hasLastAttemptedStatePackage {};
    // Nonzero while an edit audition holds Service exclusively, so the
    // ordinary confident-match-driven updateServiceBinding() path cannot
    // steal it mid-retune.
    uint64_t serviceOwnedByEditAuditionForId = 0;

    bool hasConfigured = false;
    uint64_t configuredGeneration = 0;
    double configuredStrength = std::numeric_limits<double>::quiet_NaN();
    double configuredSampleRate = 0.0;

    int64_t runtimeSamplePos = 0;

    juce::AudioBuffer<float> chunkInput;
    juce::AudioBuffer<float> chunkOutput;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> fallbackDelay;

    // Phase 9: runtime measurement capture (see Section 12-14). Owns its own
    // fixed slots/rings; measurementKickAlignDelay supplies the fixed
    // 20 ms-aligned raw kick reference fed alongside the actual processed
    // bass at every output sample.
    DynamicRuntimeMeasurementCapture measurementCapture;
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None> measurementKickAlignDelay;
    // Persistent scratch for takeCompletedMeasurementCapture(); sized once in
    // prepare() so the hand-off never allocates.
    DynamicRuntimeMeasurementCaptureResult measurementHandoffScratch;

    // Phase 12, Section 15: same-thread staging ring for Unknown/Ambiguous
    // match events (see takeRecentUnknownEvent()).
    DynamicRecentUnknownStagingRing recentUnknownStaging;

    DynamicProductionDiagnostics diagnostics;
};
