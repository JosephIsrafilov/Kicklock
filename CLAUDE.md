# KickLock Notes

See `ARCHITECTURE.md` for the current architecture and interface contracts.

Realtime invariants:
- Allocate only in `prepare()` or other non-audio-thread setup paths.
- `processBlock()`, DSP `processSample()`, and util `push*()` paths must not allocate, lock, log, or throw.
- `reset()` must clear state without reallocating.
- The live analysis path is `analyzeFix()` / `beginBackgroundAnalyze()` + `applyLatestFix()`.
- `AlignmentAnalyzer.h` remains a live dependency of `PhaseFixEngine`; do not remove it as legacy code.
