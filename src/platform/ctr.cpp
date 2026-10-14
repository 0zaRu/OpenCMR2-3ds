// Nintendo 3DS services: see platform/ctr.h.
//
// The game is played with its keyboard controls, as on Android: each button
// presses the key of the game's default binding. A and B accelerate and brake
// on a stage but confirm and go back in the menus, so the mapping follows the
// context the game reports (Input_SetDrivingContext).

#include "platform/ctr.h"

#include <3ds.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "port/sys.h"

namespace {

// DirectInput scan codes of the game's default keyboard controls.
enum {
    DIK_ESCAPE = 0x01,
    DIK_R = 0x13,
    DIK_LBRACKET = 0x1A,
    DIK_RBRACKET = 0x1B,
    DIK_RETURN = 0x1C,
    DIK_C = 0x2E,
    DIK_SPACE = 0x39,
    DIK_UP = 0xC8,
    DIK_LEFT = 0xCB,
    DIK_RIGHT = 0xCD,
    DIK_DOWN = 0xD0
};

// Circle pad deflection (of about 156) that presses a direction.
const int s_circleThreshold = 48;
// Menu key repeat, as a keyboard's (milliseconds).
const u64 s_repeatDelay = 400, s_repeatInterval = 110;

bool s_console;
bool s_driving;
bool s_down[256];
u64 s_nextRepeat[256];

struct KeyEvent {
    unsigned char dik;
    bool repeat;
};
KeyEvent s_events[64];
int s_eventHead, s_eventCount;

void Push(int dik, bool repeat)
{
    if (s_eventCount == 64)
        return;
    s_events[(s_eventHead + s_eventCount) % 64] = { (unsigned char)dik, repeat };
    s_eventCount++;
}

// Keys held by the buttons in the current context.
void MapButtons(u32 held, const circlePosition &circle, bool touch, bool down[256])
{
    memset(down, 0, 256);
    bool left = (held & KEY_DLEFT) || circle.dx < -s_circleThreshold;
    bool right = (held & KEY_DRIGHT) || circle.dx > s_circleThreshold;
    bool up = (held & KEY_DUP) || circle.dy > s_circleThreshold;
    bool dn = (held & KEY_DDOWN) || circle.dy < -s_circleThreshold;
    down[DIK_LEFT] = left;
    down[DIK_RIGHT] = right;
    // The touch screen confirms everywhere (result screens on a stage).
    down[DIK_RETURN] = touch;
    if (s_driving) {
        down[DIK_UP] = up || (held & KEY_A);                // accelerate
        down[DIK_DOWN] = dn || (held & KEY_B);              // brake
        down[DIK_SPACE] = (held & KEY_Y) != 0;             // handbrake
        down[DIK_RBRACKET] = (held & KEY_R) != 0;          // gear up
        down[DIK_LBRACKET] = (held & KEY_L) != 0;          // gear down
        down[DIK_C] = (held & KEY_X) != 0;                 // camera
        down[DIK_R] = (held & KEY_ZR) != 0;                // (New 3DS) rear view
        down[DIK_ESCAPE] = (held & KEY_START) != 0;        // pause
        down[DIK_RETURN] = touch || (held & KEY_SELECT);
    } else {
        down[DIK_UP] = up;
        down[DIK_DOWN] = dn;
        down[DIK_RETURN] = touch || (held & (KEY_A | KEY_START));
        down[DIK_ESCAPE] = (held & KEY_B) != 0;
    }
}

} // namespace

// ---- watchdog ----------------------------------------------------------------

namespace {

volatile u64 s_lastFrame;
volatile unsigned s_polls;      // event polls since the last frame
char s_breadcrumb[160] = "starting";
volatile bool s_watching;

void Watchdog(void *)
{
    u64 lastReport = 0;
    while (s_watching) {
        svcSleepThread(1000000000LL);
        u64 now = osGetTime();
        if (s_lastFrame != 0 && now - s_lastFrame >= 3000 && now - lastReport >= 5000) {
            char crumb[sizeof(s_breadcrumb)];
            memcpy(crumb, s_breadcrumb, sizeof(crumb));
            crumb[sizeof(crumb) - 1] = 0;
            Sys_Log("watchdog: no frame for %u s, %u event polls; last: %s", (unsigned)((now - s_lastFrame) / 1000),
                    s_polls, crumb);
            lastReport = now;
        }
    }
}

} // namespace

void Ctr_StartWatchdog(void)
{
    if (s_watching)
        return;
    s_watching = true;
    s_lastFrame = osGetTime();
    // Above the game's thread (0x30), so it runs while that one is busy.
    if (threadCreate(Watchdog, NULL, 16 * 1024, 0x28, -2, true) == NULL)
        s_watching = false;
}

void Ctr_FrameShown(void)
{
    s_lastFrame = osGetTime();
    s_polls = 0;
}

void Ctr_Breadcrumb(const char *format, ...)
{
    char text[sizeof(s_breadcrumb)];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    memcpy(s_breadcrumb, text, sizeof(text));
}

bool Ctr_Init(void)
{
    osSetSpeedupEnable(true);   // New 3DS: 804 MHz and the L2 cache
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    s_console = true;
    printf("\x1b[1;1HOpenCMR2 for Nintendo 3DS\n\n");
    return true;
}

void Ctr_Shutdown(void)
{
    if (s_console) {
        s_console = false;
        gfxExit();
    }
}

void Ctr_Print(const char *line)
{
    if (s_console) {
        // 40 columns: long lines wrap, which keeps them readable.
        printf("%s\n", line);
    }
}

void Ctr_ShowMessage(const char *title, const char *text)
{
    if (!s_console)
        return;
    consoleClear();
    printf("\x1b[1;1H\x1b[33m%s\x1b[0m\n\n%s\n\n\x1b[36mPress A or START to continue.\x1b[0m\n", title, text);
    // Release first: the button that led here may still be held.
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysHeld() == 0)
            break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & (KEY_A | KEY_START))
            break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
}

bool Ctr_Update(void)
{
    s_polls++;
    if (!aptMainLoop())
        return false;
    hidScanInput();
    circlePosition circle;
    hidCircleRead(&circle);
    bool down[256];
    MapButtons(hidKeysHeld(), circle, (hidKeysHeld() & KEY_TOUCH) != 0, down);
    u64 now = osGetTime();
    for (int dik = 0; dik < 256; dik++) {
        if (down[dik] && !s_down[dik]) {
            Push(dik, false);
            s_nextRepeat[dik] = now + s_repeatDelay;
        } else if (down[dik] && !s_driving && now >= s_nextRepeat[dik] && dik != DIK_RETURN && dik != DIK_ESCAPE) {
            Push(dik, true);
            s_nextRepeat[dik] = now + s_repeatInterval;
        }
        s_down[dik] = down[dik];
    }
    return true;
}

bool Ctr_TakeKeyEvent(int *dik, bool *repeat)
{
    if (s_eventCount == 0)
        return false;
    *dik = s_events[s_eventHead].dik;
    *repeat = s_events[s_eventHead].repeat;
    s_eventHead = (s_eventHead + 1) % 64;
    s_eventCount--;
    return true;
}

bool Ctr_IsKeyDown(int dik)
{
    return dik >= 0 && dik < 256 && s_down[dik];
}

void Ctr_SetDrivingContext(bool driving)
{
    s_driving = driving;
}
