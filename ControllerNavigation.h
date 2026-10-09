//
// Created by jeppe on 09.10.2026.
//

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>

#include "FileDescriptor.h"
#include "Logger.h"
#include "PowerStateMachine.h"
#include "SteamControllerDecoder.h"
#include "VirtualKeyboard.h"

// Turns controller reports into navigation: waking or taking over the TV through the
// power state machine, or key taps on the virtual keyboard, with key repeat for directions.
// TODO: hand the controller off to Steam Link while it runs (needs the lease); until then,
//  presses inside Steam Link also arrive as arrow and Enter keys.
class ControllerNavigation {
public:
    // virtualKeyboard may be null (uinput unavailable): waking still works, keys are skipped.
    // Throws std::system_error if the repeat timer can't be created.
    ControllerNavigation(int epollFileDescriptor, PowerStateMachine& powerStateMachine,
                         VirtualKeyboard* virtualKeyboard, Logger& log);

    void onReport(std::span<const uint8_t> report);

    // Returns true if the file descriptor was the repeat timer (and was handled).
    bool handleReadyFileDescriptor(int readyFileDescriptor);

private:
    void onButtonPressed(ControllerButton button);
    void startRepeating(ControllerButton button, int linuxKeyCode);
    void stopRepeating();

    PowerStateMachine& powerStateMachine_;
    VirtualKeyboard* virtualKeyboard_;
    Logger& log_;
    SteamControllerDecoder decoder_;
    FileDescriptor repeatTimerFileDescriptor_;
    std::optional<ControllerButton> repeatingButton_;
    int repeatingKeyCode_ = 0;
    bool hasReceivedReport_ = false;
    std::chrono::steady_clock::time_point lastReportTime_;
};
