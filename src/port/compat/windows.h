/*
 * Stand-in for <windows.h> in the game source.
 *
 * CMR2Decomp includes the Win32 headers because MSVC6 builds it against the
 * real SDK. Here they resolve to port/types.h, which carries only the type
 * names; this directory is on the include path of the game source only, so
 * code that needs the real SDK on Windows still gets it.
 */
#include "port/types.h"
