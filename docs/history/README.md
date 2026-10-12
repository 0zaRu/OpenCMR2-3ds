# Validation history

These notes preserve evidence from individual development batches. Function
counts, audit totals, test counts and pending work describe the named baseline,
including any edits deliberately excluded from that validation. They are not
current project totals.

| Batch | Evidence |
| --- | --- |
| [Native runtime crashes](64bit-runtime-crashes.md) | Ghosts, sessions, replay storage and shadow buffers. |
| [Sector culling](64bit-sector-culling.md) | Terrain bounds and visibility-list corruption. |
| [Map and draw distance](64bit-map-draw-distance.md) | Preliminary-map callbacks and route distance limits. |
| [Finland results](64bit-stage-results.md) | Result-row variadic arguments and category best records. |
| [Runtime fixes, 2026-10-11](runtime-fixes-2026-10-11.md) | Country selection, headlight collision, replay events and audit coverage. |
| [Windows x64 and LAN](windows-x64-validation.md) | High-address regressions, Wine validation and two-player scenarios. |

The Finland results note links its original backtrace and validation manifest.
MSVC6 and original/rebuilt harness results referenced here belong to CMR2Decomp;
native port tests and runtime scenarios belong to OpenCMR2.

See the [documentation index](../README.md), [roadmap](../PLAN.md) and
[generated raw-access audit](../64bit-raw-access.md) for maintained guidance.
