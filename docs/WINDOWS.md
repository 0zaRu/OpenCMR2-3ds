# Windows x64 builds

The [build workflow](../.github/workflows/build.yml) builds, tests and
packages Linux (i686, x86-64, ARM64), macOS (ARM64) and Windows (x64).
Pushing a tag whose name starts with `v` publishes every package and its
SHA-256 in a GitHub release, only after all jobs pass.

The Windows package, `OpenCMR2-Windows-x64.zip`, contains `opencmr2.exe`,
SDL3 and the other non-system DLLs it needs, and the licences. It contains
no game data: on first launch, players choose the folder of their own PC
installation.

## Packaging

`tools/package_release.py` builds every release archive. For Windows
(requires Python `pefile`) it follows the executable's DLL imports
recursively, rejects missing or non-x64 DLLs and writes the ZIP and its
checksum.

## Cross-build from Linux

Use the official LLVM-MinGW UCRT bundle. The local build was validated with
`llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64.tar.xz`, SHA-256
`5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4`.
The Windows SDL3 package used locally is the official `SDL3-devel-3.2.16-mingw.tar.gz`,
SHA-256 `e95581bbef8f94042c7433a57b6e9ab4517c0ea54faddc85d0a706b2d35e04ef`,
from [SDL's release](https://github.com/libsdl-org/SDL/releases/tag/release-3.2.16).
Host-side CMake, Ninja and glslangValidator generate the embedded Vulkan
shaders.

```sh
export OPENCMR2_LLVM_MINGW=/path/to/extracted/llvm-mingw
cmake -S . -B build/windows-x64-mingw -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/windows-llvm-mingw.cmake \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOPENCMR2_SHADERS_MSL=OFF \
  -DSDL3_DIR=/path/to/windows/SDL3/lib/cmake/SDL3
cmake --build build/windows-x64-mingw
python3 -m pip install pefile==2024.8.26
python3 tools/package_release.py \
  --binary build/windows-x64-mingw/src/opencmr2.exe \
  --dll-dir "$OPENCMR2_LLVM_MINGW/x86_64-w64-mingw32/bin" \
  --license "$OPENCMR2_LLVM_MINGW/LICENSE.TXT" \
  --license /path/to/SDL3/LICENSE.txt \
  --output build/OpenCMR2-Windows-x64.zip
```

## Validation

The Windows build has passed the Finland physics fixture at six render
cadences and two-player LAN races under Wine, and the window and intro videos
start with the Vulkan renderer. Wine runs the executable above 4 GB, which
exposed pointer truncations that a Linux build with low addresses hid; the
[raw-access report](64bit-raw-access.md) tracks the remaining 64-bit review.
Testing on physical Windows machines is still pending.
