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

This document freezes architecture only. Commit 1 adds persistent
DynamicStateMap v1 contract and serialization. It does not activate runtime,
Learn, DSP, transport, UI, or legacy compatibility behavior.
