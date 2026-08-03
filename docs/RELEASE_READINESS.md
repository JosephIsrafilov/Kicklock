# KickLock 0.4.0 Release Readiness

## Status

KickLock 0.4.0 preserves DynamicStateMap v1, stable IDs, serialized project
state, musical matching gates, correction ranges, 20 ms PDC, and DSP package
math. This release hardens the input-state boundary and prepares signed
distribution workflows.

| Area | Status | Evidence / gate |
| --- | --- | --- |
| Dynamic input status | READY FOR AUTOMATED GATE | Fixed five-value status, held activity, usable-input epoch, and workspace priority tests. |
| Dynamic signal loss | READY FOR AUTOMATED GATE | Hold/State/Service/fingerprint reset path; stale measurement scores are rejected after input loss. |
| Dynamic matcher and thresholds | PRESERVED | No DynamicStateMap v1, stable-ID, Candidate/Stable, threshold, ambiguity, or correction-range changes. |
| Main/sidechain bus layouts | READY FOR AUTOMATED GATE | Mono/mono and stereo/stereo only; sidechain disabled/mono/stereo remain legal. |
| APVTS compatibility | READY FOR AUTOMATED GATE | Canonical IDs remain primary; legacy IDs remain grouped and legacy-only state migrates. |
| Project version | READY | CMake/plugin version is 0.4.0; hosted metadata must report 0.4.0. |
| KickLockFastTests | REQUIRED | Runs the short approved unit/runtime suite. |
| KickLockHostAcceptanceTests | REQUIRED | Loads the built VST3 through JUCE, exercises sidechain routing, loss/recovery, PDC, and save/reload. |
| KickLockGuiAcceptanceTests | REQUIRED | Exercises Dynamic Workspace statuses, editor construction, canonical IDs, and close/reopen. |
| KickLockMalformedStateTests | REQUIRED | Truncated/wrong-root/old-state recovery stays safe and finite. |
| pluginval strictness 10 | REQUIRED | Windows and macOS release workflows. |
| macOS auval | REQUIRED | Runs against the built AU bundle in the macOS workflow. |
| Windows x64 VST3 | BLOCKED UNTIL SECRETS | Authenticode SHA-256 signature and verification are mandatory before packaging. |
| macOS universal VST3/AU | BLOCKED UNTIL SECRETS | Developer ID signing, hardened runtime, notarization Accepted, staple, and validation are mandatory. |
| Final GitHub Release | BLOCKED UNTIL SECRETS | Preflight skips publication when any required signing secret is absent. |

## Release artifacts

The final release contains exactly:

- `KickLock-v0.4.0-windows-x64-vst3.zip`
- `KickLock-v0.4.0-macos-universal-vst3.zip`
- `KickLock-v0.4.0-macos-universal-au.zip`

No installer and no unsigned release fallback are produced. Internal unsigned
bundles may be uploaded for CI diagnostics when signing secrets are missing,
but they cannot reach the GitHub Release job.

## Required signing secrets

`WINDOWS_CERTIFICATE_PFX`, `WINDOWS_CERTIFICATE_PASSWORD`,
`MACOS_CERTIFICATE_P12`, `MACOS_CERTIFICATE_PASSWORD`,
`APPLE_SIGNING_IDENTITY`, `APPLE_API_KEY_ID`, `APPLE_API_ISSUER_ID`, and
`APPLE_API_PRIVATE_KEY` must all be present.

## Scope exclusions

Manual DAW runs, listening review, real kick/bass stems, AudioStress, sanitizer
matrices, and other real-audio suites are intentionally outside the 0.4.0
acceptance gate. Their absence is not reported as a release pass or failure.

## Tag discipline

The publish workflow accepts only `v0.4.0`, checks out the tag with full history,
and fails unless `HEAD` exactly equals the tag commit and the checkout is clean.
The tag must be created separately on the final clean commit; this workspace
does not create or publish it while signing secrets are absent.
