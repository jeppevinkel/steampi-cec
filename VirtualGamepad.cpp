#include "VirtualGamepad.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <system_error>
#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {
    // A wired Xbox 360 controller, as the kernel's xpad driver reports it.
    constexpr uint16_t XBOX_360_VENDOR_ID = 0x045e;
    constexpr uint16_t XBOX_360_PRODUCT_ID = 0x028e;
    constexpr uint16_t XBOX_360_VERSION = 0x0110;

    struct ButtonCode {
        ControllerButton button;
        uint16_t linuxCode;
    };

    // Same codes as xpad: X and Y use BTN_X and BTN_Y (which are BTN_NORTH and BTN_WEST).
    constexpr ButtonCode BUTTON_CODES[] = {
        {ControllerButton::A,               BTN_A},
        {ControllerButton::B,               BTN_B},
        {ControllerButton::X,               BTN_X},
        {ControllerButton::Y,               BTN_Y},
        {ControllerButton::LeftBumper,      BTN_TL},
        {ControllerButton::RightBumper,     BTN_TR},
        {ControllerButton::View,            BTN_SELECT},   // two squares, left of the Steam button
        {ControllerButton::Menu,            BTN_START},    // three lines, right of the Steam button
        {ControllerButton::Steam,           BTN_MODE},
        {ControllerButton::LeftStickClick,  BTN_THUMBL},
        {ControllerButton::RightStickClick, BTN_THUMBR},
    };

    constexpr int STICK_MINIMUM = -32768;
    constexpr int STICK_MAXIMUM = 32767;
    constexpr int TRIGGER_MAXIMUM = 32767;

    [[noreturn]] void throwSystemError(const char* operation) {
        throw std::system_error(errno, std::generic_category(), operation);
    }

    input_event makeInputEvent(const uint16_t type, const uint16_t code, const int32_t value) {
        input_event inputEvent{};
        inputEvent.type = type;
        inputEvent.code = code;
        inputEvent.value = value;
        return inputEvent;
    }

    bool isPressed(const ControllerState& state, const ControllerButton button) {
        return (state.buttons & buttonBit(button)) != 0;
    }

    // Our sticks count up as positive; evdev (and xpad) count down as positive.
    int flipVertical(const int stickY) {
        return std::clamp(-stickY, STICK_MINIMUM, STICK_MAXIMUM);
    }

    void setUpAxis(const int uinput, const uint16_t axisCode, const int minimum, const int maximum,
                   const int fuzz, const int flat) {
        uinput_abs_setup axisSetup{};
        axisSetup.code = axisCode;
        axisSetup.absinfo.minimum = minimum;
        axisSetup.absinfo.maximum = maximum;
        axisSetup.absinfo.fuzz = fuzz;
        axisSetup.absinfo.flat = flat;
        if (ioctl(uinput, UI_SET_ABSBIT, axisCode) < 0) throwSystemError("UI_SET_ABSBIT");
        if (ioctl(uinput, UI_ABS_SETUP, &axisSetup) < 0) throwSystemError("UI_ABS_SETUP");
    }
}

std::vector<input_event> buildGamepadEvents(const ControllerState& state) {
    std::vector<input_event> events;
    for (const auto& [button, linuxCode] : BUTTON_CODES) {
        events.push_back(makeInputEvent(EV_KEY, linuxCode, isPressed(state, button) ? 1 : 0));
    }

    // The D-pad is a hat, like on xpad: -1/0/1 on each axis, down and right positive.
    const int hatX = (isPressed(state, ControllerButton::DPadRight) ? 1 : 0)
                     - (isPressed(state, ControllerButton::DPadLeft) ? 1 : 0);
    const int hatY = (isPressed(state, ControllerButton::DPadDown) ? 1 : 0)
                     - (isPressed(state, ControllerButton::DPadUp) ? 1 : 0);
    events.push_back(makeInputEvent(EV_ABS, ABS_HAT0X, hatX));
    events.push_back(makeInputEvent(EV_ABS, ABS_HAT0Y, hatY));

    events.push_back(makeInputEvent(EV_ABS, ABS_X, state.leftStickX));
    events.push_back(makeInputEvent(EV_ABS, ABS_Y, flipVertical(state.leftStickY)));
    events.push_back(makeInputEvent(EV_ABS, ABS_RX, state.rightStickX));
    events.push_back(makeInputEvent(EV_ABS, ABS_RY, flipVertical(state.rightStickY)));
    events.push_back(makeInputEvent(EV_ABS, ABS_Z, std::clamp(state.leftTrigger, 0, TRIGGER_MAXIMUM)));
    events.push_back(makeInputEvent(EV_ABS, ABS_RZ, std::clamp(state.rightTrigger, 0, TRIGGER_MAXIMUM)));

    events.push_back(makeInputEvent(EV_SYN, SYN_REPORT, 0));
    return events;
}

VirtualGamepad::VirtualGamepad()
    : uinputFileDescriptor_(open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC)) {
    // If anything below throws, FileDescriptor closes the device on the way out.
    if (!uinputFileDescriptor_.isValid()) throwSystemError("open /dev/uinput");

    const int uinput = uinputFileDescriptor_.get();
    if (ioctl(uinput, UI_SET_EVBIT, EV_KEY) < 0) throwSystemError("UI_SET_EVBIT EV_KEY");
    if (ioctl(uinput, UI_SET_EVBIT, EV_ABS) < 0) throwSystemError("UI_SET_EVBIT EV_ABS");
    for (const auto& [button, linuxCode] : BUTTON_CODES) {
        if (ioctl(uinput, UI_SET_KEYBIT, linuxCode) < 0) throwSystemError("UI_SET_KEYBIT");
    }

    // Small fuzz and flat (dead zone), like xpad's sticks; games add their own dead zones.
    setUpAxis(uinput, ABS_X, STICK_MINIMUM, STICK_MAXIMUM, 16, 128);
    setUpAxis(uinput, ABS_Y, STICK_MINIMUM, STICK_MAXIMUM, 16, 128);
    setUpAxis(uinput, ABS_RX, STICK_MINIMUM, STICK_MAXIMUM, 16, 128);
    setUpAxis(uinput, ABS_RY, STICK_MINIMUM, STICK_MAXIMUM, 16, 128);
    setUpAxis(uinput, ABS_Z, 0, TRIGGER_MAXIMUM, 0, 0);
    setUpAxis(uinput, ABS_RZ, 0, TRIGGER_MAXIMUM, 0, 0);
    setUpAxis(uinput, ABS_HAT0X, -1, 1, 0, 0);
    setUpAxis(uinput, ABS_HAT0Y, -1, 1, 0, 0);

    uinput_setup deviceSetup{};
    deviceSetup.id.bustype = BUS_USB;
    deviceSetup.id.vendor = XBOX_360_VENDOR_ID;
    deviceSetup.id.product = XBOX_360_PRODUCT_ID;
    deviceSetup.id.version = XBOX_360_VERSION;
    std::strncpy(deviceSetup.name, "SteamPi Steam Controller (Xbox 360 layout)", UINPUT_MAX_NAME_SIZE - 1);
    if (ioctl(uinput, UI_DEV_SETUP, &deviceSetup) < 0) throwSystemError("UI_DEV_SETUP");
    if (ioctl(uinput, UI_DEV_CREATE) < 0) throwSystemError("UI_DEV_CREATE");
}

VirtualGamepad::~VirtualGamepad() {
    // Remove the device before FileDescriptor closes the file. Apps see the gamepad disconnect.
    ioctl(uinputFileDescriptor_.get(), UI_DEV_DESTROY);
}

void VirtualGamepad::update(const ControllerState& state) {
    const std::vector<input_event> events = buildGamepadEvents(state);
    // The input core drops values that didn't change, so sending the full state every report
    // (about 60 per second) costs apps nothing.
    [[maybe_unused]] const auto bytesWritten = write(uinputFileDescriptor_.get(), events.data(),
                                                     events.size() * sizeof(input_event));
}
