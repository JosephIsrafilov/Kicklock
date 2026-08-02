#pragma once

#include "../dsp/DynamicStateMap.h"

// =============================================================================
// Phase 12, Section 12: bounded editing strategy - pure coalescing logic.
//
// While a slider drag is in progress, the UI calls setDraft() on every
// onValueChange. A separate, thin juce::Timer (owned by DynamicStateInspector,
// not this class) polls hasPendingEdit()/takePending() at a bounded rate
// (e.g. every 30 ms) and publishes through
// KickLockAudioProcessor::applyDynamicStateEdit(). This class holds at most
// one pending value - "newest wins" - never a queue, so rapid dragging can
// never accumulate unbounded work. Kept free of juce::Timer/JUCE UI
// dependencies so the coalescing behavior itself is unit-testable without a
// message loop.
// =============================================================================

class DynamicStateEditThrottle
{
public:
    void setDraft (const DynamicManualTrim& trim) noexcept
    {
        pendingTrim = trim;
        hasPending = true;
    }

    bool hasPendingEdit() const noexcept { return hasPending; }

    // Returns the newest draft and clears the pending flag. Callers publish
    // it; if publication fails, the caller may setDraft() again to retry -
    // this class does not retry on its own.
    DynamicManualTrim takePending() noexcept
    {
        hasPending = false;
        return pendingTrim;
    }

    void cancel() noexcept
    {
        hasPending = false;
    }

private:
    DynamicManualTrim pendingTrim;
    bool hasPending = false;
};
