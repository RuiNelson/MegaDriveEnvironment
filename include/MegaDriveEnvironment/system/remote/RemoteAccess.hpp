#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

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
    static constexpr std::size_t MAX_EXECUTION_DATA_SIZE = 16u * 1024u * 1024u;

    explicit RemoteAccess(MegaDriveEnvironment *environment, std::uint16_t port = 6969);
    ~RemoteAccess();

    RemoteAccess(const RemoteAccess &) = delete;
    RemoteAccess &operator=(const RemoteAccess &) = delete;

    void start();
    void stop();

    std::uint16_t port() const;

    /// Returns a thread-safe snapshot of the game-defined debugging buffer.
    std::vector<std::uint8_t> executionData() const;

    /// Atomically replaces the game-defined debugging buffer. Empty data
    /// clears it. Returns false when @p data exceeds the protocol size limit.
    bool setExecutionData(std::span<const std::uint8_t> data);

    private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
