#include "system/remote/RemoteAccess.hpp"

#include "system/MegaDriveEnvironment.hpp"
#include "util/Logger.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <limits>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
static constexpr SocketHandle kInvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
static constexpr SocketHandle kInvalidSocket = -1;
#endif

namespace {

constexpr std::uint8_t kSOH = 0x01;
constexpr std::uint8_t kETX = 0x03;
constexpr std::uint8_t kACK = 0x06;
constexpr std::uint8_t kNAK = 0x15;
constexpr std::uint8_t kProtocolVersion = 1;
constexpr std::uint32_t kMaximumPayload =
    static_cast<std::uint32_t>(RemoteAccess::MAX_EXECUTION_DATA_SIZE);
constexpr std::uint32_t kBusSize = 0x01000000u;
constexpr std::uint32_t kRomEnd = 0x00400000u;

enum class Command : std::uint8_t {
    Ping = 0x00,
    RestartGame = 0x01,
    GetGameUptime = 0x02,
    GetExecutionData = 0x03,
    SetExecutionData = 0x04,
    GetGameUptimeFrames = 0x05,
    PressButtons = 0x10,
    ReleaseButtons = 0x11,
    SetLockstep = 0x12,
    StepInput = 0x13,
    ReadMemory = 0x20,
    WriteMemory = 0x21,
    WaitMemoryChanged = 0x22,
    WaitMemoryEquals = 0x23,
    ReadFramebuffer = 0x30,
    ReadVDPState = 0x31,
    ReadVRAM = 0x32,
    ReadPalettes = 0x33,
    ReadSAT = 0x34,
    ReadTilemap = 0x35,
    WaitVSyncCount = 0x40,
    WaitHSyncCount = 0x41,
    WaitHSyncReachLine = 0x42,
};

enum class Error : std::uint16_t {
    MalformedPayload = 1,
    UnknownCommand = 2,
    Timeout = 3,
    InvalidArgument = 4,
    Unavailable = 5,
    TooLarge = 6,
    Internal = 7,
};

struct Request {
    std::uint8_t command = 0;
    std::uint32_t id = 0;
    std::vector<std::uint8_t> payload;
};

struct Result {
    bool success = true;
    Error error = Error::Internal;
    std::string message;
    std::vector<std::uint8_t> payload;

    static Result failure(Error code, std::string text) {
        return {.success = false, .error = code, .message = std::move(text)};
    }
};

void closeSocket(SocketHandle socket) {
    if (socket == kInvalidSocket)
        return;
#if defined(_WIN32)
    closesocket(socket);
#else
    close(socket);
#endif
}

void shutdownSocket(SocketHandle socket) {
    if (socket == kInvalidSocket)
        return;
#if defined(_WIN32)
    shutdown(socket, SD_BOTH);
#else
    shutdown(socket, SHUT_RDWR);
#endif
}

bool receiveExact(SocketHandle socket, void *destination, std::size_t size) {
    auto *bytes = static_cast<std::uint8_t *>(destination);
    while (size != 0) {
        const auto chunk = static_cast<int>(std::min<std::size_t>(size, std::numeric_limits<int>::max()));
        const int received = recv(socket, reinterpret_cast<char *>(bytes), chunk, 0);
        if (received <= 0)
            return false;
        bytes += received;
        size -= static_cast<std::size_t>(received);
    }
    return true;
}

bool sendExact(SocketHandle socket, const void *source, std::size_t size) {
    const auto *bytes = static_cast<const std::uint8_t *>(source);
    while (size != 0) {
        const auto chunk = static_cast<int>(std::min<std::size_t>(size, std::numeric_limits<int>::max()));
#if defined(MSG_NOSIGNAL)
        const int sent = send(socket, reinterpret_cast<const char *>(bytes), chunk, MSG_NOSIGNAL);
#else
        const int sent = send(socket, reinterpret_cast<const char *>(bytes), chunk, 0);
#endif
        if (sent <= 0)
            return false;
        bytes += sent;
        size -= static_cast<std::size_t>(sent);
    }
    return true;
}

std::uint16_t readU16(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[offset]) << 8) | bytes[offset + 1]);
}

std::uint32_t readU32(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

void appendU8(std::vector<std::uint8_t> &out, std::uint8_t value) {
    out.push_back(value);
}

void appendU16(std::vector<std::uint8_t> &out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void appendU32(std::vector<std::uint8_t> &out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

void appendU64(std::vector<std::uint8_t> &out, std::uint64_t value) {
    appendU32(out, static_cast<std::uint32_t>(value >> 32));
    appendU32(out, static_cast<std::uint32_t>(value));
}

void appendI16(std::vector<std::uint8_t> &out, std::int16_t value) {
    appendU16(out, static_cast<std::uint16_t>(value));
}

bool validRange(std::uint32_t address, std::uint32_t length) {
    return address < kBusSize && length <= kBusSize - address;
}

PlayerControlsState decodeButtons(std::uint8_t mask) {
    return {
        .connected = mask != 0,
        .up = (mask & 0x01) != 0,
        .down = (mask & 0x02) != 0,
        .left = (mask & 0x04) != 0,
        .right = (mask & 0x08) != 0,
        .a = (mask & 0x10) != 0,
        .b = (mask & 0x20) != 0,
        .c = (mask & 0x40) != 0,
        .start = (mask & 0x80) != 0,
    };
}

} // namespace

class RemoteAccess::Impl {
    public:
    Impl(MegaDriveEnvironment *environment, std::uint16_t port) : environment_(environment), port_(port) {
#if defined(_WIN32)
        WSADATA data{};
        winsockReady_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
#endif
    }

    ~Impl() {
        stop();
#if defined(_WIN32)
        if (winsockReady_)
            WSACleanup();
#endif
    }

    void start() {
        if (port_ == 0 || running_.exchange(true, std::memory_order_acq_rel))
            return;
        if (worker_.joinable())
            worker_.join();
        worker_ = std::thread([this] { serverLoop(); });
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        environment_->vdp().setRemoteLockstep(false, 1);
        environment_->vdp().wakeSyncWaiters();
        const SocketHandle client = clientSocket_.exchange(kInvalidSocket, std::memory_order_acq_rel);
        shutdownSocket(client);
        closeSocket(client);
        const SocketHandle listener = listenSocket_.exchange(kInvalidSocket, std::memory_order_acq_rel);
        shutdownSocket(listener);
        closeSocket(listener);
        if (worker_.joinable())
            worker_.join();
        environment_->controllers().clearRemoteState();
    }

    std::uint16_t port() const {
        return port_;
    }

    std::vector<std::uint8_t> executionData() const {
        std::lock_guard lock(executionDataMutex_);
        return executionData_;
    }

    bool setExecutionData(std::span<const std::uint8_t> data) {
        if (data.size() > RemoteAccess::MAX_EXECUTION_DATA_SIZE)
            return false;
        std::lock_guard lock(executionDataMutex_);
        executionData_.assign(data.begin(), data.end());
        return true;
    }

    private:
    void serverLoop() {
#if defined(_WIN32)
        if (!winsockReady_) {
            Logger::log("[remote] Winsock initialization failed");
            running_.store(false, std::memory_order_release);
            return;
        }
#endif
        SocketHandle listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == kInvalidSocket) {
            Logger::log("[remote] could not create TCP socket");
            running_.store(false, std::memory_order_release);
            return;
        }
        int reuse = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof reuse);

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(port_);
        if (bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof address) != 0 || listen(listener, 1) != 0) {
            Logger::log("[remote] could not bind TCP port %u", static_cast<unsigned>(port_));
            closeSocket(listener);
            running_.store(false, std::memory_order_release);
            return;
        }
        listenSocket_.store(listener, std::memory_order_release);
        Logger::log("[remote] listening on 0.0.0.0:%u", static_cast<unsigned>(port_));

        while (running_.load(std::memory_order_acquire)) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(listener, &readSet);
            timeval timeout{0, 200'000};
#if defined(_WIN32)
            const int ready = select(0, &readSet, nullptr, nullptr, &timeout);
#else
            const int ready = select(listener + 1, &readSet, nullptr, nullptr, &timeout);
#endif
            if (ready <= 0)
                continue;
            SocketHandle client = accept(listener, nullptr, nullptr);
            if (client == kInvalidSocket)
                continue;
            clientSocket_.store(client, std::memory_order_release);
            handleClient(client);
            environment_->controllers().clearRemoteState();
            environment_->vdp().setRemoteLockstep(false, 1);
            if (clientSocket_.exchange(kInvalidSocket, std::memory_order_acq_rel) == client)
                closeSocket(client);
        }

        if (listenSocket_.exchange(kInvalidSocket, std::memory_order_acq_rel) == listener)
            closeSocket(listener);
    }

    void handleClient(SocketHandle client) {
        while (running_.load(std::memory_order_acquire)) {
            Request request;
            if (!receiveRequest(client, request))
                return;
            Result result;
            try {
                result = execute(request);
            } catch (const std::exception &exception) {
                result = Result::failure(Error::Internal, exception.what());
            } catch (...) {
                result = Result::failure(Error::Internal, "unknown server error");
            }
            if (!sendResponse(client, request, result))
                return;
        }
    }

    bool receiveRequest(SocketHandle client, Request &request) {
        std::uint8_t header[12];
        if (!receiveExact(client, header, sizeof header))
            return false;
        if (header[0] != kSOH || header[1] != kProtocolVersion || header[3] != 0)
            return false;
        const std::uint32_t length = readU32(header, 8);
        if (length > kMaximumPayload)
            return false;
        request.command = header[2];
        request.id = readU32(header, 4);
        request.payload.resize(length);
        if (length != 0 && !receiveExact(client, request.payload.data(), length))
            return false;
        std::uint8_t footer = 0;
        return receiveExact(client, &footer, 1) && footer == kETX;
    }

    bool sendResponse(SocketHandle client, const Request &request, const Result &result) {
        std::vector<std::uint8_t> payload;
        if (result.success) {
            payload = result.payload;
        } else {
            appendU16(payload, static_cast<std::uint16_t>(result.error));
            payload.insert(payload.end(), result.message.begin(), result.message.end());
        }
        std::uint8_t header[12] = {
            result.success ? kACK : kNAK,
            kProtocolVersion,
            request.command,
            0,
            static_cast<std::uint8_t>(request.id >> 24),
            static_cast<std::uint8_t>(request.id >> 16),
            static_cast<std::uint8_t>(request.id >> 8),
            static_cast<std::uint8_t>(request.id),
            static_cast<std::uint8_t>(payload.size() >> 24),
            static_cast<std::uint8_t>(payload.size() >> 16),
            static_cast<std::uint8_t>(payload.size() >> 8),
            static_cast<std::uint8_t>(payload.size()),
        };
        return sendExact(client, header, sizeof header) &&
               (payload.empty() || sendExact(client, payload.data(), payload.size())) &&
               sendExact(client, &kETX, 1);
    }

    Result execute(const Request &request) {
        const auto payload = std::span<const std::uint8_t>(request.payload);
        switch (static_cast<Command>(request.command)) {
            case Command::Ping:
                return payload.empty() ? Result{} : Result::failure(Error::MalformedPayload, "PING has no payload");
            case Command::RestartGame:
                return restartGame(payload);
            case Command::GetGameUptime: {
                if (!payload.empty())
                    return Result::failure(Error::MalformedPayload, "GET_GAME_UPTIME has no payload");
                Result result;
                appendU64(result.payload, environment_->gameUptimeMilliseconds());
                return result;
            }
            case Command::GetExecutionData:
                return payload.empty()
                    ? Result{.payload = executionData()}
                    : Result::failure(Error::MalformedPayload, "GET_EXECUTION_DATA has no payload");
            case Command::SetExecutionData:
                return setExecutionData(payload)
                    ? Result{}
                    : Result::failure(Error::TooLarge, "execution data exceeds the protocol payload limit");
            case Command::GetGameUptimeFrames: {
                if (!payload.empty())
                    return Result::failure(Error::MalformedPayload, "GET_GAME_UPTIME_FRAMES has no payload");
                Result result;
                appendU64(result.payload, environment_->gameUptimeFrames());
                return result;
            }
            case Command::PressButtons:
                return pressButtons(payload);
            case Command::ReleaseButtons:
                if (!payload.empty())
                    return Result::failure(Error::MalformedPayload, "RELEASE_BUTTONS has no payload");
                environment_->controllers().clearRemoteState();
                return {};
            case Command::SetLockstep:
                return setLockstep(payload);
            case Command::StepInput:
                return stepInput(payload);
            case Command::ReadMemory:
                return readMemory(payload);
            case Command::WriteMemory:
                return writeMemory(payload);
            case Command::WaitMemoryChanged:
                return waitMemoryChanged(payload);
            case Command::WaitMemoryEquals:
                return waitMemoryEquals(payload);
            case Command::ReadFramebuffer:
                return readFramebuffer(payload);
            case Command::ReadVDPState:
                return readVDPState(payload);
            case Command::ReadVRAM:
                return readVRAM(payload);
            case Command::ReadPalettes:
                return readPalettes(payload);
            case Command::ReadSAT:
                return readSAT(payload);
            case Command::ReadTilemap:
                return readTilemap(payload);
            case Command::WaitVSyncCount:
                return waitSync(payload, Command::WaitVSyncCount);
            case Command::WaitHSyncCount:
                return waitSync(payload, Command::WaitHSyncCount);
            case Command::WaitHSyncReachLine:
                return waitHSyncLine(payload);
        }
        return Result::failure(Error::UnknownCommand, "unknown command");
    }

    Result restartGame(std::span<const std::uint8_t> payload) {
        if (payload.size() != 4)
            return Result::failure(Error::MalformedPayload, "RESTART_GAME requires a 4-byte timeout");
        const std::uint32_t timeoutMs = readU32(payload, 0);
        if (timeoutMs == 0)
            return Result::failure(Error::InvalidArgument, "timeout must be non-zero");
        environment_->vdp().setRemoteLockstep(false, 1);
        return environment_->restart(timeoutMs)
            ? Result{}
            : Result::failure(Error::Timeout, "game restart timed out or the environment stopped");
    }

    Result pressButtons(std::span<const std::uint8_t> payload) {
        if (payload.size() != 10)
            return Result::failure(Error::MalformedPayload, "PRESS_BUTTONS requires 10 bytes");
        const std::uint32_t frames = readU32(payload, 2);
        const std::uint32_t timeoutMs = readU32(payload, 6);
        if (frames == 0 || timeoutMs == 0)
            return Result::failure(Error::InvalidArgument, "frames and timeout must be non-zero");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        if (!waitVSyncUntil(deadline))
            return Result::failure(Error::Timeout, "timed out before initial VSync");

        PlayersControlState state{decodeButtons(payload[0]), decodeButtons(payload[1])};
        environment_->controllers().setRemoteState(state);
        struct ReleaseGuard {
            Controllers &controllers;
            ~ReleaseGuard() { controllers.clearRemoteState(); }
        } guard{environment_->controllers()};

        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            if (!waitVSyncUntil(deadline))
                return Result::failure(Error::Timeout, "timed out while holding buttons");
        }
        return {};
    }

    Result setLockstep(std::span<const std::uint8_t> payload) {
        if (payload.size() != 8 || payload[1] != 0 || payload[2] != 0 || payload[3] != 0)
            return Result::failure(
                Error::MalformedPayload,
                "SET_LOCKSTEP requires enabled:u8, reserved:3, timeout:u32");
        if (payload[0] > 1)
            return Result::failure(Error::InvalidArgument, "lockstep enabled flag must be zero or one");
        const std::uint32_t timeoutMs = readU32(payload, 4);
        if (timeoutMs == 0)
            return Result::failure(Error::InvalidArgument, "timeout must be non-zero");
        environment_->controllers().clearRemoteState();
        return environment_->vdp().setRemoteLockstep(payload[0] != 0, timeoutMs)
            ? Result{}
            : Result::failure(
                  payload[0] != 0 ? Error::Timeout : Error::Unavailable,
                  payload[0] != 0 ? "timed out entering lockstep at a frame boundary"
                                  : "could not disable lockstep");
    }

    Result stepInput(std::span<const std::uint8_t> payload) {
        if (payload.size() != 16 || payload[2] != 0 || payload[3] != 0)
            return Result::failure(
                Error::MalformedPayload,
                "STEP_INPUT requires P1:u8, P2:u8, reserved:2, held-frames:u32, "
                "total-frames:u32, timeout:u32");
        const std::uint32_t heldFrames = readU32(payload, 4);
        const std::uint32_t totalFrames = readU32(payload, 8);
        const std::uint32_t timeoutMs = readU32(payload, 12);
        if (totalFrames == 0 || heldFrames > totalFrames || timeoutMs == 0)
            return Result::failure(
                Error::InvalidArgument,
                "total frames and timeout must be non-zero and held frames must not exceed total frames");
        if (!environment_->vdp().remoteLockstepEnabled())
            return Result::failure(Error::Unavailable, "STEP_INPUT requires enabled lockstep");

        struct ReleaseGuard {
            Controllers &controllers;
            ~ReleaseGuard() { controllers.clearRemoteState(); }
        } guard{environment_->controllers()};

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        const PlayersControlState pressed{decodeButtons(payload[0]), decodeButtons(payload[1])};
        environment_->controllers().setRemoteState(heldFrames == 0 ? PlayersControlState{} : pressed);
        for (std::uint32_t frame = 0; frame < totalFrames; ++frame) {
            if (frame == heldFrames)
                environment_->controllers().clearRemoteState();
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return Result::failure(Error::Timeout, "timed out during lockstep step");
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            const auto slice = static_cast<std::uint32_t>(
                std::clamp<std::int64_t>(remaining, 1, std::numeric_limits<std::uint32_t>::max()));
            if (!environment_->vdp().advanceRemoteLockstepFrame(slice))
                return Result::failure(Error::Timeout, "timed out during lockstep step");
        }

        environment_->controllers().clearRemoteState();
        Result result;
        appendU64(result.payload, environment_->gameUptimeFrames());
        constexpr std::uint32_t kWorkRamBase = 0x00FF0000u;
        constexpr std::size_t kWorkRamSize = 64u * 1024u;
        const auto offset = result.payload.size();
        result.payload.resize(offset + kWorkRamSize);
        environment_->memory().copyToBuffer(
            kWorkRamBase, result.payload.data() + offset, static_cast<int>(kWorkRamSize));
        return result;
    }

    bool waitVSyncUntil(std::chrono::steady_clock::time_point deadline) {
        while (running_.load(std::memory_order_acquire)) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
                return false;
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            const auto slice = static_cast<std::uint32_t>(std::clamp<std::int64_t>(remaining, 1, 100));
            if (environment_->vdp().waitForVSyncCount(1, slice))
                return true;
        }
        return false;
    }

    Result readMemory(std::span<const std::uint8_t> payload) {
        if (payload.size() != 8)
            return Result::failure(Error::MalformedPayload, "READ_MEMORY requires 8 bytes");
        const auto address = readU32(payload, 0);
        const auto length = readU32(payload, 4);
        if (!validRange(address, length))
            return Result::failure(Error::InvalidArgument, "memory range is outside the 24-bit bus");
        if (length > kMaximumPayload)
            return Result::failure(Error::TooLarge, "memory response exceeds maximum payload");
        Result result;
        result.payload.resize(length);
        std::uint32_t index = 0;
        if (address < kRomEnd) {
            const auto romLength = std::min(length, kRomEnd - address);
            environment_->memory().copyToBuffer(address, result.payload.data(), static_cast<int>(romLength));
            index = romLength;
        }
        for (; index < length; ++index)
            result.payload[index] = environment_->memory().readByte(address + index);
        return result;
    }

    Result writeMemory(std::span<const std::uint8_t> payload) {
        if (payload.size() < 4)
            return Result::failure(Error::MalformedPayload, "WRITE_MEMORY requires address and data");
        const auto address = readU32(payload, 0);
        const auto length = static_cast<std::uint32_t>(payload.size() - 4);
        if (!validRange(address, length))
            return Result::failure(Error::InvalidArgument, "memory range is outside the 24-bit bus");
        std::uint32_t offset = 0;
        if (address < kRomEnd) {
            const auto romLength = std::min(length, kRomEnd - address);
            environment_->memory().patchBytes(address, payload.data() + 4, romLength);
            offset = romLength;
        }
        for (; offset < length; ++offset)
            environment_->memory().writeByte(address + offset, payload[4 + offset]);
        return {};
    }

    std::uint32_t readMemoryValue(std::uint32_t address, std::uint8_t width) {
        switch (width) {
            case 1: return environment_->memory().readByte(address);
            case 2: return environment_->memory().readWord(address);
            case 4: return environment_->memory().readLong(address);
            default: return 0;
        }
    }

    Result validateWait(std::uint32_t address, std::uint8_t width, std::uint32_t timeoutMs) {
        if ((width != 1 && width != 2 && width != 4) || (address % width) != 0 ||
            !validRange(address, width) || timeoutMs == 0)
            return Result::failure(Error::InvalidArgument, "invalid width, alignment, address, or timeout");
        return {};
    }

    template <typename Predicate>
    Result pollMemory(std::uint32_t address, std::uint8_t width, std::uint32_t timeoutMs, Predicate predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        std::uint32_t value = readMemoryValue(address, width);
        while (!predicate(value)) {
            if (!running_.load(std::memory_order_acquire) || std::chrono::steady_clock::now() >= deadline)
                return Result::failure(Error::Timeout, "memory wait timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            value = readMemoryValue(address, width);
        }
        Result result;
        appendU32(result.payload, value);
        return result;
    }

    Result waitMemoryChanged(std::span<const std::uint8_t> payload) {
        if (payload.size() != 12 || payload[5] != 0 || payload[6] != 0 || payload[7] != 0)
            return Result::failure(Error::MalformedPayload, "WAIT_MEMORY_CHANGED requires 12 bytes");
        const auto address = readU32(payload, 0);
        const auto width = payload[4];
        const auto timeoutMs = readU32(payload, 8);
        if (Result validation = validateWait(address, width, timeoutMs); !validation.success)
            return validation;
        const auto initial = readMemoryValue(address, width);
        return pollMemory(address, width, timeoutMs, [initial](std::uint32_t value) { return value != initial; });
    }

    Result waitMemoryEquals(std::span<const std::uint8_t> payload) {
        if (payload.size() != 20 || payload[5] != 0 || payload[6] != 0 || payload[7] != 0)
            return Result::failure(Error::MalformedPayload, "WAIT_MEMORY_EQUALS requires 20 bytes");
        const auto address = readU32(payload, 0);
        const auto width = payload[4];
        const auto expected = readU32(payload, 8);
        const auto mask = readU32(payload, 12);
        const auto timeoutMs = readU32(payload, 16);
        if (Result validation = validateWait(address, width, timeoutMs); !validation.success)
            return validation;
        const std::uint32_t widthMask = width == 4 ? 0xFFFFFFFFu : ((1u << (width * 8)) - 1u);
        if ((expected & ~widthMask) != 0 || (mask & ~widthMask) != 0)
            return Result::failure(Error::InvalidArgument, "expected or mask does not fit width");
        return pollMemory(address, width, timeoutMs, [expected, mask](std::uint32_t value) {
            return (value & mask) == (expected & mask);
        });
    }

    Result readFramebuffer(std::span<const std::uint8_t> payload) {
        if (!payload.empty())
            return Result::failure(Error::MalformedPayload, "READ_FRAMEBUFFER has no payload");
        const auto snapshot = environment_->vdp().framebufferSnapshot();
        Result result;
        result.payload.reserve(8 + snapshot.pixels.size());
        appendU16(result.payload, snapshot.width);
        appendU16(result.payload, snapshot.height);
        appendU16(result.payload, snapshot.pitch);
        appendU8(result.payload, 1); // packed BGR, native 3-bit values
        appendU8(result.payload, 0);
        result.payload.insert(result.payload.end(), snapshot.pixels.begin(), snapshot.pixels.end());
        return result;
    }

    Result readVDPState(std::span<const std::uint8_t> payload) {
        if (!payload.empty())
            return Result::failure(Error::MalformedPayload, "READ_VDP_STATE has no payload");
        const auto state = environment_->vdp().stateSnapshot();
        Result result;
        appendU16(result.payload, state.status);
        appendU16(result.payload, state.hCounter);
        appendU16(result.payload, state.vCounter);
        appendU16(result.payload, state.activeWidth);
        appendU16(result.payload, state.activeHeight);
        appendU16(result.payload, state.outputHeight);
        appendU16(result.payload, state.planeWidthCells);
        appendU16(result.payload, state.planeHeightCells);
        appendU16(result.payload, state.planeABase);
        appendU16(result.payload, state.planeBBase);
        appendU16(result.payload, state.windowBase);
        appendU16(result.payload, state.windowWidthCells);
        appendU16(result.payload, state.satBase);
        appendU16(result.payload, 0);
        result.payload.insert(result.payload.end(), state.regs.begin(), state.regs.end());
        result.payload.insert(result.payload.end(), state.vram.begin(), state.vram.end());
        for (m_word value : state.cram) appendU16(result.payload, value);
        for (m_word value : state.vsram) appendU16(result.payload, value);
        result.payload.insert(result.payload.end(), state.sat.begin(), state.sat.end());
        return result;
    }

    Result readVRAM(std::span<const std::uint8_t> payload) {
        if (payload.size() != 6)
            return Result::failure(Error::MalformedPayload, "READ_VRAM requires 6 bytes");
        const auto offset = readU16(payload, 0);
        const auto length = readU32(payload, 2);
        if (length > VDPState::VRAM_SIZE - offset)
            return Result::failure(Error::InvalidArgument, "VRAM range is invalid");
        const auto state = environment_->vdp().stateSnapshot();
        Result result;
        result.payload.insert(result.payload.end(), state.vram.begin() + offset, state.vram.begin() + offset + length);
        return result;
    }

    Result readPalettes(std::span<const std::uint8_t> payload) {
        if (!payload.empty())
            return Result::failure(Error::MalformedPayload, "READ_PALETTES has no payload");
        const auto state = environment_->vdp().stateSnapshot();
        const bool fullColor = (state.regs[0] & 0x04) != 0;
        Result result;
        appendU16(result.payload, VDPState::CRAM_ENTRIES);
        appendU16(result.payload, 5);
        for (m_word raw : state.cram) {
            appendU16(result.payload, raw);
            std::uint8_t r = static_cast<std::uint8_t>((raw >> 1) & 7);
            std::uint8_t g = static_cast<std::uint8_t>((raw >> 5) & 7);
            std::uint8_t b = static_cast<std::uint8_t>((raw >> 9) & 7);
            if (!fullColor) { r &= 1; g &= 1; b &= 1; }
            appendU8(result.payload, b);
            appendU8(result.payload, g);
            appendU8(result.payload, r);
        }
        return result;
    }

    Result readSAT(std::span<const std::uint8_t> payload) {
        if (!payload.empty())
            return Result::failure(Error::MalformedPayload, "READ_SAT has no payload");
        const auto state = environment_->vdp().stateSnapshot();
        Result result;
        constexpr std::uint16_t recordSize = 24;
        appendU16(result.payload, VDPState::SAT_MAX_SPRITES);
        appendU16(result.payload, recordSize);
        for (int index = 0; index < VDPState::SAT_MAX_SPRITES; ++index) {
            const std::size_t offset = static_cast<std::size_t>(index) * 8;
            const auto sat = std::span<const m_byte>(state.sat).subspan(offset, 8);
            const std::uint16_t tableAddress = static_cast<std::uint16_t>(state.satBase + offset);
            const m_word tile = static_cast<m_word>((state.vram[tableAddress + 4] << 8) |
                                                   state.vram[tableAddress + 5]);
            const int yRaw = ((sat[0] & 3) << 8) | sat[1];
            const int xRaw = ((state.vram[tableAddress + 6] & 1) << 8) | state.vram[tableAddress + 7];
            result.payload.insert(result.payload.end(), sat.begin(), sat.end());
            appendI16(result.payload, static_cast<std::int16_t>(xRaw - 128));
            appendI16(result.payload, static_cast<std::int16_t>(yRaw - 128));
            appendU8(result.payload, static_cast<std::uint8_t>(((sat[2] >> 2) & 3) + 1));
            appendU8(result.payload, static_cast<std::uint8_t>((sat[2] & 3) + 1));
            appendU8(result.payload, static_cast<std::uint8_t>(sat[3] & 0x7F));
            appendU8(result.payload, 0);
            appendU16(result.payload, static_cast<std::uint16_t>(tile & 0x07FF));
            appendU8(result.payload, static_cast<std::uint8_t>((tile >> 13) & 3));
            appendU8(result.payload, static_cast<std::uint8_t>(((tile & 0x8000) ? 1 : 0) |
                                                               ((tile & 0x0800) ? 2 : 0) |
                                                               ((tile & 0x1000) ? 4 : 0)));
            appendU16(result.payload, tile);
            appendU16(result.payload, tableAddress);
        }
        return result;
    }

    Result readTilemap(std::span<const std::uint8_t> payload) {
        if (payload.size() != 1 || payload[0] > 2)
            return Result::failure(Error::InvalidArgument, "READ_TILEMAP plane must be 0, 1, or 2");
        const auto state = environment_->vdp().stateSnapshot();
        const std::uint16_t width = payload[0] == 2 ? state.windowWidthCells : state.planeWidthCells;
        const std::uint16_t height = payload[0] == 2 ? 32 : state.planeHeightCells;
        const std::uint16_t base = payload[0] == 0 ? state.planeABase : (payload[0] == 1 ? state.planeBBase : state.windowBase);
        Result result;
        appendU8(result.payload, payload[0]);
        appendU8(result.payload, 0);
        appendU16(result.payload, width);
        appendU16(result.payload, height);
        appendU16(result.payload, base);
        appendU16(result.payload, 8);
        appendU16(result.payload, 0);
        for (std::uint32_t index = 0; index < static_cast<std::uint32_t>(width) * height; ++index) {
            const std::uint16_t address = static_cast<std::uint16_t>(base + index * 2);
            const m_word raw = static_cast<m_word>((state.vram[address] << 8) | state.vram[static_cast<std::uint16_t>(address + 1)]);
            appendU16(result.payload, raw);
            appendU16(result.payload, static_cast<std::uint16_t>(raw & 0x07FF));
            appendU8(result.payload, static_cast<std::uint8_t>((raw >> 13) & 3));
            appendU8(result.payload, static_cast<std::uint8_t>(((raw & 0x8000) ? 1 : 0) |
                                                               ((raw & 0x0800) ? 2 : 0) |
                                                               ((raw & 0x1000) ? 4 : 0)));
            appendU16(result.payload, address);
        }
        return result;
    }

    Result waitSync(std::span<const std::uint8_t> payload, Command command) {
        if (payload.size() != 8)
            return Result::failure(Error::MalformedPayload, "sync count wait requires 8 bytes");
        const auto count = readU32(payload, 0);
        const auto timeoutMs = readU32(payload, 4);
        if (count == 0 || timeoutMs == 0)
            return Result::failure(Error::InvalidArgument, "count and timeout must be non-zero");
        const bool reached = command == Command::WaitVSyncCount
            ? environment_->vdp().waitForVSyncCount(count, timeoutMs)
            : environment_->vdp().waitForHSyncCount(count, timeoutMs);
        return reached ? Result{} : Result::failure(Error::Timeout, "sync wait timed out");
    }

    Result waitHSyncLine(std::span<const std::uint8_t> payload) {
        if (payload.size() != 8 || payload[2] != 0 || payload[3] != 0)
            return Result::failure(Error::MalformedPayload, "WAIT_HSYNC_REACH_LINE requires 8 bytes");
        const auto line = readU16(payload, 0);
        const auto timeoutMs = readU32(payload, 4);
        if (timeoutMs == 0 || line >= VDPState::MAX_SCREEN_H)
            return Result::failure(Error::InvalidArgument, "line or timeout is invalid");
        return environment_->vdp().waitForHSyncLine(line, timeoutMs)
            ? Result{}
            : Result::failure(Error::Timeout, "HSync line wait timed out");
    }

    MegaDriveEnvironment *environment_ = nullptr;
    std::uint16_t port_ = 0;
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::atomic<SocketHandle> listenSocket_{kInvalidSocket};
    std::atomic<SocketHandle> clientSocket_{kInvalidSocket};
    mutable std::mutex executionDataMutex_;
    std::vector<std::uint8_t> executionData_;
#if defined(_WIN32)
    bool winsockReady_ = false;
#endif
};

RemoteAccess::RemoteAccess(MegaDriveEnvironment *environment, std::uint16_t port)
    : impl_(std::make_unique<Impl>(environment, port)) {
}

RemoteAccess::~RemoteAccess() = default;

void RemoteAccess::start() { impl_->start(); }
void RemoteAccess::stop() { impl_->stop(); }
std::uint16_t RemoteAccess::port() const { return impl_->port(); }
std::vector<std::uint8_t> RemoteAccess::executionData() const { return impl_->executionData(); }
bool RemoteAccess::setExecutionData(std::span<const std::uint8_t> data) { return impl_->setExecutionData(data); }
