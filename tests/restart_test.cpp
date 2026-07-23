#include "system/MegaDriveEnvironment.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <atomic>
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

void appendU32(std::vector<std::uint8_t> &bytes, std::uint32_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value >> 24));
  bytes.push_back(static_cast<std::uint8_t>(value >> 16));
  bytes.push_back(static_cast<std::uint8_t>(value >> 8));
  bytes.push_back(static_cast<std::uint8_t>(value));
}

std::uint16_t reservePort() {
  const int socketFd = socket(AF_INET, SOCK_STREAM, 0);
  assert(socketFd >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(bind(socketFd, reinterpret_cast<const sockaddr *>(&address),
              sizeof address) == 0);
  socklen_t length = sizeof address;
  assert(getsockname(socketFd, reinterpret_cast<sockaddr *>(&address),
                     &length) == 0);
  close(socketFd);
  return ntohs(address.sin_port);
}

void sendAll(int socketFd, const std::vector<std::uint8_t> &bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto sent =
        send(socketFd, bytes.data() + offset, bytes.size() - offset, 0);
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

void request(int socketFd, std::uint8_t command, std::uint32_t id,
             const std::vector<std::uint8_t> &payload = {}) {
  std::vector<std::uint8_t> packet{0x01, 0x01, command, 0x00};
  appendU32(packet, id);
  appendU32(packet, static_cast<std::uint32_t>(payload.size()));
  packet.insert(packet.end(), payload.begin(), payload.end());
  packet.push_back(0x03);
  sendAll(socketFd, packet);

  std::array<std::uint8_t, 12> header{};
  receiveAll(socketFd, header.data(), header.size());
  assert(header[0] == 0x06);
  assert(header[1] == 1 && header[2] == command);
  assert(header[4] == 0 && header[5] == 0 && header[6] == 0 && header[7] == id);
  assert(header[8] == 0 && header[9] == 0 && header[10] == 0 &&
         header[11] == 0);
  std::uint8_t footer = 0;
  receiveAll(socketFd, &footer, 1);
  assert(footer == 0x03);
}

class RestartEnvironment final : public MegaDriveEnvironment {
public:
  explicit RestartEnvironment(std::uint16_t port)
      : MegaDriveEnvironment(VDP::InternalTimer, VDP::Scale1x,
                             VDP::HardwareSpriteLimit, port) {}

  std::atomic<unsigned> runCount{0};
  std::atomic<unsigned> powerOnCount{0};
  std::atomic<unsigned> resetCount{0};
  std::atomic<bool> secondRunObservedResetState{false};
  std::atomic<bool> finish{false};

protected:
  void onPowerOn() override { powerOnCount.fetch_add(1); }
  void onReset() override { resetCount.fetch_add(1); }

  void run() override {
    const unsigned invocation = runCount.fetch_add(1) + 1;
    if (invocation == 2) {
      const bool wramCleared = memory().readByte(0xFF0100) == 0;
      const bool romPreserved = memory().readLong(0x000100) == 0x12345678;
      secondRunObservedResetState.store(wramCleared && romPreserved);
      while (!finish.load())
        pace();
      return;
    }
    while (true)
      pace();
  }
};

} // namespace

int main() {
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
  const auto port = reservePort();
  RestartEnvironment environment(port);

  std::thread client([&] {
    const int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    assert(socketFd >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    bool connected = false;
    for (int attempt = 0; attempt < 200 && !connected; ++attempt) {
      connected =
          connect(socketFd, reinterpret_cast<const sockaddr *>(&address),
                  sizeof address) == 0;
      if (!connected)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    assert(connected);
    while (environment.runCount.load() == 0)
      std::this_thread::yield();

    std::vector<std::uint8_t> romWrite;
    appendU32(romWrite, 0x000100);
    romWrite.insert(romWrite.end(), {0x12, 0x34, 0x56, 0x78});
    request(socketFd, 0x21, 1, romWrite);

    std::vector<std::uint8_t> wramWrite;
    appendU32(wramWrite, 0xFF0100);
    wramWrite.push_back(0xA5);
    request(socketFd, 0x21, 2, wramWrite);

    std::vector<std::uint8_t> timeout;
    appendU32(timeout, 3'000);
    const std::vector<std::uint8_t> executionData{'d', 'e', 'b', 'u', 'g', 0x00};
    assert(environment.remoteAccess().setExecutionData(executionData));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    assert(environment.vdp().waitForVSyncCount(10, 1'000));
    const auto uptimeBeforeRestart = environment.gameUptimeMilliseconds();
    const auto framesBeforeRestart = environment.gameUptimeFrames();
    assert(framesBeforeRestart >= 10);
    request(socketFd, 0x01, 3, timeout);
    const auto uptimeAfterRestart = environment.gameUptimeMilliseconds();
    const auto framesAfterRestart = environment.gameUptimeFrames();
    assert(uptimeAfterRestart < uptimeBeforeRestart);
    assert(framesAfterRestart < framesBeforeRestart);
    assert(environment.remoteAccess().executionData() == executionData);
    request(socketFd, 0x00, 4);

    while (environment.runCount.load() < 2)
      std::this_thread::yield();
    environment.finish.store(true);
    close(socketFd);
  });

  environment.boot();
  client.join();

  assert(environment.runCount.load() == 2);
  assert(environment.powerOnCount.load() == 2);
  assert(environment.resetCount.load() == 1);
  assert(environment.secondRunObservedResetState.load());
}
#else
int main() {}
#endif
