# Dynamic Design Freeze

Dynamic State is a repeatable kick/bass conflict fingerprint cluster. It is not
a MIDI note or DAW timeline section. MIDI and pitch are optional metadata.

- Persistent map capacity: eight states.
- State package: delay delta, allpass frequency, allpass Q.
- Polarity, stage count, crossover, and delay interpolation remain Global.
- Global crossover uses the existing safe `40.0` to `500.0 Hz` core range.
- Dynamic Global Base Delay: `-4.0` to `+17.0 ms`.
- State delay delta: `-3.0` to `+3.0 ms`.
- A source package's delay delta plus its additive Manual delay trim must remain
  within the same `-3.0` to `+3.0 ms` state range.
- Look-ahead invariant: `20 ms + base delay + state delta >= 4 ms fingerprint + 8 ms fade + 1 ms margin`.
- Production design: shared crossover and delay history; Global, eight State,
  and optional Service hot branches. Reset-and-replay is not primary runtime
  architecture.
- Fingerprints preserve sign and use identical Learn/runtime extraction.
- Matching requires absolute threshold and ambiguity margin.
- Hold is bounded, then Global fallback. State fades are linear and 5-8 ms.
- Loop wrap preserves valid runtime continuity.
- Auto candidates require three repeatable hits. Stable Auto states require
  five. Manual State creation requires three recognizable repeatable hits.
- Auto States use a learned package or remain recognized with no confident
  automatic fix; Manual States use a Manual base package. Learned package,
  Manual base package, and Manual trim remain separate.
- DynamicStateMap is the new production contract. KLNoteMap is frozen
  compatibility data, with intentionally preserved legacy selector behavior.
- Legacy per-state Polarity, Stages, and Delay are not precedents for the new
  runtime. There is no KLNoteMap-to-DynamicStateMap conversion.
- Source priority is DynamicStateMap, KLNoteMap compatibility, then no map.
- This phase does not activate the new runtime. Legacy compatibility may be
  removed only in a future major project-format break.

## Phase 4 Package Morphing

- Dynamic Strength morphs frequency in `log2(frequencyHz)` space; Q is never
  linearly interpolated.
- A second-order allpass pole has `radius = sqrt(a2)`, positive pole damping
  `-log(radius)`, and damping morphs in `log(poleDamping)` space.
- Manual damping trim is additive in that same log-pole-damping coordinate.
  Manual delay trim is additive to the State delay before Dynamic Strength.
- Polarity, stage count, crossover, allpass enablement, and delay interpolation
  remain Global. Effective runtime packages are fixed transient values and are
  never serialized.
- Phase 4 defines pure package math only. It does not activate processor
  runtime, hot-branch rendering, scheduling, or DSP integration.

## Phase 5 Hot-Branch Engine

- The input is split once by one Global Linkwitz-Riley crossover.
- Low-band input is written once into one shared circular history per channel.
- Global, eight State, and one optional Service branch each read a separate
  fractional delay tap from that shared history.
- Persistent State branches process every sample continuously; they do not
  start only when selected. Reset-and-replay is not their normal path.
- All four second-order allpass stages remain warm on every active branch.
- High band is delayed once by the reported 20 ms PDC path and is added only
  after later branch selection; it is not duplicated per branch.
- Maximum Service priming from shared history is 300 ms. Priming is explicit
  and never runs inside the ordinary process loop.
- Phase 5 does not implement State selection, Hold, crossfades, transport
  policy, or PluginProcessor integration.

## Phase 6 Selector Scheduler and Continuity Mixer

- Matcher observations are acted on at their absolute `readySample`, never at
  a block-relative offset; the scheduler is advanced exactly one absolute
  sample at a time so block partitioning cannot change results.
- A required branch transition begins fading at the exact `readySample` of
  the event that requested it.
- Transition duration is derived per sample rate from the fixed 8 ms maximum,
  the actual `DynamicFingerprintWindow` sample count, and at least 1 ms of
  sample-domain safety margin, and is validated against the frozen 13 ms
  minimum physical branch tap at every supported rate.
- Target identity is always `stableStateId`, matched against the live branch
  roster; slot position alone is never identity, and a slot silently
  replaced with a different stable State collapses the selection safely to
  Global rather than routing the new occupant's audio under the old gain.
- A warm, active persistent State branch has priority over a warm Service
  branch bound to the same stable identity; Service is only selectable when
  explicitly and validly bound to that identity.
- Ambiguous/Unknown Hold is bounded to at most two unresolved events and
  250 ms from the last confidently corrected match; InvalidInput and
  NoEligibleStates never Hold.
- Continuity uses one fixed ten-position low-band gain vector (Global, eight
  State slots, Service). Every transition snapshots the exact current gain
  vector as the fade start and a one-hot target, so retargeting mid-fade
  never introduces a discontinuity or an unbounded nested fade structure.
- The common high band is added exactly once per sample after the weighted
  low-band mix; it is never crossfaded or duplicated per branch.
- An explicit transport discontinuity (seek, loop wrap, stop/start, host
  reset) clears queued events, Hold, and any active fade, and snaps gains to
  Global; it does not reset Phase-5 branch DSP state, and a non-contiguous
  block without an explicit reset is rejected rather than reinterpreted.
- Phase 6 is standalone: it does not extract fingerprints, duplicate matcher
  distance logic, compute or configure State/Service packages, or connect to
  PluginProcessor. That connection is Phase 7's responsibility.

## Phase 7 Production Runtime Integration

Phase 7 activates the DynamicStateMap runtime in the product audio path via
`DynamicProductionRuntime` (a thin coordinator over the frozen Phase 1-6
components); `PluginProcessor` stays a thin integration layer.

- Source priority uses `resolveDynamicMapSource()`. In `correction_mode ==
  Static` the existing MultibandPhaseCore path runs unchanged and Dynamic
  arbitration is bypassed entirely. In `correction_mode == Dynamic` priority is:
  runtime-eligible New DynamicStateMap → new production runtime; else usable
  legacy KLNoteMap → exact existing `selectDynamicRuntime()` + MultibandPhaseCore;
  else a deterministic latency-compensated base/Global fallback with no
  fabricated State selection. KLNoteMap is never converted to a DynamicStateMap
  and legacy compatibility is unchanged.
- New-map Global Base ownership: when New is the active source, the map's
  `DynamicGlobalBase` is the runtime source of truth for base delay, polarity,
  crossover enable/frequency, allpass enable/frequency/Q, stage count and delay
  interpolation. Static APVTS values are never substituted for those fields. The
  only live parameters controlling the New runtime are `correction_mode` and
  `dynamic_strength`. No APVTS parameter is written from the audio thread.
- Map ownership/publication: `messageOwnedDynamicStateMap` (message thread,
  guarded by the existing map mutex) and `activeDynamicStateMap` (audio thread).
  Publication is an allocation-free SPSC `DynamicStateMapUpdateQueue` modelled on
  `NoteMapUpdateQueue`; the audio thread drains it at the block boundary, newest
  complete update wins, and a malformed map activates as a complete empty map.
  The audio thread never locks the map mutex.
- Persistence: `getStateInformation()` removes any existing KLDynamicStateMap
  child and appends exactly one `dynamicStateMapToValueTree(messageOwned…)`
  alongside the independently-persisted KLNoteMap; `setStateInformation()`
  locates KLDynamicStateMap independently, parses it through
  `dynamicStateMapFromValueTree()` (missing or malformed → empty, never a prior
  project's map), and republishes through the RT-safe queue.
- Raw train/serve fingerprint wiring: the canonical raw mono-compatible bass and
  kick-sidechain pair (before correction, not user-crossover filtered, not
  double-filtered through the raw low-pass) is fed to the Phase-3
  `DynamicFingerprintCaptureBank`. A dedicated runtime kick trigger detector,
  separate from the scope/punch/Learn/legacy detectors, drives capture requests.
  The legacy `RuntimeConflictFingerprintCapture` is unchanged and serves only the
  legacy path.
- Sample-accurate production order per bounded chunk: snapshot raw bass/kick →
  trigger/`requestCapture()` (with the trigger sample inside the window) →
  `pushSample()` → drain completed observations → `matchDynamicFingerprint()` →
  submit a `DynamicSelectorEvent` preserving absolute trigger/ready samples →
  configure packages when the map generation / Strength / rate changed → process
  the original bass through the hot-branch engine → build the roster → render
  through the continuity mixer, advancing capture and scheduler to the same
  internal sample boundary. Results are invariant across process-block
  partitions (1, 7, 64, 127, 512, prepared maximum). No `AudioBuffer::setSize`
  in the callback.
- Service cold-branch policy: an explicit semantic binding
  (`serviceBoundStableStateId`, `serviceBindingValid`). Priority is warm
  persistent State → warm Service bound to the same stable ID → Global. Service
  is configured from the same Phase-4 package, explicitly primed from shared
  history at the chunk boundary (never per sample), and selectable only when
  warm; a warm persistent branch always wins and the binding is cleared on
  map/source/reset invalidation. Ambiguous/Unknown never bind Service.
- Internal monotonic scheduler timeline: the capture bank and scheduler share an
  internal monotonic sample position; the looping host timeline is never the
  scheduler's absolute time. The host position only classifies transport.
- Loop/seek/stop policy: a valid loop wrap while the host keeps playing
  preserves runtime continuity (it can never become an int64 scheduler rewind);
  a seek/jump or stop→start clears captures, events, Hold, the Service binding
  and stale delay/history and returns the scheduler to Global; prepare/host reset
  fully resets. Missing playhead fields never cause repeated resets.
- Bypass policy: shadow advance. Under host bypass the New runtime is advanced
  with the real input but its corrective output is discarded, preserving
  hot-branch and timestamp continuity so unbypass replays no stale history. The
  audible bypass output remains the fixed 20 ms delayed dry path and diagnostic
  observation stays active.
- Latency: every path reports and produces exactly the existing 20 ms PDC
  latency; the New runtime introduces no second latency layer, and the common
  high band is added exactly once by the continuity mixer.
- Scope: Phase 7 does not add New Learn clustering/State formation, Phase-9
  measurements, DynamicWorkspace UI, an APVTS redesign, or any KLNoteMap
  conversion.

## Phase 8 Dynamic Learn Formation

- Learn forms Dynamic States from repeatable signed Phase-3 conflict
  fingerprints, never MIDI notes, pitch buckets, or timeline regions. Pitch and
  MIDI remain optional metadata only.
- The worker replays the complete canonical raw mono bass/kick Learn timeline
  through one continuous `DynamicFingerprintFrontEnd` from the Learn reset
  boundary. It uses the same raw kick detector configuration as Phase 7 and
  includes the trigger sample in every exact 4 ms capture window.
- Three repeatable members form an Auto Candidate; five form an Auto Stable
  State. Candidates are recognizable but route to Global until Stable evidence
  makes correction eligible.
- A recognizable cluster may have no confident package. It remains matchable,
  has zero trim, and resolves as `GlobalRecognizedNoCorrection`.
- Formation uses deterministic normalized-L1 clustering, robust component-wise
  median prototypes, cohesion/correction repeatability, overlap ambiguity, and
  matcher calibration. The map retains at most eight total States.
- Global Base delay is selected from bounded candidate targets to maximize
  weighted representable State coverage. Polarity and stage count remain Global;
  State packages contain only delay delta, F, and Q and are never silently
  clamped into the State range.
- Existing Manual States and IDs/packages/trims are retained. New Auto clusters
  reconcile one-to-one by confident fingerprint identity; otherwise IDs advance
  monotonically from `nextStateId` and are not recycled.
- Apply validates and publishes the New map through the Phase-7 SPSC queue at
  the next audio boundary. Revert restores both DynamicStateMap and legacy
  compatibility map exactly.
- Phase 8 adds no Phase-9 measurements and no DynamicWorkspace UI.

## Phase 9 State Measurements and Snapshots

- Two distinct, never-conflated measurement layers per Dynamic State:
  Predicted (Learn-worker, offline, from retained member windows) and
  Verified (runtime, fresh audible New Dynamic output only). Predicted is
  never labelled as live verification; Verified is Unavailable/Collecting
  until enough fresh matching runtime material exists.
- One canonical score: `MultiBandCorrelation::weightedMatchPercent`, the same
  low-end-weighted ruler already driving Learn/Analyze/Apply. Predicted
  after-scores are rendered through the exact resolved
  `DynamicPackageResolution` (delay, polarity, allpass coefficients/stages,
  crossover) via `DynamicMeasurementScorer`, reusing
  `DynamicHotBranchEngine::processAllpassReference()` for the allpass
  cascade; Verified after-scores are computed directly from the actual
  captured audible output, never re-rendered.
- Retained Learn windows: deterministically selected (central + spread
  members, independent of input encounter order) by re-matching each
  originally-captured hit window against the just-formed map through the
  frozen Phase-3 matcher; bounded to 8 States x 12 windows; temporary
  worker-owned evidence, discarded once predicted summaries are computed.
- Final-package cluster verification: an Auto Stable State's `hasLearnedPackage`
  is retained only when the exact Strength-1 final package, rendered against
  every retained window, clears a centralized median-improvement / majority-
  improved / bounded-worst-regression / confidence gate. A State that fails
  is demoted (package cleared, trim reset to zero) but stays occupied and
  matchable, resolving through the existing `GlobalRecognizedNoCorrection`
  path. Manual States are never demoted; Candidate States are measured but
  never become correction-eligible from measurement alone.
- Runtime verified measurement: `DynamicRuntimeMeasurementCapture` buffers, per
  physical kick hit, the canonical pre-correction bass/kick pair and the
  actual audible processed-bass / PDC-aligned-kick pair, aligned by one fixed
  integer sample offset (the branch's physical tap) so both windows describe
  the same hit. A capture is only handed to the worker once the target
  identity is confirmed settled (not mid-fade, not Held, not stale) at
  completion time. Verification requires New DynamicStateMap active, a
  confident Matched correction, sidechain present, and audible output (never
  bypass or a different branch/identity).
- Bounded lock-free queues: fixed SPSC (juce::AbstractFifo-based, matching the
  existing DynamicStateMapUpdateQueue pattern) queues carry captures
  audio -> worker and scored results worker -> audio; overflow drops and
  increments a fixed diagnostic, never blocks audio or overwrites an
  in-flight publish. A dedicated measurement worker thread (never the audio
  thread) performs the actual scoring.
- Verified rolling aggregation: a fixed per-State ring (16 most recent
  events) reconciled by stableStateId AND map generation, requiring at least
  three fresh events before reporting VerifiedImprovement/Neutral/Unstable/
  Regressed; a stale-generation result can never update the wrong State.
- Fixed UI-ready snapshot: `DynamicRuntimeSnapshot`, a self-contained,
  string-free, vector-free, pointer-free value with 8 fixed State cards,
  published at most once per `process()`/`processBlockBypassed()` call
  through a 4-way tear-free buffer (`DynamicSnapshotPublisher`). Test-only
  accessors: `getDynamicRuntimeSnapshotForTesting()`,
  `getDynamicPredictedMeasurementForTesting()`,
  `getDynamicVerifiedMeasurementForTesting()`,
  `getDynamicMeasurementDiagnosticsForTesting()`.
- Reset/invalidation: a new map generation reconciles predicted/verified data
  by stableStateId; source change away from New Dynamic marks verification
  inactive without discarding history; sidechain loss and transport
  discontinuities discard in-flight captures; bypass output is never counted
  as verified correction.
- Measurements are non-persistent sidecar/runtime data: no map schema change,
  no raw audio, and no runtime verification history is ever serialized.
  `setStateInformation()` always starts predicted/verified Unavailable.
- Phase 9 does not implement DynamicWorkspace or change the visible editor.

## Phase 10 DynamicWorkspace

- `DynamicWorkspace` is presentation-only. The editor is the sole production
  message-thread reader of `DynamicSnapshotPublisher`, reads one Phase-9
  `DynamicRuntimeSnapshot` per Dynamic UI update, and passes that stored value
  to the workspace and its eight persistent card components.
- The workspace pre-creates exactly eight State cards. `stableStateId` is the
  only card/detail identity; slot remains visible as secondary placement data.
  MIDI and pitch are optional display metadata, never identity.
- Pending ResultReady New Dynamic Learn data is read as one coherent
  message-thread preview value (session, map, predicted sidecar, validity and
  Apply state). It is explicitly marked preview/not applied, never active, and
  always shows Verified as unavailable. Applying or reverting never fabricates
  local runtime cards; the next published runtime snapshot is authoritative.
- Applied New-map cards come only from `DynamicRuntimeSnapshot`. Predicted
  Learn evidence and fresh audible Verified evidence remain separate. Legacy
  compatibility is labelled `LEGACY MAP` and never creates New State cards or
  Phase-9 measurements; no-map remains `NO MAP`.
- Selected semantic identity and active audible identity are separate. The
  header presents Global, State, Service, Hold, fallback, bypass, and sidechain
  status from the published snapshot. Snapshot sequence gaps are latest-state
  behavior, never an event log.
- Static retains its existing scope, manual controls, analyzer and workflow.
  Dynamic replaces that lower manual/analyzer region with DynamicWorkspace;
  Dynamic Strength is the sole existing live Dynamic control and is safely
  laid out in its header without another APVTS attachment. Clean Scope hides presentation
  only and preserves processor-owned Learn/map state and UI detail selection.
- Phase 10 changes no DSP, map schema, fingerprint/matching formulas, Learn
  formation, source priority, persistence format, service behavior, latency, or
  audio-thread contract.

This document freezes architecture only. Commit 1 adds persistent
DynamicStateMap v1 contract and serialization. It does not activate runtime,
Learn, DSP, transport, UI, or legacy compatibility behavior.
