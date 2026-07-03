#pragma once

#include <algorithm>
#include <cmath>

enum class ScopeViewMode
{
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

inline ScopeViewMode scopeViewModeFromChoiceIndex (int index) noexcept
{
    switch (index)
    {
        case 1:  return ScopeViewMode::Overlay;
        case 2:  return ScopeViewMode::Separate;
        default: return ScopeViewMode::PhaseDelta;
    }
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
    const double quarterMs = bpmToQuarterMs (bpm);
    if (quarterMs <= 0.0)
        return 0.0;

    switch (division)
    {
        case GridDivision::Quarter:      return quarterMs * 4.0;
        case GridDivision::Eighth:       return quarterMs * 2.0;
        case GridDivision::Sixteenth:    return quarterMs;
        case GridDivision::ThirtySecond: return quarterMs * 0.5;
        case GridDivision::Bar:          return quarterMs * 4.0;
        case GridDivision::Milliseconds: return 0.0;
    }

    return 0.0;
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
