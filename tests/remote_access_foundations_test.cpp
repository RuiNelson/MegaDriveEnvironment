#include "system/controllers/Controllers.hpp"
#include "system/memory/SystemMemory.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <thread>

namespace {

void testRemoteControllerOverlay() {
    Controllers controllers(nullptr);
    PlayersControlState remote{};
    remote.player1.connected = true;
    remote.player1.up = true;
    remote.player1.b = true;
    remote.player2.connected = true;
    remote.player2.start = true;
    controllers.setRemoteState(remote);

    // TH defaults high: P1 Up is bit 0 and B is bit 4, both active-low.
    assert((controllers.readPlayer1DataPort() & 0x11u) == 0);

    // TH low exposes Start on bit 5 for player 2.
    controllers.writePlayer2DataPort(0x00);
    assert((controllers.readPlayer2DataPort() & 0x20u) == 0);

    controllers.clearRemoteState();
    assert((controllers.readPlayer1DataPort() & 0x11u) == 0x11u);
    assert((controllers.readPlayer2DataPort() & 0x20u) == 0x20u);
}

void testWholeSegmentRomSynchronization() {
    SystemMemory memory;
    constexpr std::size_t size = 4096;
    std::array<m_byte, size> first{};
    std::array<m_byte, size> second{};
    std::array<m_byte, size> observed{};
    first.fill(0x55);
    second.fill(0xAA);
    memory.patchBytes(0, first.data(), first.size());

    // Normal emulated bus writes retain ROM hardware semantics.
    memory.writeByte(0, 0xCC);
    assert(memory.readByte(0) == 0x55);

    std::atomic<bool> finished = false;
    std::thread writer([&] {
        for (int iteration = 0; iteration < 500; ++iteration) {
            const auto &source = (iteration & 1) ? first : second;
            memory.patchBytes(0, source.data(), source.size());
        }
        finished.store(true, std::memory_order_release);
    });

    do {
        memory.copyToBuffer(0, observed.data(), static_cast<int>(observed.size()));
        const m_byte expected = observed.front();
        assert(expected == 0x55 || expected == 0xAA);
        for (m_byte value : observed)
            assert(value == expected); // no half-applied remote patch
    } while (!finished.load(std::memory_order_acquire));

    writer.join();
}

} // namespace

int main() {
    testRemoteControllerOverlay();
    testWholeSegmentRomSynchronization();
}
