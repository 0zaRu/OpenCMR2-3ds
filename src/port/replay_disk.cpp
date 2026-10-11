#include "port/types.h"
#include "port/replay_disk.h"
#include "port/disk_layouts.h"
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {
bool PayloadSize(const ReplayStream &p, size_t &size) {
    if (p.type < 0 || p.type > 2 || p.laneCapacity <= 0 || p.samplesPerLane <= 0 ||
        p.laneCount < 0 || p.laneCount > p.laneCapacity)
        return false;
    const size_t record = p.type == 0 ? sizeof(ReplayInputLane) : sizeof(ReplayStateLane);
    const size_t sample = p.type == 2 ? sizeof(ReplaySample) : 4;
    const size_t lane = record + size_t(p.samplesPerLane) * sample + sizeof(short);
    if (size_t(p.laneCapacity) > (std::numeric_limits<size_t>::max() - sizeof(ReplayStream)) / lane)
        return false;
    size = size_t(p.laneCapacity) * lane;
    return true;
}
void Pointers(ReplayStream &p) {
    unsigned char *data = reinterpret_cast<unsigned char *>(&p) + sizeof(p);
    const size_t record = p.type == 0 ? sizeof(ReplayInputLane) : sizeof(ReplayStateLane);
    p.pInputs = p.type == 0 ? data : nullptr;
    p.pStates = p.type != 0 ? data : nullptr;
    data += size_t(p.laneCapacity) * record;
    p.pSamples = p.type == 2 ? data : nullptr;
    p.pFrames = p.type != 2 ? data : nullptr;
    data += size_t(p.laneCapacity) * size_t(p.samplesPerLane) * (p.type == 2 ? sizeof(ReplaySample) : 4);
    p.pLaneSamples = reinterpret_cast<short *>(data);
}
}

ReplayStream *ReplayDisk_Read(const void *bytes, size_t size) {
    if (!bytes || size < g_diskLayout_ReplayStream.diskSize) return nullptr;
    ReplayStream header{};
    Disk_Unpack(&header, bytes, &g_diskLayout_ReplayStream);
    size_t payload;
    if (!PayloadSize(header, payload) || size != g_diskLayout_ReplayStream.diskSize + payload) return nullptr;
    auto *p = static_cast<ReplayStream *>(calloc(1, sizeof(header) + payload));
    if (!p) return nullptr;
    *p = header;
    memcpy(reinterpret_cast<unsigned char *>(p) + sizeof(header),
           static_cast<const unsigned char *>(bytes) + g_diskLayout_ReplayStream.diskSize, payload);
    Pointers(*p);
    for (int i = 0; i < p->laneCount; ++i) {
        if (p->pLaneSamples[i] < 0 || p->pLaneSamples[i] > p->samplesPerLane) {
            free(p); return nullptr;
        }
    }
    return p;
}

bool ReplayDisk_Write(const ReplayStream *p, std::vector<unsigned char> &bytes) {
    size_t payload;
    if (!p || !PayloadSize(*p, payload)) return false;
    bytes.resize(g_diskLayout_ReplayStream.diskSize + payload);
    Disk_Pack(bytes.data(), p, &g_diskLayout_ReplayStream);
    // Stored runtime array addresses are rebuilt after loading.
    for (int i = 0; i < g_diskLayout_ReplayStream.pointerCount; ++i) {
        const auto &field = g_diskLayout_ReplayStream.pointers[i];
        uint32_t value = 0;
        memcpy(bytes.data() + field.disk, &value, sizeof(value));
    }
    memcpy(bytes.data() + g_diskLayout_ReplayStream.diskSize,
           reinterpret_cast<const unsigned char *>(p) + sizeof(*p), payload);
    return true;
}
