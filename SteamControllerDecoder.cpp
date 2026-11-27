//
// Created by jeppe on 09.10.2026.
//

#include "SteamControllerDecoder.h"

#include <algorithm>

namespace {
    struct ButtonPosition {
        ControllerButton button;
        uint8_t byteIndex;
        uint8_t bitIndex;
    };

    // Measured on my controller with pitv-diag. Bytes 2-5 are a 32-bit button field; the stick
    // clicks weren't measured and come from Valve's own driver (SDL's SDL_hidapi_steam_triton.c),
    // which agrees with every bit we did measure.
    constexpr ButtonPosition BUTTON_POSITIONS[] = {
        {ControllerButton::A,               2, 0},
        {ControllerButton::B,               2, 1},
        {ControllerButton::X,               2, 2},
        {ControllerButton::Y,               2, 3},
        {ControllerButton::RightStickClick, 2, 5},
        {ControllerButton::Menu,            2, 6},
        {ControllerButton::RightBumper,     3, 1},
        {ControllerButton::DPadDown,        3, 2},
        {ControllerButton::DPadRight,       3, 3},
        {ControllerButton::DPadLeft,        3, 4},
        {ControllerButton::DPadUp,          3, 5},
        {ControllerButton::View,            3, 6},
        {ControllerButton::LeftStickClick,  3, 7},
        {ControllerButton::Steam,           4, 0},
        {ControllerButton::LeftBumper,      4, 3},
    };

    // Signed 16-bit little-endian values (Valve's TritonMTUFull_t, after the report ID).
    constexpr std::size_t LEFT_TRIGGER_OFFSET = 6;    // 0 to 32767
    constexpr std::size_t RIGHT_TRIGGER_OFFSET = 8;
    constexpr std::size_t LEFT_STICK_X_OFFSET = 10;   // right is positive (measured)
    constexpr std::size_t LEFT_STICK_Y_OFFSET = 12;   // up is positive (measured)
    constexpr std::size_t RIGHT_STICK_X_OFFSET = 14;
    constexpr std::size_t RIGHT_STICK_Y_OFFSET = 16;

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

std::optional<ControllerState> SteamControllerDecoder::readState(const std::span<const uint8_t> report) {
    if (report.size() < REPORT_SIZE || report[0] != REPORT_ID) return std::nullopt;

    ControllerState state;
    for (const auto&[button, byteIndex, bitIndex] : BUTTON_POSITIONS) {
        if ((report[byteIndex] >> bitIndex) & 1) {
            state.buttons |= buttonBit(button);
        }
    }
    state.leftTrigger = std::max(0, readSigned16(report, LEFT_TRIGGER_OFFSET));
    state.rightTrigger = std::max(0, readSigned16(report, RIGHT_TRIGGER_OFFSET));
    state.leftStickX = readSigned16(report, LEFT_STICK_X_OFFSET);
    state.leftStickY = readSigned16(report, LEFT_STICK_Y_OFFSET);
    state.rightStickX = readSigned16(report, RIGHT_STICK_X_OFFSET);
    state.rightStickY = readSigned16(report, RIGHT_STICK_Y_OFFSET);
    return state;
}

ControllerButtonSet SteamControllerDecoder::decode(const std::span<const uint8_t> report) {
    const std::optional<ControllerState> state = readState(report);
    if (!state) return 0;

    ControllerButtonSet currentButtons = state->buttons;

    // int, not int16_t: negating -32768 would overflow a 16-bit value.
    const int stickX = state->leftStickX;
    const int stickY = state->leftStickY;
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
