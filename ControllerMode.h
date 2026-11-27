#pragma once

// Who gets the Steam Controller's input.
enum class ControllerMode {
    // The daemon turns presses into keyboard keys: the launcher and simple apps.
    Navigation,
    // The daemon passes everything on to a virtual Xbox 360 gamepad: apps that don't know
    // the 2026 controller themselves, like Steam Link.
    VirtualGamepad,
    // The app talks to the controller itself. The daemon only watches for presses that should
    // bring the TV back.
    Released,
};

constexpr const char* controllerModeName(const ControllerMode mode) {
    switch (mode) {
        case ControllerMode::Navigation:     return "Navigation";
        case ControllerMode::VirtualGamepad: return "VirtualGamepad";
        case ControllerMode::Released:       return "Released";
    }
    return "?";
}
