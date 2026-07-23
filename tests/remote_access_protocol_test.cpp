#include "system/MegaDriveEnvironment.hpp"

#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

class TestEnvironment final : public MegaDriveEnvironment {
    public:
    explicit TestEnvironment(std::uint16_t port)
        : MegaDriveEnvironment(VDP::InternalTimer, VDP::Scale1x, VDP::HardwareSpriteLimit, port) {}
    protected:
    void run() override {}
};

std::uint16_t reservePort() {
    const int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    assert(socketFd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    assert(bind(socketFd, reinterpret_cast<const sockaddr *>(&address), sizeof address) == 0);
    socklen_t length = sizeof address;
    assert(getsockname(socketFd, reinterpret_cast<sockaddr *>(&address), &length) == 0);
    const auto port = ntohs(address.sin_port);
    close(socketFd);
    return port;
}

std::uint64_t readU64(const std::vector<std::uint8_t> &bytes) {
    assert(bytes.size() == 8);
    std::uint64_t value = 0;
    for (const auto byte : bytes)
        value = (value << 8) | byte;
    return value;
}

std::uint64_t readU64Prefix(const std::vector<std::uint8_t> &bytes) {
    assert(bytes.size() >= 8);
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index)
        value = (value << 8) | bytes[index];
    return value;
}

void appendU32(std::vector<std::uint8_t> &bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 24));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    bytes.push_back(static_cast<std::uint8_t>(value));
}

void sendAll(int socketFd, const std::vector<std::uint8_t> &bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto sent = send(socketFd, bytes.data() + offset, bytes.size() - offset, 0);
        assert(sent > 0);
        offset += static_cast<std::size_t>(sent);
    }
}

void receiveAll(int socketFd, void *destination, std::size_t size) {
    auto *bytes = static_cast<std::uint8_t *>(destination);
    while (size != 0) {
        const auto received = recv(socketFd, bytes, size, 0);
        assert(received > 0);
        bytes += received;
        size -= static_cast<std::size_t>(received);
    }
}

std::vector<std::uint8_t> request(int socketFd,
                                  std::uint8_t command,
                                  std::uint32_t id,
                                  const std::vector<std::uint8_t> &payload) {
    std::vector<std::uint8_t> packet{0x01, 0x01, command, 0x00};
    appendU32(packet, id);
    appendU32(packet, static_cast<std::uint32_t>(payload.size()));
    packet.insert(packet.end(), payload.begin(), payload.end());
    packet.push_back(0x03);
    sendAll(socketFd, packet);

    std::array<std::uint8_t, 12> header{};
    receiveAll(socketFd, header.data(), header.size());
    assert(header[0] == 0x06); // ACK
    assert(header[1] == 1 && header[2] == command);
    const std::uint32_t responseId = (static_cast<std::uint32_t>(header[4]) << 24) |
                                     (static_cast<std::uint32_t>(header[5]) << 16) |
                                     (static_cast<std::uint32_t>(header[6]) << 8) | header[7];
    const std::uint32_t responseSize = (static_cast<std::uint32_t>(header[8]) << 24) |
                                       (static_cast<std::uint32_t>(header[9]) << 16) |
                                       (static_cast<std::uint32_t>(header[10]) << 8) | header[11];
    assert(responseId == id);
    std::vector<std::uint8_t> response(responseSize);
    receiveAll(socketFd, response.data(), response.size());
    std::uint8_t footer = 0;
    receiveAll(socketFd, &footer, 1);
    assert(footer == 0x03);
    return response;
}

} // namespace

int main() {
    const auto port = reservePort();
    TestEnvironment environment(port);
    assert(environment.vdpInternalFrequencyHz() == 60);
    environment.setVideoStandard(MegaDriveEnvironment::VideoStandard::Hz50);
    assert(environment.vdpInternalFrequencyHz() == 50);
    environment.setVDPTurboMultiplier(2);
    assert(environment.vdpInternalFrequencyHz() == 120);
    environment.setVDPTurboMultiplier(10);
    assert(environment.vdpInternalFrequencyHz() == 600);
    environment.setVDPTurboMultiplier(0);
    assert(environment.vdpInternalFrequencyHz() == 50);
    environment.setVideoStandard(MegaDriveEnvironment::VideoStandard::Hz60);
    environment.remoteAccess().start();

    const int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    assert(socketFd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    bool connected = false;
    for (int attempt = 0; attempt < 100 && !connected; ++attempt) {
        connected = connect(socketFd, reinterpret_cast<const sockaddr *>(&address), sizeof address) == 0;
        if (!connected)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(connected);

    assert(request(socketFd, 0x00, 1, {}).empty());

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const auto uptimeMs = readU64(request(socketFd, 0x02, 2, {}));
    assert(uptimeMs >= 5 && uptimeMs < 10'000);
    assert(readU64(request(socketFd, 0x05, 100, {})) == environment.gameUptimeFrames());

    const std::vector<std::uint8_t> initialExecutionData{'J', 'S', 'O', 'N', 0x00};
    assert(environment.remoteAccess().setExecutionData(initialExecutionData));
    assert(request(socketFd, 0x03, 3, {}) == initialExecutionData);

    const std::vector<std::uint8_t> replacementExecutionData{0x00, 0xFF, 0x10, 0x20};
    assert(request(socketFd, 0x04, 4, replacementExecutionData).empty());
    assert(environment.remoteAccess().executionData() == replacementExecutionData);
    assert(request(socketFd, 0x03, 5, {}) == replacementExecutionData);
    assert(request(socketFd, 0x04, 6, {}).empty());
    assert(environment.remoteAccess().executionData().empty());

    std::vector<std::uint8_t> writePayload;
    appendU32(writePayload, 0x000100);
    writePayload.insert(writePayload.end(), {0x12, 0x34, 0x56, 0x78});
    assert(request(socketFd, 0x21, 7, writePayload).empty());

    std::vector<std::uint8_t> readPayload;
    appendU32(readPayload, 0x000100);
    appendU32(readPayload, 4);
    assert(request(socketFd, 0x20, 8, readPayload) == std::vector<std::uint8_t>({0x12, 0x34, 0x56, 0x78}));

    std::vector<std::uint8_t> waitEqualsPayload;
    appendU32(waitEqualsPayload, 0x000100);
    waitEqualsPayload.insert(waitEqualsPayload.end(), {0x04, 0x00, 0x00, 0x00});
    appendU32(waitEqualsPayload, 0x12345678);
    appendU32(waitEqualsPayload, 0xFFFFFFFF);
    appendU32(waitEqualsPayload, 100);
    assert(request(socketFd, 0x23, 9, waitEqualsPayload) ==
           std::vector<std::uint8_t>({0x12, 0x34, 0x56, 0x78}));

    const auto framebuffer = request(socketFd, 0x30, 10, {});
    assert(framebuffer.size() == 8u + 256u * 224u * 3u);
    assert(framebuffer[6] == 1); // packed native BGR

    assert(request(socketFd, 0x31, 11, {}).size() == 66'436);

    std::vector<std::uint8_t> vramPayload{0x00, 0x00};
    appendU32(vramPayload, 4);
    assert(request(socketFd, 0x32, 12, vramPayload) == std::vector<std::uint8_t>(4, 0));
    assert(request(socketFd, 0x33, 13, {}).size() == 4u + 64u * 5u);
    assert(request(socketFd, 0x34, 14, {}).size() == 4u + 80u * 24u);
    assert(request(socketFd, 0x35, 15, {0}).size() == 12u + 32u * 32u * 8u);

    environment.vdp().start();
    std::vector<std::uint8_t> lockstepPayload{1, 0, 0, 0};
    appendU32(lockstepPayload, 1'000);
    assert(request(socketFd, 0x12, 16, lockstepPayload).empty());
    const auto pausedAt = environment.gameUptimeFrames();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    assert(environment.gameUptimeFrames() == pausedAt);

    environment.memory().writeByte(0x00FF1234u, 0xA5);
    std::vector<std::uint8_t> stepPayload{0x18, 0x80, 0, 0};
    appendU32(stepPayload, 2);
    appendU32(stepPayload, 3);
    appendU32(stepPayload, 1'000);
    const auto step = request(socketFd, 0x13, 17, stepPayload);
    assert(step.size() == 8u + 65'536u);
    assert(readU64Prefix(step) == pausedAt + 3);
    assert(step[8u + 0x1234u] == 0xA5);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    assert(environment.gameUptimeFrames() == pausedAt + 3);

    lockstepPayload[0] = 0;
    assert(request(socketFd, 0x12, 18, lockstepPayload).empty());
    assert(environment.vdp().waitForVSyncCount(2, 1'000));
    environment.vdp().stop();

    close(socketFd);
    environment.remoteAccess().stop();

    environment.setVDPTurboMultiplier(0);
    environment.vdp().start();
    const auto normalStart = std::chrono::steady_clock::now();
    assert(environment.vdp().waitForVSyncCount(12, 1'000));
    const auto normalDuration = std::chrono::steady_clock::now() - normalStart;

    environment.setVDPTurboMultiplier(10);
    assert(environment.vdp().waitForVSyncCount(1, 1'000)); // let the new deadline take effect
    const auto turboStart = std::chrono::steady_clock::now();
    assert(environment.vdp().waitForVSyncCount(12, 1'000));
    const auto turboDuration = std::chrono::steady_clock::now() - turboStart;
    environment.vdp().stop();

    assert(turboDuration * 2 < normalDuration);
}
#else
int main() {}
#endif
