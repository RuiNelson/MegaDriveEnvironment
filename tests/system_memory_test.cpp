#include "system/memory/SystemMemory.hpp"

#include <array>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <thread>

namespace {

void testAddressingAndEndianAccess() {
    SystemMemory memory;

    memory.writeLong(0xFFFF1234u, 0x89ABCDEFu);
    assert(memory.readByte(0x00FF1234u) == 0x89u);
    assert(memory.readWord(0x00FF1234u) == 0x89ABu);
    assert(memory.readLong(0x00FF1234u) == 0x89ABCDEFu);
    assert(memory.readWord(0xFFFF1236u) == 0xCDEFu);

    memory.writeWord(0x00FF1235u, 0x1020u);
    assert(memory.readByte(0xFFFF1235u) == 0x10u);
    assert(memory.readByte(0xFFFF1236u) == 0x20u);

    constexpr std::array<m_byte, 8> cartridge = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    memory.patchBytes(0x20u, cartridge.data(), cartridge.size());
    assert(memory.readLong(0x20u) == 0x01234567u);
    assert(memory.readLong(0xFF000024u) == 0x89ABCDEFu);

    memory.writeLong(0x20u, 0u);
    assert(memory.readLong(0x20u) == 0x01234567u); // cartridge bus writes are ignored

    assert(memory.readLong(0x00400000u) == 0u); // unmapped hardware with no environment
    memory.writeLong(0x00400000u, 0xFFFFFFFFu);
    assert(memory.readLong(0x00400000u) == 0u);

    bool rejected = false;
    try {
        memory.patchBytes(0x003FFFFFu, cartridge.data(), 2);
    } catch (const std::out_of_range &) {
        rejected = true;
    }
    assert(rejected);
}

void testBulkTransfers() {
    SystemMemory memory;
    std::array<m_byte, 16> source = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF,
    };
    std::array<m_byte, source.size()> observed{};

    memory.patchBytes(0x100u, source.data(), source.size());
    memory.copyBytes(0x100u, 0xFFFF2000u, static_cast<int>(source.size()));
    memory.copyToBuffer(0x00FF2000u, observed.data(), static_cast<int>(observed.size()));
    assert(observed == source);

    memory.copyBytes(0x00FF2000u, 0x00FF2004u, 12);
    memory.copyToBuffer(0x00FF2000u, observed.data(), static_cast<int>(observed.size()));
    for (int index = 0; index < 4; ++index)
        assert(observed[index] == source[index]);
    for (int index = 4; index < 16; ++index)
        assert(observed[index] == source[index - 4]);

    memory.writeFromBuffer(source.data(), 0xFFFF3000u, static_cast<int>(source.size()));
    memory.copyToBuffer(0x00FF3000u, observed.data(), static_cast<int>(observed.size()));
    assert(observed == source);

    memory.resetWorkRAM();
    memory.copyToBuffer(0x00FF3000u, observed.data(), static_cast<int>(observed.size()));
    for (m_byte value : observed)
        assert(value == 0u);
}

void testConcurrentRomPublication() {
    SystemMemory memory;
    constexpr std::array<m_byte, 4> first = {0x11, 0x22, 0x33, 0x44};
    constexpr std::array<m_byte, 4> second = {0xAA, 0xBB, 0xCC, 0xDD};
    memory.patchBytes(0, first.data(), first.size());

    std::atomic<bool> finished = false;
    std::thread writer([&] {
        for (int iteration = 0; iteration < 100'000; ++iteration) {
            const auto &value = (iteration & 1) != 0 ? first : second;
            memory.patchBytes(0, value.data(), value.size());
        }
        finished.store(true, std::memory_order_release);
    });

    do {
        const m_long value = memory.readLong(0);
        assert(value == 0x11223344u || value == 0xAABBCCDDu);
    } while (!finished.load(std::memory_order_acquire));

    writer.join();
}

void testConcurrentWorkRamBusWords() {
    SystemMemory memory;
    constexpr m_word first = 0x1122u;
    constexpr m_word second = 0xAABBu;
    memory.writeWord(0x00FF4000u, first);

    std::atomic<bool> finished = false;
    std::thread writer([&] {
        for (int iteration = 0; iteration < 100'000; ++iteration)
            memory.writeWord(0x00FF4000u, (iteration & 1) != 0 ? first : second);
        finished.store(true, std::memory_order_release);
    });

    do {
        const m_word value = memory.readWord(0x00FF4000u);
        assert(value == first || value == second);
    } while (!finished.load(std::memory_order_acquire));

    writer.join();
}

} // namespace

int main() {
    testAddressingAndEndianAccess();
    testBulkTransfers();
    testConcurrentRomPublication();
    testConcurrentWorkRamBusWords();
}
