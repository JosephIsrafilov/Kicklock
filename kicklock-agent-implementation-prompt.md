# KickLock — Implementation Task for Claude Code

## How to use this file

Run this from the root of the KickLock JUCE project (the folder containing `src/`). Paste the whole task below as your first message to Claude Code, or save it as `TASK.md` in the repo and say "follow TASK.md phase by phase." If you've installed the validator plugin, do it before starting:

```
/plugin marketplace add iPlug3/audio-plugin-dev-skills
/plugin install audio-plugin-validators@audio-plugin-dev-skills
```

---

## Context

KickLock is a JUCE VST3/AU plugin for visual phase alignment between a kick and a bass/sub-bass sidechain (competitor reference: ReVision). Source layout: `src/PluginProcessor.{h,cpp}`, `src/PluginEditor.{h,cpp}`, `src/dsp/*.h`, `src/ui/*.{h,cpp}`, `src/util/*.h`.

Two problems are being fixed and one feature removed:

1. **Remove** the "dynamic transient EQ" section entirely — the four knobs `Trans Amt / EQ Freq / EQ Q / EQ Boost`. It's a sidechain-triggered high-band exciter with no bearing on kick/bass phase alignment, and it must go cleanly without breaking anything else (Analyze, Apply Fix, Revert, Compare A/B, presets, trigger mode).
2. **Replace** the "Transient Health" readout, which is currently broken/meaningless (it measures that same EQ's own gain, so it reads a flat number no matter what), with a real kick-punch integrity meter plus a "Set Ref" button for before/after comparison.
3. **Verify and finish** the trigger-mode kick lock (kick waveform must freeze after capture; only the bass trace updates on manual adjustments) — analysis shows this is *already mostly implemented*; the job here is closing one real gap, not rebuilding it.
4. **Widen** the resizable scope area so the oscilloscope actually grows when the window is resized larger, instead of hitting an internal cap.

---

## Ground rules

- Work through the phases **in order**. After each phase: build the plugin, run the smoke test listed for that phase, `git add -A && git commit -m "<phase name>"`, then move on.
- **Do not proceed to the next phase if the build fails or a smoke test fails.** Stop, fix, re-verify, then continue.
- If `audio-plugin-validators` (or `pluginval`) is available, run it after Phase 1 and again after Phase 5 against the built VST3/AU. If it flags anything, fix before moving on.
- Stay inside the scope of each phase. Don't refactor, rename, or "clean up" code that isn't listed. Match the existing code style (JUCE conventions already used in the file you're editing).
- Don't change any parameter ID that isn't explicitly listed for removal — everything else must keep its exact ID string and range so old saved sessions still restore correctly.

---

## Phase 0 — Baseline

Confirm the project currently builds and loads in a host with a kick→bass sidechain routed. Commit as `chore: baseline before KickLock overhaul` before touching anything.

---

## Phase 1 — Remove the Dynamic Transient EQ and its broken meter

These two are removed together because the meter only ever measured this EQ's own gain.

**`src/dsp/MultibandPhaseCore.h`**
- Remove members `dynEq` (`DynamicHighBandEQ`), `envelope` (`TransientEnvelopeFollower`), `health` (`TransientHealthMeter`), and the `getHealthMeter()` accessor.
- Remove these 8 fields from `Params`: `dynEqFreqHz, dynEqQ, dynEqMaxBoostDb, dynEqAmount, dynEqAttackMs, dynEqHoldMs, dynEqReleaseMs, dynEqTriggerRatio`.
- In `processChunk`, delete the envelope/dynamic-EQ processing (`envelope.processSample`, `updateDynamicEq`, `dynEq.processSample`) and the `prePeak`/`postPeak`/`health.pushBlock` bookkeeping. The high band becomes a plain passthrough of `highDelay.popSample(...)` — no EQ stage, latency/PDC math is unaffected.

**Delete these files** (confirmed unused anywhere else): `src/dsp/DynamicHighBandEQ.h`, `src/dsp/TransientEnvelopeFollower.h`, `src/dsp/TransientHealthMeter.h`.

**`src/PluginProcessor.h`**
- Remove the 8 raw pointer members: `dynEqFreqParam, dynEqQParam, dynEqBoostDbParam, dynEqAmountParam, dynEqAttackMsParam, dynEqHoldMsParam, dynEqReleaseMsParam, dynEqTriggerRatioParam`.
- Remove `transientHealthDb`, `transientPrePeak`, `transientPostPeak` atomics and their getters (`getTransientHealthDb`, `getTransientPrePeak`, `getTransientPostPeak`).

**`src/PluginProcessor.cpp`**
- `createParameterLayout()`: delete all 8 `juce::ParameterID{"dyneq_*", 1}` blocks: `dyneq_freq, dyneq_q, dyneq_boost_db, dyneq_amount, dyneq_attack_ms, dyneq_hold_ms, dyneq_release_ms, dyneq_trigger_ratio`.
- Constructor: delete the 8 `dynEq*Param = apvts.getRawParameterValue(...)` lines. (Confirmed: none of these 8 IDs appear in the `addParameterListener` loop or in `parameterChanged()`, so nothing there needs touching.)
- `processBlock()`: delete the `coreParams.dynEq* = ...` assignment block, and the 3 lines storing `transientHealthDb/transientPrePeak/transientPostPeak` from `multibandCore.getHealthMeter()`.
- `isBassProcessingNeutral()`: remove the `dynEqAmount` local and its clause from the `neutral` check.

**`src/PluginEditor.h` / `src/PluginEditor.cpp`**
- Remove: `dynEqAmountSlider/dynEqFreqSlider/dynEqQSlider/dynEqBoostSlider`, their 4 labels, their 4 `SliderAttachment`s, `refreshTransientEqControlState()` (declaration, body, and its call in `timerCallback()`), and the `transientRow` layout block inside `resized()`.
- Leave the `TransientHealthComponent` class/member in place structurally — Phase 2 rewrites its internals, don't delete it now.

**Backward compatibility:** old saved sessions containing these 8 parameter IDs will simply have them ignored on load (APVTS does not recreate unknown parameter IDs — this does not crash). If a host had automation drawn on `Trans Amt` etc., that lane becomes inert after the update; this is expected and does not need a migration path.

**Smoke test:** build succeeds; the 4 knobs and their row are gone with no leftover blank gap or crash; Live Match %, Analyze, Apply Fix, Revert, Compare A/B, and Trigger mode all behave exactly as before.

**Commit:** `refactor: remove dynamic transient EQ and its broken health meter`

---

## Phase 2 — Kick-Punch Transient Integrity meter + Set Ref button

**New file `src/dsp/TransientPunchMeter.h`:**

```
prepare(sampleRate, windowMs = 40)
reset()
pushSample(kickLow, bassLow, transientDetected)   // gated to the kick's own transient flag
getPunchDb() const
getKickPunch() const
getSumPunch() const
isValid() const
```

Behavior: on each kick transient, open a ~40 ms window and track `kickPeak = max(|kickLow|)` and `sumPeak = max(|kickLow + bassLow|)` over that window. On window close, compute:

```
PUNCH_dB = 20 * log10( (sumPeak + eps) / (kickPeak + eps) )
```

`PUNCH_dB > 0` → the bass is reinforcing the kick's low end. `< 0` → the bass is cancelling it (the "hollow" low end). `≈ 0` → neutral. Smooth hit-to-hit with a light EMA (e.g. alpha ≈ 0.35) so the reading doesn't jitter. Gate out kick peaks below a small floor (~-80 dBFS) so silence doesn't produce garbage. If no kick transient has been seen for ~1.5 s, `isValid()` should return false so the UI can show a neutral placeholder instead of a stale number. Handle fast kick patterns by finalizing the current window early if a new transient arrives before the window closes, so nothing double-counts (this sits comfortably inside the existing kick-transient-detector's ~90 ms holdoff, so it won't normally trigger anyway).

**Wire it in `src/PluginProcessor.cpp`**, inside `processBlock()`, at the existing site where `processedBassLow`, `alignedKickLow`, and `transientDetected` are already computed together (right next to the existing `hitCapture.pushSample(mainMono, meteredSidechainMono, transientDetected);` call):

```
transientPunchMeter.pushSample (alignedKickLow, processedBassLow, transientDetected);
```

`alignedKickLow` is already latency-compensated and untouched by Delay/Polarity/Phase Filter (those only affect the bass), and `processedBassLow` is already the bass *after* those controls — so no new per-sample loop is needed, and the reading moves live the instant Delay/Polarity/Phase Filter change.

**`PluginProcessor.h` / `.cpp` additions:**
- Member: `TransientPunchMeter transientPunchMeter;`
- Getters: `getTransientPunchDb()`, `isTransientPunchValid()`.
- Reference feature: `std::atomic<float> transientPunchReferenceDb{0.0f}; std::atomic<bool> transientPunchReferenceSet{false};` with plain (non-APVTS, message-thread-only) methods `setTransientPunchReference()` (stores current `getPunchDb()` and flips the flag on) and `clearTransientPunchReference()` (flips the flag off) — same pattern already used for Analyze/Apply Fix/Revert, no host automation needed for this.
- `prepareToPlay()`: call `transientPunchMeter.prepare(sampleRate)` and reset the reference atomics.

**Editor — rebuild the indicator.** Rename `TransientHealthComponent` → `TransientPunchComponent` (semantics fully changed, worth the clean rename) and redesign its `paint()`:
- Title "KICK PUNCH", big signed dB number (green if ≥0, red if <0).
- One-line verdict: "Bass reinforces kick" / "Bass cancels kick" / "Neutral" (use a small dead zone, e.g. ±0.3 dB, for "Neutral").
- A diverging bar centered on the kick-alone baseline: extends right/green when reinforcing, left/red when cancelling.
- "Δ vs ref: +2.1 dB" line, shown only when a reference is set; otherwise a small "tap Set Ref to compare" hint.
- When `isTransientPunchValid()` is false, show a neutral placeholder ("—" / "Waiting for kick") instead of a stale or garbage value — match the existing oscilloscope's "WAITING FOR KICK" tone.
- Add a `juce::TextButton setRefButton` near this panel. Single-button toggle: first click calls `setTransientPunchReference()` and its label changes to something like "Clear Ref"; second click calls `clearTransientPunchReference()` and reverts.
- Grow this component's allocated height in `resized()` from 54px to roughly 110–130px to fit the richer content (exact value: tune by eye once building).

**Smoke test:** PUNCH shows a sane number with material playing; flips sign when Invert Polarity is toggled; goes neutral with no sidechain; Set Ref / Δ readout works and clears correctly; no divide-by-zero or NaN display under silence or a very quiet kick.

**Commit:** `feat: add kick-punch transient integrity meter with reference comparison`

---

## Phase 3 — Trigger mode: verify and finish (do not rebuild from scratch)

Code review shows the "kick freezes, only bass updates" behavior **already exists** in `src/ui/Oscilloscope.{h,cpp}` via a `kickTraceLocked` flag inside `refreshTriggeredSnapshot()`: the kick trace is locked to the first captured window and only the bass trace is swapped in on each retrigger, with existing comments describing exactly this intent. It already re-arms correctly on a timebase/zoom change and after an extended silence (the free-run watchdog). **Do not duplicate or rewrite this logic** — verify it works, then close the one real gap:

- `Oscilloscope::relockKickReference()` exists and is fully implemented but **has zero callers anywhere in the codebase**. Add a small "Relock Kick" button in the editor (near the Freeze button / view combo) wired to call it, so the kick reference can be manually re-captured (e.g. after swapping the kick sample) without needing to change zoom or tempo.
- Optional, your call, not required: `ghostBass`/`ghostKick` ghost-trail buffers are fully computed and rotated every hit in `refreshTriggeredSnapshot()` but are never actually drawn (`drawTriggeredMode()` only ever calls `drawPair` once, on the live pair) — dead computation for a visual that never renders. Either wire them up as faint prior-hit traces, or delete the dead code for cleanliness. Skip this if you'd rather not scope-creep.

**Smoke test:** with a steady kick+bass loop playing in Triggered view, confirm the kick trace stays visually static across many hits while the bass trace keeps updating as Delay/Polarity/Phase Filter are adjusted; confirm the new Relock button re-captures a fresh kick reference on demand.

**Commit:** `fix: expose manual kick relock in trigger mode`

---

## Phase 4 — Responsive layout: let the scope actually get bigger

The editor is **already resizable** (`setResizable(true, true)`, a `ComponentBoundsConstrainer` locked to the 1000:680 aspect ratio, size persisted via `apvts.state` properties `editorWidth`/`editorHeight`). This phase raises the ceiling and rebalances the internal split — it is not new plumbing.

- **`src/PluginEditor.cpp` constructor:** raise `resizeConstrainer.setSizeLimits(800, 544, 1500, 1020)` to a larger max (e.g. `2000, 1360`), keeping the same aspect ratio to avoid new edge cases (very wide/short or tall/narrow layouts).
- **`resized()`:** replace the fixed clamp `scopeBlockHeight = jlimit(190, 292, bounds.getHeight() - 318)` with a **proportional** split instead of a hard ceiling — e.g. scope height ≈ 40–45% of the space below the top bar, floored at ~190px, no upper cap. This way the scope keeps growing continuously as the window grows (today it hard-stops at 292px), and it automatically benefits from the ~92px freed by removing the Phase 1 knob row, with no separate adjustment needed.
- **`TransientPunchComponent`'s reserved height** in the right-hand column (see Phase 2) should already be sized in that phase; just confirm it still fits after this rebalance.
- **Leave `CorrelationDisplay` and `Oscilloscope` internals untouched** — both already draw entirely relative to their given bounds, so a bigger rectangle from `resized()` is sufficient.
- Known trade-off, not a bug: at very large window sizes, once the scope's proportional share is satisfied, the manual-controls panel below it (fixed-height knob rows, by design, so knobs don't stretch into odd giant shapes) will have some empty space beneath it. Leave as-is unless it's visually bothersome once running.

**Smoke test:** drag-resize from minimum to the new maximum; nothing clips or overlaps at either extreme; the scope visibly grows substantially more than before; reload a saved project and confirm window size restores correctly.

**Commit:** `feat: widen scope growth ceiling and rebalance responsive layout`

---

## Phase 5 — Final validation

Run the full smoke-test list from all four phases again, end to end, on the fully merged result:

- Sidechain connected and disconnected.
- Live Match %, Analyze, Apply Fix, Revert, Compare A/B, factory presets — unaffected by the removal.
- PUNCH meter: sane values, correct sign on polarity flip, neutral placeholder with no kick, Set Ref/Δ works.
- Trigger mode: kick static across many hits, bass live, Relock button works.
- Window resize across the full new range with no layout breakage; persisted size restores on reopen.
- A project saved **before** this update still loads without error (dyneq automation silently drops, everything else restores normally).
- If available: run `pluginval` (or the `audio-plugin-validators` marketplace plugin) against the built VST3/AU at a high strictness level and resolve anything it flags.

No parameter version-hint bumps are needed on the surviving parameters — this change deletes entries wholesale rather than altering the remaining ones' identity or range.

**Commit:** `chore: final validation pass for KickLock overhaul`
