# KickLock 0.2.0 QA Matrix

Automated results refer to the deterministic unit, runtime, serialization, and
CI checks in this repository. Manual DAW rows are intentionally not inferred
from those checks.

| Area | Status | Evidence / Follow-up |
| --- | --- | --- |
| Standalone build | Automated pass | Release build target in local release validation. |
| VST3 build | Automated pass | Release build target in local release validation. |
| pluginval VST3 strictness 10 | Automated pass in CI | Windows and macOS workflow `ctest` registration. |
| pluginval AU | Not run / requires macOS host validation | AU artifact is built on macOS CI; no AU pluginval claim is made. |
| Windows VST3 host smoke test | Not run / requires DAW | Open in a supported Windows VST3 host. |
| macOS VST3/AU host smoke test | Not run / requires DAW | Open in a supported macOS host. |
| Project save/reload | Automated pass | Phase 5 orchestration and Dynamic-mode serialization tests. |
| Sidechain routing | Automated pass | Processor observation and Learn bypass coverage. |
| Learn, Stop, Apply, Discard, Clear Map, Revert | Automated pass | Phase 5 orchestration tests. |
| Editor close/reopen | Automated pass | Editor lifecycle test for Preparing, Capturing, and ResultReady. |
| Host bypass | Automated pass | Learn and processor bypass tests. |
| Sample-rate change | Automated pass | Dynamic runtime 44.1/48/96 kHz coverage. |
| Offline bounce / deterministic render | Automated pass | Static golden and Dynamic runtime deterministic render tests. |
| Static/Dynamic automation | Automated pass | Dynamic transition smoothing coverage; verify host lane UX manually. |
| Dynamic Strength automation | Automated pass | Runtime interpolation is finite/bounded; verify host lane UX manually. |
| CPU comparison: Static, Dynamic fallback, Dynamic learned note, editor open/closed | Not run / no local DAW CPU harness | Existing runtime tests verify finite, deterministic paths; no universal CPU claim is made. |
| Linux ASan + UBSan | Automated pass in CI | Linux Debug workflow runs the complete test executable under sanitizers. |
| Cooperative worker teardown | Automated pass | Maximum Learn material, active Static analysis, and active Spectrum FFT teardown complete within the conservative 3,000 ms bound. |

## Manual DAW Follow-up

Before release, verify sidechain routing, project reload, Dynamic Learn,
automation, offline bounce, bypass, and 44.1/48/96 kHz switching in at least
one Windows VST3 host and one macOS VST3/AU host. Record the host and version
when those checks are performed.
