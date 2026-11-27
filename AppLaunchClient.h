#pragma once

#include <string_view>

// steampi-cec --launch <profile>, run by Flex Launcher inside the TV session.
// Asks the daemon to hand the controller over, runs the app, forwards its output to the daemon,
// and closes the app when the daemon asks. If the daemon is unreachable or restarts, the app
// keeps running and the client reconnects in the background.
// Returns the app's exit code, or 1 (refused) or 2 (unknown profile) if it never started.
int runAppLaunchClient(std::string_view profileName);
