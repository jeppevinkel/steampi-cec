//
// Created by jeppe on 09.10.2026.
//

#include "SteamControllerPuck.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <fcntl.h>
#include <libudev.h>
#include <linux/input.h>
#include <sys/ioctl.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "EventLoop.h"
#include "Timers.h"

namespace {

constexpr std::string_view PUCK_HID_ID = "000028DE:00001304";   // Valve, Steam Controller Puck
constexpr std::string_view PUCK_VENDOR_ID = "28de";
constexpr std::string_view PUCK_PRODUCT_ID = "1304";
// One interface per controller the puck can pair with (measured with pitv-diag).
constexpr std::string_view SLOT_INTERFACE_NAMES[] = {"input2", "input3", "input4", "input5"};
constexpr std::size_t REPORT_BUFFER_SIZE = 64;   // reports are 54 bytes

// With udev: after a failed open or grab (permissions not set yet, or an app still holding the
// grab), retry with growing pauses.
constexpr auto FIRST_RETRY_DELAY = std::chrono::milliseconds(500);
constexpr auto LONGEST_RETRY_DELAY = std::chrono::milliseconds(30'000);
// Without udev: how often to look for the puck.
constexpr auto RESCAN_INTERVAL = std::chrono::milliseconds(5'000);
// Exists while udevd runs; without it, a udev monitor never receives anything.
constexpr const char* UDEV_CONTROL_SOCKET = "/run/udev/control";

std::string readTextFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::string readFirstLine(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::string line;
    std::getline(file, line);
    return line;
}

// "HID_PHYS=usb-xhci-hcd.1-2/input2" -> "input2"
std::string interfaceNameFromUevent(const std::string& ueventText) {
    std::istringstream ueventLines(ueventText);
    for (std::string line; std::getline(ueventLines, line);) {
        if (line.starts_with("HID_PHYS=")) return line.substr(line.rfind('/') + 1);
    }
    return {};
}

bool isSlotInterface(const std::string& interfaceName) {
    return std::ranges::find(SLOT_INTERFACE_NAMES, interfaceName) != std::end(SLOT_INTERFACE_NAMES);
}

// The entries of a sysfs class directory. Never throws: a missing directory has no entries.
std::vector<std::filesystem::path> listDirectory(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> entries;
    std::error_code directoryError;
    for (auto entry = std::filesystem::directory_iterator(directory, directoryError);
         !directoryError && entry != std::filesystem::directory_iterator();
         entry.increment(directoryError)) {
        entries.push_back(entry->path());
    }
    return entries;
}

}  // namespace

void SteamControllerPuck::UdevDeleter::operator()(udev* udevContext) const {
    udev_unref(udevContext);
}

void SteamControllerPuck::UdevDeleter::operator()(udev_monitor* udevMonitor) const {
    udev_monitor_unref(udevMonitor);
}

SteamControllerPuck::SteamControllerPuck(const int epollFileDescriptor, Logger& log,
                                         ReportHandler reportHandler,
                                         DeviceDirectories deviceDirectories)
    : epollFileDescriptor_(epollFileDescriptor),
      log_(log),
      reportHandler_(std::move(reportHandler)),
      deviceDirectories_(std::move(deviceDirectories)),
      retryTimerFileDescriptor_(timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)),
      retryDelay_(FIRST_RETRY_DELAY) {
    if (!retryTimerFileDescriptor_.isValid()
        || !watchForInput(epollFileDescriptor_, retryTimerFileDescriptor_.get())) {
        throw std::system_error(errno, std::generic_category(), "puck retry timer");
    }
    // Monitor first, then look: a puck plugged in between the two is still seen.
    if (!startUdevMonitor()) {
        log_.write("Steam Controller puck: no udev events; checking for the puck every 5 s");
        armTimer(retryTimerFileDescriptor_.get(), RESCAN_INTERVAL, RESCAN_INTERVAL);
    }
    refresh();
}

SteamControllerPuck::~SteamControllerPuck() {
    for (const OpenDevice& slotInterface : slotInterfaces_) {
        stopWatching(epollFileDescriptor_, slotInterface.fileDescriptor.get());
    }
    if (udevMonitor_) stopWatching(epollFileDescriptor_, udev_monitor_get_fd(udevMonitor_.get()));
    // Members close the rest: the slot interfaces, the grabs (closing releases them), the monitor.
}

bool SteamControllerPuck::startUdevMonitor() {
    if (access(UDEV_CONTROL_SOCKET, F_OK) != 0) return false;   // no udevd (e.g. a container)

    udev_.reset(udev_new());
    if (udev_) udevMonitor_.reset(udev_monitor_new_from_netlink(udev_.get(), "udev"));
    // "udev" events, not raw kernel ones: they arrive after udev's rules ran, so the device
    // already has its permissions (the input group) when we try to open it.
    const bool started = udevMonitor_
        && udev_monitor_filter_add_match_subsystem_devtype(udevMonitor_.get(), "hidraw", nullptr) >= 0
        && udev_monitor_filter_add_match_subsystem_devtype(udevMonitor_.get(), "input", nullptr) >= 0
        && udev_monitor_enable_receiving(udevMonitor_.get()) >= 0;
    if (started) {
        const int monitorFileDescriptor = udev_monitor_get_fd(udevMonitor_.get());
        // Draining the monitor relies on it returning nothing instead of blocking.
        fcntl(monitorFileDescriptor, F_SETFL, fcntl(monitorFileDescriptor, F_GETFL) | O_NONBLOCK);
        if (watchForInput(epollFileDescriptor_, monitorFileDescriptor)) return true;
    }
    log_.log("Steam Controller puck: udev monitor unavailable: {}", std::strerror(errno));
    udevMonitor_.reset();
    udev_.reset();
    return false;
}

bool SteamControllerPuck::handleReadyFileDescriptor(const int readyFileDescriptor) {
    if (readyFileDescriptor == retryTimerFileDescriptor_.get()) {
        if (consumeTimerExpiration(readyFileDescriptor)) refresh();
        return true;
    }
    if (udevMonitor_ && readyFileDescriptor == udev_monitor_get_fd(udevMonitor_.get())) {
        handleUdevEvents();
        return true;
    }
    const bool isSlotInterfaceReady = std::ranges::any_of(slotInterfaces_,
        [readyFileDescriptor](const OpenDevice& slotInterface) {
            return slotInterface.fileDescriptor.get() == readyFileDescriptor;
        });
    if (!isSlotInterfaceReady) return false;

    readReports(readyFileDescriptor);
    return true;
}

void SteamControllerPuck::setLizardModeGrabbed(const bool grabbed) {
    if (grabbed == lizardModeGrabbed_) return;
    lizardModeGrabbed_ = grabbed;
    if (!grabbed) {
        grabbedInputs_.clear();   // closing the files releases the grabs
        logStateIfChanged();
        return;
    }
    // The app may still hold its own grab for a moment after exiting; refresh retries that.
    refresh();
}

void SteamControllerPuck::handleUdevEvents() {
    // Several events usually arrive together (a puck is a dozen devices): handle them all,
    // then look at the devices once.
    while (udev_device* device = udev_monitor_receive_device(udevMonitor_.get())) {
        const char* action = udev_device_get_action(device);
        const char* deviceNode = udev_device_get_devnode(device);
        if (action && deviceNode && std::string_view(action) == "remove") closeDevice(deviceNode);
        udev_device_unref(device);
    }
    refresh();
}

void SteamControllerPuck::refresh() {
    int failureCount = openNewSlotInterfaces();
    if (lizardModeGrabbed_) failureCount += grabNewLizardModeInputs();
    logStateIfChanged();

    if (failureCount > 0) {
        failureStreakLogged_ = true;
        scheduleRetry();
        return;
    }
    failureStreakLogged_ = false;   // the state line above already shows what's open now
    retryDelay_ = FIRST_RETRY_DELAY;
    if (udevMonitor_) disarmTimer(retryTimerFileDescriptor_.get());   // nothing left to retry
}

int SteamControllerPuck::openNewSlotInterfaces() {
    int failureCount = 0;
    for (const std::filesystem::path& hidrawEntry : listDirectory(deviceDirectories_.sysfs / "class" / "hidraw")) {
        const std::string ueventText = readTextFile(hidrawEntry / "device" / "uevent");
        if (ueventText.find(PUCK_HID_ID) == std::string::npos) continue;
        if (!isSlotInterface(interfaceNameFromUevent(ueventText))) continue;
        if (!isSetUpByUdev(hidrawEntry)) continue;   // its own udev event follows

        const std::filesystem::path devicePath = deviceDirectories_.devices / hidrawEntry.filename();
        if (std::ranges::find(slotInterfaces_, devicePath, &OpenDevice::devicePath) != slotInterfaces_.end()) continue;

        FileDescriptor slotFileDescriptor(open(devicePath.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
        if (!slotFileDescriptor.isValid()) {
            reportFailure(devicePath, "open", errno);
            ++failureCount;
            continue;
        }
        if (!watchForInput(epollFileDescriptor_, slotFileDescriptor.get())) {
            reportFailure(devicePath, "watch", errno);
            ++failureCount;
            continue;
        }
        slotInterfaces_.push_back({devicePath, std::move(slotFileDescriptor)});
    }
    return failureCount;
}

int SteamControllerPuck::grabNewLizardModeInputs() {
    int failureCount = 0;
    for (const std::filesystem::path& inputEntry : listDirectory(deviceDirectories_.sysfs / "class" / "input")) {
        const std::string inputName = inputEntry.filename().string();
        if (!inputName.starts_with("event")) continue;
        if (readFirstLine(inputEntry / "device" / "id" / "vendor") != PUCK_VENDOR_ID) continue;
        if (readFirstLine(inputEntry / "device" / "id" / "product") != PUCK_PRODUCT_ID) continue;
        // udev announces the parent input device first; its event node is often still being set up
        // (no permissions yet) at that moment. The node's own udev event follows.
        if (!isSetUpByUdev(inputEntry)) continue;

        const std::filesystem::path devicePath = deviceDirectories_.devices / "input" / inputName;
        if (std::ranges::find(grabbedInputs_, devicePath, &OpenDevice::devicePath) != grabbedInputs_.end()) continue;

        FileDescriptor inputFileDescriptor(open(devicePath.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
        if (!inputFileDescriptor.isValid() || ioctl(inputFileDescriptor.get(), EVIOCGRAB, 1) < 0) {
            reportFailure(devicePath, "grab", errno);
            ++failureCount;
            continue;
        }
        // Never read: the grab only needs the file to stay open. Unread events are dropped.
        grabbedInputs_.push_back({devicePath, std::move(inputFileDescriptor)});
    }
    return failureCount;
}

bool SteamControllerPuck::isSetUpByUdev(const std::filesystem::path& sysfsEntry) const {
    if (!udev_) return true;   // no udev: nothing to wait for
    std::error_code pathError;
    const std::filesystem::path devicePath = std::filesystem::canonical(sysfsEntry, pathError);
    if (pathError) return false;   // gone in the meantime
    udev_device* device = udev_device_new_from_syspath(udev_.get(), devicePath.c_str());
    if (!device) return false;
    const bool isSetUp = udev_device_get_is_initialized(device) > 0;
    udev_device_unref(device);
    return isSetUp;
}

void SteamControllerPuck::reportFailure(const std::filesystem::path& devicePath, const char* operation,
                                        const int errorNumber) {
    if (failureStreakLogged_) return;   // already said so; the state line shows the counts
    log_.log("Steam Controller puck: cannot {} {}: {} (retrying)", operation, devicePath.string(),
             std::strerror(errorNumber));
}

void SteamControllerPuck::closeDevice(const std::filesystem::path& devicePath) {
    const auto slotInterface = std::ranges::find(slotInterfaces_, devicePath, &OpenDevice::devicePath);
    if (slotInterface != slotInterfaces_.end()) {
        stopWatching(epollFileDescriptor_, slotInterface->fileDescriptor.get());
        slotInterfaces_.erase(slotInterface);
    }
    std::erase_if(grabbedInputs_, [&devicePath](const OpenDevice& grabbedInput) {
        return grabbedInput.devicePath == devicePath;
    });
    logStateIfChanged();
}

void SteamControllerPuck::closeSlotInterface(const int slotFileDescriptor, const char* reason) {
    const auto slotInterface = std::ranges::find_if(slotInterfaces_,
        [slotFileDescriptor](const OpenDevice& openDevice) { return openDevice.fileDescriptor.get() == slotFileDescriptor; });
    if (slotInterface == slotInterfaces_.end()) return;

    log_.log("Steam Controller puck: {} closed ({})", slotInterface->devicePath.string(), reason);
    stopWatching(epollFileDescriptor_, slotFileDescriptor);
    slotInterfaces_.erase(slotInterface);
    logStateIfChanged();
    // Unplugged: the next refresh finds nothing and that's that. Anything else: it reopens.
    scheduleRetry();
}

void SteamControllerPuck::readReports(const int slotFileDescriptor) {
    std::array<uint8_t, REPORT_BUFFER_SIZE> reportBuffer{};
    while (true) {
        // Each read returns exactly one report.
        const ssize_t bytesRead = read(slotFileDescriptor, reportBuffer.data(), reportBuffer.size());
        if (bytesRead > 0) {
            reportHandler_(std::span<const uint8_t>(reportBuffer.data(), static_cast<std::size_t>(bytesRead)));
            continue;
        }
        if (bytesRead < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;   // all read
        if (bytesRead < 0 && errno == EINTR) continue;

        closeSlotInterface(slotFileDescriptor, bytesRead < 0 ? std::strerror(errno) : "end of file");
        return;
    }
}

void SteamControllerPuck::scheduleRetry() {
    if (!udevMonitor_) return;   // the periodic check is running anyway
    armTimer(retryTimerFileDescriptor_.get(), retryDelay_);
    retryDelay_ = std::min(retryDelay_ * 2, LONGEST_RETRY_DELAY);
}

void SteamControllerPuck::logStateIfChanged() {
    const std::pair state{slotInterfaces_.size(), grabbedInputs_.size()};
    if (loggedState_ == state) return;
    loggedState_ = state;
    if (state.first == 0 && state.second == 0) {
        log_.write("Steam Controller puck: not connected");
        return;
    }
    log_.log("Steam Controller puck: slot interfaces open: {}, lizard-mode inputs grabbed: {}",
             state.first, state.second);
}
