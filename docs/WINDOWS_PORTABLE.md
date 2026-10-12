# Windows x64 portable builds

The [build workflow](../.github/workflows/build.yml) builds and tests Linux
x86, Linux x86-64/ARM64, macOS and Windows x64. Its Windows job uploads a
portable ZIP and SHA-256. Pushing a tag whose name starts with `v` creates
a GitHub release only after all those jobs pass.

Releases contain `opencmr2.exe`, SDL3 and all recursively resolved non-system
DLLs, licenses, `Jugar.cmd`, `LEEME.txt` and a SHA-256 file manifest. Original
game assets are excluded from CI packages. On first launch, choose the folder
of your original PC game installation in the native folder selector. OpenCMR2
checks the main data archives and remembers the path in `UserData/opencmr2.ini`;
the original files stay in place. Selecting a subfolder or a parent containing
the installation also works. If multiple copies are found, choose the one you
want to use. Canceling closes the application.

For a self-contained USB folder, copy your game files inside `Data` instead.
This folder is detected automatically; selecting an external installation
means that installation must remain accessible from the machine running the port.

Copy the complete folder to a USB drive and run `Jugar.cmd` on Windows 10/11
x64 with a Vulkan graphics driver. The launcher uses paths relative to itself
and passes `--user-dir` so saves, settings and logs stay in `UserData` beside
the executable. Moving the folder or changing the drive letter needs no edits.

## Cross-build from Linux

Use the official LLVM-MinGW UCRT bundle. The local build was validated with
`llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64.tar.xz`, SHA-256
`5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4`.
The Windows SDL3 package used locally is the official `SDL3-devel-3.2.16-mingw.tar.gz`,
SHA-256 `e95581bbef8f94042c7433a57b6e9ab4517c0ea54faddc85d0a706b2d35e04ef`,
from [SDL's release](https://github.com/libsdl-org/SDL/releases/tag/release-3.2.16).
Host-side CMake, Ninja
and glslangValidator generate the embedded Vulkan shaders.

```sh
export OPENCMR2_LLVM_MINGW=/path/to/extracted/llvm-mingw
cmake -S . -B build/windows-x64-mingw -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/windows-llvm-mingw.cmake \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DOPENCMR2_SHADERS_MSL=OFF \
  -DSDL3_DIR=/path/to/windows/SDL3/lib/cmake/SDL3
cmake --build build/windows-x64-mingw
python3 -m pip install pefile==2024.8.26
python3 tools/package_windows.py \
  --binary build/windows-x64-mingw/src/opencmr2.exe \
  --dll-dir "$OPENCMR2_LLVM_MINGW/x86_64-w64-mingw32/bin" \
  --license "$OPENCMR2_LLVM_MINGW/LICENSE.TXT" \
  --license /path/to/SDL3/LICENSE.txt \
  --data "$HOME/cmr2game" --output ../OpenCMR2-Windows-x64-portable
```

The destination must be new. The packager follows DLL imports recursively,
rejects missing or non-x64 DLLs, copies asset contents rather than symlinks,
and hashes every packaged file. Use `--zip PATH` to additionally create a ZIP
and checksum. Omit `--data` for a redistributable build without game assets.

## Validation

The cross-build has passed the Finland physics fixture at six render cadences
and two-player LAN scenarios under Wine. See the
[validation history](history/windows-x64-validation.md) for evidence and
reproduction commands. Testing on physical Windows and LAN machines remains open.
The [generated raw-access report](64bit-raw-access.md) tracks the current
64-bit review; snapshot counts in historical notes are not current totals.
