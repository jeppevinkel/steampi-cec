//
// Created by jeppe on 09.10.2026.
//

#include "CecEventQueue.h"

#include <cerrno>
#include <cstdint>
#include <system_error>
#include <sys/eventfd.h>
#include <unistd.h>

CecEventQueue::CecEventQueue()
    : eventFileDescriptor_(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)) {
    if (eventFileDescriptor_ < 0) {
        throw std::system_error(errno, std::generic_category(), "eventFd failed");
    }
}

CecEventQueue::~CecEventQueue() {
    close(eventFileDescriptor_);
}

void CecEventQueue::push(const CecEvent event) {
    {
        std::lock_guard lock(mutex_);
        pendingEvents_.push_back(event);
    }
    // With a valid eventfd, write can only fail on 64-bit counter overflow.
    constexpr uint64_t increment = 1;
    [[maybe_unused]] const auto bytesWritten = write(eventFileDescriptor_, &increment, sizeof increment);
}

std::deque<CecEvent> CecEventQueue::takeAll() {
    // Reset the counter BEFORE taking events: anything pushed after this
    // read re-arms the eventfd, so no event can be left without a wakeup.
    // EAGAIN (counter already zero) is harmless.
    std::uint64_t counterValue;
    [[maybe_unused]] const auto bytesRead =
        read(eventFileDescriptor_, &counterValue, sizeof counterValue);

    std::deque<CecEvent> takenEvents;
    std::lock_guard lock(mutex_);
    takenEvents.swap(pendingEvents_);
    return takenEvents;
}

int CecEventQueue::notificationFileDescriptor() const {
    return eventFileDescriptor_;
}
