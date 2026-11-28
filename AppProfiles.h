#pragma once

#include <string_view>

#include "ControllerMode.h"

// An app that Flex Launcher starts through the daemon (steampi-cec --launch <name>).
struct AppProfile {
    std::string_view name;
    // Run with /bin/sh -c inside the TV session, so the app gets the session's environment.
    std::string_view command;
    ControllerMode controllerMode;
    // Send the app's output to the daemon's log, and with it to ntfy.
    bool forwardOutput;
};

inline constexpr AppProfile APP_PROFILES[] = {
    // Steam Link supports the 2026 controller itself: it opens the puck's hidraw devices, switches
    // lizard mode off and streams the controller to Steam on the PC (trackpads and gyro included).
    {"steamlink", "exec /usr/bin/steamlink", ControllerMode::Released, true},
    // Jellyfin Desktop in its TV layout. The controller stays with the daemon: Jellyfin navigates
    // with the arrow keys, Enter and Backspace (back), the same keys the TV remote sends.
    {"jellyfin", "exec /usr/bin/jellyfin-desktop --profile steampi --tv --fullscreen",
     ControllerMode::Navigation, true},
    {"test-pattern",
     "exec mpv --fullscreen --no-osc --no-input-default-bindings"
     " 'av://lavfi:testsrc=duration=5:size=1920x1080:rate=30'",
     ControllerMode::Navigation, true},
};

inline const AppProfile* findAppProfile(const std::string_view name) {
    for (const AppProfile& profile : APP_PROFILES) {
        if (profile.name == name) return &profile;
    }
    return nullptr;
}
