# CI Validation Policy

Pull requests use the required Windows, macOS, and Linux check names in fast
mode. Fast mode builds `KickLockFastTests`, runs its focused categories in one
process, and skips pluginval, universal macOS builds, artifact packaging, and
nightly release publication. The focused PR-fast categories include `Phase10`,
`Phase9`, `Phase8`, `Phase7`, `Processor`, and `UI Helpers` (plus `DSP` on Linux).

Pushes to `main`/`master` and manual dispatches run full validation: Release
plugin builds, `KickLockDspTests`, strictness-10 pluginval, universal macOS
artifacts, Linux sanitizer regression coverage, and (for main pushes) the
nightly release. Pluginval remains a full-validation gate.
