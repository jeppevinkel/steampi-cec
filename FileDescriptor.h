//
// Created by jeppe on 09.10.2026.
//

#pragma once
#include <unistd.h>
#include <utility>

class FileDescriptor {
public:
    explicit FileDescriptor(const int rawFileDescriptor = -1)
        : rawFileDescriptor_(rawFileDescriptor) {}

    ~FileDescriptor() {
        if (rawFileDescriptor_ >= 0) close(rawFileDescriptor_);
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    FileDescriptor(FileDescriptor&& other) noexcept
        : rawFileDescriptor_(std::exchange(other.rawFileDescriptor_, -1)) {}

    FileDescriptor& operator=(FileDescriptor&& other) noexcept {
        if (this != &other) {
            if (rawFileDescriptor_ >= 0) close(rawFileDescriptor_);
            rawFileDescriptor_ = std::exchange(other.rawFileDescriptor_, -1);
        }
        return *this;
    }

    [[nodiscard]] int get() const { return rawFileDescriptor_; }
    [[nodiscard]] bool isValid() const { return rawFileDescriptor_ >= 0; }

private:
    int rawFileDescriptor_;
};
