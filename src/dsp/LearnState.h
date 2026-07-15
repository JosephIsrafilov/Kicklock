#pragma once

#include <array>
#include <cstdint>

#include "NotePhaseMap.h"

enum class LearnState
{
    Idle,
    Preparing,
    Capturing,
    Stopping,
    Draining,
    Finalizing,
    ResultReady,
    NotEnoughMaterial,
    Failed,
    Cancelling
};

inline bool learnStateIsBusy (LearnState state) noexcept
{
    return state == LearnState::Preparing || state == LearnState::Capturing
        || state == LearnState::Stopping || state == LearnState::Draining
        || state == LearnState::Finalizing || state == LearnState::Cancelling;
}

struct LearnSessionContext
{
    uint64_t sessionId = 0;
    double sampleRate = 0.0;
    InterpolationType delayInterpolation = InterpolationType::Linear;
    bool crossoverEnabled = true;
    float crossoverHz = 150.0f;
    float preLearnAllpassFreqHz = 50.0f;
    float preLearnAllpassQ = 0.7f;
    bool preLearnAllpassEnabled = false;
    int preLearnStages = 2;
    int acceptedHitsBaseline = 0;
    int droppedHitsBaseline = 0;
    int ignoredOverlapsBaseline = 0;
};

struct LearnProgressSnapshot
{
    uint64_t sessionId = 0;
    LearnState state = LearnState::Idle;
    int capturedHits = 0;
    int drainedHits = 0;              // windows drained by the worker (UI: PROCESSED)
    int pendingQueueHits = 0;
    int droppedQueueHits = 0;
    int ignoredOverlappingTriggers = 0;
    int pitchAcceptedHits = 0;        // pitch-accepted hits (UI: PITCH OK)
    int rejectedPitchHits = 0;        // UI: PITCH REJECTED / REJECTED
    int timingUsableHits = 0;         // LearnDiagnostics::analyzedHits (UI: TIMING OK)
    int unusableSignalHits = 0;
    std::array<int, NotePhaseMapSnapshot::size> trackedNoteHitCounts {};
    // Runtime-only per-note outcomes from the last finalize (not serialized).
    std::array<LearnNoteReport, NotePhaseMapSnapshot::size> noteReports {};
    bool stopRequested = false;
};

struct PendingLearnCandidate
{
    bool present = false;
    uint64_t sessionId = 0;
    LearnSessionContext context;
    LearnFinalizeResult result;
    juce::String applyBlockedReason;
};
