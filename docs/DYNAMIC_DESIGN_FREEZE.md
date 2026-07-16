# Dynamic Design Freeze

Dynamic State is a repeatable kick/bass conflict fingerprint cluster. It is not
a MIDI note or DAW timeline section. MIDI and pitch are optional metadata.

- Persistent map capacity: eight states.
- State package: delay delta, allpass frequency, allpass Q.
- Polarity, stage count, crossover, and delay interpolation remain Global.
- Global crossover uses the existing safe `40.0` to `500.0 Hz` core range.
- Dynamic Global Base Delay: `-4.0` to `+17.0 ms`.
- State delay delta: `-3.0` to `+3.0 ms`.
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
- Priority: DynamicStateMap, legacy Dynamic compatibility map, No Map.

This document freezes architecture only. Commit 1 adds persistent
DynamicStateMap v1 contract and serialization. It does not activate runtime,
Learn, DSP, transport, UI, or legacy compatibility behavior.
