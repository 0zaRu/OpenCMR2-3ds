// Process entry point: sets up the platform and runs the game's main loop
// (CMain::Initialize, which was WinMain's body).

#include "platform/platform.h"
#include "port/sys.h"
#include "diagnostics/timing.h"
#include "main.h"

// On Android, SDLActivity calls SDL_main, which SDL_main.h renames main to.
#ifndef __ANDROID__
#define SDL_MAIN_HANDLED
#endif
#include <SDL3/SDL_main.h>

#ifdef __3DS__
#include <3ds.h>
// Linear memory (the GPU's): command and vertex buffers, the textures that do
// not fit in VRAM, audio. The rest of the application's memory is the heap.
// The main thread's stack (libctru's default is 32 KB): the game keeps large
// buffers on it.
extern "C" {
u32 __ctru_linear_heap_size = 20 * 1024 * 1024;
u32 __stacksize__ = 1024 * 1024;
}
#endif

int main(int argc, char **argv)
{
    SDL_SetMainReady();
    if (!Platform_Init(argc, argv))
        return Platform_WasSetupCanceled() ? 0 : 1;
    Diagnostics::StartProfileFromEnvironment();
    unsigned int code = CMain::Initialize(Sys_GetCommandLine());
    Diagnostics::StopProfile();
    Platform_Shutdown();
    return (int)code;
}
