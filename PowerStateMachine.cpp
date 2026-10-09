//
// Created by jeppe on 09.10.2026.
//

#include "PowerStateMachine.h"

#include <sys/timerfd.h>

PowerStateMachine::PowerStateMachine(CEC::ICECAdapter* adapter,
                                     const int wakeTimerFileDescriptor,
                                     Logger& log)
    : adapter_(adapter), wakeTimerFileDescriptor_(wakeTimerFileDescriptor), log_(log) {}

void PowerStateMachine::requestWake() {
    if (state_ != PowerState::Standby) return;

    // Must be read BEFORE PowerOnDevices: afterwards libcec reports 'in transition'.
    const auto tvPowerStatus = adapter_->GetDevicePowerStatus(CEC::CECDEVICE_TV);
    log_.log("TV power status before wake: {}", adapter_->ToString(tvPowerStatus));

    const bool poweredOn = adapter_->PowerOnDevices(CEC::CECDEVICE_TV);
    log_.log("Power on TV: {}", poweredOn ? "ok" : "FAILED");

    state_ = PowerState::Waking;
    activeSourceAttempts_ = 0;
    log_.write("State: Waking");

    if (tvPowerStatus == CEC::CEC_POWER_STATUS_ON) {
        onWakeTimerExpired();              // TV already up: grab the source right away
    } else {
        armWakeTimer(TV_BOOT_DELAY);       // give the TV time to boot first
    }
}

void PowerStateMachine::requestStandby() {
    if (state_ != PowerState::Standby) enterStandby(/*sendStandbyToTv=*/true);
}

void PowerStateMachine::onWakeTimerExpired() {
    if (state_ != PowerState::Waking) return;

    if (activeSourceAttempts_ >= MAXIMUM_ACTIVE_SOURCE_ATTEMPTS) {
        log_.write("TV never confirmed Active Source; assuming active");
        enterActive();
        return;
    }

    ++activeSourceAttempts_;
    const bool activeSourceSent = adapter_->SetActiveSource(CEC::CEC_DEVICE_TYPE_PLAYBACK_DEVICE);
    log_.log("Active Source attempt {}: sent={}", activeSourceAttempts_, activeSourceSent);

    armWakeTimer(ACTIVE_SOURCE_RETRY_DELAY);   // retry unless the TV confirms first
}

void PowerStateMachine::onCecEvent(const CecEvent event) {
    log_.log("Event: {}", cecEventName(event));

    switch (event) {
        case CecEvent::TvStandby:
            // The TV is already off: transition locally, don't send standby back.
            if (state_ != PowerState::Standby) enterStandby(/*sendStandbyToTv=*/false);
            break;

        case CecEvent::TvQueriedDeckStatus:
            // Our Samsung's confirmation that it accepted our Active Source.
            if (state_ == PowerState::Waking) enterActive();
            break;

        case CecEvent::SelectedUs:
            // From Standby: the user picked "PiTV" in the TV's source menu.
            if (state_ != PowerState::Active) enterActive();
            break;

        case CecEvent::SelectedOther:
            // Ignored while Waking: that's the Samsung's boot routing noise.
            if (state_ == PowerState::Active) {
                log_.write("TV switched to another source");
                // Decide later: stop streaming, or treat as standby.
            }
            break;
    }
}

void PowerStateMachine::enterActive() {
    disarmWakeTimer();
    state_ = PowerState::Active;
    log_.write("State: Active");
    // Later: launch Flex Launcher.
}

void PowerStateMachine::enterStandby(const bool sendStandbyToTv) {
    disarmWakeTimer();
    if (sendStandbyToTv) {
        const bool standbySent = adapter_->StandbyDevices(CEC::CECDEVICE_TV);
        log_.log("Standby TV: {}", standbySent ? "ok" : "FAILED");
    }
    state_ = PowerState::Standby;
    log_.write("State: Standby");
    // Later: stop apps and Flex Launcher.
}

void PowerStateMachine::armWakeTimer(const std::chrono::milliseconds delay) const {
    itimerspec timerSpecification{};               // zero it_interval = one-shot
    timerSpecification.it_value.tv_sec  = delay.count() / 1000;
    timerSpecification.it_value.tv_nsec = (delay.count() % 1000) * 1'000'000;
    timerfd_settime(wakeTimerFileDescriptor_, 0, &timerSpecification, nullptr);
}

void PowerStateMachine::disarmWakeTimer() const {
    constexpr itimerspec zeroSpecification{};          // all-zero it_value disarms
    timerfd_settime(wakeTimerFileDescriptor_, 0, &zeroSpecification, nullptr);
}
