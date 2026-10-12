# Development workflow

Build requirements and platform presets are in the root [README](../README.md).
CI builds each supported platform using the same presets.

## Build and test

On Linux, run the native build and 32-bit reference build:

```sh
cmake --preset linux
cmake --build --preset linux
ctest --preset linux

cmake --preset linux-x86
cmake --build --preset linux-x86
ctest --preset linux-x86
```

To include real-stage physics, runtime, lighting and movie regressions,
configure with your own original game data, then rebuild and test:

```sh
cmake --preset linux -DOPENCMR2_BINK_TEST_DATA_DIR="$HOME/cmr2game"
cmake --build --preset linux
ctest --preset linux
```

Use the same option for `linux-x86`. LAN game tests are registered for native
builds when game data is supplied; network tests require loopback UDP access.
See [PHYSICS_PARITY.md](PHYSICS_PARITY.md) and [BASELINE.md](BASELINE.md) for
the fixture scope and recording commands.

The `game_data_setup` test uses synthetic archive placeholders and simulated
desktop dialogs with SDL's dummy video driver. It covers first-run selection,
missing files, reading archives and writing saves under UTF-8 paths, remembered
settings, cancellation and portable data discovery. It also checks selecting
subfolders or parent folders, ambiguous installations and symlink cycles on
every CI platform, without original assets or a display server.

## Source sync and generated files

`game/` follows CMR2Decomp; `UPSTREAM` records the imported commit.
Use `tools/sync_upstream.py`, inspect its changes and resolve any conflicts.
Port adaptations and tests must remain valid after a sync.

When compatibility-sensitive structures change, check the disk-layout header:

```sh
python3 tools/gen_disk_layouts.py --check
```

Regenerate it with `python3 tools/gen_disk_layouts.py` when needed and review
the diff. The raw-access audit requires `clang-devel` and writes its detailed
JSON and plugin caches under `build/`:

```sh
python3 tests/raw_access_audit.py --plugin
python3 tools/audit_raw_access.py --json build/raw-access-current.json \
  --markdown docs/64bit-raw-access.md --jobs 4
```

The Markdown audit is generated; update it with the tool. Individual reviews
in `tools/raw_access/reviewed.json` retain source guards and automatic evidence.

## Files to keep

- Keep maintained guides in `docs/` and link them from its index.
- Keep past validation notes and their useful evidence in `docs/history/`,
  with the baseline commit and coverage limits.
- Keep frozen regression inputs, accepted comparisons and provenance in
  `tests/reference/`. Its recordings and audit logs support reproducible tests.
- Put temporary plans, logs, captures, build products, downloaded toolchains
  and package experiments under `build/`. It is ignored by Git, as are `out/`,
  `dist/`, Python caches and local `CMakeUserPresets.json`.

Before committing, review `git status --short`, `git diff --check` and the staged
diff. Preserve the test evidence and original game data separately from public
packages; the Windows packager excludes game assets when `--data` is omitted.
