#include "AppLaunchClient.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <utility>
#include <optional>
#include <string>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "AppProfiles.h"
#include "ControlProtocol.h"
#include "FileDescriptor.h"

extern char** environ;

namespace {
    using Clock = std::chrono::steady_clock;

    // How long to wait for GO. The daemon answers within milliseconds unless it's stuck.
    constexpr auto LEASE_ANSWER_TIMEOUT = std::chrono::seconds(5);
    constexpr auto RECONNECT_INTERVAL = std::chrono::seconds(2);
    // After TERMINATE: time for the app to close cleanly before it's killed.
    constexpr auto TERMINATION_GRACE_PERIOD = std::chrono::seconds(5);
    // Only if pidfd_open isn't available: how often to check whether the app has exited.
    constexpr auto EXIT_CHECK_INTERVAL = std::chrono::milliseconds(250);
    // Keeps a chatty app from flooding the daemon's log (and the ntfy uploads).
    constexpr std::size_t MAXIMUM_FORWARDED_LINES = 5000;
    constexpr std::size_t MAXIMUM_FORWARDED_LINE_LENGTH = 1000;
    // App output waiting to be sent while the daemon is busy. Beyond this, output lines are
    // dropped (never control messages), so a flood can't eat the Pi's memory.
    constexpr std::size_t MAXIMUM_QUEUED_OUTPUT_BYTES = 256 * 1024;
    // On exit: how long to wait for the daemon to take the last messages (EXITED).
    constexpr auto FINAL_FLUSH_TIMEOUT = std::chrono::seconds(2);

    volatile std::sig_atomic_t g_terminationSignalReceived = 0;

    void onTerminationSignal(int) {
        g_terminationSignalReceived = 1;
    }

    void printMessage(const std::string_view message) {
        // Ends up in ~/.startx.log, next to Flex Launcher's own output.
        std::cerr << "steampi-cec --launch: " << message << std::endl;
    }

    // Splits a byte stream into lines.
    class LineBuffer {
    public:
        template <typename LineHandler>
        void append(const std::string_view data, LineHandler&& handleLine) {
            pending_.append(data);
            std::size_t lineStart = 0;
            for (auto newlinePosition = pending_.find('\n'); newlinePosition != std::string::npos;
                 newlinePosition = pending_.find('\n', lineStart)) {
                handleLine(std::string_view(pending_).substr(lineStart, newlinePosition - lineStart));
                lineStart = newlinePosition + 1;
            }
            pending_.erase(0, lineStart);
        }

        // A last line without a newline, e.g. when the app exits mid-line.
        template <typename LineHandler>
        void flush(LineHandler&& handleLine) {
            if (!pending_.empty()) handleLine(std::string_view(pending_));
            pending_.clear();
        }

    private:
        std::string pending_;
    };

    void installTerminationSignalHandlers() {
        struct sigaction signalAction{};
        signalAction.sa_handler = onTerminationSignal;
        sigemptyset(&signalAction.sa_mask);
        signalAction.sa_flags = 0;   // no SA_RESTART: poll() returns EINTR, so the loop notices
        for (const int signalNumber : {SIGTERM, SIGINT, SIGHUP}) sigaction(signalNumber, &signalAction, nullptr);
    }

    // Reads what's available. Returns false if the other end is gone.
    bool readAvailable(const int fileDescriptor, std::string& received) {
        std::array<char, 4096> readBuffer{};
        while (true) {
            const ssize_t bytesRead = read(fileDescriptor, readBuffer.data(), readBuffer.size());
            if (bytesRead > 0) {
                received.append(readBuffer.data(), static_cast<std::size_t>(bytesRead));
                continue;
            }
            if (bytesRead < 0 && errno == EINTR) continue;
            return bytesRead < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
        }
    }

    // The connection to the daemon, with a send queue: writes never block (a busy daemon must
    // not stall the app's output pipe), and nothing important is lost when the socket is full.
    class DaemonConnection {
    public:
        bool connect() {
            disconnect();
            FileDescriptor daemonSocket(socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
            if (!daemonSocket.isValid()) return false;

            const std::string socketPath = ControlProtocol::socketPath();
            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            if (socketPath.size() >= sizeof(address.sun_path)) return false;
            std::memcpy(address.sun_path, socketPath.c_str(), socketPath.size() + 1);
            if (::connect(daemonSocket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof address) < 0) {
                return false;
            }
            socket_ = std::move(daemonSocket);
            return true;
        }

        void disconnect() {
            socket_ = FileDescriptor();
            queuedOutput_.clear();
            input = LineBuffer();
        }

        [[nodiscard]] bool isConnected() const { return socket_.isValid(); }
        [[nodiscard]] int fileDescriptor() const { return socket_.get(); }
        [[nodiscard]] bool hasQueuedOutput() const { return !queuedOutput_.empty(); }

        // Control messages are always queued. App output is dropped while the queue is full;
        // the drop is reported once there's room again.
        void queueControlMessage(const std::string_view line) { queue(line); }
        void queueAppOutput(const std::string_view line) {
            if (queuedOutput_.size() > MAXIMUM_QUEUED_OUTPUT_BYTES) {
                ++droppedOutputLineCount_;
                return;
            }
            if (droppedOutputLineCount_ > 0) {
                queue(std::string(ControlProtocol::OUTPUT) + " (" + std::to_string(droppedOutputLineCount_)
                      + " lines dropped: the daemon couldn't keep up; see ~/.startx.log)");
                droppedOutputLineCount_ = 0;
            }
            queue(std::string(ControlProtocol::OUTPUT) + " " + std::string(line));
        }

        // Writes as much of the queue as the socket takes. Returns false if the daemon is gone.
        bool writeQueuedOutput() {
            while (!queuedOutput_.empty()) {
                const ssize_t bytesSent = send(socket_.get(), queuedOutput_.data(), queuedOutput_.size(),
                                               MSG_NOSIGNAL | MSG_DONTWAIT);
                if (bytesSent > 0) {
                    queuedOutput_.erase(0, static_cast<std::size_t>(bytesSent));
                    continue;
                }
                if (bytesSent < 0 && errno == EINTR) continue;
                return bytesSent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
            }
            return true;
        }

        // Before exiting: give the daemon a moment to take what's left.
        void flushQueuedOutput(const Clock::duration timeout) {
            const auto deadline = Clock::now() + timeout;
            while (isConnected() && hasQueuedOutput() && writeQueuedOutput() && hasQueuedOutput()) {
                const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
                if (remaining.count() <= 0) return;
                pollfd socketPoll{socket_.get(), POLLOUT, 0};
                if (poll(&socketPoll, 1, static_cast<int>(remaining.count())) == 0) return;
            }
        }

        LineBuffer input;

    private:
        void queue(const std::string_view line) {
            if (!isConnected()) return;
            queuedOutput_.append(line);
            queuedOutput_ += '\n';
        }

        FileDescriptor socket_;
        std::string queuedOutput_;
        std::size_t droppedOutputLineCount_ = 0;
    };

    enum class LeaseAnswer { Go, Denied, NoAnswer };

    // Sends LAUNCH and waits for the answer. deniedReason is set when the answer is DENIED.
    LeaseAnswer requestLease(DaemonConnection& daemon, const std::string_view profileName, std::string& deniedReason) {
        daemon.queueControlMessage(std::string(ControlProtocol::LAUNCH) + " " + std::string(profileName));
        if (!daemon.writeQueuedOutput()) return LeaseAnswer::NoAnswer;

        const auto deadline = Clock::now() + LEASE_ANSWER_TIMEOUT;
        std::optional<LeaseAnswer> answer;
        while (!answer) {
            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
            if (remaining.count() <= 0) return LeaseAnswer::NoAnswer;

            pollfd socketPoll{daemon.fileDescriptor(), POLLIN, 0};
            const int readyCount = poll(&socketPoll, 1, static_cast<int>(remaining.count()));
            if (readyCount < 0 && errno == EINTR) continue;
            if (readyCount <= 0) return LeaseAnswer::NoAnswer;

            std::string received;
            const bool stillConnected = readAvailable(daemon.fileDescriptor(), received);
            daemon.input.append(received, [&](const std::string_view line) {
                if (answer) return;
                const auto [word, argument] = ControlProtocol::parseMessage(line);
                if (word == ControlProtocol::GO) answer = LeaseAnswer::Go;
                if (word == ControlProtocol::DENIED) {
                    deniedReason = std::string(argument);
                    answer = LeaseAnswer::Denied;
                }
            });
            if (!answer && !stillConnected) return LeaseAnswer::NoAnswer;
        }
        return *answer;
    }

    // Control characters (colour codes, carriage returns) would garble the daemon's log.
    std::string cleanForLog(std::string_view line) {
        if (line.size() > MAXIMUM_FORWARDED_LINE_LENGTH) line = line.substr(0, MAXIMUM_FORWARDED_LINE_LENGTH);
        std::string cleaned(line);
        for (char& character : cleaned) {
            if (static_cast<unsigned char>(character) < 0x20 && character != '\t') character = ' ';
        }
        return cleaned;
    }

    // A file descriptor that becomes readable when the process exits, so poll() can wait for it.
    // The raw system call works on any glibc; it needs Linux 5.3 or later (the Pi runs 6.x).
    FileDescriptor openProcessFileDescriptor(const pid_t processId) {
        return FileDescriptor(static_cast<int>(syscall(SYS_pidfd_open, processId, 0)));
    }

    int exitCodeFromWaitStatus(const int waitStatus) {
        if (WIFEXITED(waitStatus)) return WEXITSTATUS(waitStatus);
        if (WIFSIGNALED(waitStatus)) return 128 + WTERMSIG(waitStatus);
        return 1;
    }
}

int runAppLaunchClient(const std::string_view profileName) {
    const AppProfile* profile = findAppProfile(profileName);
    if (!profile) {
        std::string knownNames;
        for (const AppProfile& knownProfile : APP_PROFILES) knownNames += " " + std::string(knownProfile.name);
        printMessage("unknown app '" + std::string(profileName) + "'; known apps:" + knownNames);
        return 2;
    }
    installTerminationSignalHandlers();

    // ---------- Lease ----------
    DaemonConnection daemon;
    bool reconnectAllowed = true;
    if (daemon.connect()) {
        std::string deniedReason;
        switch (requestLease(daemon, profile->name, deniedReason)) {
            case LeaseAnswer::Go:
                break;
            case LeaseAnswer::Denied:
                printMessage("the daemon refused to start " + std::string(profile->name) + ": " + deniedReason);
                return 1;
            case LeaseAnswer::NoAnswer:
                printMessage("no answer from the daemon; starting without it");
                daemon.disconnect();
                break;
        }
    } else {
        printMessage("daemon not reachable at " + ControlProtocol::socketPath()
                     + "; starting without it (the controller isn't handed over)");
    }

    // ---------- Start the app ----------
    int outputPipe[2];
    if (pipe2(outputPipe, O_CLOEXEC) < 0) {
        printMessage(std::string("pipe failed: ") + std::strerror(errno));
        return 1;
    }
    FileDescriptor outputReadEnd(outputPipe[0]);
    FileDescriptor outputWriteEnd(outputPipe[1]);
    fcntl(outputReadEnd.get(), F_SETFL, fcntl(outputReadEnd.get(), F_GETFL) | O_NONBLOCK);

    posix_spawn_file_actions_t fileActions;
    posix_spawn_file_actions_init(&fileActions);
    // dup2 clears close-on-exec on the copies, so the app keeps its stdout and stderr.
    posix_spawn_file_actions_adddup2(&fileActions, outputWriteEnd.get(), STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&fileActions, outputWriteEnd.get(), STDERR_FILENO);

    posix_spawnattr_t spawnAttributes;
    posix_spawnattr_init(&spawnAttributes);
    // Its own process group, so TERMINATE reaches everything the app starts.
    posix_spawnattr_setpgroup(&spawnAttributes, 0);
    sigset_t emptySignalSet;
    sigemptyset(&emptySignalSet);
    posix_spawnattr_setsigmask(&spawnAttributes, &emptySignalSet);
    posix_spawnattr_setflags(&spawnAttributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK);

    std::string command(profile->command);
    std::array<char*, 4> commandArguments{const_cast<char*>("/bin/sh"), const_cast<char*>("-c"),
                                          command.data(), nullptr};
    pid_t appProcessId = -1;
    const int spawnResult = posix_spawn(&appProcessId, "/bin/sh", &fileActions, &spawnAttributes,
                                        commandArguments.data(), environ);
    posix_spawn_file_actions_destroy(&fileActions);
    posix_spawnattr_destroy(&spawnAttributes);
    outputWriteEnd = FileDescriptor();   // only the app writes to the pipe now

    if (spawnResult != 0) {
        printMessage(std::string("could not start /bin/sh: ") + std::strerror(spawnResult));
        daemon.queueControlMessage(std::string(ControlProtocol::EXITED) + " 127");
        daemon.flushQueuedOutput(FINAL_FLUSH_TIMEOUT);
        return 127;
    }

    // ---------- Run until the app exits ----------
    FileDescriptor appProcessFileDescriptor = openProcessFileDescriptor(appProcessId);
    LineBuffer appOutput;
    std::size_t forwardedLineCount = 0;
    std::optional<Clock::time_point> forceKillTime;
    bool forceKillSent = false;
    auto nextReconnectTime = Clock::now() + RECONNECT_INTERVAL;
    std::optional<int> appWaitStatus;

    const auto forwardOutputLine = [&](const std::string_view line) {
        std::cerr << line << '\n';
        if (!profile->forwardOutput || !daemon.isConnected()) return;
        if (forwardedLineCount < MAXIMUM_FORWARDED_LINES) {
            daemon.queueAppOutput(cleanForLog(line));
        } else if (forwardedLineCount == MAXIMUM_FORWARDED_LINES) {
            daemon.queueAppOutput("(more output not forwarded; see ~/.startx.log)");
        }
        ++forwardedLineCount;
    };

    const auto terminateApp = [&](const std::string_view reason) {
        if (forceKillTime) return;
        printMessage(std::string(reason) + "; stopping " + std::string(profile->name));
        kill(-appProcessId, SIGTERM);
        forceKillTime = Clock::now() + TERMINATION_GRACE_PERIOD;
    };

    const auto handleDaemonLine = [&](const std::string_view line) {
        const auto [word, argument] = ControlProtocol::parseMessage(line);
        if (word == ControlProtocol::TERMINATE) terminateApp("the daemon asked to close the app");
        if (word == ControlProtocol::DENIED) {
            // After a reconnect the daemon may already be running something else.
            printMessage("the daemon refused the reconnect: " + std::string(argument));
            reconnectAllowed = false;
        }
    };

    while (!appWaitStatus) {
        if (g_terminationSignalReceived) terminateApp("received a termination signal");

        // The nearest deadline decides how long poll may sleep.
        std::optional<Clock::time_point> wakeTime;
        const auto considerWakeTime = [&](const Clock::time_point candidate) {
            if (!wakeTime || candidate < *wakeTime) wakeTime = candidate;
        };
        if (forceKillTime && !forceKillSent) considerWakeTime(*forceKillTime);
        if (!daemon.isConnected() && reconnectAllowed) considerWakeTime(nextReconnectTime);
        if (!appProcessFileDescriptor.isValid()) considerWakeTime(Clock::now() + EXIT_CHECK_INTERVAL);
        int pollTimeoutMilliseconds = -1;
        if (wakeTime) {
            const auto untilWake = std::chrono::ceil<std::chrono::milliseconds>(*wakeTime - Clock::now());
            pollTimeoutMilliseconds = static_cast<int>(std::max<std::chrono::milliseconds::rep>(0, untilWake.count()));
        }

        std::vector<pollfd> pollEntries;
        if (appProcessFileDescriptor.isValid()) pollEntries.push_back({appProcessFileDescriptor.get(), POLLIN, 0});
        if (outputReadEnd.isValid()) pollEntries.push_back({outputReadEnd.get(), POLLIN, 0});
        if (daemon.isConnected()) {
            const short daemonEvents = daemon.hasQueuedOutput() ? POLLIN | POLLOUT : POLLIN;
            pollEntries.push_back({daemon.fileDescriptor(), daemonEvents, 0});
        }

        const int readyCount = poll(pollEntries.data(), pollEntries.size(), pollTimeoutMilliseconds);
        if (readyCount < 0 && errno != EINTR) {
            printMessage(std::string("poll failed: ") + std::strerror(errno));
            break;
        }
        const auto readyEvents = [&](const int fileDescriptor) -> short {
            if (readyCount <= 0 || fileDescriptor < 0) return 0;
            for (const pollfd& entry : pollEntries) {
                if (entry.fd == fileDescriptor) return entry.revents;
            }
            return 0;
        };
        const auto isReady = [&](const FileDescriptor& fileDescriptor) {
            return fileDescriptor.isValid() && readyEvents(fileDescriptor.get()) != 0;
        };

        if (isReady(outputReadEnd)) {
            std::string received;
            const bool stillOpen = readAvailable(outputReadEnd.get(), received);
            appOutput.append(received, forwardOutputLine);
            if (!stillOpen) outputReadEnd = FileDescriptor();
        }

        if (daemon.isConnected()) {
            const short daemonEvents = readyEvents(daemon.fileDescriptor());
            bool stillConnected = true;
            if (daemonEvents & (POLLIN | POLLHUP | POLLERR)) {
                std::string received;
                stillConnected = readAvailable(daemon.fileDescriptor(), received);
                daemon.input.append(received, handleDaemonLine);
            }
            if (stillConnected && daemon.hasQueuedOutput()) stillConnected = daemon.writeQueuedOutput();
            if (!stillConnected) {
                printMessage("lost the connection to the daemon; reconnecting in the background");
                daemon.disconnect();
                nextReconnectTime = Clock::now() + RECONNECT_INTERVAL;
            }
        }

        if (!appProcessFileDescriptor.isValid() || isReady(appProcessFileDescriptor)) {
            int waitStatus = 0;
            if (waitpid(appProcessId, &waitStatus, WNOHANG) == appProcessId) appWaitStatus = waitStatus;
        }

        const auto now = Clock::now();
        if (!appWaitStatus && forceKillTime && !forceKillSent && now >= *forceKillTime) {
            printMessage("the app didn't close in time; killing it");
            kill(-appProcessId, SIGKILL);
            forceKillSent = true;
        }

        if (!appWaitStatus && !daemon.isConnected() && reconnectAllowed && now >= nextReconnectTime) {
            nextReconnectTime = now + RECONNECT_INTERVAL;
            if (daemon.connect()) {
                // The answer (GO, or DENIED) arrives through the loop like any other message.
                printMessage("reconnected to the daemon");
                daemon.queueControlMessage(std::string(ControlProtocol::LAUNCH) + " " + std::string(profile->name));
            }
        }
    }

    if (!appWaitStatus) {
        // Only after a poll failure: wait for the app the plain way.
        int waitStatus = 0;
        while (waitpid(appProcessId, &waitStatus, 0) < 0 && errno == EINTR) {}
        appWaitStatus = waitStatus;
    }

    // Output written just before exiting. Don't wait for end of file: something the app
    // started in the background may keep the pipe open.
    if (outputReadEnd.isValid()) {
        std::string received;
        readAvailable(outputReadEnd.get(), received);
        appOutput.append(received, forwardOutputLine);
    }
    appOutput.flush(forwardOutputLine);

    const int exitCode = exitCodeFromWaitStatus(*appWaitStatus);
    daemon.queueControlMessage(std::string(ControlProtocol::EXITED) + " " + std::to_string(exitCode));
    daemon.flushQueuedOutput(FINAL_FLUSH_TIMEOUT);
    return exitCode;
}
