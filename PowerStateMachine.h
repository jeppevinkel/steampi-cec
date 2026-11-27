//
// Created by jeppe on 09.10.2026.
//

#pragma once

#include <chrono>
#include <libcec/cec.h>

#include "CecEventQueue.h"
#include "Logger.h"

enum class PowerState { Standby, Waking, Active };

// All methods must be called from the main (epoll) thread only.
class PowerStateMachine {
public:
    PowerStateMachine(CEC::ICECAdapter* adapter, int wakeTimerFileDescriptor,
                      int unattendedAppTimerFileDescriptor, Logger& log);

    // Called once after the CEC adapter is open. Never turns the TV on: an update or reboot at
    // night shouldn't light up the living room. If the TV is already on, we take the source.
    void startUp();
    void requestWake();          // controller press
    void requestStandby();       // later: launcher's standby button
    void onWakeTimerExpired();
    void onUnattendedAppTimerExpired();
    void onCecEvent(CecEvent event);
    bool onControllerButtonPressed(bool isTakeOverButton);

private:
    void enterActive();
    void enterStandby(bool sendStandbyToTv);
    void markTvShowingUs();
    void beginWaking(const char* reason, bool tvAlreadyOn);

    // My Samsung takes ~2.1 s from Image View On to 'on'.
    static constexpr auto TV_BOOT_DELAY = std::chrono::milliseconds(3000);
    static constexpr auto ACTIVE_SOURCE_RETRY_DELAY = std::chrono::milliseconds(2000);
    static constexpr int MAXIMUM_ACTIVE_SOURCE_ATTEMPTS = 3;
    static constexpr auto APP_GRACE_AFTER_TV_STANDBY = std::chrono::minutes(5);
    static constexpr auto APP_GRACE_AFTER_SOURCE_SWITCH = std::chrono::minutes(30);
    // Our Samsung only confirms (asks for deck status) when it actually switches to us. A TV that's
    // already on hears our first message, so silence after this long means it already shows us.
    static constexpr auto TV_ALREADY_ON_CONFIRMATION_WAIT = std::chrono::milliseconds(1000);

    CEC::ICECAdapter* adapter_;
    int wakeTimerFileDescriptor_;
    int unattendedAppTimerFileDescriptor_;
    Logger& log_;
    PowerState state_ = PowerState::Standby;
    int activeSourceAttempts_ = 0;
    bool tvShowingOurInput_ = false;
    int maximumActiveSourceAttempts_ = MAXIMUM_ACTIVE_SOURCE_ATTEMPTS;
    std::chrono::milliseconds activeSourceRetryDelay_ = ACTIVE_SOURCE_RETRY_DELAY;
};