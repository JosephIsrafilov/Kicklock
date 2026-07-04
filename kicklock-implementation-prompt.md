# Kicklock — Implementation Prompt

You are working on **Kicklock**, an open-source JUCE (C++17) VST3 plugin that visually phase-aligns kick and bass — a free alternative to ReOrder Audio's "ReVision". The bass goes through the main bus; the kick is routed to the sidechain bus. The plugin measures kick/bass phase match, recommends a fix (delay + polarity + allpass "phase filter"), lets the user apply it, and displays waveforms on an oscilloscope.

A code review found one critical bug and several design flaws. Your job is to implement ALL fixes below, phase by phase, keeping the codebase compiling and tests green after every phase. Work in the order given — later phases depend on earlier ones.

## Ground rules (apply to every phase)

- **Real-time safety is non-negotiable.** Nothing called from `processBlock()` may allocate, lock, log, or block. Analysis stays on the background thread pool.
- **Keep the existing architecture**: `PhaseBands` is the single source of truth for band definitions; DSP lives in `src/dsp/`, UI in `src/ui/`; the editor only polls published atomics/lock-guarded state, never runs DSP.
- After each phase: build, run the test suite (`tests/`), and add the new tests specified for that phase. Do not move to the next phase with failing tests.
- Preserve parameter IDs and state (`getStateInformation`/`setStateInformation`) backward compatibility. New parameters get new IDs; never repurpose old ones.
- Match the existing code style (JUCE conventions, allman-ish braces, `juce::` prefixes, doc comments explaining *why*).

---

## Phase 1 — CRITICAL BUG: runtime phase filter ignores Q and stage count(Done)

**Problem.** `PhaseAlignmentEngine` (in `src/PluginProcessor.cpp`) runs ONE allpass stage per channel at hardcoded Q = 0.7071 (`updateAllpassCoefficients`). But `AlignmentAnalyzer` grid-searches Q ∈ {0.5…4} and stages ∈ {2,3,4}, predicts `afterMatch` from that multi-stage cascade, and `applyLatestFix()` writes `rotatorQ`/`rotatorStages` parameters that the audio path never reads. Predicted results have no relationship to what actually plays.

**Implement:**
1. Extend `PhaseAlignmentEngine::process()` to take `float allpassQ` and `int allpassStages` (clamped 2–4, matching `AllpassPhaseRotator`).
2. Replace the single per-channel `juce::dsp::IIR::Filter` with a per-channel cascade of up to 4 stages. Recompute coefficients when frequency, Q, or stage count changes (keep the existing smoothed-frequency + epsilon-guard pattern; add the same guard for Q). Reset filter state when stage count changes to avoid stale-state clicks.
3. In `processBlock()`, read `rotatorQ` and `rotatorStages` (both snake_case and legacy IDs, same dual-read pattern already used for the other params) and pass them through.
4. Confirm the wet/dry crossfade (`allpassWet`) still wraps the whole cascade.

**Tests (add to `tests/ProcessorTests.cpp` or a new file):**
- Render a 50 Hz sine through the processor with the phase filter at {freq=60, Q=2, stages=3} and separately through an offline `AllpassPhaseRotator` with identical settings; assert the outputs match within float tolerance after the smoothing settle time.
- **Regression test for this exact bug**: run `PhaseFixEngine::analyze()` on a synthetic kick/bass pair, apply the recommended settings through the real `PhaseAlignmentEngine`, re-measure the match with `MultiBandCorrelation`, and assert realized match ≈ predicted match within ±8 percentage points.

---

## Phase 2 — Stop over-promising: honest quality classification

**Problem.** `PhaseFixEngine::classifyQuality()` returns `StrongImprovement` for ANY polarity flip, ANY delay ≥ 0.1 ms, or ANY enabled rotator, regardless of measured improvement. And the rotator search in `AlignmentAnalyzer` enables itself on a correlation gain of 1e-4. Users get "Fix found: 61% → 63%", apply it, hear nothing, and lose trust.

**Implement:**
1. In `AlignmentAnalyzer`, only set `adjustRotator = true` when the rotator beats the non-rotator baseline by ≥ 0.03 absolute correlation (~3 match points). Keep the existing tie-break epsilon for choosing among rotator candidates.
2. Rewrite `classifyQuality()` to be improvement-driven:
   - `StrongImprovement`: `improvementPercent >= 8`
   - `PartialImprovement`: `improvementPercent >= 3`
   - `AlreadyGood`: `beforeMatchPercent >= 80` and improvement < 3
   - `NoUsefulChange`: everything else (keep the existing NotEnoughSignal / LargeTimingOffset / TimelineMoveRequired / Unstable branches above these).
3. Deterministic peak tie-breaking in `AlignmentAnalyzer`'s lag search: when two candidate peaks are within 2% relative magnitude, prefer (a) non-inverted polarity, then (b) smaller |lag|. This stops invert/delay recommendations flip-flopping between runs on the same loop.
4. `applyAllowed` should require `StrongImprovement` or `PartialImprovement`; keep `optionalApplyAllowed` for the borderline/large-offset cases with the existing warning messages.

**Tests:** feed a synthetic pair that is already ≥ 95% aligned and assert quality is `AlreadyGood` with no rotator enabled; feed a pair with a known 2 ms offset and assert `StrongImprovement` with delay within ±0.1 ms; run the same analysis 20× on identical input and assert identical polarity + delay every time.

---

## Phase 3 — One ruler: unify predicted, verified, and live match

**Problem.** Three incompatible measurements coexist: analyzer before/after (single band 30–120 Hz, filtfilt, optimal-lag xcorr), live meter (4 causal bands 20–500 Hz, EMA Pearson, weighted), and verification (offline rotator render). Numbers never agree; users read that as "the percentage is wrong". Also `PhaseFixEngine::score()` leaves `Score.multi` default-initialized, so all band-comparison logic compares empty structs.

**Implement:**
1. Create one canonical scoring function, e.g. `PhaseMatchScore MultiBandCorrelation::scoreRendered(bass, kick, n, sampleRate)` used by BOTH the analyzer's before/after computation and the verification step. It must use the `PhaseBands` table and the same weighted blend as `RealtimeMultiBandMeter::getWeightedMatchPercent()`, so offline and live numbers share definitions and weighting. Keep `AlignmentAnalyzer`'s 30–120 Hz xcorr for *finding* the lag — only the reported *percentages* change ruler.
2. Fill `Score.multi` properly wherever scores are computed; delete or wire up the currently-dead `testCandidate`/`testPhaseCandidate` grid-search path (prefer delete if unused — check callers first).
3. Fix `RealtimeMultiBandMeter` confidence: replace `norm * 200` (absolute-level dependent — quiet signals pin the display at 50%) with a relative measure: each band's energy share of the total across bands, floored by a −60 dBFS gate. A well-aligned bass at −30 dBFS must still read correctly.
4. Remove the dead `bufA`/`bufB` ring buffers from `CorrelationMeter` and `RealtimeMultiBandMeter` (the EMA implementation never reads them) and update the class comments, which still describe the old rolling-sum design.
5. Fix `smoothAtomic` in `processBlock()`: the `abs(current − 50) < 0.001` "uninitialized" check makes values snap whenever they legitimately pass through 50. Use a separate `std::atomic<bool>` initialized flag or a sentinel like −1.

**Tests:** render a candidate offline, score it with the canonical function, then stream the same rendered audio through `RealtimeMultiBandMeter` and assert the two match within ±5 points after settle; assert a −30 dBFS aligned pair reports > 85%, not 50%.

---

## Phase 4 — Make Analyze fast

**Problem.** `PhaseFixEngine::score()` calls `AlignmentAnalyzer::analyze()` with `maxDelayMs = 0` just to read lag-0 correlation — but `analyze()` unconditionally runs the full rotator grid search (9 freqs × 5 Qs × 3 stages = 135 filtered cross-correlations), all discarded. With 8 hits, one Analyze click does thousands of redundant FFT xcorrs.

**Implement:**
1. Add `AlignmentAnalyzer::matchAtZeroLag(bass, kick, n, sampleRate, lowHz, highHz)` — band-pass + one normalized dot product, no FFT, no rotator search. Use it in `score()`/`scoreSettings()`.
2. Add a `bool searchRotator` flag (default true) to `analyze()` so callers that only need lag+polarity can skip the grid.
3. Sanity-check the per-hit loop in `analyzeAggregatedHits`: the Hann window is applied BEFORE the candidate delay is rendered, so the delay shifts content relative to the window and the delay-line fill-in eats early-window energy. Render the candidate on the raw hit slice first, then window both signals identically, then score.

**Tests:** assert `matchAtZeroLag` agrees with the FFT path's `beforeMatch` within 1 point on synthetic input; add a coarse timing assertion (or at least a comment-documented benchmark) that an 8-hit Analyze completes in < 250 ms at 48 kHz on the CI machine.

---

## Phase 5 — Trigger-locked oscilloscope (the ReVision experience)

**Problem.** The scope free-scrolls with transient markers. ReVision's core UX is a kick-triggered display: every hit draws at the same X position, waveforms stand still, and the user watches alignment change as they turn the delay knob. All ingredients already exist (`TransientDetector`, `HitCaptureBuffer`).

**Implement:**
1. Add a `Triggered` view mode (new first entry in the view combo; make it the DEFAULT). On each detected kick transient, the processor publishes the latest hit window (−20 ms … +150 ms, mono bass + mono kick, post-processing so knob changes are visible) into a lock-free double buffer (`std::atomic` index swap; audio thread writes, UI reads — no locks). Reuse or extend `HitCaptureBuffer`; do NOT add allocations to the audio thread (preallocate both slots in `prepareToPlay`).
2. `Oscilloscope` in Triggered mode: draw kick and bass overlaid, time axis fixed to the hit window, t=0 line at the transient. Keep the last 3 hits ghosted at ~20% alpha behind the current hit. No scrolling.
3. Horizontal click-drag on the scope in Triggered mode nudges the `delay_ms` parameter (0.1 ms per ~4 px; Shift = 0.01 ms; double-click resets to 0). Route through the parameter attachment/`setValueNotifyingHost` with begin/endChangeGesture.
4. Keep the existing scrolling modes untouched behind the view combo.

**Tests:** UI logic is hard to unit-test; at minimum, unit-test the double-buffer handoff (writer thread + reader thread, assert no torn reads via a sequence counter) and the pixel↔delay mapping helper as a pure function in `src/ui/` or `src/util/`.

---

## Phase 6 — UI hierarchy and trust loop

**Implement (in `PluginEditor` / `CorrelationDisplay`):**
1. **One hero number**: the live low-end weighted match, large (≈48 pt), centered above the scope, color-graded (< 55 red, 55–75 amber, > 75 green). Move weighted/low-end/broadband trio and the per-band values into a compact "Details" row of four small labeled band meters (SUB/LOW/LOW MID/BODY) that can be collapsed.
2. **Trust loop**: after Apply Fix, show the live number plus a small "before: N%" ghost captured at apply time. Remove "predicted" from the primary display (keep it in the analyzer details text).
3. **Revert**: `applyLatestFix()` snapshots the previous {delay, polarity, phase filter, freq, Q, stages} values; add a "Revert" button next to Apply Fix that restores them. Wrap ALL parameter writes in `beginChangeGesture()`/`endChangeGesture()`.
4. **Explain disabled buttons in place**: when Analyze is disabled, its button text shows the reason ("Analyze — no sidechain", "Analyze — waiting for kick…", "Analyze — capturing…"), reusing `classifyAnalysisMaterialStatus`.
5. **Resizable editor**: `setResizable(true, true)`, constrainer with fixed aspect ratio, range roughly 800×544 to 1500×1020; store the size in the plugin state.

**Tests:** extend `UiHelperTests.cpp` for the button-text-from-status mapping and the revert snapshot/restore round-trip (processor-level, no UI instantiation needed).

---

## Phase 7 — Commercial polish (do what fits, in this order)

1. A/B compare: two settings slots + copy, processor-side, two toolbar buttons.
2. 4 factory presets ("Tight EDM", "Deep House Sub", "Trap 808", "Neutral") via `getStateInformation` presets or JUCE program API.
3. Tooltip coverage audit — every interactive control.
4. A "?" help overlay listing kick→sidechain routing steps for Ableton, FL Studio, Logic, Cubase, Reaper.
5. Run `pluginval --strictness-level 10` and fix everything it reports.
6. Update README: what it does, routing GIF placeholder, build instructions, comparison-to-ReVision table, GPL-3.0 note (JUCE GPL compatibility).

---

## Definition of done

- All phases implemented; build clean with no new warnings; full test suite green.
- The Phase 1 regression test (predicted ≈ realized match) passes.
- Manual smoke test documented in the PR/commit description: load in a DAW (or pluginval), route a kick to sidechain, confirm the hero % reacts per hit, Analyze completes < 1 s, Apply moves the live number toward the prediction, Revert restores prior settings.
- Each phase is its own commit (or PR) with a message explaining WHY, referencing the issue it fixes.

Start with Phase 1. Before writing code, read these files fully: `src/PluginProcessor.cpp` (esp. `PhaseAlignmentEngine`, `processBlock`, `applyLatestFix`, `analyzeAggregatedHits`), `src/dsp/PhaseFixEngine.h`, `src/dsp/AlignmentAnalyzer.h`, `src/dsp/RealtimeMultiBandMeter.h`, `src/dsp/PhaseBands.h`, `src/ui/Oscilloscope.*`, `src/PluginEditor.*`, and the existing tests. Then present a short plan of the exact edits for Phase 1 and proceed.
