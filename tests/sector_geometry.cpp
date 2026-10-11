#include "port/types.h"
#include "Sector.h"
#include "Mesh.h"
#include "Graphics.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern void Scene_InitFixedMathTables();
extern void Sector_ComputeBounds();
extern int Sector_NearestCornerHeight(unsigned int, int);

#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "sector geometry: line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main()
{
    Scene_InitFixedMathTables();
    CGraphics::m_65536 = 65536.0;
    g_sectorCount = 1;
    g_sectorHalfSize = 2 * 65536;
    float vertices[3][12] = {};
    vertices[0][0] = 20; vertices[0][1] = 1; vertices[0][2] = 20;
    vertices[1][0] = -2; vertices[1][1] = 7; vertices[1][2] = 2;
    vertices[2][0] = 5; vertices[2][1] = 9; vertices[2][2] = -3;
    const int expected[4][2] = {
        {-2 * 65536, 20 * 65536}, {20 * 65536, 20 * 65536},
        {20 * 65536, -3 * 65536}, {-2 * 65536, -3 * 65536}
    };
    // LOD 2 exercises the native geometry prefix stride. It also keeps the
    // base mesh's metadata separate from the selected geometry's pointers.
    for (int lod = 0; lod <= 2; lod += 2) {
        const size_t stride = offsetof(Mesh, field_0x108);
        BYTE *storage = (BYTE *)calloc(1, lod * stride + sizeof(Mesh));
        REQUIRE(storage);
        Mesh *base = (Mesh *)storage;
        Mesh *selected = (Mesh *)(storage + lod * stride);
        base->lodIndex = (BYTE)lod;
        selected->pVertexData = (DWORD *)vertices;
        selected->field_0x10 = 3;
        Sector sector = {};
        sector.pMesh = base;
        g_sectors[0] = &sector;
        Sector_ComputeBounds();
        REQUIRE(!memcmp(sector.bounds, expected, sizeof(expected)));
        REQUIRE(Sector_NearestCornerHeight(0, 0) == 7 * 65536);
        REQUIRE(sector.corners[0].x == 0 && sector.corners[0].z == 0);
        REQUIRE(base->lodIndex == lod && selected->pVertexData == (DWORD *)vertices);
        free(storage);
    }
    g_sectors[0] = NULL;
    puts("sector geometry: native LOD stride, bounds and corner height passed");
    return 0;
}
