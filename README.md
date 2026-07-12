# KickLock

KickLock is an open-source JUCE plugin for visually aligning kick and bass phase.
It processes the bass on the main input, reads the kick from the sidechain, and
shows a trigger-locked oscilloscope plus a live low-end match score.

![Routing GIF placeholder](docs/routing-placeholder.gif)

## What It Does

- Shows kick-triggered bass and kick waveforms so each hit stands still on the scope.
- Measures a multi-band, low-end-weighted phase match from 20 Hz to 500 Hz.
- Lets you adjust bass delay, polarity, and an allpass phase filter by hand.
- Can Analyze recent kick hits and recommend delay, polarity, frequency, Q, and stage settings.
- Supports Static correction and Dynamic learned per-note phase maps.
- Supports A/B compare slots, Revert after Apply Fix, and four factory presets.

## Downloads

You can download the latest automatically built plugins (VST3 and AU) from the Nightly Release:
- [⬇️ Download Latest Nightly Build](https://github.com/JosephIsrafilov/Kicklock/releases/tag/nightly)

*(Artifacts are automatically uploaded to this release on every push to the main branch)*

## Routing

KickLock has two inputs:

1. Put KickLock on the bass track or bass bus. This is the audio it processes.
2. Route the kick to KickLock's sidechain input. The kick is only a reference.

DAW notes:

- Ableton Live: Drop KickLock on bass, open the device sidechain chooser, select the kick track.
- FL Studio: Put KickLock on the bass mixer insert, route the kick insert with "Sidechain to this track", then select that input in the wrapper.
- Logic Pro: Insert KickLock on bass, use the plugin sidechain menu, choose the kick track or bus.
- Cubase: Insert KickLock on bass, enable the plugin sidechain, send the kick channel to it.
- Reaper: Put KickLock on bass, route kick channels 1/2 to bass channels 3/4.

## Workflow

1. Play a loop with bass on the main input and kick on the sidechain.
2. Use the Triggered scope to watch the kick and bass around each hit.
3. Drag horizontally on the scope, or use the Delay control, to nudge bass timing.
4. Try Invert Polarity if the low end is cancelling.
5. Use the Phase Filter for frequency-local phase conflicts that delay cannot fix cleanly.
6. Press Analyze after a few hits, review the recommendation, then Apply Fix if it is useful.
7. Use Revert to restore the settings that were active before Apply Fix.

### Static

Static is the default for new and existing projects. Route the kick to the
sidechain, play a representative loop, press **Analyze**, review the proposed
fix, then press **Apply Fix** only if it is useful. Revert restores the state
from before the first applied fix.

### Dynamic

1. Select **Dynamic** and play a representative bassline with the kick routed
   to the sidechain.
2. Press **Learn**, then **Stop Learn** after capturing enough stable hits.
3. Review the learned note chips and summary. Nothing has changed yet.
4. Press **Apply Learn** to activate the global correction and per-note map, or
   **Discard** to leave the current sound unchanged.

Dynamic Strength blends from the learned global correction at 0% to the full
per-note correction at 100%. Pitch Follow is ignored in Dynamic mode but its
saved value is not changed.

Dynamic status labels are explicit: **NO MAP** means no applied map exists,
**FALLBACK** means the current note uses the learned global correction,
**MAP STALE - RE-LEARN** means the learned base context no longer matches, and
**PHASE FILTER OFF** means the map is retained but inaudible. **Clear Map**
removes the applied map without changing manual parameters; **Revert** restores
the previous map when available.

Older projects load as Static with no map.

## Controls

- Delay: signed bass timing offset from -20 ms to +20 ms. Negative values use host PDC headroom to advance the bass.
- Polarity: flips bass polarity by 180 degrees.
- Phase Filter: allpass phase rotation around the selected frequency.
- Q / Stages: shape and depth of the phase rotation.
- A / B: two processor-side compare slots for bass-path settings.
- Copy: copies the active A/B slot into the other slot.
- Details: expands or collapses the live weighted, low-end, broadband, and per-band match meters.

## Factory Presets

- Tight EDM
- Deep House Sub
- Trap 808
- Neutral

The presets are starting points for the bass-path controls. They do not replace
manual listening or the Analyze workflow.

## KickLock vs. ReVision

| Area | KickLock | ReVision |
| --- | --- | --- |
| License | GPL-3.0 open source | Commercial proprietary |
| Core workflow | Triggered scope, manual controls, Analyze assistant | Commercial kick/bass phase alignment workflow |
| Bass processing | Delay, polarity, allpass phase filter | Product-specific processing |
| Scope | Trigger-locked hit display with ghost hits | Triggered visual alignment focus |
| Cost | Free/open source | Paid |

KickLock is an original implementation. It does not use code, assets, presets,
or proprietary behavior from ReVision or any other commercial plugin.

## Building

Requirements:

- CMake 3.22 or newer
- A C++20 compiler
- Network access for the first configure so CMake can fetch JUCE 8

Configure and build:

```sh
cmake -B build
cmake --build build --config Release
```

Build and run tests:

```sh
cmake --build build --target KickLockDspTests --config Debug
./build/tests/Debug/KickLockDspTests
```

On Windows with Visual Studio's bundled CMake, use the generated build directory
and run the test executable directly if `ctest` is not on PATH.

## Validation

The intended release gate is:

```sh
pluginval --strictness-level 10 path/to/KickLock.vst3
```

## License

KickLock is licensed under GPL-3.0. The project uses JUCE under GPL-compatible
terms; distributing non-GPL builds requires satisfying JUCE's licensing terms.
