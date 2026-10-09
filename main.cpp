#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <iostream>
#include <string>
#include <libcec/cec.h>
#include <libcec/cecloader.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <chrono>
#include <memory>
#include <system_error>
#include <linux/input-event-codes.h>

#include "CecEventQueue.h"
#include "FileDescriptor.h"
#include "Logger.h"
#include "LogUploader.h"
#include "PhysicalAddress.h"
#include "PowerStateMachine.h"
#include "VirtualKeyboard.h"

static constexpr auto DEVICE_NAME = "PiTV";
static constexpr auto LOG_PATH     = "/home/jeppe/cec.log";
static constexpr auto LOG_UPLOAD_CHUNK_PATH = "/home/jeppe/cec-upload-chunk.log";
static constexpr auto NTFY_TOPIC = "pitv-cec-jeppe";
static constexpr auto FIRST_LOG_UPLOAD_DELAY = std::chrono::seconds(10);
static constexpr auto LOG_UPLOAD_INTERVAL = std::chrono::seconds(60);
static constexpr int MAXIMUM_ADAPTERS = 4;
static constexpr int MAXIMUM_EVENTS_PER_WAIT = 8;
static constexpr int VIRTUAL_KEYBOARD_KEY_CODES[] = {
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_ENTER, KEY_BACKSPACE,
};

// Shared with libcec callback threads.
static CEC::ICECAdapter* g_adapter = nullptr;
static Logger g_log;                       // internally synchronized
static CecEventQueue g_cecEventQueue;
static std::atomic<uint16_t> g_ownPhysicalAddress{CEC_INVALID_PHYSICAL_ADDRESS};
// Created before libcec starts and reset after it's closed, so libcec's thread can always use it.
static std::unique_ptr<VirtualKeyboard> g_virtualKeyboard;

// ---------- libcec callbacks ----------
// These run on libcec's threads: log and push events only, never call libcec commands.

static void pushSelectionEvent(const uint16_t targetAddress) {
    if (targetAddress == CEC_INVALID_PHYSICAL_ADDRESS) return;   // Samsung sends ffff while booting
    g_cecEventQueue.push(targetAddress == g_ownPhysicalAddress.load()
                             ? CecEvent::SelectedUs
                             : CecEvent::SelectedOther);
}

static void onCommand(void*, const CEC::cec_command* command) {
    std::string parametersHex;
    for (uint8_t parameterIndex = 0; parameterIndex < command->parameters.size; ++parameterIndex) {
        parametersHex += std::format(" {:02x}", command->parameters[parameterIndex]);
    }

    g_log.log("CMD {} -> {}: {}{}",
              g_adapter->ToString(command->initiator),
              g_adapter->ToString(command->destination),
              g_adapter->ToString(command->opcode),
              parametersHex);

    const bool fromTv = command->initiator == CEC::CECDEVICE_TV;

    switch (command->opcode) {
        case CEC::CEC_OPCODE_STANDBY:
            if (fromTv) g_cecEventQueue.push(CecEvent::TvStandby);
            break;

        case CEC::CEC_OPCODE_GIVE_DECK_STATUS:
            // My Samsung asks for deck status right after accepting our Active Source.
            if (fromTv) g_cecEventQueue.push(CecEvent::TvQueriedDeckStatus);
            break;

        case CEC::CEC_OPCODE_SET_STREAM_PATH:
            pushSelectionEvent(readPhysicalAddress(command->parameters));
            break;

        case CEC::CEC_OPCODE_ROUTING_CHANGE:
            pushSelectionEvent(readPhysicalAddress(command->parameters, 2));
            break;

        case CEC::CEC_OPCODE_ACTIVE_SOURCE: {
            // Another device (e.g. Playback 1 on HDMI 2) claimed the TV.
            const uint16_t sourceAddress = readPhysicalAddress(command->parameters);
            if (sourceAddress != CEC_INVALID_PHYSICAL_ADDRESS
                && sourceAddress != g_ownPhysicalAddress.load()) {
                g_cecEventQueue.push(CecEvent::SelectedOther);
            }
            break;
        }
        default: break;
    }
}

// libcec's own internal log (very useful for debugging CEC)
static void onLog(void*, const CEC::cec_log_message* logMessage) {
    const char* levelName = "?";
    switch (logMessage->level) {
        case CEC::CEC_LOG_ERROR:   levelName = "ERROR";   break;
        case CEC::CEC_LOG_WARNING: levelName = "WARNING"; break;
        case CEC::CEC_LOG_NOTICE:  levelName = "NOTICE";  break;
        case CEC::CEC_LOG_TRAFFIC: levelName = "TRAFFIC"; break;
        case CEC::CEC_LOG_DEBUG:   levelName = "DEBUG";   break;
        default: break;
    }
    g_log.log("[libcec {}] {}", levelName, logMessage->message);
}

static int linuxKeyForCecKey(const CEC::cec_user_control_code cecKey) {
    switch (cecKey) {
        case CEC::CEC_USER_CONTROL_CODE_UP:     return KEY_UP;
        case CEC::CEC_USER_CONTROL_CODE_DOWN:   return KEY_DOWN;
        case CEC::CEC_USER_CONTROL_CODE_LEFT:   return KEY_LEFT;
        case CEC::CEC_USER_CONTROL_CODE_RIGHT:  return KEY_RIGHT;
        case CEC::CEC_USER_CONTROL_CODE_SELECT: return KEY_ENTER;
        case CEC::CEC_USER_CONTROL_CODE_EXIT:   return KEY_BACKSPACE;
        default:                                return KEY_RESERVED;
    }
}

// Runs on libcec's thread. Writing to uinput is not a libcec call, so it can't block libcec.
static void onKeyPress(void*, const CEC::cec_keypress* keyPress) {
    // libcec reports each key twice: on press (duration 0) and on release (how long it was held).
    if (keyPress->duration != 0) return;

    const int linuxKeyCode = linuxKeyForCecKey(keyPress->keycode);
    g_log.log("Remote key: {} -> {}", g_adapter->ToString(keyPress->keycode),
              linuxKeyCode == KEY_RESERVED ? std::string("unmapped") : std::to_string(linuxKeyCode));

    if (linuxKeyCode != KEY_RESERVED && g_virtualKeyboard) {
        g_virtualKeyboard->tap(linuxKeyCode);
    }
}

// ---------- helpers ----------

static bool watchForInput(const int epollFileDescriptor, const int watchedFileDescriptor) {
    epoll_event interest{};
    interest.events = EPOLLIN;
    interest.data.fd = watchedFileDescriptor;
    return epoll_ctl(epollFileDescriptor, EPOLL_CTL_ADD, watchedFileDescriptor, &interest) == 0;
}

static void armRepeatingTimer(const int timerFileDescriptor,
                              const std::chrono::seconds firstDelay,
                              const std::chrono::seconds interval) {
    itimerspec timerSpecification{};
    timerSpecification.it_value.tv_sec = firstDelay.count();
    timerSpecification.it_interval.tv_sec = interval.count();
    timerfd_settime(timerFileDescriptor, 0, &timerSpecification, nullptr);
}

static void closeCecAdapter() {
    g_adapter->Close();
    UnloadLibCec(g_adapter);
    g_log.write("Adapter closed");
}

// Successful uploads are deliberately NOT logged: that line would itself be new
// content and trigger another upload every interval, forever.
static void uploadNewLogContent(LogUploader& logUploader) {
    if (const auto exitCode = logUploader.reapFinishedUpload(); exitCode && *exitCode != 0) {
        g_log.log("Log upload failed with exit code {} (will retry)", *exitCode);
    }
    if (logUploader.startUploadOfNewContent() == LogUploader::StartResult::Failed) {
        g_log.write("Log upload could not be started");
    }
}

// Used on every exit path, so failures during startup get uploaded too.
static void closeLogAndUploadRemainder(LogUploader& logUploader) {
    if (const auto exitCode = logUploader.waitForPendingUpload(); exitCode && *exitCode != 0) {
        g_log.log("Log upload failed with exit code {}", *exitCode);
    }
    g_log.close();   // everything is on disk now
    if (logUploader.startUploadOfNewContent() == LogUploader::StartResult::Started) {
        const int exitCode = logUploader.waitForPendingUpload().value_or(-1);
        std::cout << "Final log upload exited with code " << exitCode << std::endl;
    }
}

// ---------- main ----------

int main() {
    // 1. Block termination signals FIRST, before libcec creates any threads,
    //    so every thread inherits the mask and signals arrive only via signalfd.
    sigset_t terminationSignals;
    sigemptyset(&terminationSignals);
    sigaddset(&terminationSignals, SIGINT);
    sigaddset(&terminationSignals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &terminationSignals, nullptr);

    if (!g_log.open(LOG_PATH)) {
        std::cerr << "Could not open log file\n";
        return 3;
    }
    LogUploader logUploader(LOG_PATH, LOG_UPLOAD_CHUNK_PATH, NTFY_TOPIC);

    // 2. Event sources for the loop.
    FileDescriptor signalFileDescriptor(
        signalfd(-1, &terminationSignals, SFD_NONBLOCK | SFD_CLOEXEC));
    FileDescriptor wakeTimerFileDescriptor(
        timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
    FileDescriptor unattendedAppTimerFileDescriptor(
        timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
    FileDescriptor uploadTimerFileDescriptor(
        timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
    FileDescriptor epollFileDescriptor(epoll_create1(EPOLL_CLOEXEC));

    if (!signalFileDescriptor.isValid() || !wakeTimerFileDescriptor.isValid()
        || !unattendedAppTimerFileDescriptor.isValid() || !uploadTimerFileDescriptor.isValid()
        || !epollFileDescriptor.isValid()) {
        g_log.log("Failed to create event file descriptors: {}", std::strerror(errno));
        closeLogAndUploadRemainder(logUploader);
        return 4;
    }

    try {
        g_virtualKeyboard = std::make_unique<VirtualKeyboard>(VIRTUAL_KEYBOARD_KEY_CODES);
        g_log.write("Virtual keyboard created");
    } catch (const std::system_error& error) {
        g_log.log("Virtual keyboard disabled: {}", error.what());
    }

    // 3. libcec. Callbacks and configuration must outlive the adapter (they do: main's scope).
    CEC::ICECCallbacks callbacks;
    callbacks.Clear();
    callbacks.commandReceived = &onCommand;
    callbacks.logMessage = &onLog;
    callbacks.keyPress = &onKeyPress;

    CEC::libcec_configuration configuration;
    configuration.Clear();
    snprintf(configuration.strDeviceName, sizeof(configuration.strDeviceName), "%s", DEVICE_NAME);
    configuration.clientVersion = CEC::LIBCEC_VERSION_CURRENT;
    configuration.bActivateSource = 0;     // we decide when to take the source
    configuration.deviceTypes.Add(CEC::CEC_DEVICE_TYPE_PLAYBACK_DEVICE);
    configuration.callbacks = &callbacks;

    g_adapter = LibCecInitialise(&configuration);
    if (!g_adapter) {
        g_log.write("LibCecInitialise failed");
        closeLogAndUploadRemainder(logUploader);
        return 1;
    }

    CEC::cec_adapter_descriptor adapterDescriptors[MAXIMUM_ADAPTERS];
    const int adapterCount =
        g_adapter->DetectAdapters(adapterDescriptors, MAXIMUM_ADAPTERS, nullptr, true);
    for (int adapterIndex = 0; adapterIndex < adapterCount; ++adapterIndex) {
        g_log.log("Found adapter {}: {} ({})", adapterIndex,
                  adapterDescriptors[adapterIndex].strComName,
                  adapterDescriptors[adapterIndex].strComPath);
    }
    if (adapterCount <= 0 || !g_adapter->Open(adapterDescriptors[0].strComName)) {
        g_log.write("Failed to detect/open adapter");
        UnloadLibCec(g_adapter);
        closeLogAndUploadRemainder(logUploader);
        return 2;
    }

    CEC::libcec_configuration currentConfiguration;
    if (g_adapter->GetCurrentConfiguration(&currentConfiguration)) {
        g_ownPhysicalAddress = currentConfiguration.iPhysicalAddress;
    }
    g_log.log("Own physical address: {}", formatPhysicalAddress(g_ownPhysicalAddress.load()));

    // 4. Register event sources with epoll.
    if (!watchForInput(epollFileDescriptor.get(), signalFileDescriptor.get())
        || !watchForInput(epollFileDescriptor.get(), wakeTimerFileDescriptor.get())
        || !watchForInput(epollFileDescriptor.get(), unattendedAppTimerFileDescriptor.get())
        || !watchForInput(epollFileDescriptor.get(), uploadTimerFileDescriptor.get())
        || !watchForInput(epollFileDescriptor.get(), g_cecEventQueue.notificationFileDescriptor())) {
        g_log.log("epoll_ctl failed: {}", std::strerror(errno));
        closeCecAdapter();
        g_virtualKeyboard.reset();
        closeLogAndUploadRemainder(logUploader);
        return 5;
    }
    armRepeatingTimer(uploadTimerFileDescriptor.get(), FIRST_LOG_UPLOAD_DELAY, LOG_UPLOAD_INTERVAL);

    // 5. State machine. For testing we wake on startup; later the controller does this.
    PowerStateMachine powerStateMachine(g_adapter, wakeTimerFileDescriptor.get(),
                                        unattendedAppTimerFileDescriptor.get(), g_log);
    powerStateMachine.requestWake();

    // 6. Event loop: sleeps in the kernel until something is ready.
    bool running = true;
    while (running) {
        epoll_event readyEvents[MAXIMUM_EVENTS_PER_WAIT];
        const int readyCount = epoll_wait(epollFileDescriptor.get(), readyEvents,
                                          MAXIMUM_EVENTS_PER_WAIT, -1);
        if (readyCount < 0) {
            if (errno == EINTR) continue;
            g_log.log("epoll_wait failed: {}", std::strerror(errno));
            break;
        }

        for (int readyIndex = 0; readyIndex < readyCount; ++readyIndex) {
            const int readyFileDescriptor = readyEvents[readyIndex].data.fd;

            if (readyFileDescriptor == signalFileDescriptor.get()) {
                signalfd_siginfo signalInformation{};
                [[maybe_unused]] const auto bytesRead =
                    read(readyFileDescriptor, &signalInformation, sizeof signalInformation);
                g_log.log("Received signal {}, shutting down", signalInformation.ssi_signo);
                running = false;

            } else if (readyFileDescriptor == wakeTimerFileDescriptor.get()) {
                std::uint64_t expirationCount = 0;
                const auto bytesRead =
                    read(readyFileDescriptor, &expirationCount, sizeof expirationCount);
                // EAGAIN here means the timer was disarmed earlier in this batch.
                if (bytesRead == sizeof expirationCount) {
                    powerStateMachine.onWakeTimerExpired();
                }

            } else if (readyFileDescriptor == unattendedAppTimerFileDescriptor.get()) {
                std::uint64_t expirationCount = 0;
                const auto bytesRead =
                    read(readyFileDescriptor, &expirationCount, sizeof expirationCount);
                // EAGAIN means it was disarmed earlier in this batch (the user came back).
                if (bytesRead == sizeof expirationCount) {
                    powerStateMachine.onUnattendedAppTimerExpired();
                }

            } else if (readyFileDescriptor == uploadTimerFileDescriptor.get()) {
                std::uint64_t expirationCount = 0;
                [[maybe_unused]] const auto bytesRead =
                    read(readyFileDescriptor, &expirationCount, sizeof expirationCount);
                uploadNewLogContent(logUploader);

            } else if (readyFileDescriptor == g_cecEventQueue.notificationFileDescriptor()) {
                for (const CecEvent event : g_cecEventQueue.takeAll()) {
                    powerStateMachine.onCecEvent(event);   // main thread: safe to call libcec
                }
            }
        }
    }

    // 7. Shutdown. The TV is deliberately left alone: restarts shouldn't blank the screen.
    closeCecAdapter();
    g_virtualKeyboard.reset();
    closeLogAndUploadRemainder(logUploader);
    return 0;
}