// Runs one of the host tests (compiled with main renamed to test_main) on a
// 3DS or an emulator: its output and exit code go to sdmc:/opencmr2-test.log.
// Its argument comes from TEST_ARGS (a path on the SD card), or it takes none
// (TEST_NO_ARGUMENTS).

#include <3ds.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef TEST_NO_ARGUMENTS
int test_main();
#else
int test_main(int argc, char **argv);
#endif

int main(int, char **)
{
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    // The test's stdout and stderr go to the log on the SD card.
    freopen("sdmc:/opencmr2-test.log", "w", stdout);
    setvbuf(stdout, NULL, _IONBF, 0);
    dup2(fileno(stdout), fileno(stderr));
#ifdef TEST_NO_ARGUMENTS
    int code = test_main();
#else
    char *argv[] = { (char *)"test", (char *)TEST_ARGS, NULL };
    int code = test_main(2, argv);
#endif
    printf("exit code %d\n", code);
    fclose(stdout);
    while (aptMainLoop())
        gspWaitForVBlank();
    gfxExit();
    return code;
}
