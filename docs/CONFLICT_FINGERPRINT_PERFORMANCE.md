# Runtime fingerprint matching - audio-thread cost (Layer C)

Layer C has one shared four-feature descriptor for both Dynamic Learn and
runtime: anti-phase overlap in three 1-3 ms onset bands plus the kick's early
energy fraction. Learn runs the same accumulator on a captured hit; runtime
starts it with the existing Learn `TransientDetector` settings and completes a
4 ms causal onset window. No FFT, DTW, allocation, lock, or parameter lookup
is performed by this path.

The completed descriptor is compared with the fixed
`NotePhaseMapSnapshot::kMaxStates` array using mean L1 distance. Category L's
Release benchmark performs 100,000 worst-case 16-state selections. Latest raw
output on the development machine: `0.000323069 ms/select at 48 kHz`.

The only per-sample runtime work while an onset is being captured is three
energy/dot-product accumulators. State targets are then retargeted through the
existing Dynamic core smoothing, while `AllpassPhaseRotator` and
`FractionalDelayLine` now expose 1-5 ms re-targetable ramps for their direct
callers. This adds no latency and does not alter Static Analyze/shared
analysis functions.

## Validation scope

Category L proves train/serve descriptor parity, known-state selection,
weak-match fallback, and dense state changes faster than a 3 ms ramp. This is
synthetic ground-truth coverage only. Real-audio plugin integration is tracked
separately by `RealAudioPluginIntegrationTests.cpp`; it remains gated on real
kick and bass stems and must not be represented as validated until those assets
are supplied.
