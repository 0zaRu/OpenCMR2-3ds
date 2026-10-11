// Separate SDK translation unit: the game headers replace Win32 type names.
#include "high_address.h"
#include <stdlib.h>
#if defined(_WIN64)
#include <windows.h>
#endif

void *Test_AllocateHighAddress(size_t size)
{
#if defined(_WIN64)
    // Wine may keep even large malloc blocks below 4 GB. These regressions
    // must exercise a real pointer whose upper 32 bits are nonzero.
    for (size_t address = 0x100000000ULL; address < 0x1000000000ULL; address += 0x100000000ULL) {
        void *memory = VirtualAlloc((void *)address, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (memory) return memory;
    }
    return NULL;
#else
    return calloc(1, size);
#endif
}

void Test_FreeHighAddress(void *memory)
{
#if defined(_WIN64)
    VirtualFree(memory, 0, MEM_RELEASE);
#else
    free(memory);
#endif
}
