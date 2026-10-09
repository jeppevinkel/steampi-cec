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
    PowerStateMachine(CEC::ICECAdapter* adapter, int wakeTimerFileDescriptor, Logger& log);

    void requestWake();          // later: controller press
    void requestStandby();       // later: launcher's standby button
    void onWakeTimerExpired();
    void onCecEvent(CecEvent event);
    bool onControllerButtonPressed(bool isTakeOverButton);

private:
    void enterActive();
    void enterStandby(bool sendStandbyToTv);
    void armWakeTimer(std::chrono::milliseconds delay);   // delay must be > 0
    void disarmWakeTimer();
    void beginWaking(const char* reason);

    // My Samsung takes ~2.1 s from Image View On to 'on'.
    static constexpr auto TV_BOOT_DELAY = std::chrono::milliseconds(3000);
    static constexpr auto ACTIVE_SOURCE_RETRY_DELAY = std::chrono::milliseconds(2000);
    static constexpr int MAXIMUM_ACTIVE_SOURCE_ATTEMPTS = 3;

    CEC::ICECAdapter* adapter_;
    int wakeTimerFileDescriptor_;
    Logger& log_;
    PowerState state_ = PowerState::Standby;
    int activeSourceAttempts_ = 0;
    bool tvShowingOurInput_ = false;
};