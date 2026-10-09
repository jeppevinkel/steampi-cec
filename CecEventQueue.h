//
// Created by jeppe on 09.10.2026.
//

#pragma once

#include <deque>
#include <mutex>

enum class CecEvent { TvStandby, SelectedUs, SelectedOther, TvQueriedDeckStatus };

constexpr const char* cecEventName(const CecEvent event) {
    switch (event) {
        case CecEvent::TvStandby:           return "TvStandby";
        case CecEvent::TvQueriedDeckStatus: return "TvQueriedDeckStatus";
        case CecEvent::SelectedUs:          return "SelectedUs";
        case CecEvent::SelectedOther:       return "SelectedOther";
    }
    return "Unknown";
}

// Thread-safe queue: libcec callback threads push, the main thread
// waits on notificationFileDescriptor() with epoll and calls takeAll().
class CecEventQueue {
public:
    CecEventQueue();
    ~CecEventQueue();

    CecEventQueue(const CecEventQueue&) = delete;
    CecEventQueue& operator=(const CecEventQueue&) = delete;
    CecEventQueue(CecEventQueue&&) = delete;
    CecEventQueue& operator=(CecEventQueue&&) = delete;

    void push(CecEvent event);
    std::deque<CecEvent> takeAll();
    [[nodiscard]] int notificationFileDescriptor() const;

private:
    int eventFileDescriptor_;
    std::mutex mutex_;
    std::deque<CecEvent> pendingEvents_;
};
