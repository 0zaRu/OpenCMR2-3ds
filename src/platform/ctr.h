// Nintendo 3DS services of the platform layer (libctru): the screens before
// the renderer starts, the log on the bottom screen and the console's
// buttons, which press the keyboard keys the game reads.
#ifndef OPENCMR2_CTR_H
#define OPENCMR2_CTR_H

// Speeds up the CPU on a New 3DS, starts the screens and the bottom-screen log.
bool Ctr_Init(void);
void Ctr_Shutdown(void);

// A line on the bottom-screen log.
void Ctr_Print(const char *line);
// Shows a message on the bottom screen until A or START is pressed (or the
// system asks the game to close).
void Ctr_ShowMessage(const char *title, const char *text);

// Reads the buttons and the system state; false when the game should close.
bool Ctr_Update(void);
// Key presses since the last call, as DirectInput scan codes (with repeats
// in menus).
bool Ctr_TakeKeyEvent(int *dik, bool *repeat);
bool Ctr_IsKeyDown(int dik);
// Driving on a stage, or in a menu: the buttons press different keys.
void Ctr_SetDrivingContext(bool driving);

// The watchdog: when no frame has been shown for a few seconds, the log
// gets what the game was last doing (the breadcrumb), every few seconds.
void Ctr_StartWatchdog(void);
void Ctr_FrameShown(void);
void Ctr_Breadcrumb(const char *format, ...);

// render/gfx_c3d.cpp: 3D draws with the minimal vertex shader (the GPU test).
void Gfx_CtrSetMinimalShader(bool on);

#endif
