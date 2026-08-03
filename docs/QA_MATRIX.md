# KickLock 0.4.0 QA Matrix

The 0.4.0 gate is deliberately short and automated. It does not infer DAW
behavior from unit tests and does not require real kick/bass stems.

| Area | Gate | Evidence |
| --- | --- | --- |
| DynamicInputStatus values and labels | `KickLockFastTests` | All five fixed values and workspace priority. |
| Activity hold and signal loss | `KickLockFastTests` | Normal hit gaps stay held; expired/quiet input clears runtime state. |
| No stale Unknown/Verified work | `KickLockFastTests` | Runtime input epoch rejects post-loss worker scores. |
| Mono/stereo bus rules | `KickLockFastTests` / host test | Symmetric main layouts pass; asymmetric layouts fail; sidechain disabled/mono/stereo are supported. |
| Canonical/legacy APVTS IDs | `KickLockFastTests` / GUI test | Canonical writes, legacy grouping, migration, and equivalent output. |
| Malformed and old state | `KickLockMalformedStateTests` | Truncated/wrong-root payloads and missing 0.4.0 parameters fail safely. |
| Dynamic Workspace/editor lifecycle | `KickLockGuiAcceptanceTests` | Status model, cards, canonical IDs, resize, and close/reopen. |
| Hosted VST3 routing/state | `KickLockHostAcceptanceTests` | JUCE host scan, metadata 0.4.0, actual sidechain buses, signal loss/recovery, 20 ms PDC, finite output, save/reload. |
| VST3 ABI/runtime | pluginval strictness 10 | Windows and macOS release workflows. |
| AU host validation | macOS `auval` | Runs against the built AU bundle. |
| Windows distribution | release workflow | Authenticode SHA-256 sign + verify before ZIP. |
| macOS distribution | release workflow | Universal arm64+x86_64, hardened runtime, timestamp, notarization, staple, validate. |

## Explicitly outside this matrix

Manual DAW runs, listening review, real-audio suites, AudioStress, sanitizer
matrices, and installer packaging are not 0.4.0 acceptance requirements.
