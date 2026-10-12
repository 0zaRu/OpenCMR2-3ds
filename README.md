# OpenCMR2

A native, modernised port of **Colin McRae Rally 2.0** (PC, 2000), built from
the [CMR2Decomp](https://github.com/pablocpas/CMR2Decomp) decompilation. It
replaces DirectDraw/Direct3D 7, DirectSound, DirectInput, DirectPlay and Bink
with SDL3 and the SDL GPU API (Vulkan, Direct3D 12, Metal) and our own code,
so the game runs natively on Linux, Windows and macOS.

OpenCMR2 contains no game data. You need your own copy of the original game.

> [!NOTE]
> Work in progress: Linux x86 and x86-64 play stages, and Windows x64 has
> been checked under Wine. Full-game parity and visual fidelity remain open.
> See [docs/PLAN.md](docs/PLAN.md) for the roadmap,
> [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the architecture and
> [docs/MODERNIZATION.md](docs/MODERNIZATION.md) for the design of graphics
> modernisation with the original simulation.
> The [documentation index](docs/README.md) also links development instructions
> and historical validation evidence.

## Goals

- **Faithful by default.** The target is the original simulation, bit for bit
  in the reference build. Fixed-point helpers have assembly equivalence tests;
  500 Finland stage 1 updates now match original + SilentPatch at six render
  cadences, with stable 25 Hz scheduling. Full-game parity remains open;
  see [the comparison](docs/PHYSICS_PARITY.md).
- **Modern where it helps, always optional.** Any resolution and aspect ratio,
  high refresh rates with the physics at its original rate, better image
  quality and lighting, modern controllers and force feedback, and
  quality-of-life improvements. The original look is one preset away.

## Building

OpenCMR2 builds with Clang, CMake (3.24+), Ninja and `glslang` (or
`glslangValidator` on older installations), on
every platform from the presets in `CMakePresets.json`. SDL3 comes from the
system when it is installed; otherwise CMake downloads and builds it (or pass
`-DOPENCMR2_VENDORED_SDL=ON`).

| Preset | Target | Status |
|---|---|---|
| `linux-x86` | Linux, 32-bit x86 | Reference build: plays the game; golden tests compare it with the original |
| `linux` | Linux, 64-bit (x86-64 or ARM64) | x86-64 plays stages and passes original physics fixtures; ARM64 needs gameplay validation |
| `macos` | macOS, Apple silicon | Metal shaders are translated from SPIR-V; gameplay validation remains open |
| `windows-x64` | Windows x64 (`clang++`, MSVC runtime) | Vulkan renderer; also cross-built with LLVM-MinGW and checked under Wine |

The remaining raw accesses are tracked in [docs/64bit-raw-access.md](docs/64bit-raw-access.md).
Preservation changes are also checked in CMR2Decomp with MSVC6 and original-code comparisons.

### Linux, 32-bit reference build

Ubuntu 24.04 LTS (the CI environment):

    sudo dpkg --add-architecture i386
    sudo apt-get update
    sudo apt-get install clang g++-multilib ninja-build glslang-tools pkg-config \
        libx11-dev:i386 libxext-dev:i386 libxrandr-dev:i386 libxcursor-dev:i386 \
        libxi-dev:i386 libxss-dev:i386 libxkbcommon-dev:i386 \
        libwayland-dev:i386 wayland-protocols libegl1-mesa-dev:i386 \
        libasound2-dev:i386 libudev-dev:i386 libdbus-1-dev:i386
    export PKG_CONFIG_LIBDIR=/usr/lib/i386-linux-gnu/pkgconfig:/usr/share/pkgconfig

SDL3 is built from source when it is not installed.

Fedora packages:

    sudo dnf install clang gcc-c++ glibc-devel.i686 libstdc++-devel.x86_64 libstdc++-devel.i686 SDL3-devel.i686 ninja-build glslang

Then:

    cmake --preset linux-x86
    cmake --build --preset linux-x86
    ctest --preset linux-x86

### Linux, 64-bit

    sudo dnf install clang SDL3-devel ninja-build glslang     # Fedora
    cmake --preset linux && cmake --build --preset linux

### macOS

    brew install ninja glslang sdl3
    cmake --preset macos && cmake --build --preset macos

SDL_GPU uses Metal there, which takes MSL: the build fetches and builds
SPIRV-Cross to translate the shaders (or uses `spirv-cross` from the PATH).

### Windows

From a "x64 Native Tools" prompt of Visual Studio 2022 with its Clang
component (or LLVM's installer), Ninja and `glslangValidator` on the PATH:

    cmake --preset windows-x64
    cmake --build --preset windows-x64

`clang-cl` is not supported: the build uses GNU-style options. SDL_GPU runs on
Vulkan there (Direct3D 12 would need DXIL shaders).

CI builds and tests all platforms. Successful Windows jobs upload a portable
ZIP containing the executable, runtime DLLs, licenses and launcher. A `v*` tag
publishes that ZIP and its SHA-256 after all platform jobs pass. The original
game data is added locally, so it is never included in public release assets.
See [docs/WINDOWS_PORTABLE.md](docs/WINDOWS_PORTABLE.md) for USB packaging and
cross-building on Linux.

To record and replay a real stage at different render cadences, or measure CPU
frame/tick/presentation timings, see [docs/BASELINE.md](docs/BASELINE.md).
For frozen Windows original + SilentPatch recordings and field-by-field
comparison with the port, see [docs/ORIGINAL_REFERENCE.md](docs/ORIGINAL_REFERENCE.md).
Vehicle physics and game-speed checks at different render FPS are summarized
in [docs/PHYSICS_PARITY.md](docs/PHYSICS_PARITY.md).

    python3 tools/stage_baseline.py run --data ~/cmr2game --output build/baseline-first

## Updating the game source

`game/` follows CMR2Decomp. To bring in upstream changes:

    git remote add upstream https://github.com/pablocpas/CMR2Decomp   # once
    tools/sync_upstream.py

## Licence

GPL-3.0, like CMR2Decomp.
