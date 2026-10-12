# Documentation

Start with the root [README](../README.md) for requirements and build presets.
These guides describe the maintained workflow and current project scope.

| Guide | Contents |
| --- | --- |
| [Architecture](ARCHITECTURE.md) | Source layout, platform APIs and renderer design. |
| [Roadmap](PLAN.md) | Completed work and remaining validation. |
| [Development](DEVELOPMENT.md) | Tests, source sync, generated files and repository hygiene. |
| [Modernisation](MODERNIZATION.md) | Rendering and timing design while preserving the simulation. |
| [Baseline capture](BASELINE.md) | Stage capture, replay and profiling. |
| [Original reference](ORIGINAL_REFERENCE.md) | Original + SilentPatch recordings and comparison workflow. |
| [Physics parity](PHYSICS_PARITY.md) | Corrected six-cadence Finland fixture, timing and coverage limits. |
| [Raw-access audit](64bit-raw-access.md) | Generated i386/x86-64 layout review and remaining unresolved accesses. |
| [Network play](NETWORK.md) | LAN, VPN and internet setup, protocol and tests. |
| [Video](VIDEO.md) | Bink decoder scope and validation. |
| [Windows portable builds](WINDOWS_PORTABLE.md) | Cross-build, packaging and local game-data setup. |

[Validation history](history/README.md) preserves past fixes, crash evidence and
results at specific commits. Its counts and open issues describe those commits;
use the roadmap and generated audit for current status.

Frozen regression inputs and their provenance remain in
[`tests/reference/`](../tests/reference/). Local build outputs and exploratory
captures belong under the ignored `build/` directory.
