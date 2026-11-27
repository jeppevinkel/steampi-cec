#pragma once

#include <vector>
#include <linux/input.h>

#include "FileDescriptor.h"
#include "SteamControllerDecoder.h"

// A virtual Xbox 360 gamepad created through /dev/uinput, fed from the Steam Controller.
// It reports the same IDs and event codes as the kernel's xpad driver, so SDL (Steam Link)
// recognises and maps it without any configuration.
class VirtualGamepad {
public:
    // Throws std::system_error if /dev/uinput can't be opened or set up.
    VirtualGamepad();
    ~VirtualGamepad();

    VirtualGamepad(const VirtualGamepad&) = delete;
    VirtualGamepad& operator=(const VirtualGamepad&) = delete;

    void update(const ControllerState& state);

private:
    FileDescriptor uinputFileDescriptor_;
};

// The input events (ending in a SYN_REPORT) that describe this state. Separate from the
// device so it can be tested without /dev/uinput.
std::vector<input_event> buildGamepadEvents(const ControllerState& state);
