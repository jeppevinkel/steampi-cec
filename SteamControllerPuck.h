//
// Created by jeppe on 09.10.2026.
//

#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "FileDescriptor.h"
#include "Logger.h"

struct udev;
struct udev_monitor;

// Where SteamControllerPuck looks for devices. Only tests change these.
struct DeviceDirectories {
    std::filesystem::path sysfs = "/sys";
    std::filesystem::path devices = "/dev";
};

// The 2026 Steam Controller's wireless puck. Reads the controller's reports from the puck's
// slot interfaces and grabs the puck's lizard-mode keyboard and mouse, so their events never
// reach X. Follows the puck being plugged in and out through udev events; without a running
// udev it checks for the puck every 5 s instead.
class SteamControllerPuck {
public:
    using ReportHandler = std::function<void(std::span<const uint8_t> report)>;

    // Throws std::system_error if the retry timer can't be created.
    SteamControllerPuck(int epollFileDescriptor, Logger& log, ReportHandler reportHandler,
                        DeviceDirectories deviceDirectories = {});
    ~SteamControllerPuck();

    SteamControllerPuck(const SteamControllerPuck&) = delete;
    SteamControllerPuck& operator=(const SteamControllerPuck&) = delete;

    // Returns true if the file descriptor belonged to the puck (and was handled).
    bool handleReadyFileDescriptor(int readyFileDescriptor);

    // Whether lizard mode's keyboard and mouse are kept away from X. Released only for apps that
    // read the controller themselves. Reports keep arriving either way.
    void setLizardModeGrabbed(bool grabbed);

private:
    struct OpenDevice {
        std::filesystem::path devicePath;
        FileDescriptor fileDescriptor;
    };

    struct UdevDeleter {
        void operator()(udev* udevContext) const;
        void operator()(udev_monitor* udevMonitor) const;
    };

    bool startUdevMonitor();
    void handleUdevEvents();
    // Opens slot interfaces and grabs lizard-mode inputs that aren't open yet. Safe to call any
    // time: devices already handled are skipped, and failures are retried later.
    void refresh();
    // Both return the number of puck devices that couldn't be opened or grabbed.
    int openNewSlotInterfaces();
    int grabNewLizardModeInputs();
    // Whether udev has finished setting the device up (permissions included). Always true
    // without udev.
    [[nodiscard]] bool isSetUpByUdev(const std::filesystem::path& sysfsEntry) const;
    void reportFailure(const std::filesystem::path& devicePath, const char* operation, int errorNumber);
    void closeDevice(const std::filesystem::path& devicePath);
    void closeSlotInterface(int slotFileDescriptor, const char* reason);
    void readReports(int slotFileDescriptor);
    void scheduleRetry();
    void logStateIfChanged();

    int epollFileDescriptor_;
    Logger& log_;
    ReportHandler reportHandler_;
    DeviceDirectories deviceDirectories_;
    std::unique_ptr<udev, UdevDeleter> udev_;
    std::unique_ptr<udev_monitor, UdevDeleter> udevMonitor_;
    std::vector<OpenDevice> slotInterfaces_;
    std::vector<OpenDevice> grabbedInputs_;
    // With udev: one-shot retries after failures. Without udev: the periodic check.
    FileDescriptor retryTimerFileDescriptor_;
    std::chrono::milliseconds retryDelay_;
    bool lizardModeGrabbed_ = true;
    bool failureStreakLogged_ = false;   // a run of failures is logged once, not on every retry
    std::optional<std::pair<std::size_t, std::size_t>> loggedState_;   // slot interfaces, grabbed inputs
};
