# OpenCMR2 roadmap

Milestones, in order. Each phase lists what "done" means.

For graphics modernisation, follow [MODERNIZATION.md](MODERNIZATION.md): capture
the simulation baseline and establish state ownership before changing clocks or
adding passes. A working renderer does not by itself establish simulation parity.

## F0: Portable build of the game code

- [x] Repository, `game/` imported from CMR2Decomp, `tools/sync_upstream.py`.
- [x] CMake presets: `linux-x86` (32-bit reference), `linux-x86-debug` (ASan/UBSan).
- [x] `src/port/types.h` replaces `<windows.h>` (type names only).
- [x] Fixed-point helpers in portable C, equal to the original assembly
      (`tests/fixedpoint_equiv`); no inline assembly left in `game/`.
- [ ] Platform API headers (`src/port/*.h`) and game headers free of
      DirectX/Win32 types.
- [x] Game translation units compile with Clang on the supported targets;
      CMake requires Clang with GNU-style options.
- [x] `/QIfist` TUs: float-to-int conversions made explicit (`FTOI`),
      found with the Clang AST and checked against x87 FISTP.

## F1: Platform layer (SDL3)

- [x] `main()`, window, event loop, focus handling (replaces WinMain/WndProc).
- [x] Time, files (case-insensitive game data lookup, user directory for
      saves and settings), configuration (replaces the registry and the CD
      check), message boxes.
- [x] Input backend: keyboard (DIK codes), mouse, joysticks and SDL haptic
      effects. Hardware and force-feedback validation remains open.
- [x] Audio: mixer with DirectSound semantics (3D voices, streaming music),
      WAV and MS-ADPCM decoding.

## F2: Renderer (SDL_GPU)

- [x] `gfx.h` API and game rendering calls ported to it.
- [ ] Uber-shader reproducing the fixed-function pipeline (lighting, fog,
      texture stages, bump env map, reflection texgen): GLSL compiled to
      SPIR-V and translated to MSL. Full visual parity remains open;
      the Windows backend uses Vulkan and DXIL is not implemented.
- [x] Textures (BGRA8, BC1, BC3, bump), render targets, cube maps.
- [x] 2D path (sprites, triangles, lines, fonts).

## F3: Parity (0.1)

- [ ] Menus, rallies, arcade, replays, split screen, saves.
- [x] In-tree Bink 1 (`BIKi`) video and stereo audio decoder (no FFmpeg
      dependency); see [VIDEO.md](VIDEO.md) for scope and validation.
- [x] Headless stage probe and golden Finland fixture against original
      executable traces at six render cadences. Full-stage coverage remains open.

## F4: 64-bit and other platforms

- [ ] Remaining raw-offset accesses typed; pointer-holding structures loaded
      from disk split into on-disk and in-memory forms; size/offset
      `static_assert`s in the 32-bit build.
- [x] Linux x86-64/ARM64, Windows x64, macOS ARM64 presets that compile and
      link; CI for all of them (`.github/workflows/build.yml`); Metal shaders
      from SPIR-V; SDL3 fetched where there is no package.
- [ ] Fixed sizes and strides (`AllocateLockedBuffer(0x130)`, `* 0x30`) as
      `sizeof`; `long` in on-disk structures as `DWORD`. Done in CMR2Decomp,
      MSVC6 bytes unchanged.
- [x] Linux x86-64 and Windows x64 under Wine load and play stages and pass
      the original physics fixture. ARM64 gameplay validation remains open.

## F5: Modernisation (every item optional, "Original" preset)

- [x] Initial M0: real-stage capture and recorded tick-input playback with a
      controlled clock; scheduler contract tests and optional CPU profiling.
      See [BASELINE.md](BASELINE.md); full-game parity coverage remains open.
- [x] Record original + SilentPatch and latest standalone decompilation;
      separate vehicle-state and elapsed-time pacing comparisons across FPS.
      See [PHYSICS_PARITY.md](PHYSICS_PARITY.md).
- [x] Resolve the Finland fixture's numeric differences; original math tables
      and 500 vehicle steps match. Fix duplicate scheduling and the extra
      countdown step; verify six cadences at 25 Hz. Expand fidelity fixtures
      before declaring full-game parity.
- [ ] Validate and isolate the existing normal 25 Hz car scheduling and
      body/wheel/camera interpolation; preserve stored replay/ghost rates.
- [ ] Tick-indexed input/state traces prove that render cadence, graphics
      settings and original/enhanced mode switching preserve the simulation.
- [ ] Decouple host, simulation and presentation clocks with explicit
      pause/stall policies and measured frame pacing/input latency.
- [ ] Any resolution, widescreen (Hor+ FOV), HUD anchored to the screen
      edges, UI scaling, borderless fullscreen, VSync options.
- [ ] MSAA, anisotropic filtering, reversed-Z, longer draw and fog
      distance, per-pixel lighting, cube-map car reflections, optional
      tonemapping and bloom, texture packs.
- [ ] Controls: rebinding, dead zones and response curves, hot-plug,
      gamepad glyphs, wheel force feedback.
- [ ] Quality of life: skip intros, quick stage restart, pause on focus
      loss, per-category volume, camera and FOV options, mods through an
      overlay data directory, `FIX_BUGS` fixes for original bugs.
- [x] Network play over UDP (ENet), replacing DirectPlay: LAN, Tailscale
      and internet hosts, host migration. See [NETWORK.md](NETWORK.md).

## F6: Distribution

- [x] Windows portable ZIP, runtime DLLs, licenses, launcher and checksums;
      CI artifacts and tagged releases. See [WINDOWS_PORTABLE.md](WINDOWS_PORTABLE.md).
- [ ] Flatpak, AppImage and macOS packages; first-run setup that locates
      and checks the original game data.
