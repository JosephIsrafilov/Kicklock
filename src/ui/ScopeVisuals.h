#pragma once

#include <algorithm>
#include <cmath>

enum class ScopeViewMode
{
    Triggered,
    FreeRun,
    PhaseDelta,
    Overlay,
    Separate
};

enum class GridDivision
{
    Quarter,
    Eighth,
    Sixteenth,
    ThirtySecond,
    Bar,
    Milliseconds
};

enum class PhaseRelation
{
    Constructive,
    Destructive,
    Silent
};

enum class KickReferenceState
{
    NoReference,
    Locked,
    RelockPending
};

struct ScopePeakMarkers
{
    int bassPeakIndex = -1;
    int kickPeakIndex = -1;
    float deltaMs = 0.0f;
    bool valid = false;
};

struct DisplayHistoryIndices
{
    int bassIndex = 0;
    int kickIndex = 0;
};

inline ScopeViewMode scopeViewModeFromChoiceIndex (int index) noexcept
{
    switch (index)
    {
        case 1:  return ScopeViewMode::FreeRun;
        case 2:  return ScopeViewMode::PhaseDelta;
        case 3:  return ScopeViewMode::Overlay;
        case 4:  return ScopeViewMode::Separate;
        default: return ScopeViewMode::Triggered;
    }
}

inline float scopeDragPixelsToDelayDeltaMs (float pixelDelta, bool fine) noexcept
{
    return pixelDelta * (fine ? 0.01f : 0.1f) / 4.0f;
}

inline int calculateTriggeredRenderPointCount (int sampleCount, int pixelWidth) noexcept
{
    if (sampleCount <= 1)
        return std::max (0, sampleCount);

    const int targetPoints = std::max (2, pixelWidth * 2);
    return std::clamp (targetPoints, 2, sampleCount);
}

inline int triggeredRenderSampleIndex (int pointIndex, int pointCount, int sampleCount) noexcept
{
    if (sampleCount <= 1 || pointCount <= 1)
        return 0;

    return std::clamp ((int) std::lround ((double) pointIndex * (double) (sampleCount - 1)
                                          / (double) (pointCount - 1)),
                       0, sampleCount - 1);
}

struct TriggeredVisibleRange
{
    int first = 0;
    int visible = 0;
};

// Per-pixel-column peak envelope: for each of numColumns, the min and max
// sample within that column's share of [first, first + count). Point-sampling
// individual samples when several samples share one pixel misses the peaks
// between picks, so the trace "boils" frame to frame; a min/max envelope is
// stable and also caps the rendered path size at 2 * numColumns regardless of
// zoom. O(count), no allocation (caller owns the output arrays).
inline void buildMinMaxColumns (const float* src, int first, int count,
                                int numColumns, float* minOut, float* maxOut) noexcept
{
    if (src == nullptr || minOut == nullptr || maxOut == nullptr
        || count <= 0 || numColumns <= 0)
        return;

    for (int c = 0; c < numColumns; ++c)
    {
        const int s0 = first + (int) ((long long) c * count / numColumns);
        const int s1 = first + (int) ((long long) (c + 1) * count / numColumns);

        float lo = src[s0];
        float hi = src[s0];
        for (int i = s0 + 1; i < std::max (s0 + 1, s1); ++i)
        {
            lo = std::min (lo, src[i]);
            hi = std::max (hi, src[i]);
        }

        minOut[c] = lo;
        maxOut[c] = hi;
    }
}

// Whether a trace should be rendered as a per-column min/max envelope band
// instead of a point-sampled polyline: only once several samples genuinely
// share each pixel — below that a polyline is smoother-looking and cheaper.
inline bool scopeShouldRenderMinMaxBand (int visibleSamples, int pixelWidth) noexcept
{
    return pixelWidth > 0 && visibleSamples > pixelWidth * 3;
}

// Time-zoom for the triggered scope. The capture includes a short pre-roll, and
// the display keeps that pre-roll visible so the trigger/0 line is slightly
// inside the left edge, like a hardware/pro plugin scope. The -pre-roll label is
// intentionally not drawn; the visible context is useful, the negative label is
// not.
inline TriggeredVisibleRange computeTriggeredVisibleRange (int n, int preRoll, float timeZoom) noexcept
{
    TriggeredVisibleRange range;

    if (n <= 2)
    {
        range.first = 0;
        range.visible = std::max (0, n);
        return range;
    }

    const float zoom = std::max (1.0f, timeZoom);
    int visible = (int) std::lround ((double) n / (double) zoom);
    visible = std::clamp (visible, 2, n);

    const int clampedPreRoll = std::clamp (preRoll, 0, n - 1);
    const double triggerFraction = (double) clampedPreRoll / (double) (n - 1);

    int first = clampedPreRoll - (int) std::lround (triggerFraction * (double) (visible - 1));
    first = std::clamp (first, 0, n - visible);

    range.first = first;
    range.visible = visible;
    return range;
}

// The audio trigger is envelope/energy based, so it can legitimately fire a
// few ms after the first visible kick lobe. For the scope axis, 0 ms should sit
// on the first meaningful kick sample inside the captured pre-roll.
inline int findTriggeredKickOnsetIndex (const float* kick, int n, int triggerIndex) noexcept
{
    if (kick == nullptr || n <= 0)
        return 0;

    const int trigger = std::clamp (triggerIndex, 0, n - 1);
    const int searchEnd = std::clamp (trigger + std::max (16, trigger / 2), trigger + 1, n);

    float peak = 0.0f;
    for (int i = 0; i < searchEnd; ++i)
        peak = std::max (peak, std::abs (kick[i]));

    if (peak <= 1.0e-5f)
        return trigger;

    const float threshold = std::max (peak * 0.04f, 1.0e-4f);

    for (int i = 0; i <= trigger; ++i)
    {
        const float a0 = std::abs (kick[i]);
        const float a1 = i + 1 < n ? std::abs (kick[i + 1]) : 0.0f;
        const float a2 = i + 2 < n ? std::abs (kick[i + 2]) : 0.0f;

        if (a0 >= threshold && (a1 >= threshold * 0.5f || a2 >= threshold * 0.5f))
            return i;
    }

    return trigger;
}

inline float clampTriggeredPanScrollMs (float scrollMs,
                                        int n,
                                        int visible,
                                        int anchoredFirst,
                                        double sampleRate) noexcept
{
    if (sampleRate <= 0.0 || n <= 1 || visible <= 1 || visible >= n)
        return 0.0f;

    const int maxFirst = std::max (0, n - visible);
    const int anchor = std::clamp (anchoredFirst, 0, maxFirst);
    const float minScrollMs = (float) ((double) (anchor - maxFirst) * 1000.0 / sampleRate);
    const float maxScrollMs = (float) ((double) anchor * 1000.0 / sampleRate);

    return std::clamp (scrollMs, minScrollMs, maxScrollMs);
}

inline int computeTriggeredPannedFirst (int n,
                                        int visible,
                                        int anchoredFirst,
                                        float scrollMs,
                                        double sampleRate) noexcept
{
    if (n <= 1 || visible <= 1 || visible >= n)
        return 0;

    const int maxFirst = std::max (0, n - visible);
    const int panSamples = sampleRate > 0.0
        ? (int) std::lround ((double) scrollMs * sampleRate / 1000.0)
        : 0;
    return std::clamp (anchoredFirst - panSamples, 0, maxFirst);
}

inline GridDivision gridDivisionFromChoiceIndex (int index) noexcept
{
    switch (index)
    {
        case 0:  return GridDivision::Quarter;
        case 1:  return GridDivision::Eighth;
        case 2:  return GridDivision::Sixteenth;
        case 3:  return GridDivision::ThirtySecond;
        case 4:  return GridDivision::Bar;
        default: return GridDivision::Milliseconds;
    }
}

inline double bpmToQuarterMs (double bpm) noexcept
{
    return bpm > 0.0 ? 60000.0 / bpm : 0.0;
}

inline double gridDivisionToMs (double bpm, GridDivision division) noexcept
{
    const double quarterMs = bpmToQuarterMs (bpm);
    if (quarterMs <= 0.0)
        return 0.0;

    switch (division)
    {
        case GridDivision::Quarter:      return quarterMs;
        case GridDivision::Eighth:       return quarterMs * 0.5;
        case GridDivision::Sixteenth:    return quarterMs * 0.25;
        case GridDivision::ThirtySecond: return quarterMs * 0.125;
        case GridDivision::Bar:          return quarterMs * 4.0;
        case GridDivision::Milliseconds: return 0.0;
    }

    return 0.0;
}

inline double gridWindowToMs (double bpm, GridDivision division) noexcept
{
    return gridDivisionToMs (bpm, division);
}

inline PhaseRelation classifyPhaseRelation (float bass, float kick, float epsilon = 1.0e-5f) noexcept
{
    if (std::abs (bass) <= epsilon || std::abs (kick) <= epsilon)
        return PhaseRelation::Silent;

    return bass * kick >= 0.0f ? PhaseRelation::Constructive
                               : PhaseRelation::Destructive;
}

inline float samplesToMs (int samples, double sampleRate) noexcept
{
    if (sampleRate <= 0.0)
        return 0.0f;

    return (float) ((double) samples * 1000.0 / sampleRate);
}

inline int msToSamples (float ms, double sampleRate) noexcept
{
    if (sampleRate <= 0.0)
        return 0;

    return (int) std::lround ((double) ms * sampleRate / 1000.0);
}

inline int visualOffsetToDisplaySamples (int visualOffsetSamples, int decimationFactor) noexcept
{
    const int safeDecimation = std::max (1, decimationFactor);
    return (int) std::lround ((double) visualOffsetSamples / (double) safeDecimation);
}

inline int calculateVisibleScopeSamples (int historyLength,
                                         double sampleRate,
                                         int decimationFactor,
                                         float timeZoom,
                                         GridDivision gridDivision,
                                         bool bpmAvailable,
                                         double bpm) noexcept
{
    const int safeHistory = std::max (2, historyLength);
    const int safeDecimation = std::max (1, decimationFactor);
    const double safeZoom = std::max (1.0, (double) timeZoom);

    if (sampleRate > 0.0 && bpmAvailable && gridDivision != GridDivision::Milliseconds)
    {
        const double targetWindowMs = gridWindowToMs (bpm, gridDivision) / safeZoom;
        if (targetWindowMs > 0.0)
        {
            const double visible = targetWindowMs * sampleRate / 1000.0 / (double) safeDecimation;
            return std::clamp ((int) std::lround (visible), 2, safeHistory);
        }
    }

    return std::clamp ((int) std::lround ((double) safeHistory / safeZoom), 2, safeHistory);
}

inline double calculateVisibleWindowMs (int visibleSamples,
                                        double sampleRate,
                                        int decimationFactor) noexcept
{
    return samplesToMs (visibleSamples * std::max (1, decimationFactor), sampleRate);
}

// Effective display-freeze predicate. The manual Freeze button and the
// temporary mouse-hold inspection are tracked separately so releasing the
// mouse never clears a freeze the user set on purpose; the display is held
// whenever either is active.
inline bool scopeDisplayHeld (bool manualFrozen, bool interactionHoldActive) noexcept
{
    return manualFrozen || interactionHoldActive;
}

// Free-run is a raw live scope: it deliberately IGNORES the display-only
// visual/PDC offset so it reads as the untouched incoming signal. Every other
// mode applies the offset so the bass and kick line up for comparison. This is
// the semantic that makes Free-run and Overlay genuinely different rather than
// two labels over identical drawing.
inline bool scopeModeAppliesVisualOffset (ScopeViewMode mode) noexcept
{
    return mode != ScopeViewMode::FreeRun;
}

// Only the triggered view maps a horizontal drag to the Delay parameter, and
// even then only as an explicit modifier gesture (Shift + drag). A plain
// click/hold/drag always pauses the display and scrubs the waveform instead,
// so a user inspecting the scope in Triggered mode can never silently move
// Delay by accident. Every scrolling view ignores this entirely.
inline bool scopeModeUsesDelayDrag (ScopeViewMode mode) noexcept
{
    return mode == ScopeViewMode::Triggered;
}

inline bool scopeWantsDelayDragGesture (ScopeViewMode mode, bool shiftHeld) noexcept
{
    return scopeModeUsesDelayDrag (mode) && shiftHeld;
}

inline const char* scopeModeCaption (ScopeViewMode mode) noexcept
{
    switch (mode)
    {
        case ScopeViewMode::FreeRun:    return "FREE-RUN: live raw scope";
        case ScopeViewMode::Overlay:    return "OVERLAY: aligned bass/kick comparison";
        case ScopeViewMode::PhaseDelta: return "PHASE DELTA";
        case ScopeViewMode::Separate:   return "SEPARATE";
        case ScopeViewMode::Triggered:  return "TRIGGERED";
    }

    return "";
}

inline KickReferenceState kickReferenceStateAfterRelock() noexcept
{
    return KickReferenceState::RelockPending;
}

inline bool kickReferenceShouldReplaceOnCapture (KickReferenceState state) noexcept
{
    return state != KickReferenceState::Locked;
}

inline KickReferenceState kickReferenceStateAfterCapture (KickReferenceState state) noexcept
{
    return kickReferenceShouldReplaceOnCapture (state) ? KickReferenceState::Locked : state;
}

inline const char* triggeredScopeEmptyText (KickReferenceState state) noexcept
{
    return state == KickReferenceState::RelockPending ? "WAITING FOR NEW KICK REF"
                                                      : "WAITING FOR KICK";
}

// --- Triggered sweep display helpers (ReVision-style per-hit redraw) --------

// Auto-gain target from a window peak: scale quiet material up so it fills
// ~88% of the lane, clamped so silence doesn't explode and loud material
// isn't attenuated below unity.
inline float scopeAutoGainTargetFromPeak (float peak) noexcept
{
    return peak > 1.0e-4f ? std::clamp (0.88f / peak, 1.0f, 40.0f) : 1.0f;
}

// Sticky auto-gain retargeting. A scope must not "breathe" with every kick:
// once a useful vertical scale has been established, keep it unless the input
// is genuinely much louder (protect against clipping) or much quieter (new
// material would otherwise be unreadable). Small hit-to-hit peak differences
// are deliberately ignored.
inline bool scopeAutoGainShouldRetarget (float currentTarget, float candidateTarget,
                                         float louderRatio = 0.70f,
                                         float quieterRatio = 2.50f) noexcept
{
    if (currentTarget <= 0.0f)
        return true;

    const float ratio = candidateTarget / currentTarget;
    return ratio < louderRatio || ratio > quieterRatio;
}

inline float scopeGlideAutoGain (float currentGain, float targetGain) noexcept
{
    const float coeff = targetGain < currentGain ? 0.30f : 0.035f;
    return currentGain + coeff * (targetGain - currentGain);
}

// Past-sweep ghosts are disabled for a cleaner realtime oscilloscope read.
// The current sweep and locked kick reference should be visually authoritative;
// old faint bass traces looked like level modulation/noise on repeated hits.
inline float scopeSweepGhostAlpha (int ghostIndex, int ghostCount) noexcept
{
    (void) ghostIndex;
    (void) ghostCount;
    return 0.0f;
}

// A ghost trace must be a completed previous bass sweep. Partial windows can
// happen when the display is held or the progressive stream drops a window; if
// we keep those as ghosts they look like valid old hits even though the tail is
// missing.
inline bool scopeSweepWorthKeepingAsGhost (int fillSamples, int windowSamples) noexcept
{
    return windowSamples > 0 && fillSamples >= windowSamples;
}

// Minimum completed-window kick energy for replacing the locked reference.
// This is intentionally far below real production kick levels, but above
// denormals/noise and blank accidental captures.
inline bool scopeKickReferenceCaptureIsValid (float kickPeak) noexcept
{
    return kickPeak > 1.0e-4f;
}

// Deterministic pan: the displayed scroll is always derived from the
// mouse-down anchor (startScrollMs + how far the mouse has moved since), never
// integrated frame-to-frame, so a drag can't drift or jump. Grab-and-move
// metaphor: dragging the waveform to the right (positive pixels) reveals older
// material, i.e. scrolls further back in time (larger scrollMs).
inline float scopeDragToScrollMs (float startScrollMs, float pixelsMoved, float msPerPixel) noexcept
{
    return startScrollMs + pixelsMoved * msPerPixel;
}

// Cursor-anchored zoom: keep the time under a given horizontal fraction of the
// view (0 = left edge, 1 = right/live edge) fixed while the visible window
// changes size. displayScrollMs measures the right edge's age, so a point at
// fraction f from the left has age scroll + (1 - f) * window; holding that age
// constant across the window change gives the compensation below. Anchoring at
// the right edge (f = 1) is a no-op, matching the live view's behaviour.
inline float scopeAnchoredZoomScrollMs (float scrollMs, float oldWindowMs,
                                        float newWindowMs, float anchorFraction) noexcept
{
    const float f = std::clamp (anchorFraction, 0.0f, 1.0f);
    return scrollMs + (oldWindowMs - newWindowMs) * (1.0f - f);
}

// Clamp the display scroll to a valid history range: 0 ms is the latest/live
// edge, and the maximum is the recorded history duration minus the currently
// visible window (so the left edge can never fall before the oldest sample).
inline float clampScopeScrollMs (float scrollMs,
                                 int historyLength,
                                 int visibleSamples,
                                 double sampleRate,
                                 int decimationFactor) noexcept
{
    const int safeDecimation = std::max (1, decimationFactor);
    const int safeVisible    = std::max (1, visibleSamples);
    const int maxScrollSamples = std::max (0, historyLength - safeVisible) * safeDecimation;
    const float maxScrollMs = samplesToMs (maxScrollSamples, sampleRate);

    return std::clamp (scrollMs, 0.0f, maxScrollMs);
}

inline int wrapHistoryIndex (int index, int historyLength) noexcept
{
    if (historyLength <= 0)
        return 0;

    const int wrapped = index % historyLength;
    return wrapped < 0 ? wrapped + historyLength : wrapped;
}

inline int resolveDisplayHistoryIndex (int writeIndex,
                                       int historyLength,
                                       int firstVisibleSample,
                                       int visibleIndex,
                                       int visualOffsetSamples,
                                       int decimationFactor) noexcept
{
    const int displayOffset = visualOffsetToDisplaySamples (visualOffsetSamples, decimationFactor);
    return wrapHistoryIndex (writeIndex + firstVisibleSample + visibleIndex + displayOffset,
                             historyLength);
}

inline DisplayHistoryIndices resolveRelativeDisplayHistoryIndices (int writeIndex,
                                                                   int historyLength,
                                                                   int firstVisibleSample,
                                                                   int visibleIndex,
                                                                   int visualOffsetSamples,
                                                                   int decimationFactor) noexcept
{
    const int baseIndex = wrapHistoryIndex (writeIndex + firstVisibleSample + visibleIndex,
                                            historyLength);
    const int displayOffset = visualOffsetToDisplaySamples (visualOffsetSamples, decimationFactor);

    return { wrapHistoryIndex (baseIndex + displayOffset, historyLength),
             baseIndex };
}

inline ScopePeakMarkers findScopePeakMarkers (const float* bass,
                                              const float* kick,
                                              int numSamples,
                                              double sampleRate) noexcept
{
    ScopePeakMarkers markers;

    if (bass == nullptr || kick == nullptr || numSamples <= 0 || sampleRate <= 0.0)
        return markers;

    float bassPeak = 0.0f;
    float kickPeak = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const float bassAbs = std::abs (bass[i]);
        if (bassAbs > bassPeak)
        {
            bassPeak = bassAbs;
            markers.bassPeakIndex = i;
        }

        const float kickAbs = std::abs (kick[i]);
        if (kickAbs > kickPeak)
        {
            kickPeak = kickAbs;
            markers.kickPeakIndex = i;
        }
    }

    if (markers.bassPeakIndex < 0 || markers.kickPeakIndex < 0
        || bassPeak <= 1.0e-5f || kickPeak <= 1.0e-5f)
    {
        return markers;
    }

    markers.valid = true;
    markers.deltaMs = samplesToMs (markers.bassPeakIndex - markers.kickPeakIndex, sampleRate);
    return markers;
}

inline float calculatePeakDeltaMs (const float* bass,
                                   const float* kick,
                                   int numSamples,
                                   double sampleRate) noexcept
{
    return findScopePeakMarkers (bass, kick, numSamples, sampleRate).deltaMs;
}

// P7: centered moving average of the raw scope signal. The scope feed is already
// decimated (~2 kHz), so a short symmetric average strips the sample-to-sample
// broadband jitter and leaves the low/body content that actually decides the
// kick/bass phase relationship. Symmetric window => zero net phase shift, so
// peaks and zero-crossings are not moved. O(n), no allocation (caller owns out).
inline void smoothScopeSignal (const float* in, float* out, int n, int halfWindow) noexcept
{
    if (in == nullptr || out == nullptr || n <= 0)
        return;

    halfWindow = std::clamp (halfWindow, 0, n / 2);
    if (halfWindow == 0)
    {
        for (int i = 0; i < n; ++i)
            out[i] = in[i];
        return;
    }

    double sum = 0.0;
    int count = 0;
    for (int j = 0; j <= halfWindow && j < n; ++j) { sum += (double) in[j]; ++count; }

    for (int i = 0; i < n; ++i)
    {
        out[i] = count > 0 ? (float) (sum / (double) count) : 0.0f;

        const int add = i + 1 + halfWindow;
        const int rem = i - halfWindow;
        if (add < n) { sum += (double) in[add]; ++count; }
        if (rem >= 0) { sum -= (double) in[rem]; --count; }
    }
}

// P7: signed low-band smooth sized from the scope sample rate. Picks a centered
// window of roughly half a ~180 Hz period so sub/low/body content survives while
// broadband jitter is averaged out, preserving sign (needed for the
// constructive/destructive shading). Zero net phase shift (symmetric window).
inline void smoothScopeEnvelope (const float* in, float* out, int n, double scopeSampleRate) noexcept
{
    if (in == nullptr || out == nullptr || n <= 0)
        return;

    // ~180 Hz reference: half-period in samples at the scope's decimated rate.
    int halfWindow = 3;
    if (scopeSampleRate > 0.0)
        halfWindow = std::clamp ((int) std::lround (scopeSampleRate / 180.0 / 2.0), 1, n / 2);

    smoothScopeSignal (in, out, n, halfWindow);
}

// P7: transient markers / Δ ms from the signal ENVELOPE, not the raw peak. The
// raw peak sample jumps between adjacent samples frame to frame; the envelope
// peak (rectify + centered smooth) sits on the stable centre of the kick/bass
// body, so the marker and Δ ms read musically instead of jittering. O(n), no
// allocation (rolling window sum of |x|).
inline ScopePeakMarkers findEnvelopePeakMarkers (const float* bass,
                                                 const float* kick,
                                                 int numSamples,
                                                 double sampleRate,
                                                 int halfWindow = 6) noexcept
{
    ScopePeakMarkers markers;

    if (bass == nullptr || kick == nullptr || numSamples <= 0 || sampleRate <= 0.0)
        return markers;

    halfWindow = std::clamp (halfWindow, 0, numSamples / 2);

    auto envelopePeak = [numSamples, halfWindow] (const float* x, float& peakOut) -> int
    {
        int bestIdx = -1;
        float best = 0.0f;
        double sum = 0.0;
        int count = 0;

        for (int j = 0; j <= halfWindow && j < numSamples; ++j) { sum += (double) std::abs (x[j]); ++count; }

        for (int i = 0; i < numSamples; ++i)
        {
            const float env = count > 0 ? (float) (sum / (double) count) : 0.0f;
            if (env > best)
            {
                best = env;
                bestIdx = i;
            }

            const int add = i + 1 + halfWindow;
            const int rem = i - halfWindow;
            if (add < numSamples) { sum += (double) std::abs (x[add]); ++count; }
            if (rem >= 0) { sum -= (double) std::abs (x[rem]); --count; }
        }

        peakOut = best;
        return bestIdx;
    };

    float bassPeak = 0.0f, kickPeak = 0.0f;
    markers.bassPeakIndex = envelopePeak (bass, bassPeak);
    markers.kickPeakIndex = envelopePeak (kick, kickPeak);

    if (markers.bassPeakIndex < 0 || markers.kickPeakIndex < 0
        || bassPeak <= 1.0e-5f || kickPeak <= 1.0e-5f)
    {
        return markers;
    }

    markers.valid = true;
    markers.deltaMs = samplesToMs (markers.bassPeakIndex - markers.kickPeakIndex, sampleRate);
    return markers;
}
