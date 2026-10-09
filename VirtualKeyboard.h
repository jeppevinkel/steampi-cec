//
// Created by jeppe on 09.10.2026.
//

#pragma once

#include <span>

#include "FileDescriptor.h"

// A virtual keyboard created through /dev/uinput. X picks it up like any other keyboard.
class VirtualKeyboard {
public:
    // Throws std::system_error if /dev/uinput can't be opened or set up.
    explicit VirtualKeyboard(std::span<const int> supportedKeyCodes);
    ~VirtualKeyboard();

    VirtualKeyboard(const VirtualKeyboard&) = delete;
    VirtualKeyboard& operator=(const VirtualKeyboard&) = delete;

    // Press and release. Safe from any thread: each tap is a single write.
    void tap(int keyCode);

private:
    FileDescriptor uinputFileDescriptor_;
};
