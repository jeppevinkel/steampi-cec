#pragma once

// Who gets the Steam Controller's input.
enum class ControllerMode {
    // The daemon turns presses into keyboard keys: the launcher and simple apps.
    Navigation,
    // The app talks to the controller itself (Steam Link does, through hidraw). The daemon lets go
    // of lizard mode and only watches for presses that should bring the TV back.
    Released,
};

constexpr const char* controllerModeName(const ControllerMode mode) {
    switch (mode) {
        case ControllerMode::Navigation: return "Navigation";
        case ControllerMode::Released:   return "Released";
    }
    return "?";
}
