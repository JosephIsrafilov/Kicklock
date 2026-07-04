# KickLock

KickLock is an open-source kick/bass phase alignment plugin focused on visual
manual alignment with optional automatic assistance.

It puts a large oscilloscope at the center of the interface so you can *see* how
your kick and bass line up in the low end, then correct that relationship by
hand — delay, polarity, and a phase-rotating filter — with an Analyze helper
that recommends settings and explains what it found. Nothing is hidden behind a
single "fix it" button: the manual controls are always live, and Analyze only
ever recommends.

- **Format:** VST3 / AU / Standalone (JUCE 8, C++20)
- **Platforms:** Windows, macOS
- **Input model:** bass on the main input, kick on the sidechain

---

## What KickLock does

When a kick and a bass note occupy the same low-frequency range at the same
time, their waveforms can partially or fully cancel, leaving the low end weak or
inconsistent. KickLock helps you line up the bass to the kick so they reinforce
each other instead of fighting.

It continuously measures a multi-band, low-end-weighted **phase match** between
the two signals and draws both waveforms on a shared oscilloscope. You correct
the relationship with three tools — a signed **Delay**, a **Polarity** flip, and
an all-pass **Phase Filter** — and watch the match improve in real time.

## How to route kick and bass

KickLock listens to two inputs:

1. **Main input — bass.** Insert KickLock on your bass track (or bus). This is
   the signal KickLock actually processes and outputs.
2. **Sidechain input — kick.** Route your kick to KickLock's sidechain input.
   The kick is only used as a reference; it is never altered or passed to the
   output.

The top bar shows the current routing state:

- `NO SIDECHAIN` — no kick is routed to the sidechain.
- `WAITING FOR KICK` / `WAITING FOR BASS` — one side is silent.
- `SIGNAL TOO LOW` — both are present but too quiet for a reliable read.
- `SIDECHAIN ACTIVE` — both are playing and KickLock is comparing them.

The oscilloscope stays visible even with no sidechain connected; it simply shows
a reminder to route the kick.

## Manual workflow

The **MANUAL ALIGNMENT** section is always live — you never have to press
Analyze to use it.

1. Play the loop and watch the oscilloscope. In **Overlay** view the bass and
   kick share a center line; in **Separate** view they get their own lanes; in
   **Phase Delta** view the display emphasises where they reinforce (green) or
   cancel (red).
2. If the low end sounds hollow or thin, try **Invert Polarity** first — a full
   cancellation often clears up instantly.
3. Use **Delay** to slide the bass earlier or later until the waveforms line up
   and the live match rises. The Δ read-out and peak markers on the scope help
   you judge the offset.
4. If a specific frequency region still conflicts, enable the **Phase Filter**
   and set its frequency and Q to rotate phase around that region without moving
   the whole signal in time.

## Auto-align workflow

Analyze is an assistant, not a replacement for the manual controls.

1. Let the loop play for a second or two so KickLock captures several kick hits.
2. Press **Analyze**. KickLock searches recent kick hits for the best delay,
   polarity, and phase-filter settings and scores the low-end match before and
   after.
3. Read the **Analyzer** panel. It reports the recommended Delay, Polarity, and
   Phase Filter, a **Confidence** figure, a before → after low-end match, and a
   short plain-language reason. It also warns when a large timing offset would
   be better fixed by moving the clip in your DAW than by delaying the bass.
4. If you like the recommendation, press **Apply Fix** to write it into the
   manual controls. You can then fine-tune anything by hand. If you don't, just
   ignore it — nothing changes until you apply.

## What the controls mean

### Delay
Moves the bass earlier or later relative to the kick, from **−20.00 ms to
+20.00 ms**.

- Negative values *advance* the bass (make it earlier).
- Positive values *delay* the bass (make it later).

Because audio can't literally be moved earlier, KickLock reports a fixed 20 ms
of latency to your host (see PDC) and shifts within that headroom, so a negative
Delay is achieved without ever running a negative delay line.

### Polarity
**Invert Polarity** flips the bass by 180°. Use it when the kick and bass are
cancelling each other — an inverted bass that was subtracting from the kick will
start adding to it.

### Phase Filter
An all-pass filter that **rotates phase around a chosen frequency without moving
the whole signal in time**. Set the **Phase Freq** (20 Hz – 2 kHz) to the region
where kick and bass conflict and adjust **Q** for how narrow that rotation is.
Unlike Delay, this only changes phase near that frequency, so it can resolve a
localised conflict without smearing the transient.

The **Advanced** section adds Delay Interpolation (Linear / Allpass) and Phase
Stages (2 / 3 / 4) for finer control; the defaults are fine for most material.

### Visual Offset
Moves **only the waveform display** left or right. This is a viewing aid for
lining traces up by eye — it does **not** affect the audio in any way. Keep it
separate in your mind from Delay: Delay changes what you hear, Visual Offset
changes only what you see.

### PDC (Plugin Delay Compensation)
KickLock reports a fixed 20 ms of latency to your DAW so it has headroom to move
the bass *earlier* as well as later. Your host compensates for this
automatically, keeping everything in sync. The top bar shows the exact latency
reported, in both samples and milliseconds.

## Recommended use case

Insert KickLock on a bass track and sidechain the kick to it in an
electronic/hip-hop/pop context where a sustained bass and a tonal kick share the
sub range. It's most useful on looped or repetitive material, where a consistent
kick hit lets Analyze form a confident recommendation and where a stable
low-end lock is most audible.

## Limitations

- KickLock processes **bass only**. It can recommend moving the kick, but it
  cannot move it for you — the kick is a reference on the sidechain.
- Analyze works best on repetitive loops with clear, consistent kick hits.
  Very sparse, wildly varying, or extremely quiet material yields low
  confidence.
- Very large timing offsets (well beyond a few milliseconds) are better fixed by
  moving the clip in your DAW timeline; KickLock will say so rather than apply an
  unnaturally large delay.
- The delay range is intentionally limited to ±20 ms. It is a phase-alignment
  tool, not a general-purpose delay.

## Building

KickLock uses CMake and fetches JUCE automatically.

```
cmake -B build
cmake --build build --config Release
```

Tests build as a console target and can be run directly:

```
cmake --build build --target KickLockDspTests --config Debug
./build/tests/Debug/KickLockDspTests
```

## License / positioning

KickLock is an original, clean-room implementation of the kick/bass
phase-alignment category. It is not a clone of, and contains no code or assets
from, any commercial plugin.
