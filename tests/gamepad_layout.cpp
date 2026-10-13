// Gamepads reach the game as DirectInput showed an Xbox controller, with the
// triggers on their own axes, centred when released (input_sdl.cpp). Uses an
// SDL virtual Xbox controller, so no device or display is needed.

#include "port/types.h"
#include "port/input.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>

namespace {

int s_failures;

void Check(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        s_failures++;
    }
}

const LONG kRange = 1000;   // the game sets -range..range on each axis it uses

InputJoystickState Read(InputDevice *device)
{
    InputJoystickState state;
    Check(Input_ReadJoystick(device, &state) != FALSE, "joystick reads");
    return state;
}

} // namespace

int main()
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_JOYSTICK | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    // An Xbox 360 controller: SDL knows its layout, as for a real one.
    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.vendor_id = 0x045e;
    desc.product_id = 0x028e;
    desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
    desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    desc.name = "Virtual Xbox 360 Controller";
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    SDL_Joystick *virt = SDL_OpenJoystick(id);
    if (virt == NULL) {
        fprintf(stderr, "virtual joystick: %s\n", SDL_GetError());
        return 1;
    }
    // Released triggers rest at their minimum, as on real controllers.
    SDL_SetJoystickVirtualAxis(virt, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
    SDL_SetJoystickVirtualAxis(virt, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
    SDL_UpdateJoysticks();

    Check(Input_GetJoystickCount() == 1, "one joystick");
    InputDevice *device = Input_OpenJoystick(0);
    if (device == NULL) {
        fprintf(stderr, "Input_OpenJoystick failed\n");
        return 1;
    }
    InputCaps caps;
    Input_GetCaps(device, &caps);
    Check(caps.axes == 6 && caps.buttons == 10 && caps.povs == 1, "Xbox layout: 6 axes, 10 buttons, 1 POV");
    for (DWORD offset = 0; offset < 6 * 4; offset += 4)
        Input_SetAxisRange(device, offset, -kRange, kRange);

    InputJoystickState s = Read(device);
    Check(s.lZ == 0 && s.lRz == 0, "released triggers are centred");
    Check(s.lX == 0 && s.lY == 0 && s.lRx == 0 && s.lRy == 0, "sticks are centred");
    Check(s.rgdwPOV[0] == INPUT_POV_CENTERED, "D-pad centred");

    SDL_SetJoystickVirtualAxis(virt, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
    SDL_UpdateJoysticks();
    s = Read(device);
    Check(s.lRz == kRange && s.lZ == 0, "right trigger moves Rz alone");

    SDL_SetJoystickVirtualAxis(virt, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
    SDL_UpdateJoysticks();
    s = Read(device);
    Check(s.lZ == kRange && s.lRz == kRange, "both triggers held at once");

    SDL_SetJoystickVirtualAxis(virt, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
    SDL_UpdateJoysticks();
    s = Read(device);
    Check(s.lRz == 0 && s.lZ == kRange, "released right trigger returns to centre");

    SDL_SetJoystickVirtualAxis(virt, SDL_GAMEPAD_AXIS_LEFTX, SDL_JOYSTICK_AXIS_MIN);
    SDL_SetJoystickVirtualAxis(virt, SDL_GAMEPAD_AXIS_RIGHTY, SDL_JOYSTICK_AXIS_MAX);
    SDL_SetJoystickVirtualButton(virt, SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_SetJoystickVirtualButton(virt, SDL_GAMEPAD_BUTTON_START, true);
    SDL_SetJoystickVirtualButton(virt, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
    SDL_UpdateJoysticks();
    s = Read(device);
    Check(s.lX == -kRange && s.lRy == kRange, "left stick X and right stick Y");
    Check(s.rgbButtons[0] == 0x80 && s.rgbButtons[7] == 0x80 && s.rgbButtons[1] == 0, "A is button 0, Start button 7");
    Check(s.rgdwPOV[0] == 9000, "D-pad right is POV 90 degrees");

    char name[64];
    Check(Input_GetObjectName(device, 2 * 4, name, sizeof(name)) && strcmp(name, "Z Axis") == 0, "Z axis named");

    Input_CloseDevice(device);
    SDL_CloseJoystick(virt);
    SDL_DetachVirtualJoystick(id);
    SDL_Quit();
    if (s_failures == 0)
        puts("gamepad layout: triggers centred and separate, Xbox buttons and D-pad");
    return s_failures == 0 ? 0 : 1;
}
