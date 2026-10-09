//
// Created by jeppe on 09.10.2026.
//

#include "SteamControllerPuck.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <fcntl.h>
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
constexpr auto RESCAN_INTERVAL = std::chrono::seconds(5);
constexpr std::size_t REPORT_BUFFER_SIZE = 64;   // reports are 54 bytes

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

}  // namespace

SteamControllerPuck::SteamControllerPuck(const int epollFileDescriptor, Logger& log,
                                         ReportHandler reportHandler)
    : epollFileDescriptor_(epollFileDescriptor),
      log_(log),
      reportHandler_(std::move(reportHandler)),
      rescanTimerFileDescriptor_(timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC)) {
    if (!rescanTimerFileDescriptor_.isValid()
        || !watchForInput(epollFileDescriptor_, rescanTimerFileDescriptor_.get())) {
        throw std::system_error(errno, std::generic_category(), "puck rescan timer");
    }
    if (!openPuck()) {
        log_.write("Steam Controller puck not found; checking every 5 s");
        armTimer(rescanTimerFileDescriptor_.get(), RESCAN_INTERVAL, RESCAN_INTERVAL);
    }
}

SteamControllerPuck::~SteamControllerPuck() {
    closePuck();
}

bool SteamControllerPuck::handleReadyFileDescriptor(const int readyFileDescriptor) {
    if (readyFileDescriptor == rescanTimerFileDescriptor_.get()) {
        if (consumeTimerExpiration(readyFileDescriptor)
            && slotFileDescriptors_.empty() && openPuck()) {
            disarmTimer(rescanTimerFileDescriptor_.get());
        }
        return true;
    }

    const bool isSlot = std::ranges::any_of(slotFileDescriptors_,
        [readyFileDescriptor](const FileDescriptor& slot) { return slot.get() == readyFileDescriptor; });
    if (!isSlot) return false;

    readReports(readyFileDescriptor);
    return true;
}

bool SteamControllerPuck::openPuck() {
    std::error_code directoryError;   // no hidraw devices at all is not an error
    for (const auto& hidrawEntry : std::filesystem::directory_iterator("/sys/class/hidraw", directoryError)) {
        const std::string ueventText = readTextFile(hidrawEntry.path() / "device" / "uevent");
        if (ueventText.find(PUCK_HID_ID) == std::string::npos) continue;
        if (!isSlotInterface(interfaceNameFromUevent(ueventText))) continue;

        const std::string devicePath = "/dev/" + hidrawEntry.path().filename().string();
        FileDescriptor slotFileDescriptor(open(devicePath.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
        if (!slotFileDescriptor.isValid()) {
            log_.log("Cannot open {}: {}", devicePath, std::strerror(errno));
            continue;
        }
        if (!watchForInput(epollFileDescriptor_, slotFileDescriptor.get())) {
            log_.log("Cannot watch {}: {}", devicePath, std::strerror(errno));
            continue;
        }
        slotFileDescriptors_.push_back(std::move(slotFileDescriptor));
    }

    if (slotFileDescriptors_.empty()) return false;

    grabLizardModeInputs();
    if (grabbedInputFileDescriptors_.empty()) {
        // Reading without the grab would let lizard mode's keys and clicks reach X: try again later.
        log_.write("Steam Controller puck found, but its lizard-mode inputs couldn't be grabbed yet");
        closePuck();
        return false;
    }

    log_.log("Steam Controller puck: {} slot interfaces open, {} lizard-mode inputs grabbed",
             slotFileDescriptors_.size(), grabbedInputFileDescriptors_.size());
    return true;
}

void SteamControllerPuck::grabLizardModeInputs() {
    std::error_code directoryError;
    for (const auto& inputEntry : std::filesystem::directory_iterator("/sys/class/input", directoryError)) {
        const std::string inputName = inputEntry.path().filename().string();
        if (!inputName.starts_with("event")) continue;
        if (readFirstLine(inputEntry.path() / "device" / "id" / "vendor") != PUCK_VENDOR_ID) continue;
        if (readFirstLine(inputEntry.path() / "device" / "id" / "product") != PUCK_PRODUCT_ID) continue;

        const std::string devicePath = "/dev/input/" + inputName;
        FileDescriptor inputFileDescriptor(open(devicePath.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC));
        if (!inputFileDescriptor.isValid() || ioctl(inputFileDescriptor.get(), EVIOCGRAB, 1) < 0) {
            log_.log("Cannot grab {}: {}", devicePath, std::strerror(errno));
            continue;
        }
        // Never read: the grab only needs the file to stay open. Unread events are dropped.
        grabbedInputFileDescriptors_.push_back(std::move(inputFileDescriptor));
    }
}

void SteamControllerPuck::closePuck() {
    for (const FileDescriptor& slotFileDescriptor : slotFileDescriptors_) {
        stopWatching(epollFileDescriptor_, slotFileDescriptor.get());
    }
    slotFileDescriptors_.clear();           // closes the slot interfaces
    grabbedInputFileDescriptors_.clear();   // closing the files releases the grabs
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

        log_.log("Steam Controller puck lost ({}); checking every 5 s",
                 bytesRead < 0 ? std::strerror(errno) : "end of file");
        closePuck();
        armTimer(rescanTimerFileDescriptor_.get(), RESCAN_INTERVAL, RESCAN_INTERVAL);
        return;
    }
}
