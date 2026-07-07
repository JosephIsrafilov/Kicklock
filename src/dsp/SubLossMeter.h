#pragma once

#include <algorithm>
#include <cmath>

// Turns an abstract correlation reading into an honest, producer-legible cost:
// how many dB of combined sub/low-end energy the CURRENT phase relationship
// throws away compared to the best physically achievable alignment (same
// content, correlation driven to correlationBest via delay/polarity). Every
// other read-out in this plugin reports "72% match"; this answers the
// question a producer actually asks — "how much level am I losing".
//
// For two band-limited, zero-mean signals with individual energies (mean
// square) energyA/energyB and Pearson correlation r, the energy of their sum
// is exactly:
//
//     E(r) = energyA + energyB + 2r * sqrt(energyA * energyB)
//
// lossDb compares E(correlationNow) against E(correlationBest):
//
//     lossDb = 10 * log10( E(correlationNow) / E(correlationBest) )
//
// which is <= 0 whenever correlationBest is the (achievable) maximum, i.e. the
// current relationship never reads as a GAIN over the best case. Correlation
// asymmetry between the two signals' levels is exactly represented (a quiet
// bass under a loud kick can only lose a little, however out of phase it is;
// two equal-level signals at r = -1 lose (almost) everything) — clamped to
// -60 dB rather than -inf so full cancellation still renders as a number.
inline float subLossDb (double energyA, double energyB,
                        double correlationNow, double correlationBest = 1.0) noexcept
{
    energyA = std::max (0.0, energyA);
    energyB = std::max (0.0, energyB);
    correlationNow  = std::clamp (correlationNow,  -1.0, 1.0);
    correlationBest = std::clamp (correlationBest, -1.0, 1.0);

    const double cross = 2.0 * std::sqrt (energyA * energyB);
    const double energyNow  = energyA + energyB + cross * correlationNow;
    const double energyBest = energyA + energyB + cross * correlationBest;

    constexpr float floorDb = -60.0f;

    if (energyBest <= 1.0e-12)
        return 0.0f;
    if (energyNow <= 1.0e-12)
        return floorDb;

    return std::max (floorDb, (float) (10.0 * std::log10 (energyNow / energyBest)));
}

// Converts this codebase's 0..100 "match percent" convention (50 = silence/
// uncorrelated, 100 = r = +1, 0 = r = -1) back to a Pearson correlation.
inline double correlationFromMatchPercent (float matchPercent) noexcept
{
    return std::clamp ((double) matchPercent / 50.0 - 1.0, -1.0, 1.0);
}
