//
// Created by jeppe on 09.10.2026.
//

#pragma once

#include <sys/epoll.h>

// Registers a file descriptor with epoll for reading. Returns false on failure (errno is set).
inline bool watchForInput(const int epollFileDescriptor, const int watchedFileDescriptor) {
    epoll_event interest{};
    interest.events = EPOLLIN;
    interest.data.fd = watchedFileDescriptor;
    return epoll_ctl(epollFileDescriptor, EPOLL_CTL_ADD, watchedFileDescriptor, &interest) == 0;
}

inline void stopWatching(const int epollFileDescriptor, const int watchedFileDescriptor) {
    epoll_ctl(epollFileDescriptor, EPOLL_CTL_DEL, watchedFileDescriptor, nullptr);
}
