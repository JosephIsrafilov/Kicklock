#pragma once

#include <array>

// Shared band plan for the multi-band phase/correlation analysis (P5).
//
// Kick and bass fight for the same space mostly down low. Keep the phase
// matching range capped at 500 Hz so click/attack content cannot make the
// displayed percentage look better than the actual low/body relationship.
//
// Both the offline analyzer (zero-phase filtfilt) and the live meter (causal
// biquads) read this same table so their band definitions never drift apart.
// The two still produce different NUMBERS (different filter phase), which is why
// the UI labels them distinctly - see P8.
struct PhaseBandDef
{
    float lowHz;
    float highHz;
    float weight;      // decision weight in the overall score
    const char* name;
};

namespace PhaseBands
{
    inline constexpr int numBands = 4;

    // SUB + LOW weighted highest; LOW MID / BODY moderate.
    inline constexpr std::array<PhaseBandDef, numBands> table {{
        {  20.0f,   60.0f, 1.00f, "SUB"     },
        {  60.0f,  120.0f, 1.00f, "LOW"     },
        { 120.0f,  250.0f, 0.50f, "LOW MID" },
        { 250.0f,  500.0f, 0.35f, "BODY"    },
    }};

    // Indices into the table, for callers that need a specific band.
    inline constexpr int subBand    = 0;
    inline constexpr int lowBand     = 1;
    inline constexpr int lowMidBand  = 2;
    inline constexpr int bodyBand    = 3;

    // The low-end bands that must dominate automatic decisions.
    inline constexpr int lowEndBandCount = 2; // SUB + LOW
}
