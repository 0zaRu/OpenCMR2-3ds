// tests/stage_probe.cpp on a 3DS or an emulator: drives the menus to the
// Finland stage and runs it with a controlled clock, with the game files in
// sdmc:/3ds/OpenCMR2/Data. The trace goes to sdmc:/3ds/OpenCMR2/probe/, with
// the game's log (opencmr2.log) and the probe's output (probe.txt).

#include <3ds.h>

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

int test_main(int argc, char **argv);

#ifndef PROBE_TICKS
#define PROBE_TICKS "300"
#endif

int main(int, char **)
{
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/OpenCMR2", 0777);
    mkdir("sdmc:/3ds/OpenCMR2/probe", 0777);
    freopen("sdmc:/3ds/OpenCMR2/probe/probe.txt", "w", stderr);
    setvbuf(stderr, NULL, _IONBF, 0);
    setenv("XDG_DATA_HOME", "sdmc:/3ds/OpenCMR2/probe", 1);
    char *argv[] = { (char *)"stage_probe", (char *)"sdmc:/3ds/OpenCMR2/Data",
                     (char *)"sdmc:/3ds/OpenCMR2/probe/trace.jsonl", (char *)"30", (char *)PROBE_TICKS, NULL };
    int code = test_main(5, argv);
    fprintf(stderr, "probe: exit code %d\n", code);
    fclose(stderr);
    while (aptMainLoop())
        svcSleepThread(16000000);
    return code;
}
