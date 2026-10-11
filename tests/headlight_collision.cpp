#include "port/types.h"
#include "Car.h"
#include "CarLightState.h"
#include "Collision2D.h"
#include "high_address.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

extern void Scene_InitFixedMathTables();
extern void StageObject_TestHeadlightGlowsAgainstCarBox(Car *, CollisionBox *);
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "glow collision: line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main()
{
    Scene_InitFixedMathTables();
    // A high native centre pointer with a zero pArray exposes reading word
    // 0x25: on x64 that word is the upper half of pArray, not pVertex.
    FixVector *centre = (FixVector *)Test_AllocateHighAddress(2 * 1024 * 1024);
    REQUIRE(centre);
    if (sizeof(void *) == 8) REQUIRE((uintptr_t)centre > UINT32_MAX);
    CollisionBox box = {};
    box.pVertex = centre;
    box.top = 2 * 65536;
    box.bottom = -2 * 65536;
    box.halfWidth = box.halfLength = 65536;
    box.axisA.x = box.axisB.z = 65536;
    Car car = {};
    memset(g_carHeadlightGlowRecords, 0, sizeof(g_carHeadlightGlowRecords));
    auto &glow = g_carHeadlightGlowRecords[0];
    glow.active = 1;
    glow.car = 1;
    glow.position.x = 101 * 65536;
    Car original = car;
    StageObject_TestHeadlightGlowsAgainstCarBox(&car, &box);
    REQUIRE(glow.active && !memcmp(&car, &original, sizeof(car)));
    glow.position.x = 2 * 65536;
    StageObject_TestHeadlightGlowsAgainstCarBox(&car, &box);
    REQUIRE(glow.active && !memcmp(&car, &original, sizeof(car)));
    glow.car = car.index;
    glow.position.x = 0;
    StageObject_TestHeadlightGlowsAgainstCarBox(&car, &box);
    REQUIRE(glow.active && !memcmp(&car, &original, sizeof(car)));
    memset(g_carHeadlightGlowRecords, 0, sizeof(g_carHeadlightGlowRecords));
    Test_FreeHighAddress(centre);
    puts("glow collision: native centre pointer, distant/axis rejection and owner filter passed");
    return 0;
}
