# Windows x64 portable builds

The existing `.github/workflows/build.yml` builds/tests Linux x86, Linux
x86-64/ARM64, macOS and Windows x64. Its Windows job now uploads a portable ZIP
and SHA-256. Pushing a tag whose name starts with `v` creates a GitHub release
only after all those jobs pass. No workflow is triggered merely by committing
these changes locally; the native MSVC-runtime CI job still needs a remote run.

Releases contain `opencmr2.exe`, SDL3 and all recursively resolved non-system
DLLs, licenses, `Jugar.cmd`, `LEEME.txt` and a SHA-256 file manifest. Original
game assets are excluded from CI packages. Add your own files inside `Data`.

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

## Validation and audit findings

The cross-build was tested with an isolated Wine x64 prefix. Its Finland
fixture matches the original plus SilentPatch for 500 updates at 30, 60, 120,
144 and 240 FPS and variable rendering cadence, with the same 25 Hz simulation.
The real window and both intro videos also start with SDL's Vulkan backend
under Wine on the desktop. Xvfb lacks DRI3 presentation support and cannot
validate that renderer. This does not replace testing on a physical Windows machine.

Actual stage startup exposed `CNetworkLeaderboards::Reset` converting a native
address to a 32-bit `int`. A Linux executable with low static addresses had
hidden the error; the Windows image starts above 4 GB. Reset now uses typed
records and still clears only each record's loaded flag. The raw-access plugin
also records pointer-to-integer widths, independently of pointee layout.

The observer now obtains matrices from native Car pointers before serializing
them, avoiding dereferencing pointer bits truncated into a Win32 capture.
The capture file format and original 32-bit recorder remain compatible.
The high-address regression fixtures reserve actual memory above 4 GB on
Windows; Wine's ordinary malloc can otherwise leave those tests below 4 GB.

The wider audit also recovered camera-offset and menu-sound cursors, a
pointer-sized stage archive cursor and the knockout round-entry pointer.
Keyboard repeat values are integer milliseconds rather than fake pointers;
result sorting uses the distance within its order array. Address arguments
to verified unused release-build parameters are passed as zero. Particle
appearance retains the original signed low-32-bit address hash explicitly;
those bits are a visual seed and are never converted back into a pointer.

The updated i386/x86-64 report contains 0 automatic bugs and 613 unresolved
accesses, down from 648 before this batch. Those remaining accesses still
need investigation. Two precise replay header/payload layout reviews retain
their automatic evidence and source guards. Pointer-width conversions are
checked even when the pointed-to record has an identical layout; merely
forming a pointer-sized one-past sentinel is not treated as dereferencing it.

The raw repository also recovers the transition callback-machine parameter.
MSVC6 emits an identical complete `.text` section, and its 11003-case original
comparison passes. Linux passes 28 x64 and 29 x86 tests, including real-data
scenarios and physics fixtures. This batch does not resolve the separate
Japan Rally Challenge rival stall also observed with the original installation.
