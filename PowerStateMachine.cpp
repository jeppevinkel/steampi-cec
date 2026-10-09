//
// Created by jeppe on 09.10.2026.
//

#include "PowerStateMachine.h"

#include <sys/timerfd.h>

// TODO: switch to armTimer/disarmTimer from Timers.h so there's only one copy.
namespace {

// delay must be > 0: an all-zero it_value disarms the timer instead of firing it.
void armOneShotTimer(const int timerFileDescriptor, const std::chrono::milliseconds delay) {
    itimerspec timerSpecification{};               // zero it_interval = one-shot
    timerSpecification.it_value.tv_sec  = delay.count() / 1000;
    timerSpecification.it_value.tv_nsec = (delay.count() % 1000) * 1'000'000;
    timerfd_settime(timerFileDescriptor, 0, &timerSpecification, nullptr);
}

void disarmTimer(const int timerFileDescriptor) {
    constexpr itimerspec zeroSpecification{};      // all-zero it_value disarms
    timerfd_settime(timerFileDescriptor, 0, &zeroSpecification, nullptr);
}

}  // namespace

PowerStateMachine::PowerStateMachine(CEC::ICECAdapter* adapter,
                                     const int wakeTimerFileDescriptor,
                                     const int unattendedAppTimerFileDescriptor,
                                     Logger& log)
    : adapter_(adapter),
      wakeTimerFileDescriptor_(wakeTimerFileDescriptor),
      unattendedAppTimerFileDescriptor_(unattendedAppTimerFileDescriptor),
      log_(log) {}

void PowerStateMachine::requestWake() {
    if (state_ == PowerState::Waking) return;

    if (state_ == PowerState::Active) {
        // Either the TV is on another input, or we believe we're on screen. Taking the source
        // again is harmless if we are, and recovers if our state is stale (e.g. the TV lost
        // power and never sent Standby). SetActiveSource also sends Image View On, and the
        // retries cover a TV that still has to boot.
        beginWaking(tvShowingOurInput_ ? "taking the source again" : "taking the TV back");
        onWakeTimerExpired();
        return;
    }

    // Must be read BEFORE PowerOnDevices: afterwards libcec reports 'in transition'.
    const auto tvPowerStatus = adapter_->GetDevicePowerStatus(CEC::CECDEVICE_TV);
    log_.log("TV power status before wake: {}", adapter_->ToString(tvPowerStatus));
    beginWaking("from standby");

    if (tvPowerStatus == CEC::CEC_POWER_STATUS_ON) {
        // SetActiveSource sends Image View On itself; no separate power-on needed.
        onWakeTimerExpired();
        return;
    }

    const bool poweredOn = adapter_->PowerOnDevices(CEC::CECDEVICE_TV);
    log_.log("Power on TV: {}", poweredOn ? "ok" : "FAILED");
    armOneShotTimer(wakeTimerFileDescriptor_, TV_BOOT_DELAY);
}

void PowerStateMachine::beginWaking(const char* reason) {
    state_ = PowerState::Waking;
    activeSourceAttempts_ = 0;
    log_.log("State: Waking ({})", reason);
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

    // Retry unless the TV confirms first.
    armOneShotTimer(wakeTimerFileDescriptor_, ACTIVE_SOURCE_RETRY_DELAY);
}

// The TV confirmed it shows us: anything counting down towards closing the app stops.
void PowerStateMachine::markTvShowingUs() {
    tvShowingOurInput_ = true;
    disarmTimer(unattendedAppTimerFileDescriptor_);
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
            markTvShowingUs();
            if (state_ == PowerState::Waking) enterActive();
            break;

        case CecEvent::SelectedUs:
            // The TV picked our input: from its source menu, or on power-up if it was last on us.
            markTvShowingUs();
            if (state_ != PowerState::Active) enterActive();
            break;

        case CecEvent::SelectedOther:
            // Ignored while Waking (the Samsung's boot noise). The tvShowingOurInput_ check makes
            // the duplicate events, and later switches between other inputs, not restart the countdown.
            if (state_ == PowerState::Active && tvShowingOurInput_) {
                tvShowingOurInput_ = false;
                log_.write("TV switched to another source; app grace period started");
                armOneShotTimer(unattendedAppTimerFileDescriptor_, APP_GRACE_AFTER_SOURCE_SWITCH);
                // TODO: updateBackgroundPlayback();
            }
            break;
    }
}

// Called for every controller button press while the daemon owns the controller.
// Returns true if the press was used for waking/taking over, false if it's navigation.
bool PowerStateMachine::onControllerButtonPressed(const bool isTakeOverButton) {
    if (state_ == PowerState::Waking) return true;   // swallow presses until the TV is ready

    const bool wantsTheTv = state_ == PowerState::Standby
                            || !tvShowingOurInput_
                            || isTakeOverButton;
    if (wantsTheTv) requestWake();
    return wantsTheTv;
}

void PowerStateMachine::enterActive() {
    disarmTimer(wakeTimerFileDescriptor_);
    state_ = PowerState::Active;
    log_.log("State: Active ({})", tvShowingOurInput_ ? "on screen" : "not confirmed");
    // TODO: updateBackgroundPlayback();
}

void PowerStateMachine::enterStandby(const bool sendStandbyToTv) {
    disarmTimer(wakeTimerFileDescriptor_);
    if (sendStandbyToTv) {
        // The user chose standby from the launcher: close everything right away.
        const bool standbySent = adapter_->StandbyDevices(CEC::CECDEVICE_TV);
        log_.log("Standby TV: {}", standbySent ? "ok" : "FAILED");
        disarmTimer(unattendedAppTimerFileDescriptor_);
        // TODO: end the running app's lease (closes the app).
    } else {
        // The TV turned itself off (remote, or its own auto power-off): the app may still be wanted.
        armOneShotTimer(unattendedAppTimerFileDescriptor_, APP_GRACE_AFTER_TV_STANDBY);
    }
    tvShowingOurInput_ = false;
    state_ = PowerState::Standby;
    log_.write("State: Standby");
    // TODO: updateBackgroundPlayback();
}

void PowerStateMachine::onUnattendedAppTimerExpired() {
    if (tvShowingOurInput_) return;   // they came back in the same epoll batch
    // TODO: once the lease exists, return early here if no app is running.
    log_.write("App unattended past its grace period; closing it");
    // TODO: end the running app's lease. The controller returns to Navigation when the app exits.
}
