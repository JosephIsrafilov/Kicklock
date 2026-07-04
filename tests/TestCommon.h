#pragma once

#include <juce_core/juce_core.h>
#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <iostream>
#include <thread>
#include <vector>

#include "CorrelationMeter.h"
#include "FractionalDelayLine.h"
#include "AllpassPhaseRotator.h"
#include "AlignmentAnalyzer.h"
#include "TransientDetector.h"
#include "MultiBandCorrelation.h"
#include "RealtimeMultiBandMeter.h"
#include "SignalActivityTracker.h"
#include "PhaseFixEngine.h"
#include "AnalyzeState.h"
#include "PhaseBands.h"
#include "PluginProcessor.h"
#include "util/HitCaptureBuffer.h"
#include "util/RawCaptureBuffer.h"
#include "ui/ScopeVisuals.h"
#include "ui/StatusHelpers.h"

namespace
{
    constexpr double kSampleRate = 48000.0;
    constexpr double kTwoPi = juce::MathConstants<double>::twoPi;
}
