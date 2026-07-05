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
