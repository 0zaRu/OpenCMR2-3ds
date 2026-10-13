// Gamepads reach the game as DirectInput showed an Xbox controller, and
// triggers and pedals as DirectInput wheels report pedals: the maximum when
// released, the minimum when pressed fully (input_sdl.cpp). Uses SDL virtual
// devices, so no hardware or display is needed.

#include "port/types.h"
#include "port/input.h"
#include "Input.h"

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

    Check(Input_IsGamepad(device) != FALSE, "recognised as a gamepad");
    InputJoystickState s = Read(device);
    Check(s.lZ == kRange && s.lRz == kRange, "released triggers read as released pedals");
    Check(s.lX == 0 && s.lY == 0 && s.lRx == 0 && s.lRy == 0, "sticks are centred");
    Check(s.rgdwPOV[0] == INPUT_POV_CENTERED, "D-pad centred");

    SDL_SetJoystickVirtualAxis(virt, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
    SDL_UpdateJoysticks();
    s = Read(device);
    Check(s.lRz == -kRange && s.lZ == kRange, "right trigger moves Rz alone");

    SDL_SetJoystickVirtualAxis(virt, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
    SDL_UpdateJoysticks();
    s = Read(device);
    Check(s.lZ == -kRange && s.lRz == -kRange, "both triggers held at once");

    SDL_SetJoystickVirtualAxis(virt, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
    SDL_UpdateJoysticks();
    s = Read(device);
    Check(s.lRz == kRange && s.lZ == -kRange, "released right trigger is released again");

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

    // The game's own detection gives a gamepad throttle on the right trigger
    // (Rz, axis 5) and brake on the left (Z, axis 2), as separate pedals.
    CInput::GetAttachedJoysticks();
    DeviceInfo *pad = NULL;
    for (int i = 0; i < 8 && pad == NULL; i++)
        if (CInput::m_availableDevices[i].field_0x0 == 3)
            pad = &CInput::m_availableDevices[i];
    Check(pad != NULL, "the game set up the gamepad");
    if (pad != NULL) {
        CInput::InitDetectedControllerSlot(pad, 2);
        const ControllerData &slot = CInput::m_controllerInfo[2];
        Check(slot.field_0x210[0].field_0x0 != 0 && slot.field_0x2d8[0] == 0, "default steering on the left stick");
        Check(slot.field_0x210[2].field_0x0 != 0 && slot.field_0x2d8[2] == 5, "default throttle on the right trigger");
        Check(slot.field_0x210[3].field_0x0 != 0 && slot.field_0x2d8[3] == 2, "default brake on the left trigger");
        Input_CloseDevice(CInput::m_unk0x0059f6b0[pad->field_0x18]);
        CInput::m_unk0x0059f6b0[pad->field_0x18] = NULL;
    }

    SDL_CloseJoystick(virt);
    SDL_DetachVirtualJoystick(id);

    // A wheel whose pedal axis rests at its minimum, as some report it.
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_WHEEL;
    desc.naxes = 3;
    desc.name = "Virtual Wheel";
    id = SDL_AttachVirtualJoystick(&desc);
    virt = SDL_OpenJoystick(id);
    SDL_SetJoystickVirtualAxis(virt, 2, SDL_JOYSTICK_AXIS_MIN);
    SDL_UpdateJoysticks();
    device = Input_OpenJoystick(0);
    if (device == NULL) {
        fprintf(stderr, "Input_OpenJoystick failed for the wheel\n");
        return 1;
    }
    Check(Input_IsGamepad(device) == FALSE, "a wheel is not a gamepad");
    for (DWORD offset = 0; offset < 3 * 4; offset += 4)
        Input_SetAxisRange(device, offset, -kRange, kRange);
    s = Read(device);
    Check(s.lZ == kRange && s.lX == 0, "released pedal reads released, wheel centred");
    SDL_SetJoystickVirtualAxis(virt, 2, SDL_JOYSTICK_AXIS_MAX);
    SDL_UpdateJoysticks();
    s = Read(device);
    Check(s.lZ == -kRange, "pressed pedal reads pressed");
    Input_CloseDevice(device);
    SDL_CloseJoystick(virt);
    SDL_DetachVirtualJoystick(id);

    SDL_Quit();
    if (s_failures == 0)
        puts("gamepad layout: triggers and pedals read as wheel pedals, Xbox buttons and D-pad");
    return s_failures == 0 ? 0 : 1;
}
