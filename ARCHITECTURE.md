# KickLock — Architecture (current, authoritative)

This file supersedes the M1–M4 plan in CLAUDE.md/AGENTS.md, which describe an
older, simpler architecture. Trust this file and the code.

## What the plugin does

Bass on the main input, kick on the sidechain. A live multi-band match
percentage shows how well their low-end phase lines up. The user can fix
misalignment manually (polarity flip, ± delay, allpass phase rotator) or click
Analyze → Apply Fix for an assisted correction. Original clean-room
implementation of the phase-alignment-tool category; no third-party branding.

## Live analysis path (the only one wired to the UI)

UI (`PluginEditor.cpp`) calls exactly four processor entry points for
analysis, driven by a 12 Hz poll of an `AnalyzeState` state machine:

1. `beginBackgroundAnalyze()` — snapshots `rawCapture` on the message thread,
   dispatches a `juce::ThreadPool` job. States: Idle → Preparing → Analyzing →
   ResultReady / NotEnoughMaterial / Failed.
2. `getAnalyzeState()` / `acknowledgeAnalyzeState()` — UI polling handshake.
3. `applyLatestFix()` — applies the published `PhaseFixResult` to APVTS params
   under `resultMutex`, gated by `applyAllowed` / `optionalApplyAllowed`.

The analysis job runs `computeAndPublishFix()` (PluginProcessor.cpp):
- `extractRecentHitWindows()` finds up to 8 recent kick-hit windows in the
  raw capture (transient-based).
- `analyzeAggregatedHits()` runs `PhaseFixEngine::analyze()` per hit window,
  then aggregates: median delay, majority polarity, phase-filter consensus,
  and stability classification (`unstableRecommendation`).
- Results are scored before/after via `PhaseFixEngine::scoreSettings()` and
  published under `resultMutex` + atomics for the UI.

### Aggregation stability semantics (bug fixed 2026-07, do not regress)

"Stable" means the hits **agree with each other**, in either direction —
consensus on "needs a fix" OR consensus on "already fine". An earlier version
required a 60% majority to *need* a correction, which misclassified perfectly
aligned loops (0% need a fix = perfect agreement) as Unstable and blocked
Apply Fix. Current rule in `analyzeAggregatedHits()`:
`improvementStable = validHits <= 1 || share >= 0.60 || share <= 0.40`.

### DSP components (src/dsp/, all header-only, realtime-safe)

| Component | Role | Thread |
|---|---|---|
| `PhaseFixEngine` | Grid search over delay/polarity/rotator settings; scores match %; quality classification and apply gates | message/worker |
| `MultiBandCorrelation` | Multi-band phase-match scoring used by the engine | message/worker |
| `AlignmentAnalyzer` | FFT cross-correlation aligner; called by `PhaseFixEngine::analyzeCore()` as an unconstrained cross-check for large timing offsets | message/worker |
| `RealtimeMultiBandMeter` | Live match % (weighted/low-end/broadband) fed from processBlock | audio |
| `TransientDetector`, `SignalActivityTracker` | Kick-hit detection, signal presence | audio |
| `FractionalDelayLine`, `AllpassPhaseRotator` | Correction DSP | audio |
| `RawCaptureBuffer`, `HitCaptureBuffer` (util/) | Lock-free rolling capture of raw pre-processing audio | audio write / message snapshot |

### Quality gates (PhaseFixEngine)

`classifyQuality()` → NotEnoughSignal / Unstable / LargeTimingOffset /
TimelineMoveRequired / AlreadyGood / StrongImprovement / PartialImprovement /
NoUsefulChange. `applyAllowed` requires Strong/Partial; `optionalApplyAllowed`
adds Partial and AlreadyGood+phase-filter. Thresholds:
`alreadyGoodThreshold=85`, `strongAfterThreshold=75`,
`usefulImprovementThreshold=5`, `partialImprovementThreshold=8`.
Never loosen these gates to "make the button work" — fix the classification
that misroutes legitimate results instead.

## Legacy path — REMOVED (decision 2026-07-04)

`PerHitAnalyzer`, `AnalyzerInstructionBuilder`/`AnalyzerInstruction`,
`KickLockAudioProcessor::analyzeAndApply()`, `analyzeLatestHit()`, and
`HitAnalysisHistory` were an earlier single-hit analysis path that the UI no
longer reached (verified by grep of PluginEditor.cpp). They disagreed with the
live PhaseFixEngine path and were deleted rather than left as a second
"brain". `AlignmentAnalyzer` stays: it is a live dependency of
`PhaseFixEngine::analyzeCore()`.

## Realtime constraints (binding on every change)

- `processBlock()` and every `processSample()`/`pushSample()` in src/dsp/ and
  src/util/: no allocation, no locks, no logging, no unbounded work.
- Allocation only in `prepare()`/`prepareToPlay()`, message thread, or the
  analysis worker.
- `setInterpolationType` / rotator `prepare()` only on actual value change
  (they reset internal state).

## Build & test

- JUCE 8.0.13, C++20, CMake + FetchContent, VS 2026 generator on Windows.
- Tests: `tests/DspTests.cpp`, JUCE UnitTest console target
  (`KickLockDspTests`). Build with MSBuild on the generated .vcxproj; run the
  exe directly. The user's real-world loop is Windows + Bitwig.
