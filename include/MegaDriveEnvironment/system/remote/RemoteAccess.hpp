#pragma once

#include <array>
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
    static constexpr std::size_t FLAG_COUNT = 1024;
    static constexpr std::size_t FLAG_BYTES = FLAG_COUNT / 8;

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

    /// Sets or clears game-defined flag @p index (0-1023), reported by GET_FLAGS. Flag meanings belong to the
    /// running game. Flag n is bit (n % 8) of byte (n / 8), least significant bit first. Flags persist across
    /// cold resets and client reconnects. Returns false when @p index is out of range.
    bool setFlag(std::size_t index, bool value);

    /// Returns flag @p index, or false when it is out of range.
    bool flag(std::size_t index) const;

    /// Returns a thread-safe snapshot of all flags, in GET_FLAGS wire order.
    std::array<std::uint8_t, FLAG_BYTES> flags() const;

    private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
