// ENet's Windows public header includes the whole Win32 SDK. Keep it out of
// translation units that use port/types.h (whose game types are fixed width).
// These are ENet's public socket/buffer declarations, not Win32 API calls;
// ENet itself still builds against the real SDK in third_party/enet.
#ifndef OPENCMR2_ENET_PLATFORM_H
#define OPENCMR2_ENET_PLATFORM_H

#ifdef _WIN32
#include <stddef.h>
#include <stdint.h>
#define __ENET_WIN32_H__
typedef uintptr_t ENetSocket;
typedef struct fd_set ENetSocketSet; // only used behind pointers in enet.h
typedef struct {
    size_t dataLength;
    void *data;
} ENetBuffer;
#define ENET_CALLBACK __cdecl
#define ENET_API extern
#endif

#include <enet/enet.h>
#endif
