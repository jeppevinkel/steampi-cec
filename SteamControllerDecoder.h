//
// Created by jeppe on 09.10.2026.
//

#pragma once

#include <cstdint>
#include <cstddef>
#include <span>

// Buttons of the 2026 Steam Controller as decoded from its 0x42 input report.
// The left stick's directions are included so it can navigate like a D-pad.
enum class ControllerButton : uint8_t {
    A, B, X, Y, Menu, View, Steam, LeftBumper, RightBumper,
    DPadUp, DPadDown, DPadLeft, DPadRight,
    StickUp, StickDown, StickLeft, StickRight,
};

using ControllerButtonSet = uint32_t;   // one bit per ControllerButton

constexpr ControllerButtonSet buttonBit(const ControllerButton button) {
    return ControllerButtonSet{1} << static_cast<uint8_t>(button);
}

class SteamControllerDecoder {
public:
    static constexpr uint8_t REPORT_ID = 0x42;
    static constexpr std::size_t REPORT_SIZE = 54;

    // Decodes one report and returns the buttons that went from released to pressed.
    // Reports with another ID or size are ignored (returns an empty set).
    ControllerButtonSet decode(std::span<const uint8_t> report);

    // Forget the held state, e.g. after the controller was handed to an app and back.
    void reset() { pressedButtons_ = 0; }

private:
    ControllerButtonSet pressedButtons_ = 0;
};