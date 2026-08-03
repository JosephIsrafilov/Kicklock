# KickLock 0.4.0 Dynamic Evaluation

This package is a Windows x64 VST3 evaluation candidate for the hardened
Dynamic Mode path. It is built from the 0.4.0 release candidate and is intended
for sound-engineer testing, not for production distribution.

## Install

Copy `KickLock.vst3` to a dedicated VST3 scan folder, rescan that folder in the
DAW, and confirm that the plugin reports version `0.4.0`. Keep the evaluation
copy separate from any older KickLock installation.

## Test setup

1. Put KickLock on the bass track or bass bus.
2. Route the kick to the plugin sidechain. The main input is bass; the
   sidechain is kick reference only.
3. Use Dynamic mode, run Learn on a representative loop, then Apply Learn.
4. Keep the same loop available for the recovery and save/reload checks.

## Dynamic checks

- After a confident match, confirm the workspace can show `ACTIVE STATE` or
  `ACTIVE SERVICE` and that the correction is audible only when the matched
  State is selected.
- Create normal gaps shorter than about 1.5 seconds between hits. The status
  must not flicker into a loss state and the selected State must not be cleared.
- Stop or mute the kick and bass. After the held activity expires, expect
  `WAITING FOR KICK`, `WAITING FOR BASS`, or `NO SIDECHAIN` as appropriate,
  with `GLOBAL FALLBACK`; the previous State must not remain applied forever.
- Restore the kick and bass. The old State must not reactivate immediately.
  The runtime should return through the input status/Global path and activate a
  State only after a new confident fingerprint match.
- Test a routed but very quiet signal. Expect `SIGNAL TOO LOW`; it must not
  create new Unknown or Verified runtime evidence.
- Toggle bypass, reset transport, save, reload, and repeat the signal-return
  check. Confirm output remains finite and there are no audible discontinuities.

## Report back

Please include the DAW and version, Windows version, sample rate, buffer size,
sidechain routing, the observed status sequence, and a short note about any
audible State switching or discontinuity. A short screen recording of the
Dynamic workspace during loss and recovery is useful.

## Important status

This is an unsigned internal evaluation build. It is not the final GitHub
release and should not be treated as a signed production installer.
