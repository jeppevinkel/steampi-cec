//
// Created by jeppe on 09.10.2026.
//

#include "ControllerRouter.h"

#include <cerrno>
#include <system_error>
#include <linux/input-event-codes.h>
#include <sys/timerfd.h>

#include "EventLoop.h"
#include "Timers.h"

namespace {

// Same feel as the TV remote: first repeat after ~350 ms, then every ~100 ms.
constexpr auto REPEAT_DELAY = std::chrono::milliseconds(350);
constexpr auto REPEAT_INTERVAL = std::chrono::milliseconds(100);
// The controller sends ~60 reports per second while on. Silence longer than this means
// it turned off, so held buttons from before are meaningless.
constexpr auto CONTROLLER_SILENCE_LIMIT = std::chrono::milliseconds(500);
// Holding B this long does what holding Return on the TV remote does: close the running app.
constexpr auto BACK_HOLD_DURATION = std::chrono::milliseconds(1000);

}  // namespace

int ControllerRouter::linuxKeyForControllerButton(const ControllerButton button) const {
    switch (button) {
        case ControllerButton::DPadUp:
        case ControllerButton::StickUp:    return KEY_UP;
        case ControllerButton::DPadDown:
        case ControllerButton::StickDown:  return KEY_DOWN;
        case ControllerButton::DPadLeft:
        case ControllerButton::StickLeft:  return KEY_LEFT;
        case ControllerButton::DPadRight:
        case ControllerButton::StickRight: return KEY_RIGHT;
        case ControllerButton::A:          return KEY_ENTER;
        case ControllerButton::B:          return backKeyCode_;
        default:                           return KEY_RESERVED;
    }
}

namespace {

bool isDirection(const ControllerButton button) {
    switch (button) {
        case ControllerButton::DPadUp:  case ControllerButton::DPadDown:
        case ControllerButton::DPadLeft: case ControllerButton::DPadRight:
        case ControllerButton::StickUp: case ControllerButton::StickDown:
        case ControllerButton::StickLeft: case ControllerButton::StickRight:
            return true;
        default:
            return false;
    }
}

const char* controllerButtonName(const ControllerButton button) {
    switch (button) {
        case ControllerButton::A:           return "A";
        case ControllerButton::B:           return "B";
        case ControllerButton::X:           return "X";
        case ControllerButton::Y:           return "Y";
        case ControllerButton::Menu:        return "Menu";
        case ControllerButton::View:        return "View";
        case ControllerButton::Steam:       return "Steam";
        case ControllerButton::LeftBumper:  return "LeftBumper";
        case ControllerButton::RightBumper: return "RightBumper";
        case ControllerButton::LeftStickClick:  return "LeftStickClick";
        case ControllerButton::RightStickClick: return "RightStickClick";
        case ControllerButton::DPadUp:      return "DPadUp";
        case ControllerButton::DPadDown:    return "DPadDown";
        case ControllerButton::DPadLeft:    return "DPadLeft";
        case ControllerButton::DPadRight:   return "DPadRight";
        case ControllerButton::StickUp:     return "StickUp";
        case ControllerButton::StickDown:   return "StickDown";
        case ControllerButton::StickLeft:   return "StickLeft";
        case ControllerButton::StickRight:  return "StickRight";
    }
    return "?";
}

}  // namespace

ControllerRouter::ControllerRouter(const int epollFileDescriptor,
                                           PowerStateMachine& powerStateMachine,
                                           VirtualKeyboard* virtualKeyboard,
                                           Logger& log,
                                           std::function<void()> backHeld)
    : powerStateMachine_(powerStateMachine),
      virtualKeyboard_(virtualKeyboard),
      log_(log),
      repeatTimerFileDescriptor_(timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)),
      backHeld_(std::move(backHeld)),
      backHoldTimerFileDescriptor_(timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)) {
    if (!repeatTimerFileDescriptor_.isValid()
        || !watchForInput(epollFileDescriptor, repeatTimerFileDescriptor_.get())) {
        throw std::system_error(errno, std::generic_category(), "controller repeat timer");
    }
    if (!backHoldTimerFileDescriptor_.isValid()
        || !watchForInput(epollFileDescriptor, backHoldTimerFileDescriptor_.get())) {
        throw std::system_error(errno, std::generic_category(), "controller back hold timer");
    }
}

void ControllerRouter::onReport(const std::span<const uint8_t> report) {
    const auto now = std::chrono::steady_clock::now();
    const bool controllerCameBack = hasReceivedReport_ && now - lastReportTime_ > CONTROLLER_SILENCE_LIMIT;
    hasReceivedReport_ = true;
    lastReportTime_ = now;

    if (controllerCameBack) {
        // Turning the controller on counts as a press: it wakes the TV from standby, or takes
        // it back from another input. The press that turned it on may never appear in a report.
        decoder_.reset();
        stopRepeating();
        cancelBackHold();
        log_.write("Controller: turned on");
        powerStateMachine_.onControllerButtonPressed(/*isTakeOverButton=*/false);
    }

    const ControllerButtonSet newlyPressedButtons = decoder_.decode(report);

    if (mode_ == ControllerMode::Navigation) {
        forEachButton(newlyPressedButtons,
                      [this](const ControllerButton button) { onNavigationButtonPressed(button); });
        if (repeatingButton_ && !(decoder_.pressedButtons() & buttonBit(*repeatingButton_))) {
            stopRepeating();
        }
        if (isTimingBackHold_ && !(decoder_.pressedButtons() & buttonBit(ControllerButton::B))) {
            cancelBackHold();   // let go before the hold counted
        }
        return;
    }

    // An app has the controller. Presses only matter here if the TV is off or showing another
    // input: then they bring it back. Never a take-over, so the Steam button stays the app's.
    if (newlyPressedButtons != 0) {
        powerStateMachine_.onControllerButtonPressed(/*isTakeOverButton=*/false);
    }
}

void ControllerRouter::setMode(const ControllerMode mode) {
    if (mode == mode_) return;

    stopRepeating();
    cancelBackHold();
    decoder_.reset();   // held buttons from the previous mode mean nothing in the new one
    mode_ = mode;
    log_.log("Controller: mode {}", controllerModeName(mode_));
}

void ControllerRouter::onNavigationButtonPressed(const ControllerButton button) {
    const bool isTakeOverButton = button == ControllerButton::Steam;
    if (powerStateMachine_.onControllerButtonPressed(isTakeOverButton)) {
        stopRepeating();   // used for waking or taking over, not navigation
        return;
    }

    if (button == ControllerButton::B) {
        // Timed even without a virtual keyboard: closing the app doesn't need one.
        isTimingBackHold_ = true;
        armTimer(backHoldTimerFileDescriptor_.get(), BACK_HOLD_DURATION);
    }

    const int linuxKeyCode = linuxKeyForControllerButton(button);
    if (linuxKeyCode == KEY_RESERVED || !virtualKeyboard_) return;

    log_.log("Controller: {} -> {}", controllerButtonName(button), linuxKeyCode);
    virtualKeyboard_->tap(linuxKeyCode);
    if (isDirection(button)) startRepeating(button, linuxKeyCode);
}

bool ControllerRouter::handleReadyFileDescriptor(const int readyFileDescriptor) {
    if (readyFileDescriptor == backHoldTimerFileDescriptor_.get()) {
        if (!consumeTimerExpiration(readyFileDescriptor) || !isTimingBackHold_) return true;
        isTimingBackHold_ = false;
        // Reports stopped while B was down (controller turned off): that's not a hold.
        if (std::chrono::steady_clock::now() - lastReportTime_ > CONTROLLER_SILENCE_LIMIT) return true;
        log_.write("Controller: B held");
        if (backHeld_) backHeld_();
        return true;
    }
    if (readyFileDescriptor != repeatTimerFileDescriptor_.get()) return false;
    if (!consumeTimerExpiration(readyFileDescriptor) || !repeatingButton_) return true;

    // Reports stopped while a direction was held (controller turned off): don't repeat forever.
    if (std::chrono::steady_clock::now() - lastReportTime_ > CONTROLLER_SILENCE_LIMIT) {
        stopRepeating();
        return true;
    }
    if (virtualKeyboard_) virtualKeyboard_->tap(repeatingKeyCode_);
    return true;
}

void ControllerRouter::startRepeating(const ControllerButton button, const int linuxKeyCode) {
    repeatingButton_ = button;
    repeatingKeyCode_ = linuxKeyCode;
    armTimer(repeatTimerFileDescriptor_.get(), REPEAT_DELAY, REPEAT_INTERVAL);
}

void ControllerRouter::stopRepeating() {
    repeatingButton_.reset();
    disarmTimer(repeatTimerFileDescriptor_.get());
}

void ControllerRouter::cancelBackHold() {
    isTimingBackHold_ = false;
    disarmTimer(backHoldTimerFileDescriptor_.get());
}
