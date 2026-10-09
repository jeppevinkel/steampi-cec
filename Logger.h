//
// Created by jeppe on 08.10.2026.
//

#pragma once
#include <format>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

class Logger {
public:
    bool open(const char* path);
    void close();

    template <typename... Args>
    void log(std::format_string<Args...> fmt, Args&&... args) {
        write(std::format(fmt, std::forward<Args>(args)...));
    }
    void write(std::string_view msg);

private:
    static std::string timestamp();

    std::mutex mutex_;
    std::ofstream fileStream_;
};
