# Nintendo 3DS

OpenCMR2 runs on the Nintendo 3DS family as a homebrew application: an
installable `OpenCMR2.cia` for consoles with custom firmware (Luma3DS) and
`opencmr2.3dsx` for the Homebrew Launcher. It is built with devkitARM,
libctru and citro3d. The game is the same code as on the other platforms,
built for the 3DS's 32-bit ARM11, so its data structures keep the original
layout. Only the platform layer and the renderer are specific to the 3DS.

> [!IMPORTANT]
> The 3DS port is experimental. On an Old 3DS the menus work; stages are
> being brought up. The renderer is checked against a test scene in an
> emulator, and the game's fixed-point maths against the original. Reports
> with the log (see [Problems](#problems)) are welcome.

## Why the 3DS and not the DS

A Nintendo DS version is not possible. The game needs tens of megabytes of
memory (a stage's archive is read whole), while the DS has 4 MB (16 MB in
DSi mode). Its 67 MHz ARM9 has no floating-point unit, and its GPU draws at
most 2048 triangles per frame. Smaller textures would not change that. The
3DS has 64 to 124 MB for an application, a CPU with hardware floating point,
and a GPU with a depth buffer and texture combiners close to Direct3D 7's
fixed-function pipeline.

## What to expect

| | Old 3DS / 2DS | New 3DS / New 2DS XL |
| --- | --- | --- |
| CPU | 268 MHz | 804 MHz, with the L2 cache |
| Memory for the game | 80 MB (CIA) | 124 MB (CIA) |
| Expected speed | slow; stages may run well below 30 fps | better |

The game keeps its 640x480 picture and draws it at half size, 320x240 in 4:3,
in the middle of the 400x240 top screen. The bottom screen shows the log.

To fit the memory, textures are shrunk (to at most 128x128 by default) and
stored in 16-bit formats. The full-size copy in main memory is released once
a texture is on the GPU; a 16-bit copy of the shrunk texture stays, so that
the textures the game changes later (car lights, for example) are rebuilt
from it. Some effects are left out:

* car reflections (a cube map the game renders every frame) are not drawn;
* the intro and country videos are skipped by default (decoding Bink is too
  slow);
* there is no specular light, no bump mapping and no lens flare, and spot
  lights are lit as point lights;
* there is no network play.

## Installing

1. Install `OpenCMR2.cia` with FBI (or another title manager). Instead, you
   can copy `opencmr2.3dsx` to `sd:/3ds/` and start it from the Homebrew
   Launcher. The CIA gets more memory and, on a New 3DS, the faster CPU.
2. Copy your installed PC game's folders (`Game`, `FrontEnd`, `Sounds`, ...)
   to `sd:/3ds/OpenCMR2/Data/`, so that `sd:/3ds/OpenCMR2/Data/Game/Bigfiles/common.bfl`
   exists. The movie folder can be left out, as the videos are skipped. If
   you have the CD, install the game on a PC first: the 3DS reads the
   installed folders.
3. Sound needs the console's DSP firmware dump (`sd:/3ds/dspfirm.cdc`). Run
   the DSP1 homebrew once if you do not have it. Without it the game runs
   silently.

Settings, saves and the log go to `sd:/3ds/OpenCMR2/`.

## Controls

The buttons press the game's default keyboard keys, so the controls menu
shows the keyboard. In menus and on a stage they mean different things:

| Button | Menus | Stage |
| --- | --- | --- |
| Circle Pad, D-Pad | Move | Steer (up and down also accelerate and brake) |
| A | Confirm | Accelerate |
| B | Back | Brake |
| Y | | Handbrake |
| R / L | | Gear up / down |
| X | | Camera |
| START | Confirm | Pause |
| SELECT | | Confirm |
| ZR (New 3DS) | | Key R of the keyboard controls |
| Touch screen | Confirm | Confirm |

The pause menu, the results after a stage and its replay are menus: there A
or START confirm and B goes back.

## Settings

`sd:/3ds/OpenCMR2/opencmr2.ini` takes these in its `[3ds]` section, in
addition to the [general settings](../README.md#configuration):

| Setting | Default | Description |
| --- | --- | --- |
| `texture_max` | `128` | Largest texture side, 8 to 1024. Higher is sharper and needs more memory |
| `movies` | `0` | `1` plays the Bink videos |
| `stretch` | `0` | `1` stretches the 4:3 picture over the whole top screen |
| `vsync` | `0` | `1` starts each frame on the screen's refresh: steadier, but the frame rate drops in steps (60, 30, 20, 15) |
| `console` | `1` | `0` stops writing the log on the bottom screen |
| `debug_lighting`, `debug_fog`, `debug_mipmaps` | `1` | `0` draws without lighting, fog or mipmaps, to find a feature the GPU does not take |
| `debug_shader` | `0` | `1` draws 3D with the minimal vertex shader (`fixedmin.v.pica`: no lighting or texture matrices) |

## Problems

The log is `sd:/3ds/OpenCMR2/opencmr2.log`. Every 600 frames it gets three
lines on memory, drawing and where a frame's time goes (the game's own work,
the renderer, waiting for the GPU, and the GPU's drawing time):

```
mem: heap 7072 of 65268 KB used, linear 26040 KB, VRAM 5343 KB free
gfx: 24.0 fps, up to 3006 draws and 30% of the command buffer a frame, 0 frames cut short
time: a frame 30.1 ms game, 9.2 ms renderer, 0.4 ms waiting for the GPU (GPU 12.0 ms); 410 KB of vertices
```

On a stage, the game's graphics options (draw distance, effects, shadows,
reflections) change most of the game's and the GPU's share. Car reflections
of the "environment" kind are not drawn on the 3DS, so they cost nothing.

Files of 512 KB or more are logged when they are opened, with the heap in use
then (`file Game/Bigfiles/....bfl: ... KB, heap ... of ... KB used`). A frame
that would overflow the GPU's command buffer is cut short instead, and
logged (`gfx: command buffer full`). If the game stops or
returns to the HOME menu, please send that file with a description of what
you were doing. With Luma3DS, a crash screen or dump (`sd:/luma/dumps/`)
helps too.

## Building

You need devkitPro with devkitARM, libctru and citro3d (the
`devkitpro/devkitarm` Docker image has them), Ninja, and for the CIA
[makerom](https://github.com/3DSGuy/Project_CTR) and
[bannertool](https://github.com/diasurgical/bannertool) on the `PATH` or in
`OPENCMR2_3DS_TOOLS`:

```sh
docker run --rm -v "$PWD:/src" -w /src devkitpro/devkitarm \
    sh -c 'cmake --preset 3ds -DOPENCMR2_3DS_TOOLS=/path/to/tools && cmake --build --preset 3ds'
```

CMake downloads and builds SDL3 for the 3DS (its audio, threads and files are
used; its video is not). The results are in `build/3ds/src/`:

| File | |
| --- | --- |
| `OpenCMR2.cia` | Installable title (only with makerom and bannertool) |
| `opencmr2.3dsx` | Homebrew Launcher application |
| `opencmr2_gfx_scene.3dsx` | Renderer test scene (`tests/3ds/gfx_scene.cpp`) |
| `opencmr2_gfx_stress.3dsx` | The test scene with 3000 more draws a frame, about a stage's load |
| `opencmr2_gpu_bisect.3dsx` | Draws one feature at a time, from the simplest up, and writes each phase that worked to `sd:/opencmr2-gpu.log`; the phase after the last one logged stops the GPU |
| `opencmr2_test_math_tables.3dsx`, `opencmr2_test_car_scheduler.3dsx` | Host tests that run game code; their output goes to `sd:/opencmr2-test.log` (the math tables test needs `tests/reference/finland-stage1/math-tables.bin` at `sd:/math-tables.bin`) |

The CI job `nintendo-3ds` builds them in the same container and uploads
`OpenCMR2-3DS.zip`.

### How it is put together

* `src/platform/ctr.cpp`: screens before the renderer starts, the log on the
  bottom screen, and the buttons, which press keyboard keys. The game reports
  whether a stage is being driven (`Input_SetDrivingContext`, a `PORT:` line
  in `CGame::DispatchFrontendResourceState` and in the pause menu).
* `src/render/gfx_c3d.cpp`: the executor of the recorded frames on citro3d,
  with the vertex shaders `src/render/shaders/fixed.v.pica` (lighting, up to
  4 lights, texture coordinates), `fixedtl.v.pica` (pre-transformed 2D) and
  `fixedmin.v.pica` (3D without lighting, for debugging). The shaders write
  their outputs whole and declare the texture coordinates with two
  components (`texcoord0.xy`): an emulator accepts a shader that leaves
  output components unwritten, a console's GPU may not.
  Fog uses the PICA's fog table, and the texture stages use its combiners.
* `src/render/gfx_core.cpp`: hands finished textures to the executor and
  releases their CPU copies (`ReleaseTextureCopies`). Vertex buffers get
  their memory on the first lock.
* `src/net/net_none.cpp`: no network connection is offered.
* `res/3ds/`: icon, banner and the makerom settings (`opencmr2.rsf`).

The game builds with GCC here, with `-fms-extensions -fpermissive` and empty
`__fastcall`/`__forceinline` (`src/CMakeLists.txt`). The 3DS build keeps the
compiler options the game relies on (wrapping signed overflow, no strict
aliasing, signed `char`, no fused multiply-add).

### The test scene

`opencmr2_gfx_scene.3dsx` draws, through `port/gfx.h`:

* a blue background, the 4:3 picture centred with black bars;
* top left, a textured square with red (top left), green (top right), blue
  (bottom left) and white (bottom right) quarters, and a half-transparent
  yellow square over it;
* top right, a green triangle (clockwise, drawn) and no red one
  (counter-clockwise, culled);
* a white cube lit from the upper left, whose front face has the value 133 of
  255. L turns it;
* a checkered ground that fades into grey fog with distance;
* bottom left, a copy of the corners texture made after its CPU copy was
  released (the same four colours), while the original's green quarter has
  turned magenta, changed through a lock after the release.

`opencmr2_gfx_stress.3dsx` adds 3000 small fogged, lit triangles, each with
its own world matrix; the log shows the command buffer use (about 30%).
