#pragma once

#include <JuceHeader.h>

#include <algorithm>
#include <cmath>

inline float normaliseBassDelayMs (float delayMs) noexcept
{
    constexpr float zeroSnap = 5.0e-4f;

    if (std::abs (delayMs) <= zeroSnap)
        return 0.0f;

    return std::clamp (delayMs, -20.0f, 20.0f);
}

inline juce::String formatBassDelayMs (float delayMs)
{
    return juce::String (normaliseBassDelayMs (delayMs), 2) + " ms";
}

// Signed delay read-out for the manual Delay control and the scope markers.
// Negative advances bass, positive delays it. Zero (within a small snap) shows
// as "0.00 ms" with no sign; positive values carry an explicit "+".
inline juce::String formatSignedDelayMs (float delayMs)
{
    const float normalised = normaliseBassDelayMs (delayMs);

    if (normalised == 0.0f)
        return "0.00 ms";

    const juce::String sign = normalised > 0.0f ? "+" : "-";
    return sign + juce::String (std::abs (normalised), 2) + " ms";
}

// Producer-language verdict for the scope's timing markers. The Δ value is
// bassPeak - kickPeak in ms, so positive means the bass body lands AFTER the
// kick. "Δ +5.20 ms" alone is engineer-speak; this says what to do about it.
inline const char* timingVerdictText (float deltaMs, float inTimeToleranceMs = 0.25f) noexcept
{
    if (deltaMs > inTimeToleranceMs)
        return "bass late";
    if (deltaMs < -inTimeToleranceMs)
        return "bass early";
    return "in time";
}
