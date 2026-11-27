#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "AppProfiles.h"
#include "FileDescriptor.h"
#include "Logger.h"

// The daemon's side of the control socket (see ControlProtocol.h). Launch clients started by
// Flex Launcher connect here; at most one of them holds the lease for the running app.
class ControlServer {
public:
    struct Callbacks {
        // Before GO is sent: hand the controller over the way the profile asks.
        std::function<void(const AppProfile&)> leaseStarted;
        // The app has exited (or its launch client went away): take the controller back.
        std::function<void(const AppProfile&)> leaseEnded;
    };

    // Throws std::system_error if the socket can't be set up.
    ControlServer(int epollFileDescriptor, Logger& log, Callbacks callbacks);
    ~ControlServer();

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    // Returns true if the file descriptor belonged to the server (and was handled).
    bool handleReadyFileDescriptor(int readyFileDescriptor);

    // Asks the running app to close. Returns false if no app is running.
    bool closeRunningApp(std::string_view reason);

private:
    struct ClientConnection {
        FileDescriptor socket;
        std::string pendingInput;
        const AppProfile* leasedProfile = nullptr;
    };

    void acceptConnections();
    // Returns false if the connection should be closed.
    bool readFromConnection(ClientConnection& connection);
    void handleLine(ClientConnection& connection, std::string_view line);
    void closeConnection(std::size_t connectionIndex);
    static void sendLine(const ClientConnection& connection, std::string_view line);

    int epollFileDescriptor_;
    Logger& log_;
    Callbacks callbacks_;
    std::string socketPath_;
    FileDescriptor listeningSocket_;
    std::vector<std::unique_ptr<ClientConnection>> connections_;
    ClientConnection* leaseHolder_ = nullptr;
};
