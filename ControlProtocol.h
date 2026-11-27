#pragma once

#include <cstdlib>
#include <string>
#include <string_view>

// The daemon's control socket. Messages are text lines ending in '\n'.
//
//   launch client -> daemon   LAUNCH <profile>   start a lease for this app profile
//   daemon -> launch client   GO                 controller handed over; start the app
//   daemon -> launch client   DENIED <reason>    unknown profile, or another app is running
//   daemon -> launch client   TERMINATE          close the app (standby, grace period, remote)
//   launch client -> daemon   OUTPUT <text>      a line the app printed
//   launch client -> daemon   EXITED <code>      the app has exited
//
// The lease lasts as long as the connection: when it closes for any reason, the daemon takes
// the controller back.
namespace ControlProtocol {

// /run/steampi is created by systemd for the service (RuntimeDirectory=steampi).
inline constexpr std::string_view DEFAULT_SOCKET_PATH = "/run/steampi/control.sock";
// Lets tests run a daemon and a client somewhere else.
inline constexpr const char* SOCKET_PATH_ENVIRONMENT_VARIABLE = "STEAMPI_CONTROL_SOCKET";

inline constexpr std::string_view LAUNCH = "LAUNCH";
inline constexpr std::string_view GO = "GO";
inline constexpr std::string_view DENIED = "DENIED";
inline constexpr std::string_view TERMINATE = "TERMINATE";
inline constexpr std::string_view OUTPUT = "OUTPUT";
inline constexpr std::string_view EXITED = "EXITED";

inline std::string socketPath() {
    if (const char* overridePath = std::getenv(SOCKET_PATH_ENVIRONMENT_VARIABLE);
        overridePath != nullptr && *overridePath != '\0') {
        return overridePath;
    }
    return std::string(DEFAULT_SOCKET_PATH);
}

// "LAUNCH steamlink" -> {"LAUNCH", "steamlink"}; "GO" -> {"GO", ""}.
struct Message {
    std::string_view word;
    std::string_view argument;
};

inline Message parseMessage(const std::string_view line) {
    const auto spacePosition = line.find(' ');
    if (spacePosition == std::string_view::npos) return {line, {}};
    return {line.substr(0, spacePosition), line.substr(spacePosition + 1)};
}

}  // namespace ControlProtocol
