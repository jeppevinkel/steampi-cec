#include "ControlServer.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <system_error>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "ControlProtocol.h"
#include "EventLoop.h"

namespace {
    // Lines are short; anything longer without a newline is a broken or hostile client.
    constexpr std::size_t MAXIMUM_PENDING_INPUT = 8192;
    constexpr int LISTEN_BACKLOG = 4;

    [[noreturn]] void throwSystemError(const std::string& operation) {
        throw std::system_error(errno, std::generic_category(), operation);
    }
}

ControlServer::ControlServer(const int epollFileDescriptor, Logger& log, Callbacks callbacks)
    : epollFileDescriptor_(epollFileDescriptor),
      log_(log),
      callbacks_(std::move(callbacks)),
      socketPath_(ControlProtocol::socketPath()),
      listeningSocket_(socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0)) {
    if (!listeningSocket_.isValid()) throwSystemError("control socket");

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socketPath_.size() >= sizeof(address.sun_path)) {
        errno = ENAMETOOLONG;
        throwSystemError("control socket path " + socketPath_);
    }
    std::memcpy(address.sun_path, socketPath_.c_str(), socketPath_.size() + 1);

    unlink(socketPath_.c_str());   // a socket file left behind by a crashed daemon
    if (bind(listeningSocket_.get(), reinterpret_cast<const sockaddr*>(&address), sizeof address) < 0) {
        throwSystemError("bind " + socketPath_);
    }
    if (listen(listeningSocket_.get(), LISTEN_BACKLOG) < 0) throwSystemError("listen " + socketPath_);
    if (!watchForInput(epollFileDescriptor_, listeningSocket_.get())) throwSystemError("watch control socket");

    log_.log("Control socket listening on {}", socketPath_);
}

ControlServer::~ControlServer() {
    // Closing the connections ends any lease without callbacks: the daemon is shutting down.
    connections_.clear();
    unlink(socketPath_.c_str());
}

bool ControlServer::handleReadyFileDescriptor(const int readyFileDescriptor) {
    if (readyFileDescriptor == listeningSocket_.get()) {
        acceptConnections();
        return true;
    }

    const auto connectionPosition = std::ranges::find_if(connections_,
        [readyFileDescriptor](const auto& connection) { return connection->socket.get() == readyFileDescriptor; });
    if (connectionPosition == connections_.end()) return false;

    const auto connectionIndex = static_cast<std::size_t>(connectionPosition - connections_.begin());
    if (!readFromConnection(*connections_[connectionIndex])) closeConnection(connectionIndex);
    return true;
}

bool ControlServer::closeRunningApp(const std::string_view reason) {
    if (!leaseHolder_) return false;
    log_.log("Closing {}: {}", leaseHolder_->leasedProfile->name, reason);
    sendLine(*leaseHolder_, ControlProtocol::TERMINATE);
    return true;
}

void ControlServer::acceptConnections() {
    while (true) {
        FileDescriptor clientSocket(accept4(listeningSocket_.get(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC));
        if (!clientSocket.isValid()) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                log_.log("Control socket: accept failed: {}", std::strerror(errno));
            }
            return;
        }
        if (!watchForInput(epollFileDescriptor_, clientSocket.get())) {
            log_.log("Control socket: cannot watch connection: {}", std::strerror(errno));
            continue;
        }
        auto connection = std::make_unique<ClientConnection>();
        connection->socket = std::move(clientSocket);
        connections_.push_back(std::move(connection));
    }
}

bool ControlServer::readFromConnection(ClientConnection& connection) {
    std::array<char, 4096> readBuffer{};
    bool connectionEnded = false;
    while (true) {
        const ssize_t bytesRead = read(connection.socket.get(), readBuffer.data(), readBuffer.size());
        if (bytesRead > 0) {
            connection.pendingInput.append(readBuffer.data(), static_cast<std::size_t>(bytesRead));
            continue;
        }
        if (bytesRead < 0 && errno == EINTR) continue;
        // EAGAIN: everything read for now. End of file or an error: the client is gone, but
        // what it sent before leaving (like EXITED) is still handled below.
        connectionEnded = !(bytesRead < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
        break;
    }

    std::size_t lineStart = 0;
    for (auto newlinePosition = connection.pendingInput.find('\n');
         newlinePosition != std::string::npos;
         newlinePosition = connection.pendingInput.find('\n', lineStart)) {
        handleLine(connection, std::string_view(connection.pendingInput).substr(lineStart, newlinePosition - lineStart));
        lineStart = newlinePosition + 1;
    }
    connection.pendingInput.erase(0, lineStart);

    if (connection.pendingInput.size() > MAXIMUM_PENDING_INPUT) {
        log_.write("Control socket: line too long; closing the connection");
        return false;
    }
    return !connectionEnded;
}

void ControlServer::handleLine(ClientConnection& connection, const std::string_view line) {
    const auto [word, argument] = ControlProtocol::parseMessage(line);

    if (word == ControlProtocol::OUTPUT) {
        if (connection.leasedProfile) log_.log("[{}] {}", connection.leasedProfile->name, argument);
        return;
    }
    if (word == ControlProtocol::EXITED) {
        if (connection.leasedProfile) log_.log("{} exited with code {}", connection.leasedProfile->name, argument);
        return;
    }
    if (word == ControlProtocol::LAUNCH) {
        const AppProfile* profile = findAppProfile(argument);
        if (!profile) {
            log_.log("Launch of unknown app '{}' refused", argument);
            sendLine(connection, std::string(ControlProtocol::DENIED) + " unknown app");
            return;
        }
        if (leaseHolder_ && leaseHolder_ != &connection) {
            log_.log("Launch of {} refused: {} is still running", profile->name, leaseHolder_->leasedProfile->name);
            sendLine(connection, std::string(ControlProtocol::DENIED) + " another app is running");
            return;
        }
        if (leaseHolder_ == &connection) return;   // repeated LAUNCH on the same connection

        leaseHolder_ = &connection;
        connection.leasedProfile = profile;
        log_.log("Lease started: {} (controller: {})", profile->name, controllerModeName(profile->controllerMode));
        if (callbacks_.leaseStarted) callbacks_.leaseStarted(*profile);
        sendLine(connection, ControlProtocol::GO);
        return;
    }
    log_.log("Control socket: unknown message '{}'", line);
}

void ControlServer::closeConnection(const std::size_t connectionIndex) {
    // Moved out first, so the callback below can't see a half-removed connection.
    std::unique_ptr<ClientConnection> connection = std::move(connections_[connectionIndex]);
    connections_.erase(connections_.begin() + static_cast<std::ptrdiff_t>(connectionIndex));
    stopWatching(epollFileDescriptor_, connection->socket.get());

    if (leaseHolder_ == connection.get()) {
        leaseHolder_ = nullptr;
        log_.log("Lease ended: {}", connection->leasedProfile->name);
        if (callbacks_.leaseEnded) callbacks_.leaseEnded(*connection->leasedProfile);
    }
}

void ControlServer::sendLine(const ClientConnection& connection, const std::string_view line) {
    std::string message(line);
    message += '\n';
    // MSG_NOSIGNAL: a client that already went away must not kill the daemon with SIGPIPE.
    // Messages are tiny, so a full socket buffer (a client that stopped reading) just drops them.
    [[maybe_unused]] const auto bytesSent =
        send(connection.socket.get(), message.data(), message.size(), MSG_NOSIGNAL | MSG_DONTWAIT);
}
