#pragma once

#include <cstdint>
#include <memory>

class MegaDriveEnvironment;

/**
 * Single-client binary TCP automation server for a MegaDriveEnvironment.
 *
 * The server binds all interfaces, sends data only in direct response to a
 * request, and owns one worker thread. Port 0 disables it. See
 * docs/remote-access-protocol.md for the wire format.
 */
class RemoteAccess {
    public:
    explicit RemoteAccess(MegaDriveEnvironment *environment, std::uint16_t port = 6969);
    ~RemoteAccess();

    RemoteAccess(const RemoteAccess &) = delete;
    RemoteAccess &operator=(const RemoteAccess &) = delete;

    void start();
    void stop();

    std::uint16_t port() const;

    private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
