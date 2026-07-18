# Release Readiness

## Status

KickLock `0.3.1` retains the frozen DynamicStateMap v1 architecture described
in `DYNAMIC_DESIGN_FREEZE.md`. Phase 11 adds release gates only; it does not
change parameters, persistence schema, DSP topology, source arbitration, or
latency.

| Area | Status | Evidence |
| --- | --- | --- |
| Static path | VERIFIED | Processor regression suite and Phase 11 callback gate. |
| Legacy Dynamic path | VERIFIED | Legacy compatibility and source-fallback tests. |
| New Dynamic Learn | VERIFIED | Phase 11 waits for the audio-thread Capturing acknowledgement before feeding deterministic Learn material. |
| New Dynamic runtime | VERIFIED | Actual Learn -> Apply -> New map runtime -> save/reload preserves source, stable IDs, packages, snapshots, and finite bounded output. |
| Measurements | VERIFIED | Actual-Learn predicted evidence remains separate from runtime evidence; the processor-backed Performance gate exercises fresh Verified worker handoff, and Verified starts Unavailable after restore. |
| DynamicWorkspace | VERIFIED | Phase 10 regression and Phase 11 construction/snapshot smoke coverage. |
| Latency/PDC | VERIFIED | Exact 20 ms at 44.1, 48, 96, and 192 kHz. |
| Sample rates | VERIFIED | Native fixture generation at 44.1, 48, 96, and 192 kHz. |
| Block sizes | VERIFIED | 48 kHz: 1, 7, 32, 64, 127, 256, 512, 2048; pairwise irregular/small/large coverage at other supported rates. |
| Transport/lifecycle | VERIFIED | Fixture ground-truth boundaries drive stop/start, seek, host-reset, loop-wrap, reprepare, stale-selection, and 20 ms PDC gates. |
| Serialization/migration | VERIFIED | New/legacy independence, high-bit IDs, malformed child rejection, and sidecar reset coverage. |
| Real-time allocations | VERIFIED | Test-only current-thread hooks cover scalar, array, nothrow, aligned, and aligned-nothrow forms; warmed non-zero process and bypass callbacks report zero allocations and bytes. |
| Continuity metric | VERIFIED | Local pre/post derivative and signal-RMS score covers processor retargeting, passes a smooth crossfade, and rejects an injected click. |
| Performance measurements | VERIFIED | Release-only stereo host-block batches are fully built and warmed before timing; median and robust upper-quantile ratios use represented audio time. |
| Windows VST3 | UNVERIFIED | Full workflow artifact gate is required for each release candidate. |
| macOS VST3 | UNVERIFIED | Full universal workflow artifact gate is required for each release candidate. |
| macOS AU | UNVERIFIED | Universal AU artifact gate runs in CI; AU registration/`auval` remains an external host check. |
| pluginval strictness 10 | UNVERIFIED | Runs only on full Windows/macOS validation. |
| Linux ASan/UBSan | UNVERIFIED | Runs on PR-fast and full Linux validation. |
| Deterministic fixture | VERIFIED | `DynamicReleaseFixture` uses fixed mathematics and a fixed LCG seed only. |
| Real recorded audio | UNVERIFIED | `real_kick.wav` and `real_bass.wav` are private optional inputs and are absent from this repository. |

## Fixture Contract

`tests/DynamicReleaseFixture` creates native-rate audio rather than resampling a
48 kHz recording. It contains four correction-capable repeatable families,
five occurrences per family, one recognizable Global-only family, foreign
material, and ambiguous material. Each event carries ground truth for trigger
sample, semantic family, correction eligibility, and two transport boundaries.
The audio is synthetic production-style material, not recorded audio.

The fixture deliberately uses no wall-clock seed, platform entropy,
`random_device`, unordered container iteration, or pointer-derived order.
State expectations are stable-ID/fingerprint based; MIDI/pitch is optional
metadata and is not used as identity.

## Gates

The `Phase11` category covers fixture formation, cooperative Learn capture,
actual Learn-to-Apply runtime activation, source priority, Global-only and
Candidate routing, distinct Unknown/Ambiguous decisions with bounded Hold,
PDC, persistence, snapshot coherence, transport-boundary resets, localized
continuity controls, callback allocation, lifecycle, and editor construction.
Existing Phase 7-10 suites provide granular branch, measurement, and workspace
coverage used by the release path.

`Performance` is Release-only. It builds reusable stereo host blocks, map
instances, input/output buffers, and warm branch state before timing complete
fixed batches using `steady_clock`; the timed region only feeds pre-generated
audio and invokes processing. It discards warm-up passes and compares median
and robust upper-quantile processing duration to represented audio duration.
The limits are ratios, not machine-specific microsecond budgets: median below
0.75x real time and upper quantile below 1.0x real time. Setting
`KICKLOCK_SKIP_TIMED_ASSERTS=1` leaves finiteness/correctness coverage enabled
and logs that wall-clock assertions were skipped.

The current-thread allocation counter is linked only into test binaries. It is
enabled after all fixture, map, buffer, MIDI, and snapshot storage is prepared;
it records count and requested bytes from ordinary, array, nothrow, aligned,
and aligned-nothrow global allocation forms.

## Known Limitations

- Recorded kick/bass stems are not included. Ordinary builds log
  `REAL_AUDIO_STATUS: UNVERIFIED` and execute no synthetic substitute under the
  RealAudio label. Set `KICKLOCK_REQUIRE_REAL_AUDIO_FIXTURES=1` to make missing
  or undecodable `tests/assets/real_kick.wav` and `tests/assets/real_bass.wav`
  a failure.
- TSAN is an optional local investigation, not a permanent CI job.
- AU build/artifact architecture is CI-gated, but deterministic hosted AU
  registration is not asserted. Run `auval -v aufx Klck Oskl` in a controlled
  signed/installed host environment.
- Code signing, notarization, DAW certification, and human listening review are
  external/manual release checks.

## Release Blockers

A candidate is blocked by a failed Phase11 functional gate, callback allocation,
Release performance ratio, sanitizer failure, pluginval failure, malformed
artifact, missing required real-audio assets when opt-in enforcement is active,
or a failed manual host validation. Missing optional private stems alone are
UNVERIFIED, not a fabricated pass.
