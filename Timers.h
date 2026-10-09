//
// Created by jeppe on 09.10.2026.
//

#pragma once

#include <chrono>
#include <cstdint>
#include <sys/timerfd.h>
#include <unistd.h>

// Arms a timerfd: first expiry after firstDelay, then every interval (zero = one-shot).
// firstDelay must be > 0: an all-zero it_value disarms the timer instead of firing it.
inline void armTimer(const int timerFileDescriptor,
                     const std::chrono::milliseconds firstDelay,
                     const std::chrono::milliseconds interval = std::chrono::milliseconds(0)) {
    const auto toTimespec = [](const std::chrono::milliseconds duration) {
        timespec time{};
        time.tv_sec = duration.count() / 1000;
        time.tv_nsec = (duration.count() % 1000) * 1'000'000;
        return time;
    };
    itimerspec timerSpecification{};
    timerSpecification.it_value = toTimespec(firstDelay);
    timerSpecification.it_interval = toTimespec(interval);
    timerfd_settime(timerFileDescriptor, 0, &timerSpecification, nullptr);
}

inline void disarmTimer(const int timerFileDescriptor) {
    constexpr itimerspec zeroSpecification{};
    timerfd_settime(timerFileDescriptor, 0, &zeroSpecification, nullptr);
}

// Reads a ready timerfd. Returns false if there was nothing to read (disarmed in the meantime).
inline bool consumeTimerExpiration(const int timerFileDescriptor) {
    std::uint64_t expirationCount = 0;
    return read(timerFileDescriptor, &expirationCount, sizeof expirationCount)
           == sizeof expirationCount;
}
