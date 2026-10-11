// Native copies of file records, see port/disk.h.

#include "port/disk.h"
#include "port/types.h"
#include "port/sys.h"

#include <stdlib.h>

#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#define ALLOCATION_SIZE(p) _msize(p)
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#define ALLOCATION_SIZE(p) malloc_size(p)
#else
#include <malloc.h>
#define ALLOCATION_SIZE(p) malloc_usable_size(p)
#endif

namespace {

struct Attachment {
    const char *fileData;
    void *native;
};

// Few at a time: one per loaded stage mesh file.
std::vector<Attachment> s_attachments;

} // namespace

void Disk_Unpack(void *native, const void *record, const DiskLayout *layout)
{
    memset(native, 0, layout->nativeSize);
    for (int i = 0; i < layout->runCount; i++) {
        const DiskRun &run = layout->runs[i];
        memcpy((char *)native + run.native, (const char *)record + run.disk, run.size);
    }
    for (int i = 0; i < layout->pointerCount; i++)
        memset((char *)native + layout->pointers[i].native, 0, sizeof(void *));
}

void Disk_Pack(void *record, const void *native, const DiskLayout *layout)
{
    memset(record, 0, layout->diskSize);
    for (int i = 0; i < layout->runCount; i++) {
        const DiskRun &run = layout->runs[i];
        memcpy((char *)record + run.disk, (const char *)native + run.native, run.size);
    }
    for (int i = 0; i < layout->pointerCount; i++) {
        uintptr_t pointer;
        memcpy(&pointer, (const char *)native + layout->pointers[i].native, sizeof(pointer));
        uint32_t low = (uint32_t)pointer;
        memcpy((char *)record + layout->pointers[i].disk, &low, sizeof(low));
    }
}

void Disk_AttachToBuffer(const void *fileData, void *native)
{
    s_attachments.push_back({(const char *)fileData, native});
}

void Disk_ReleaseBuffer(void *buffer)
{
    if (buffer == NULL || s_attachments.empty())
        return;
    // The file data can sit inside a larger buffer (an archive).
    const char *begin = (const char *)buffer;
    const char *end = begin + ALLOCATION_SIZE(buffer);
    for (size_t i = 0; i < s_attachments.size();) {
        if (s_attachments[i].fileData >= begin && s_attachments[i].fileData < end) {
            free(s_attachments[i].native);
            s_attachments[i] = s_attachments.back();
            s_attachments.pop_back();
        } else {
            i++;
        }
    }
}

void Disk_BadOffset(const void *record, unsigned off, uint32_t value)
{
    Sys_Log("disk: record %p field 0x%x holds offset 0x%x outside its array", record, off, (unsigned)value);
}
