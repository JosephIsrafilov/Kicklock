# Changelog

All notable changes to KickLock are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

## [0.2.1]

### Fixed
- **macOS build compatibility.** Use portable synchronized access
  for the Learn worker session and correct a const Timer test helper, enabling
  Release VST3/AU builds on Xcode.

## [0.2.0]

### Added
- **Static / Dynamic workflow UI.** A host-automatable Static/Dynamic selector,
  Dynamic Strength control, Learn progress, per-note chips, runtime status, and
  Apply Learn / Discard / Clear Map actions now use the existing processor
  state machine and map APIs.
- **Per-note Learn maps.** Learn captures a pending map, requires explicit
  Apply, persists the applied map, and supports map-aware Revert and Clear Map.
- **Release QA matrix.** `docs/QA_MATRIX.md` records automated coverage and
  manual DAW follow-ups without claiming host checks that were not run.

### Changed
- **Lifecycle and CI hardening.** Timer callback teardown and blocking worker
  completion paths are covered by deterministic lifecycle tests; CI continues
  to run Windows/macOS pluginval strictness 10 and Linux ASan/UBSan tests.
- **Compatibility.** Sessions without Dynamic parameters or a note map restore
  as Static with no map.

### Phase -1 — Static stabilization (pre-feature baseline)

Brings the current Static path to an honest, frozen baseline before the
Static/Dynamic work begins. No new user-facing controls are added.

#### Changed
- **Removed hidden high-band processing.** The undocumented, sidechain-reactive
  `DynamicTransientEQ` that boosted ~3.5 kHz on the high band by up to +10 dB has
  been removed from `MultibandPhaseCore`. The high band is now a plain
  latency-compensated passthrough, matching the intended architecture
  (low band corrected, high band compensated). This is the only audible change
  in this release and it only affects material where a kick sidechain was
  present with the crossover engaged.
- **Completed A/B compare-slot serialization.** Crossover enable, crossover
  frequency, delay interpolation and Pitch Follow are now persisted per compare
  slot, so no `ParameterSnapshot` field is lost across save → reload → A/B
  switch. Older projects that predate these fields load unchanged.

#### Added
- **`RevertBundle` foundation.** Revert now routes through a single rollback
  bundle captured once by `ensureRevertBundleCaptured()`, laying the groundwork
  for map-aware Revert in later phases. Existing Analyze → Apply → Revert
  behavior is unchanged.
- **Static stabilization tests** (`tests/DynamicModeTests.cpp`): high-band
  transparency (T0), a deterministic Static golden baseline with block-size and
  sample-rate coverage (T1), and full parameter / A-B serialization completeness
  (T2).
- **CI validation gate.** GitHub Actions now builds the tests, runs `ctest`
  (including pluginval strictness 10 on Windows and macOS), and adds a Linux
  ASan/UBSan job. The nightly release is gated on all build/test/validation jobs.

[Unreleased]: https://github.com/JosephIsrafilov/Kicklock/compare/main...HEAD
