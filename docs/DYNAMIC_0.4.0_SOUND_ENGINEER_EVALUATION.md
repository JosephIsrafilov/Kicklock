# KickLock 0.4.0 — Dynamic Mode guide for sound-engineer evaluation

This is a focused evaluation build for checking the hardened Dynamic Mode
workflow. Dynamic Mode does not follow MIDI pitch and does not apply a learned
State forever. It learns repeatable kick/bass conflict fingerprints and uses a
State only after a fresh, confident runtime match.

The 0.4.0 hardening keeps normal gaps between hits inside the activity hold,
returns safely to Global when usable input disappears, and requires a new
fingerprint after signal recovery. It does not retune the musical thresholds or
change the DSP package math.

## 1. Install and confirm the version

Use one format in a DAW test install and keep it separate from older KickLock
copies.

### macOS

- For VST3, unzip `KickLock-Mac-VST3.zip` and copy `KickLock.vst3` to
  `~/Library/Audio/Plug-Ins/VST3/` or to a dedicated VST3 scan folder.
- For AU, unzip `KickLock-Mac-AU.zip` and copy `KickLock.component` to
  `~/Library/Audio/Plug-Ins/Components/`.
- Rescan the plug-in in the DAW and confirm that the plug-in reports `0.4.0`.
- These evaluation archives are unsigned and unnotarized until the signing
  secrets are installed. macOS may show a Gatekeeper warning; use a dedicated
  evaluation copy and follow the DAW/macOS prompt for that specific plug-in.

### Windows

- Copy `KickLock.vst3` from the Windows archive to a VST3 scan folder, such as
  `%CommonProgramFiles%\VST3`, then rescan that folder.
- Confirm that the plug-in reports `0.4.0` before testing.

## 2. Routing

1. Put KickLock on the bass track or bass bus. The main input is the bass that
   KickLock processes.
2. Route the kick to the plug-in sidechain. The kick is a timing/reference
   signal; it is not sent to the processed output.
3. Keep the main input/output symmetric: mono-to-mono or stereo-to-stereo.
   Asymmetric main layouts are intentionally rejected.
4. The sidechain may be disabled, mono, or stereo. For a useful Dynamic test,
   use a real kick reference and a bass signal with enough level.

Do not route the kick into the main bass input. If there is no usable reference,
Dynamic Mode must stay safe rather than hold an old correction indefinitely.

## 3. Learn and apply a Dynamic map

1. Select **Dynamic**.
2. Press **Learn** and play a representative loop containing the kick/bass
   conflicts you want to correct. Keep the routing, sample rate, and buffer
   stable while learning.
3. Capture several clean repetitions of each repeatable conflict. Three
   repeatable hits can form a Candidate; five consistent hits are the Stable
   gate. MIDI and pitch labels are optional and are not the matching identity.
4. Press **Stop Learn**. Review the workspace preview. A pending preview is not
   active and does not change the current sound.
5. Press **Apply Learn** only when the preview is useful. **Discard** leaves the
   currently applied map and sound unchanged.

Dynamic Strength blends from the learned Global correction toward the selected
State correction. A Candidate can be recognized but safely remains on Global; a
recognized State without a confident correction also falls back to Global.

## 4. Read the Dynamic workspace

The workspace presents information in this order:

`NO MAP / LEGACY` → `BYPASSED` → input status → `HOLD` → active State/Service →
`GLOBAL FALLBACK`

- **NO SIDECHAIN** — no sidechain bus is available.
- **WAITING FOR KICK** — the sidechain exists, but no usable kick activity is
  held.
- **WAITING FOR BASS** — kick reference exists, but usable bass activity is
  missing.
- **SIGNAL TOO LOW** — a routed signal is present but below the usable-input
  floor.
- **ACTIVE** — both inputs have usable held activity and new matching can run.
- **HOLD** — the runtime is retaining a valid recent routing decision while a
  short unresolved interval is being resolved.
- **ACTIVE STATE / ACTIVE SERVICE** — a learned branch is currently selected.
- **GLOBAL FALLBACK** — no individual learned branch is safely active; the
  global package is used.

Normal pauses between hits shorter than roughly 1.5 seconds must not clear the
selected State. After the hold expires without usable kick or bass, the runtime
clears the unfinished fingerprint, Hold, and State/Service branch and returns
to Global. `SignalTooLow`, `WaitingForKick`, and `WaitingForBass` do not create
new Unknown or Verified evidence. **Verified** means fresh runtime evidence;
Learn predictions are not the same thing.

## 5. Recovery check

Use this sequence to evaluate the 0.4.0 fix:

1. Learn and Apply a map, then play until an **ACTIVE STATE** or **ACTIVE
   SERVICE** is visible.
2. Create ordinary musical gaps shorter than the hold. The state must not
   flicker away between normal kick hits.
3. Stop or mute the kick and/or bass for longer than the hold. The workspace
   should move through the appropriate input status and **GLOBAL FALLBACK**.
4. Restore both signals. The old State must not reactivate immediately. It may
   become active only after a new confident fingerprint match.
5. Try a routed but very quiet signal. Expect **SIGNAL TOO LOW**, with no new
   Unknown or Verified event.
6. Repeat after bypass, transport reset, save, and reload. Confirm finite
   output, no stuck old correction, and no audible discontinuity at recovery.

## 6. Report back

Please include:

- DAW and version, operating system, sample rate, and buffer size;
- whether VST3 or AU was used and the exact main/sidechain routing;
- the observed status sequence during Learn, signal loss, and recovery;
- whether the expected State activated only after a new match;
- any audible switch, click, dropout, or unexpected Global fallback.

A short screen recording of the Dynamic workspace during loss and recovery is
more useful than a screenshot of the final State alone.

This is a sound-engineer evaluation guide, not a claim that every musical
arrangement will benefit from Dynamic correction. The release gate is
automated; manual listening and real stems are intentionally left to this
evaluation.
