#pragma once

#include "TransientDetector.h"

// The Learn worker and production runtime must detect exactly the same raw-kick
// trigger positions. Keep the complete detector contract in one pure helper.
inline void configureDynamicFingerprintTrigger (TransientDetector& detector,
                                                double sampleRate) noexcept
{
    detector.prepare (sampleRate);
    detector.setThreshold (1.0e-7f);
    detector.setMinimumEnergyGate (1.0e-8f);
    detector.setAttackReleaseMs (2.0f, 60.0f);
    detector.setTriggerRatio (1.35f);
    detector.setHoldoffMs (90.0f);
}
