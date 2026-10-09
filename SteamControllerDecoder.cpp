//
// Created by jeppe on 09.10.2026.
//

#include "SteamControllerDecoder.h"

namespace {
    struct ButtonPosition {
        ControllerButton button;
        uint8_t byteIndex;
        uint8_t bitIndex;
    };

    // Measured on my controller.
    constexpr ButtonPosition BUTTON_POSITIONS[] = {
        {ControllerButton::A,           2, 0},
        {ControllerButton::B,           2, 1},
        {ControllerButton::X,           2, 2},
        {ControllerButton::Y,           2, 3},
        {ControllerButton::Menu,        2, 6},
        {ControllerButton::RightBumper, 3, 1},
        {ControllerButton::DPadDown,    3, 2},
        {ControllerButton::DPadRight,   3, 3},
        {ControllerButton::DPadLeft,    3, 4},
        {ControllerButton::DPadUp,      3, 5},
        {ControllerButton::View,        3, 6},
        {ControllerButton::Steam,       4, 0},
        {ControllerButton::LeftBumper,  4, 3},
    };

    constexpr std::size_t LEFT_STICK_X_OFFSET = 10;   // signed 16-bit LE, right is positive
    constexpr std::size_t LEFT_STICK_Y_OFFSET = 12;   // signed 16-bit LE, up is positive

    // Hysteresis: press beyond 50%, release below 30%, so a stick near the edge doesn't flicker.
    constexpr int STICK_PRESS_THRESHOLD = 16384;
    constexpr int STICK_RELEASE_THRESHOLD = 9830;

    int readSigned16(const std::span<const uint8_t> report, const std::size_t offset) {
        return static_cast<int16_t>(report[offset] | (report[offset + 1] << 8));
    }

    bool isStickDirectionPressed(const int deflection, const bool wasPressed) {
        return deflection > (wasPressed ? STICK_RELEASE_THRESHOLD : STICK_PRESS_THRESHOLD);
    }
}

ControllerButtonSet SteamControllerDecoder::decode(const std::span<const uint8_t> report) {
    if (report.size() < REPORT_SIZE || report[0] != REPORT_ID) return 0;

    ControllerButtonSet currentButtons = 0;
    for (const auto&[button, byteIndex, bitIndex] : BUTTON_POSITIONS) {
        if ((report[byteIndex] >> bitIndex) & 1) {
            currentButtons |= buttonBit(button);
        }
    }

    // int, not int16_t: negating -32768 would overflow a 16-bit value.
    const int stickX = readSigned16(report, LEFT_STICK_X_OFFSET);
    const int stickY = readSigned16(report, LEFT_STICK_Y_OFFSET);
    const auto wasPressed = [this](const ControllerButton button) {
        return (pressedButtons_ & buttonBit(button)) != 0;
    };
    if (isStickDirectionPressed(stickX, wasPressed(ControllerButton::StickRight))) currentButtons |= buttonBit(ControllerButton::StickRight);
    if (isStickDirectionPressed(-stickX, wasPressed(ControllerButton::StickLeft))) currentButtons |= buttonBit(ControllerButton::StickLeft);
    if (isStickDirectionPressed(stickY, wasPressed(ControllerButton::StickUp)))    currentButtons |= buttonBit(ControllerButton::StickUp);
    if (isStickDirectionPressed(-stickY, wasPressed(ControllerButton::StickDown))) currentButtons |= buttonBit(ControllerButton::StickDown);

    const ControllerButtonSet newlyPressedButtons = currentButtons & ~pressedButtons_;
    pressedButtons_ = currentButtons;
    return newlyPressedButtons;
}
