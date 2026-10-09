//
// Created by jeppe on 08.10.2026.
//

#include "Logger.h"
#include <chrono>
#include <format>

bool Logger::open(const char* path) {
    std::scoped_lock lock(mutex_);
    fileStream_.open(path, std::ios::out | std::ios::trunc);
    return fileStream_.is_open();
}

void Logger::close() {
    std::scoped_lock lock(mutex_);
    if (fileStream_.is_open()) fileStream_.close();
}

void Logger::write(const std::string_view msg) {
    const auto ts = timestamp();
    std::scoped_lock lock(mutex_);
    if (!fileStream_.is_open()) return;
    fileStream_ << ts << "  " << msg << '\n';
    fileStream_.flush();
}

std::string Logger::timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto now_floored = std::chrono::floor<std::chrono::milliseconds>(now);
    const auto local_now = std::chrono::zoned_time{std::chrono::current_zone(), now_floored};
    return std::format("{:%Y-%m-%d %H:%M:%S}", local_now);
}
