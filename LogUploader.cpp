//
// Created by jeppe on 09.10.2026.
//

#include "LogUploader.h"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <format>
#include <fstream>
#include <utility>
#include <vector>
#include <spawn.h>
#include <sys/wait.h>

extern char** environ;

LogUploader::LogUploader(std::string logPath, std::string chunkPath, std::string ntfyTopic)
    : logPath_(std::move(logPath)),
      chunkPath_(std::move(chunkPath)),
      ntfyTopic_(std::move(ntfyTopic)) {}

LogUploader::~LogUploader() {
    waitForPendingUpload();
}

LogUploader::StartResult LogUploader::startUploadOfNewContent() {
    if (uploadProcessId_ > 0) return StartResult::PreviousStillRunning;

    std::ifstream logFile(logPath_, std::ios::binary | std::ios::ate);
    if (!logFile) return StartResult::Failed;

    const auto fileSize = static_cast<std::uintmax_t>(logFile.tellg());
    if (fileSize < uploadedByteCount_) uploadedByteCount_ = 0;   // log was truncated
    if (fileSize == uploadedByteCount_) return StartResult::NothingNew;

    std::string newContent(fileSize - uploadedByteCount_, '\0');
    logFile.seekg(static_cast<std::streamoff>(uploadedByteCount_));
    logFile.read(newContent.data(), static_cast<std::streamsize>(newContent.size()));

    // Only upload complete lines; a line being written right now goes in the next chunk.
    const auto lastNewlinePosition = newContent.rfind('\n');
    if (lastNewlinePosition == std::string::npos) return StartResult::NothingNew;
    newContent.resize(lastNewlinePosition + 1);

    std::ofstream chunkFile(chunkPath_, std::ios::binary | std::ios::trunc);
    chunkFile << newContent;
    chunkFile.close();
    if (!chunkFile) return StartResult::Failed;

    const auto timestamp = std::format("{:%Y%m%d-%H%M%S}",
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));

    std::vector<std::string> arguments = {
        "curl", "-sS", "--fail", "--max-time", "30",
        "-T", chunkPath_,
        "-H", std::format("Filename: cec-{}Z.log", timestamp),
        std::format("https://ntfy.sh/{}", ntfyTopic_),
    };
    std::vector<char*> argumentPointers;
    for (std::string& argument : arguments) argumentPointers.push_back(argument.data());
    argumentPointers.push_back(nullptr);

    // main() blocks SIGINT/SIGTERM and children inherit that; give curl a normal mask.
    posix_spawnattr_t spawnAttributes;
    posix_spawnattr_init(&spawnAttributes);
    sigset_t emptySignalSet;
    sigemptyset(&emptySignalSet);
    posix_spawnattr_setsigmask(&spawnAttributes, &emptySignalSet);
    posix_spawnattr_setflags(&spawnAttributes, POSIX_SPAWN_SETSIGMASK);

    const int spawnResult = posix_spawnp(&uploadProcessId_, "curl", nullptr, &spawnAttributes,
                                         argumentPointers.data(), environ);
    posix_spawnattr_destroy(&spawnAttributes);

    if (spawnResult != 0) {
        uploadProcessId_ = -1;
        return StartResult::Failed;
    }
    pendingUploadEndOffset_ = uploadedByteCount_ + newContent.size();
    return StartResult::Started;
}

std::optional<int> LogUploader::reapFinishedUpload() {
    return collectExitStatus(WNOHANG);
}

std::optional<int> LogUploader::waitForPendingUpload() {
    return collectExitStatus(0);
}

std::optional<int> LogUploader::collectExitStatus(const int waitOptions) {
    if (uploadProcessId_ <= 0) return std::nullopt;

    int status = 0;
    pid_t waitResult;
    do {
        waitResult = waitpid(uploadProcessId_, &status, waitOptions);
    } while (waitResult < 0 && errno == EINTR);

    if (waitResult == 0) return std::nullopt;   // WNOHANG: still running

    uploadProcessId_ = -1;
    const int exitCode = (waitResult > 0 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
    if (exitCode == 0) uploadedByteCount_ = pendingUploadEndOffset_;   // commit only on success
    return exitCode;
}