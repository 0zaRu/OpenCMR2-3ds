#ifndef OPENCMR2_PORT_REPLAY_DISK_H
#define OPENCMR2_PORT_REPLAY_DISK_H
#include <stddef.h>
#include <vector>
#include "ReplayStream.h"

// Original 0x110-byte headers plus pointer-free lane/sample arrays.
// The decoded buffer is independent storage; release it with the game allocator.
ReplayStream *ReplayDisk_Read(const void *bytes, size_t size);
bool ReplayDisk_Write(const ReplayStream *stream, std::vector<unsigned char> &bytes);
#endif
