//
// Created by jeppe on 09.10.2026.
//

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <linux/input-event-codes.h>

#include "ControllerMode.h"
#include "FileDescriptor.h"
#include "Logger.h"
#include "PowerStateMachine.h"
#include "SteamControllerDecoder.h"
#include "VirtualKeyboard.h"

// Routes controller reports according to the current ControllerMode:
// - Navigation: waking or taking over the TV through the power state machine, or key taps on
//   the virtual keyboard, with key repeat for directions.
// - Released: the app reads the controller itself. Presses still bring the TV back if it's off
//   or showing another input.
class ControllerRouter {
public:
    // virtualKeyboard may be null (uinput unavailable): waking still works, keys are skipped.
    // backHeld runs when B is held in Navigation mode, like Return held on the TV remote.
    // Throws std::system_error if a timer can't be created.
    ControllerRouter(int epollFileDescriptor, PowerStateMachine& powerStateMachine,
                     VirtualKeyboard* virtualKeyboard, Logger& log, std::function<void()> backHeld);

    void onReport(std::span<const uint8_t> report);

    // Returns true if the file descriptor was one of the router's timers (and was handled).
    bool handleReadyFileDescriptor(int readyFileDescriptor);

    void setMode(ControllerMode mode);
    [[nodiscard]] ControllerMode mode() const { return mode_; }

    // The key B sends: Backspace for Flex Launcher; a running app may want another one.
    void setBackKeyCode(int linuxKeyCode) { backKeyCode_ = linuxKeyCode; }

private:
    [[nodiscard]] int linuxKeyForControllerButton(ControllerButton button) const;
    void onNavigationButtonPressed(ControllerButton button);
    void startRepeating(ControllerButton button, int linuxKeyCode);
    void stopRepeating();
    void cancelBackHold();

    PowerStateMachine& powerStateMachine_;
    VirtualKeyboard* virtualKeyboard_;
    Logger& log_;
    ControllerMode mode_ = ControllerMode::Navigation;
    SteamControllerDecoder decoder_;
    FileDescriptor repeatTimerFileDescriptor_;
    std::function<void()> backHeld_;
    FileDescriptor backHoldTimerFileDescriptor_;
    bool isTimingBackHold_ = false;
    std::optional<ControllerButton> repeatingButton_;
    int repeatingKeyCode_ = 0;
    int backKeyCode_ = KEY_BACKSPACE;
    bool hasReceivedReport_ = false;
    std::chrono::steady_clock::time_point lastReportTime_;
};
