//
// Created by jeppe on 09.10.2026.
//

#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "FileDescriptor.h"
#include "Logger.h"

// The 2026 Steam Controller's wireless puck. Reads the controller's reports from the puck's
// slot interfaces and grabs the puck's lizard-mode keyboard and mouse, so their events never
// reach X. Looks for the puck again if it's missing or unplugged.
class SteamControllerPuck {
public:
    using ReportHandler = std::function<void(std::span<const uint8_t> report)>;

    // Throws std::system_error if the rescan timer can't be created.
    SteamControllerPuck(int epollFileDescriptor, Logger& log, ReportHandler reportHandler);
    ~SteamControllerPuck();

    SteamControllerPuck(const SteamControllerPuck&) = delete;
    SteamControllerPuck& operator=(const SteamControllerPuck&) = delete;

    // Returns true if the file descriptor belonged to the puck (and was handled).
    bool handleReadyFileDescriptor(int readyFileDescriptor);

private:
    bool openPuck();
    void grabLizardModeInputs();
    void closePuck();
    void readReports(int slotFileDescriptor);

    int epollFileDescriptor_;
    Logger& log_;
    ReportHandler reportHandler_;
    std::vector<FileDescriptor> slotFileDescriptors_;
    std::vector<FileDescriptor> grabbedInputFileDescriptors_;
    FileDescriptor rescanTimerFileDescriptor_;
};
