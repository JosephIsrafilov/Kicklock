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
