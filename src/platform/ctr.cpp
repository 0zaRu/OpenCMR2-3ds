// Nintendo 3DS services: see platform/ctr.h.
//
// The game is played with its keyboard controls, as on Android: each button
// presses the key of the game's default binding. A and B accelerate and brake
// on a stage but confirm and go back in the menus, so the mapping follows the
// context the game reports (Input_SetDrivingContext).

#include "platform/ctr.h"

#include <3ds.h>

#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"
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
bool s_consolePaused;
bool s_driving;
bool s_down[256];
u64 s_nextRepeat[256];

struct KeyEvent {
    unsigned char dik;
    bool repeat;
};
KeyEvent s_events[64];
int s_eventHead, s_eventCount;
// Typed text (Windows-1252), from the system keyboard.
unsigned char s_text[128];
int s_textHead, s_textCount;
// The sockets' service (network play) and its buffer.
u32 *s_socBuffer;
const u32 s_socSize = 0x100000;

// X in a menu: the system keyboard, whose text the game gets as typed
// (names, the network chat).
void TypeText()
{
    SwkbdState keyboard;
    swkbdInit(&keyboard, SWKBD_TYPE_NORMAL, 2, 63);
    swkbdSetHintText(&keyboard, "Text for the game");
    char text[256];
    if (swkbdInputText(&keyboard, text, sizeof(text)) != SWKBD_BUTTON_CONFIRM)
        return;
    // UTF-8 to Windows-1252: ASCII and Latin-1 (all the game's fonts have).
    for (const unsigned char *p = (const unsigned char *)text; *p && s_textCount < 128;) {
        int c;
        if (*p < 0x80) {
            c = *p++;
        } else if ((*p & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) {
            c = ((*p & 0x1f) << 6) | (p[1] & 0x3f);
            p += 2;
            if (c > 0xff)
                continue;
        } else {
            p++;
            while ((*p & 0xc0) == 0x80)
                p++;
            continue;
        }
        s_text[(s_textHead + s_textCount++) % 128] = (unsigned char)c;
    }
}

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
// The last breadcrumbs, the newest at s_crumbIndex.
char s_breadcrumbs[8][160] = { "starting" };
volatile unsigned s_crumbIndex;
volatile bool s_watching;
Thread s_watchdog;

void Watchdog(void *)
{
    u64 lastReport = 0;
    while (s_watching) {
        // A second in tenths, to stop quickly (Ctr_StopWatchdog).
        for (int i = 0; i < 10 && s_watching; i++)
            svcSleepThread(100000000LL);
        if (!s_watching)
            break;
        u64 now = osGetTime();
        if (s_lastFrame != 0 && now - s_lastFrame >= 3000 && now - lastReport >= 5000) {
            unsigned newest = s_crumbIndex;
            char crumb[160];
            memcpy(crumb, s_breadcrumbs[newest % 8], sizeof(crumb));
            crumb[sizeof(crumb) - 1] = 0;
            Sys_Log("watchdog: no frame for %u s, %u event polls; last: %s", (unsigned)((now - s_lastFrame) / 1000),
                    s_polls, crumb);
            // The first time, what led there.
            if (lastReport == 0 || now - s_lastFrame < 5000)
                for (unsigned i = 7; i >= 1; i--) {
                    memcpy(crumb, s_breadcrumbs[(newest - i) % 8], sizeof(crumb));
                    crumb[sizeof(crumb) - 1] = 0;
                    if (crumb[0])
                        Sys_Log("watchdog:   before: %s", crumb);
                }
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
    s_watchdog = threadCreate(Watchdog, NULL, 16 * 1024, 0x28, -2, false);
    if (s_watchdog == NULL)
        s_watching = false;
}

void Ctr_StopWatchdog(void)
{
    if (s_watchdog == NULL)
        return;
    s_watching = false;
    threadJoin(s_watchdog, U64_MAX);
    threadFree(s_watchdog);
    s_watchdog = NULL;
}

void Ctr_FrameShown(void)
{
    s_lastFrame = osGetTime();
    s_polls = 0;
}

void Ctr_Breadcrumb(const char *format, ...)
{
    unsigned next = (s_crumbIndex + 1) % 8;
    va_list args;
    va_start(args, format);
    vsnprintf(s_breadcrumbs[next], sizeof(s_breadcrumbs[next]), format, args);
    va_end(args);
    s_crumbIndex = next;
}

bool Ctr_Init(void)
{
    osSetSpeedupEnable(true);   // New 3DS: 804 MHz and the L2 cache
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    s_console = true;
    printf("\x1b[1;1HOpenCMR2 for Nintendo 3DS\n\n");
    // Sockets for network play; without them the game has no network.
    s_socBuffer = (u32 *)memalign(0x1000, s_socSize);
    if (s_socBuffer != NULL && R_FAILED(socInit(s_socBuffer, s_socSize))) {
        free(s_socBuffer);
        s_socBuffer = NULL;
    }
    return true;
}

void Ctr_Shutdown(void)
{
    if (s_socBuffer != NULL) {
        socExit();
        free(s_socBuffer);
        s_socBuffer = NULL;
    }
    if (s_console) {
        s_console = false;
        gfxExit();
    }
}

void Ctr_Print(const char *line)
{
    if (s_console && !s_consolePaused) {
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
    // [3ds] debug_quit_after = seconds: quits then, as the HOME Menu's
    // close does (to test the exit in an emulator).
    static u64 s_quitAt = ~0ull;
    if (s_quitAt == ~0ull) {
        int seconds = Platform_GetSettingInt("3ds.debug_quit_after", 0);
        s_quitAt = seconds > 0 ? osGetTime() + (u64)seconds * 1000 : 0;
    }
    if (s_quitAt != 0 && osGetTime() >= s_quitAt) {
        static bool s_logged;
        if (!s_logged)
            Sys_Log("debug_quit_after: quitting");
        s_logged = true;
        return false;
    }
    hidScanInput();
    circlePosition circle;
    hidCircleRead(&circle);
    bool down[256];
    MapButtons(hidKeysHeld(), circle, (hidKeysHeld() & KEY_TOUCH) != 0, down);
    if (!s_driving && (hidKeysDown() & KEY_X))
        TypeText();
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

bool Ctr_TakeText(int *character)
{
    if (s_textCount == 0)
        return false;
    *character = s_text[s_textHead];
    s_textHead = (s_textHead + 1) % 128;
    s_textCount--;
    return true;
}

bool Ctr_IsKeyDown(int dik)
{
    return dik >= 0 && dik < 256 && s_down[dik];
}

void Ctr_PauseConsole(bool paused)
{
    if (s_consolePaused == paused)
        return;
    s_consolePaused = paused;
    if (!paused && s_console)
        consoleClear();
}

bool Ctr_IsDriving(void)
{
    return s_driving;
}

void Ctr_SetDrivingContext(bool driving)
{
    if (driving != s_driving)
        Sys_Log("input: %s keys", driving ? "driving" : "menu");
    s_driving = driving;
}
