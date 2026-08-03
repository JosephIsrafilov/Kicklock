# KickLock 0.4.0 CI Validation Policy

Pull requests build the short approved test targets on Windows and macOS:

- `KickLockFastTests`
- `KickLockHostAcceptanceTests`
- `KickLockGuiAcceptanceTests`
- `KickLockMalformedStateTests`

Push validation additionally builds the plugin, runs pluginval strictness 10,
and runs macOS `auval`. Release packaging is separate from ordinary CI.

The release workflow checks out exactly `v0.4.0`, verifies that `HEAD` equals the
tag commit, and runs the same approved tests before packaging. Windows signs the
internal VST3 module with Authenticode SHA-256 and verifies the signature.
macOS builds universal VST3/AU bundles, signs them with Developer ID and
hardened runtime/timestamp, submits one combined ZIP to `notarytool`, requires
`Accepted`, staples and validates both bundles, and only then creates the three
final ZIPs.

A preflight job checks all eight signing secrets. Missing secrets permit
internal diagnostic artifacts but skip the final GitHub Release job. There is
no automatic unsigned fallback and no appended unsigned/notarized warning in
release notes; notes are extracted only from the `0.4.0` changelog section.

AudioStress, sanitizer matrices, manual DAW checks, listening, and real-audio
suites are intentionally not invoked by these workflows.
