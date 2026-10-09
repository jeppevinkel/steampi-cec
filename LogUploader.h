//
// Created by jeppe on 09.10.2026.
//

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <sys/types.h>

// Uploads new log content to ntfy with curl without blocking the event loop.
// Content only counts as uploaded once curl succeeded, so failed chunks are resent.
class LogUploader {
public:
    enum class StartResult { Started, NothingNew, PreviousStillRunning, Failed };

    LogUploader(std::string logPath, std::string chunkPath, std::string ntfyTopic);
    ~LogUploader();

    LogUploader(const LogUploader&) = delete;
    LogUploader& operator=(const LogUploader&) = delete;

    StartResult startUploadOfNewContent();
    std::optional<int> reapFinishedUpload();     // non-blocking; exit code if one finished
    std::optional<int> waitForPendingUpload();   // blocking, bounded by curl --max-time

private:
    std::optional<int> collectExitStatus(int waitOptions);

    std::string logPath_;
    std::string chunkPath_;
    std::string ntfyTopic_;
    pid_t uploadProcessId_ = -1;
    std::uintmax_t uploadedByteCount_ = 0;
    std::uintmax_t pendingUploadEndOffset_ = 0;
};
