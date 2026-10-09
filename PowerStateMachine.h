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

    void requestWake();          // later: controller press
    void requestStandby();       // later: launcher's standby button
    void onWakeTimerExpired();
    void onUnattendedAppTimerExpired();
    void onCecEvent(CecEvent event);
    bool onControllerButtonPressed(bool isTakeOverButton);

private:
    void enterActive();
    void enterStandby(bool sendStandbyToTv);
    void markTvShowingUs();
    void beginWaking(const char* reason);

    // My Samsung takes ~2.1 s from Image View On to 'on'.
    static constexpr auto TV_BOOT_DELAY = std::chrono::milliseconds(3000);
    static constexpr auto ACTIVE_SOURCE_RETRY_DELAY = std::chrono::milliseconds(2000);
    static constexpr int MAXIMUM_ACTIVE_SOURCE_ATTEMPTS = 3;
    static constexpr auto APP_GRACE_AFTER_TV_STANDBY = std::chrono::minutes(5);
    static constexpr auto APP_GRACE_AFTER_SOURCE_SWITCH = std::chrono::minutes(30);

    CEC::ICECAdapter* adapter_;
    int wakeTimerFileDescriptor_;
    int unattendedAppTimerFileDescriptor_;
    Logger& log_;
    PowerState state_ = PowerState::Standby;
    int activeSourceAttempts_ = 0;
    bool tvShowingOurInput_ = false;
};