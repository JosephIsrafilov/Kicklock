# Conflict Localization - worker-thread cost (Layer A)

Layer A runs only while Dynamic Learn finalizes a captured hit. It is not in
the audio callback, Static Analyze path, or runtime correction path.

The localizer performs one `TransientDetector` pass and overlapping 4 ms frame
dot products over the existing 170 ms capture (about 84 frames at 48 kHz).
Scratch storage is worker-owned. `PhaseFixEngine` and `AlignmentAnalyzer` keep
their existing signatures and budgets; this is a separately measured
worker-only cost.

The Release category-J benchmark measured `0.0481673 ms/hit` at 48 kHz on the
development worker (about 0.06 ms/hit). It runs 100 iterations of each required
synthetic case (attack/body/tail x short punch/long 808), measures only
`ConflictRegionLocalizer::localize`, and excludes process startup, UI, and the
existing PhaseFixEngine work. This cost is not added to the audio-thread CPU or
latency budget.

## Real-audio validation status

The repository contains `tests/assets/controlled_real_bass_48k_mono.wav`, but
it does not contain a real kick sample or a kick/bass stem. Because the
attack/body/tail geometry is derived from the kick envelope, the bass-only file
cannot validate the kick-driven region boundaries. No synthetic kick is being
reported as real-audio validation; this remains an explicit open limitation
until a real kick sample or stem is supplied.

`tests/RealAudioPluginIntegrationTests.cpp` now exists to close this gap. It
drives the full `PluginProcessor` (Learn -> Stop -> Apply -> processBlock, with
bass on the main bus and kick on the sidechain bus, exactly as a host would)
against `tests/assets/real_kick.wav` and `tests/assets/real_bass.wav`. Neither
file is checked in yet, so every test in that file currently logs why it is
skipped and records 0 assertions rather than a false pass. Once both files are
supplied (same loop, aligned downbeat, real recorded kick with 6-8+ hits and a
bass part moving across 3+ notes -- see the asset-spec comment in that file),
the suite automatically starts validating: Learn reaching `ResultReady` on
genuine material, Apply measurably improving `MultiBandCorrelation::
weightedMatchPercent` versus the untouched baseline, finite/bounded output,
determinism across independent instances, and survival across representative
host block sizes (64/128/512/2048).

## Phase 11 release gate

Phase 11 adds a separate deterministic synthetic fixture and Release-only
runtime ratio gate. It is not reported as real audio. Missing private stems log
`REAL_AUDIO_STATUS: UNVERIFIED`; setting `KICKLOCK_REQUIRE_REAL_AUDIO_FIXTURES=1`
makes absent or undecodable `real_kick.wav` / `real_bass.wav` fail explicitly.
The throughput gate warms fixed buffers, measures whole batches with
`steady_clock`, and compares median plus robust upper quantile against represented
audio duration. Sanitizer jobs skip wall-clock thresholds but retain finite and
correctness checks.
