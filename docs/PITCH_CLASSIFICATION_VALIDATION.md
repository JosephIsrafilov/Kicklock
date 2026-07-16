# Dynamic Learn pitch-classification validation

Dynamic Learn now keeps the raw audio-thread tracker reading separate from
the worker-selected offline pitch. The selected pitch is used for MIDI labels
and state grouping; the raw reading remains an independent cross-check.

## Octave policy

The worker evaluates the selected YIN lag and the nearby half-lag hypothesis.
It promotes the higher octave only when that half-lag is independently
periodic at the existing confidence floor and its normalized harmonic score
is greater. If both lag hypotheses are credible but harmonic evidence does
not choose one, the hit is reported as octave ambiguous and does not teach a
Dynamic state. No Learn acceptance threshold is relaxed.

Category N covers C1 (32.70 Hz), A1 (55.00 Hz), B1 (61.74 Hz), and C2
(65.41 Hz), the deterministic octave-evidence decision that permits a weak
F/2 correction, an unresolved-evidence rejection, and protection for a
strong genuine low fundamental with a dominant second harmonic.

## Recorded-audio status

An external CC0 proxy was run from a temporary directory using Yamaha RBX
bass samples from FreePats and a Sample Pi kick. It processed 32/32 hits and
learned four notes, so it is a negative control rather than a reproduction of
the original screenshot failure. It is deliberately not copied into
`tests/assets/real_kick.wav` or `tests/assets/real_bass.wav`.

The original project stems remain the final acceptance gate. Until they are
available, `RealAudioPluginIntegrationTests.cpp` remains an explicit asset
skip and must not be described as real-project validation.
