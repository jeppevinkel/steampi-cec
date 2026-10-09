//
// Created by jeppe on 09.10.2026.
//

#include "VirtualKeyboard.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <system_error>
#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {
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
}

VirtualKeyboard::VirtualKeyboard(const std::span<const int> supportedKeyCodes)
    : uinputFileDescriptor_(open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC)) {
    // If anything below throws, FileDescriptor closes the device on the way out.
    if (!uinputFileDescriptor_.isValid()) throwSystemError("open /dev/uinput");

    const int uinput = uinputFileDescriptor_.get();
    if (ioctl(uinput, UI_SET_EVBIT, EV_KEY) < 0) throwSystemError("UI_SET_EVBIT");
    for (const int keyCode : supportedKeyCodes) {
        if (ioctl(uinput, UI_SET_KEYBIT, keyCode) < 0) throwSystemError("UI_SET_KEYBIT");
    }

    uinput_setup deviceSetup{};
    deviceSetup.id.bustype = BUS_VIRTUAL;
    std::strncpy(deviceSetup.name, "SteamPi Virtual Keyboard", UINPUT_MAX_NAME_SIZE - 1);
    if (ioctl(uinput, UI_DEV_SETUP, &deviceSetup) < 0) throwSystemError("UI_DEV_SETUP");
    if (ioctl(uinput, UI_DEV_CREATE) < 0) throwSystemError("UI_DEV_CREATE");
}

VirtualKeyboard::~VirtualKeyboard() {
    // Remove the device before FileDescriptor closes the file.
    ioctl(uinputFileDescriptor_.get(), UI_DEV_DESTROY);
}

void VirtualKeyboard::tap(const int keyCode) {
    const input_event tapEvents[] = {
        makeInputEvent(EV_KEY, static_cast<uint16_t>(keyCode), 1),
        makeInputEvent(EV_SYN, SYN_REPORT, 0),
        makeInputEvent(EV_KEY, static_cast<uint16_t>(keyCode), 0),
        makeInputEvent(EV_SYN, SYN_REPORT, 0),
    };
    // A single write is processed as a unit, so taps from different threads can't interleave.
    [[maybe_unused]] const auto bytesWritten =
        write(uinputFileDescriptor_.get(), tapEvents, sizeof tapEvents);
}
