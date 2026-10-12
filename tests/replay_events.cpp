#include "port/types.h"
#include "Car.h"
#include "ReplayStream.h"
#include <cstdio>
#include <initializer_list>

extern void Replay_PlayStreamFrame(ReplayStream *);
#define REQUIRE(x) do { if (!(x)) { fprintf(stderr, "replay events: line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main()
{
    Car car{};
    car.simulationStepsRemaining = 1;
    g_cars[0] = &car;
    for (int type = 0; type < 2; ++type) {
        for (int eventCount : {0, 2}) {
            ReplayInputLane inputs{};
            ReplayStateLane states{};
            ReplayStream stream{};
            BYTE packets[12] = {252, 4, 130, 95, 252, 4, 130, 95, 0, 4, 2, 0};
            short lengths[] = {3};
            stream.type = type;
            stream.playing = stream.playStarted = 1;
            stream.laneCapacity = stream.laneCount = 1;
            stream.samplesPerLane = 3;
            stream.pFrames = packets;
            stream.pLaneSamples = lengths;
            stream.field_0x10c = 42;
            // The unused format's array must remain NULL, as it is in actual
            // input CPU runs and state replays. MSVC6 kept the old unconditional
            // pStates read and crashed here after the last input event.
            stream.pInputs = type == 0 ? &inputs : nullptr;
            stream.pStates = type == 1 ? &states : nullptr;
            ReplayEvent *events = type == 0 ? inputs.events : states.events;
            inputs.eventCount = states.eventCount = eventCount;
            events[0] = {0, 0, 7, 0};
            events[1] = {1, 0, 9, 0};
            Replay_PlayStreamFrame(&stream);
            REQUIRE(stream.frame == 0 && stream.field_0x21 == 1);
            REQUIRE(stream.field_0x10c == (eventCount ? 7 : 42));
            REQUIRE(car.flag0x1d0[0] == 31 && car.flag0x1d0[2] == 63 && car.handbrake == 1);
            Replay_PlayStreamFrame(&stream);
            REQUIRE(stream.frame == 1 && stream.field_0x21 == 0);
            REQUIRE(stream.field_0x10c == (eventCount ? 9 : 42));
            Replay_PlayStreamFrame(&stream);
            REQUIRE(stream.frame == 1 && stream.field_0x21 == 1);
            REQUIRE(stream.field_0x10c == (eventCount ? 9 : 42));
        }
    }
    g_cars[0] = nullptr;
    puts("replay events: both formats, repeat boundaries, empty and final events passed");
    return 0;
}
