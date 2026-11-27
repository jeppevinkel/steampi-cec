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
#include <optional>
#include <system_error>
#include <span>
#include <linux/input-event-codes.h>

#include "AppLaunchClient.h"
#include "CecEventQueue.h"
#include "ControllerRouter.h"
#include "ControlServer.h"
#include "EventLoop.h"
#include "FileDescriptor.h"
#include "Logger.h"
#include "LogUploader.h"
#include "PhysicalAddress.h"
#include "PowerStateMachine.h"
#include "SteamControllerPuck.h"
#include "Timers.h"
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
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_ENTER, KEY_BACKSPACE, KEY_HOME,
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

    // Our Samsung sends "clear" when Return is held. Whether that closes the running app or goes
    // to the launcher's home menu is decided on the main thread, which knows about apps.
    if (keyPress->keycode == CEC::CEC_USER_CONTROL_CODE_CLEAR) {
        g_log.write("Remote key: clear (Return held)");
        g_cecEventQueue.push(CecEvent::RemoteHeldReturn);
        return;
    }

    const int linuxKeyCode = linuxKeyForCecKey(keyPress->keycode);
    g_log.log("Remote key: {} -> {}", g_adapter->ToString(keyPress->keycode),
              linuxKeyCode == KEY_RESERVED ? std::string("unmapped") : std::to_string(linuxKeyCode));

    if (linuxKeyCode != KEY_RESERVED && g_virtualKeyboard) {
        g_virtualKeyboard->tap(linuxKeyCode);
    }
}

// ---------- helpers ----------

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

// Return held on the TV remote: like on the TV itself, it closes the running app.
// Without an app, it takes the launcher back to its main menu.
static void handleRemoteHeldReturn(std::optional<ControlServer>& controlServer) {
    if (controlServer && controlServer->closeRunningApp("Return was held on the TV remote")) return;
    if (g_virtualKeyboard) g_virtualKeyboard->tap(KEY_HOME);
}

static int runCommandLine(const int argumentCount, char* arguments[]) {
    const std::string_view option = arguments[1];
    if (option == "--launch" && argumentCount == 3) return runAppLaunchClient(arguments[2]);

    std::cerr << "Usage:\n"
                 "  steampi-cec                    run the daemon (normally started by systemd)\n"
                 "  steampi-cec --launch <app>     start an app through the daemon (used by Flex Launcher)\n";
    return 2;
}

// ---------- main ----------

int main(const int argumentCount, char* arguments[]) {
    if (argumentCount > 1) return runCommandLine(argumentCount, arguments);

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
    armTimer(uploadTimerFileDescriptor.get(), FIRST_LOG_UPLOAD_DELAY, LOG_UPLOAD_INTERVAL);

    // 5. Controller and app objects. Declared before they're created, because they refer to
    //    each other: an app's lease decides who gets the controller, and the power state
    //    machine (created in between) closes apps nobody is watching.
    std::optional<ControllerRouter> controllerRouter;
    std::optional<SteamControllerPuck> steamControllerPuck;
    std::optional<ControlServer> controlServer;

    const auto applyControllerMode = [&controllerRouter, &steamControllerPuck](const ControllerMode mode) {
        if (!controllerRouter) return;
        controllerRouter->setMode(mode);
        if (steamControllerPuck) {
            steamControllerPuck->setLizardModeGrabbed(controllerRouter->mode() != ControllerMode::Released);
        }
    };

    // App launching is optional too: without it, Flex Launcher's tiles still start the apps.
    try {
        controlServer.emplace(epollFileDescriptor.get(), g_log, ControlServer::Callbacks{
            .leaseStarted = [&applyControllerMode](const AppProfile& profile) {
                applyControllerMode(profile.controllerMode);
            },
            .leaseEnded = [&applyControllerMode](const AppProfile&) {
                applyControllerMode(ControllerMode::Navigation);
            },
        });
    } catch (const std::system_error& error) {
        g_log.log("App launching through the daemon disabled: {}", error.what());
    }

    // 6. State machine. The controller or the TV itself wakes us; startup never turns the TV on.
    PowerStateMachine powerStateMachine(g_adapter, wakeTimerFileDescriptor.get(),
                                        unattendedAppTimerFileDescriptor.get(), g_log,
        [&controlServer](const std::string_view reason) {
            return controlServer && controlServer->closeRunningApp(reason);
        });
    powerStateMachine.startUp();

    // Controller support is optional: if it can't be set up, CEC and the TV remote keep working.
    try {
        controllerRouter.emplace(epollFileDescriptor.get(), powerStateMachine,
                                 g_virtualKeyboard.get(), g_log);
        steamControllerPuck.emplace(epollFileDescriptor.get(), g_log,
            [&controllerRouter](const std::span<const uint8_t> report) {
                controllerRouter->onReport(report);
            });
    } catch (const std::system_error& error) {
        g_log.log("Controller support disabled: {}", error.what());
        steamControllerPuck.reset();
        controllerRouter.reset();
    }

    // 7. Event loop: sleeps in the kernel until something is ready.
    int exitCode = 0;
    try {
        bool running = true;
        while (running) {
            epoll_event readyEvents[MAXIMUM_EVENTS_PER_WAIT];
            const int readyCount = epoll_wait(epollFileDescriptor.get(), readyEvents,
                                              MAXIMUM_EVENTS_PER_WAIT, -1);
            if (readyCount < 0) {
                if (errno == EINTR) continue;
                g_log.log("epoll_wait failed: {}", std::strerror(errno));
                exitCode = 7;   // non-zero, so systemd restarts the daemon
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
                    // Nothing to read means the timer was disarmed earlier in this batch.
                    if (consumeTimerExpiration(readyFileDescriptor)) {
                        powerStateMachine.onWakeTimerExpired();
                    }

                } else if (readyFileDescriptor == unattendedAppTimerFileDescriptor.get()) {
                    // Nothing to read means it was disarmed earlier in this batch (the user came back).
                    if (consumeTimerExpiration(readyFileDescriptor)) {
                        powerStateMachine.onUnattendedAppTimerExpired();
                    }

                } else if (readyFileDescriptor == uploadTimerFileDescriptor.get()) {
                    consumeTimerExpiration(readyFileDescriptor);
                    uploadNewLogContent(logUploader);

                } else if (readyFileDescriptor == g_cecEventQueue.notificationFileDescriptor()) {
                    for (const CecEvent event : g_cecEventQueue.takeAll()) {
                        if (event == CecEvent::RemoteHeldReturn) {
                            handleRemoteHeldReturn(controlServer);
                            continue;
                        }
                        powerStateMachine.onCecEvent(event);   // main thread: safe to call libcec
                    }
                } else if (steamControllerPuck
                           && steamControllerPuck->handleReadyFileDescriptor(readyFileDescriptor)) {
                    // Controller reports, or a rescan for the puck: handled inside.
                } else if (controllerRouter
                           && controllerRouter->handleReadyFileDescriptor(readyFileDescriptor)) {
                    // Key repeat for a held direction: handled inside.
                } else if (controlServer
                           && controlServer->handleReadyFileDescriptor(readyFileDescriptor)) {
                    // Launch clients: leases, app output, exits. Handled inside.
                }
            }
        }
    } catch (const std::exception& error) {
        g_log.log("Unexpected error, shutting down: {}", error.what());
        exitCode = 6;   // non-zero, so systemd restarts the daemon
    }

    // 8. Shutdown. The TV is deliberately left alone: restarts shouldn't blank the screen.
    //    A running app keeps running: its launch client reconnects to the next daemon.
    closeCecAdapter();
    g_virtualKeyboard.reset();
    closeLogAndUploadRemainder(logUploader);
    return exitCode;
}