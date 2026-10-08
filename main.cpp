#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <iostream>
#include <string>
#include <thread>
#include <libcec/cec.h>
#include <libcec/cecloader.h>
#include <sys/wait.h>

#include "Logger.h"

static constexpr auto DEVICE_NAME = "PiTV";
static constexpr auto LOG_PATH     = "/home/jeppe/cec.log";
static constexpr auto NTFY_TOPIC = "pitv-cec-jeppe";
static constexpr auto LISTEN_TIME  = std::chrono::minutes(2);

static CEC::ICECAdapter* g_adapter = nullptr;
static std::atomic g_running(true);
static Logger g_log;

// ---------- libcec callbacks ----------
// libcec calls these from its own threads; Logger is internally synchronized.
static void onCommand(void*, const CEC::cec_command* cmd) {
    std::string params;
    for (uint8_t i = 0; i < cmd->parameters.size; ++i)
        params += std::format(" {:02x}", cmd->parameters[i]);

    g_log.log("CMD {} -> {}: {}{}",
              g_adapter->ToString(cmd->initiator),
              g_adapter->ToString(cmd->destination),
              g_adapter->ToString(cmd->opcode),
              params);

    switch (cmd->opcode) {
        case CEC::CEC_OPCODE_STANDBY:         g_log.write("  >> STANDBY"); break;
        case CEC::CEC_OPCODE_SET_STREAM_PATH: g_log.write("  >> SET_STREAM_PATH"); break;
        default: break;
    }
}

// libcec's own internal log (very useful for debugging CEC)
static void onLog(void*, const CEC::cec_log_message* msg) {
    const char* lvl = "?";
    switch (msg->level) {
        case CEC::CEC_LOG_ERROR:   lvl = "ERROR";   break;
        case CEC::CEC_LOG_WARNING: lvl = "WARNING"; break;
        case CEC::CEC_LOG_NOTICE:  lvl = "NOTICE";  break;
        case CEC::CEC_LOG_TRAFFIC: lvl = "TRAFFIC"; break;
        case CEC::CEC_LOG_DEBUG:   lvl = "DEBUG";   break;
        default: break;
    }
    g_log.log("[libcec {}] {}", lvl, msg->message);
}

static void signalHandler(int) {
    g_running.store(false);
}

static void uploadLog() {
    const std::string cmd = std::format(
        "curl -sS --fail --max-time 30 -T '{0}' -H 'Filename: cec.log' https://ntfy.sh/{1}",
        LOG_PATH, NTFY_TOPIC);

    const int status = std::system(cmd.c_str());
    if (WIFEXITED(status))
        std::cout << "\nUpload exited with code " << WEXITSTATUS(status) << std::endl;
    else
        std::cout << "\nUpload terminated abnormally" << std::endl;
}

int main() {
    if (!g_log.open(LOG_PATH)) {
        std::cerr << "Could not open log file\n";
        return 3;
    }

    // Setup callbacks
    CEC::ICECCallbacks callbacks;
    callbacks.Clear();
    callbacks.commandReceived = &onCommand;
    callbacks.logMessage = &onLog;

    // Setup config
    CEC::libcec_configuration config;
    config.Clear();
    snprintf(config.strDeviceName, sizeof(config.strDeviceName), "%s", DEVICE_NAME);
    config.clientVersion = CEC::LIBCEC_VERSION_CURRENT;
    config.bActivateSource = 0;
    config.deviceTypes.Add(CEC::CEC_DEVICE_TYPE_PLAYBACK_DEVICE);
    config.callbacks = &callbacks;

    // Initialize adapter with config
    g_adapter = LibCecInitialise(&config);
    if (!g_adapter) {
        g_log.write("LibCecInitialise failed");
        g_log.close();
        uploadLog();
        return 1;
    }

    // Attempt to connect adapter
    CEC::cec_adapter_descriptor devs[4];
    const int8_t n = g_adapter->DetectAdapters(devs, 4, nullptr, true);
    if (n <= 0 || !g_adapter->Open(devs[0].strComName)) {
        g_log.write("Failed to detect/open adapter");
        UnloadLibCec(g_adapter);
        g_log.close();
        uploadLog();
        return 2;
    }

    // Configure termination signal handler
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // ---------- Wake TV and grab focus ----------
    const bool powered = g_adapter->PowerOnDevices(CEC::CECDEVICE_TV);
    g_log.log("Power on TV: {}", powered ? "ok" : "FAILED");

    // TVs can take a few seconds to wake; Active Source sent too early may be ignored
    std::this_thread::sleep_for(std::chrono::seconds(3));

    const bool active = g_adapter->SetActiveSource(CEC::CEC_DEVICE_TYPE_PLAYBACK_DEVICE);
    g_log.log("Set active source: {}", active ? "ok" : "FAILED");

    // ---------- Listen ----------
    g_log.write("Listening...");
    const auto deadline = std::chrono::steady_clock::now() + LISTEN_TIME;
    while (g_running && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    g_log.write(g_running ? "Listen time elapsed" : "Interrupted by signal");

    // ---------- Standby TV ----------
    if (g_running) {
        const bool standby = g_adapter->StandbyDevices(CEC::CECDEVICE_TV);
        g_log.log("Standby TV: {}", standby ? "ok" : "FAILED");
    }

    // ---------- Shutdown ----------
    g_adapter->Close();
    UnloadLibCec(g_adapter);
    g_log.write("Adapter closed");
    g_log.close();
    uploadLog();
    return 0;
}