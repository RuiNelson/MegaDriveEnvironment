/**
 * @file SystemMemory.cpp
 * @brief 68K address-space memory implementation (ported from memory.cpp).
 */

#include "SystemMemory.hpp"

#include "system/MegaDriveEnvironment.hpp"

#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <ios>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#define ROM_SIZE      0x400000u
#define WORK_RAM_SIZE 0x10000u

#define ROM_END       ROM_SIZE
#define WORK_RAM_BASE 0xFF0000u

// Control RAM is on the `Controllers` instance, VDP RAM is on the `VDP`
// instance, and Z80 RAM is on the `Z80` instance — all reached through the
// owning MegaDriveEnvironment.

SystemMemory::SystemMemory(MegaDriveEnvironment *env) : env_(env) {
    rom_  = static_cast<m_byte *>(malloc(ROM_SIZE));
    wram_ = new std::atomic<m_word>[WORK_RAM_WORDS];
    initRAM();
}

SystemMemory::~SystemMemory() {
    free(rom_);
    delete[] wram_;
    rom_  = nullptr;
    wram_ = nullptr;
}

void SystemMemory::initRAM() {
    {
        std::unique_lock lock(romMutex_);
        clearROM();
    }
    for (std::size_t index = 0; index < WORK_RAM_WORDS; ++index)
        wram_[index].store(0, std::memory_order_relaxed);
}

void SystemMemory::resetWorkRAM() {
    for (std::size_t index = 0; index < WORK_RAM_WORDS; ++index)
        wram_[index].store(0, std::memory_order_relaxed);
}

bool SystemMemory::loadROM(const std::string &path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        std::cerr << std::format("SystemMemory: cannot open ROM file '{}'\n", path);
        return false;
    }

    const std::streamoff size = file.tellg();
    if (size < 0) {
        std::cerr << std::format("SystemMemory: cannot determine size of ROM file '{}'\n", path);
        return false;
    }
    file.seekg(0, std::ios::beg);

    // Never read past the ROM region; a larger image is truncated to 4 MiB.
    auto toRead = static_cast<std::streamsize>(size);
    if (static_cast<unsigned long long>(size) > ROM_SIZE) {
        std::cerr << std::format("SystemMemory: ROM file '{}' is {} bytes, larger than the {} byte ROM "
                                 "region; truncating to {} bytes\n",
                                 path,
                                 static_cast<unsigned long long>(size),
                                 ROM_SIZE,
                                 ROM_SIZE);
        toRead = static_cast<std::streamsize>(ROM_SIZE);
    }

    std::vector<m_byte> image(static_cast<std::size_t>(toRead));
    file.read(reinterpret_cast<char *>(image.data()), toRead);
    if (!file && !file.eof()) {
        std::cerr << std::format("SystemMemory: error reading ROM file '{}'\n", path);
        return false;
    }

    std::unique_lock lock(romMutex_);
    publishROMBytes(0, image.data(), image.size());
    return true;
}

void SystemMemory::patchBytes(m_long address, const void *data, std::size_t count) {
    const m_long a = address & 0x00FFFFFFu;
    if (a >= ROM_END || count > static_cast<std::size_t>(ROM_END - a))
        throw std::out_of_range("SystemMemory::patchBytes range is outside cartridge ROM");
    std::unique_lock lock(romMutex_);
    publishROMBytes(a, static_cast<const m_byte *>(data), count);
}

void SystemMemory::publishROMBytes(std::size_t offset, const m_byte *data, std::size_t count) {
    romSequence_.fetch_add(1, std::memory_order_acq_rel);
    for (std::size_t index = 0; index < count; ++index)
        std::atomic_ref<m_byte>(rom_[offset + index]).store(data[index], std::memory_order_relaxed);
    romSequence_.fetch_add(1, std::memory_order_release);
    romSequence_.notify_all();
}

void SystemMemory::clearROM() {
    romSequence_.fetch_add(1, std::memory_order_acq_rel);
    for (std::size_t index = 0; index < ROM_SIZE; ++index)
        std::atomic_ref<m_byte>(rom_[index]).store(0, std::memory_order_relaxed);
    romSequence_.fetch_add(1, std::memory_order_release);
    romSequence_.notify_all();
}

m_byte SystemMemory::waitForByteValue(m_long address,
                                      m_byte desiredValue,
                                      const std::function<bool()> &waitForProgress) {
    m_byte value = readByte(address);
    while (value != desiredValue) {
        if (!waitForProgress())
            break;
        value = readByte(address);
    }
    return value;
}

m_word SystemMemory::waitForWordBits(m_long address,
                                     m_word mask,
                                     m_word expected,
                                     const std::function<bool()> &waitForProgress) {
    expected &= mask;
    m_word value = readWord(address);
    while ((value & mask) != expected) {
        if (!waitForProgress())
            break;
        value = readWord(address);
    }
    return value;
}

// ── Memory-mapped hardware (VDP / I/O / Z80 / TMSS) ─────────────────────────────
//
// Everything between the ROM (< 0x400000) and work RAM (>= 0xFF0000) is the
// hardware region. VDP ports and the controller I/O ports route to the matching
// subsystem; the remaining areas (Z80, TMSS, PSG, control) are safe stubs —
// reads return 0, writes are ignored — enough to let the recompiled boot run.

m_byte SystemMemory::hwReadByte(m_long address) {
    m_long a = address;
    if (env_ == nullptr)
        return 0;
    if (a >= 0xA00000u && a < 0xA02000u) {
        return env_->z80().readRAMFor68K(static_cast<uint16_t>(a));
    }
    switch (a) {
        case 0xA10001u:
            return env_->hardwareVersionRegister();
        case 0xA10003u:
            return env_->controllers().readPlayer1DataPort();
        case 0xA10005u:
            return env_->controllers().readPlayer2DataPort();
        case 0xA11100u:
            // Convention: bit 0 = 1 while the Z80 hasn't relinquished the bus
            // yet (matches the commonly documented real-hardware meaning).
            return (env_->z80().busRequestAcked()) ? 0x00u : 0x01u;
        default:
            break;
    }
    if (a >= 0xA04000u && a <= 0xA04003u) {
        // readYM2612 stamps the access with the shared wall clock; the 68K
        // instruction counter is not wall-time paced and must not be used here.
        return env_->sound().readYM2612(static_cast<int>(a - 0xA04000u));
    }
    if (a >= 0xC00000u && a < 0xC00010u) {
        m_word w = hwReadWord(a & ~1u);
        return static_cast<m_byte>((a & 1u) ? (w & 0xFFu) : (w >> 8));
    }
    return 0;
}

m_word SystemMemory::hwReadWord(m_long address) {
    m_long a = address;
    if (env_ == nullptr)
        return 0;
    if (a >= 0xA00000u && a < 0xA02000u) {
        m_byte hi = env_->z80().readRAMFor68K(static_cast<uint16_t>(a));
        m_byte lo = env_->z80().readRAMFor68K(static_cast<uint16_t>(a + 1));
        return static_cast<m_word>((static_cast<m_word>(hi) << 8) | lo);
    }
    if (a == 0xA11100u) {
        // The 68k always touches this register with word instructions
        // (e.g. `move.w #$0100,$A11100`); the meaningful bit lives in the
        // high byte, matching the byte-access convention above.
        return static_cast<m_word>(hwReadByte(a)) << 8;
    }
    if (a >= 0xA04000u && a <= 0xA04003u) {
        m_word hi = env_->sound().readYM2612(static_cast<int>(a - 0xA04000u));
        m_word lo = (a + 1 <= 0xA04003u) ? env_->sound().readYM2612(static_cast<int>(a + 1 - 0xA04000u)) : 0u;
        return static_cast<m_word>((hi << 8) | lo);
    }
    if (a >= 0xC00000u && a < 0xC00010u) {
        if (a < 0xC00004u)
            return env_->vdp().readDataPort();
        if (a < 0xC00008u)
            return env_->vdp().readControlPort();
        return env_->vdp().readHVCounter();
    }
    // I/O ports are byte-wide; a word read takes the low (odd) byte.
    return static_cast<m_word>(hwReadByte(a + 1));
}

m_long SystemMemory::hwReadLong(m_long address) {
    return (static_cast<m_long>(hwReadWord(address)) << 16) | static_cast<m_long>(hwReadWord(address + 2));
}

void SystemMemory::hwWriteByte(m_long address, m_byte value) {
    m_long a = address;
    if (env_ == nullptr)
        return;
    if (a >= 0xA00000u && a < 0xA02000u) {
        env_->z80().writeRAMFor68K(static_cast<uint16_t>(a), value);
        return;
    }
    switch (a) {
        case 0xA10003u:
            env_->controllers().writePlayer1DataPort(value);
            return;
        case 0xA10005u:
            env_->controllers().writePlayer2DataPort(value);
            return;
        case 0xA10009u:
            env_->controllers().writePlayer1ControlPort(value);
            return;
        case 0xA1000Bu:
            env_->controllers().writePlayer2ControlPort(value);
            return;
        case 0xA11100u:
            env_->z80().setBusRequest((value & 1u) != 0);
            return;
        case 0xA11200u:
            env_->z80().setReset((value & 1u) == 0); // active-low on real hardware
            return;
        default:
            break;
    }
    if (a >= 0xA04000u && a <= 0xA04003u) {
        env_->sound().writeYM2612(static_cast<int>(a - 0xA04000u), value);
        return;
    }
    if (a >= 0xC00010u && a < 0xC00018u && (a & 1u) != 0) {
        env_->sound().writePSG(value);
        return;
    }
    if (a >= 0xC00000u && a < 0xC00008u) {
        // A byte write to a VDP port puts the byte on both halves of the bus.
        m_word w = static_cast<m_word>((value << 8) | value);
        if ((a & 4u) != 0)
            env_->vdp().writeControlPort(w);
        else
            env_->vdp().writeDataPort(w);
    }
    // TMSS and other control regions: no-op stub.
}

void SystemMemory::hwWriteWord(m_long address, m_word value) {
    m_long a = address;
    if (env_ == nullptr)
        return;
    if (a >= 0xA00000u && a < 0xA02000u) {
        env_->z80().writeRAMFor68K(static_cast<uint16_t>(a), static_cast<m_byte>(value >> 8));
        env_->z80().writeRAMFor68K(static_cast<uint16_t>(a + 1), static_cast<m_byte>(value & 0xFFu));
        return;
    }
    if (a == 0xA11100u) {
        // See hwReadWord: the 68k always uses word access here, with the
        // meaningful bit in the high byte.
        env_->z80().setBusRequest(((value >> 8) & 1u) != 0);
        return;
    }
    if (a == 0xA11200u) {
        env_->z80().setReset(((value >> 8) & 1u) == 0);
        return;
    }
    if (a >= 0xA04000u && a <= 0xA04003u) {
        env_->sound().writeYM2612(static_cast<int>(a - 0xA04000u), static_cast<m_byte>(value >> 8));
        if (a + 1 <= 0xA04003u)
            env_->sound().writeYM2612(static_cast<int>(a + 1 - 0xA04000u), static_cast<m_byte>(value & 0xFFu));
        return;
    }
    if (a >= 0xC00000u && a < 0xC00008u) {
        if ((a & 4u) != 0)
            env_->vdp().writeControlPort(value);
        else
            env_->vdp().writeDataPort(value);
        return;
    }
    hwWriteByte(a + 1, static_cast<m_byte>(value & 0xFFu)); // I/O low byte
}

void SystemMemory::hwWriteLong(m_long address, m_long value) {
    // 32-bit port access = two word accesses (high word first), matching the
    // VDP control/data port protocol (e.g. address-set + register commands).
    hwWriteWord(address, static_cast<m_word>(value >> 16));
    hwWriteWord(address + 2, static_cast<m_word>(value & 0xFFFFu));
}

void SystemMemory::copyByte(m_long from, m_long to) {
    writeByte(to, readByte(from));
}

void SystemMemory::copyBytes(m_long from, m_long to, int count) {
    if (count <= 0)
        return;

    const m_long source = from & 0x00FFFFFFu;
    const m_long destination = to & 0x00FFFFFFu;
    const auto size = static_cast<std::size_t>(count);

    if (source >= WORK_RAM_BASE && destination >= WORK_RAM_BASE
        && size <= WORK_RAM_SIZE - (source - WORK_RAM_BASE)
        && size <= WORK_RAM_SIZE - (destination - WORK_RAM_BASE)) {
        const m_long sourceOffset = source - WORK_RAM_BASE;
        const m_long destinationOffset = destination - WORK_RAM_BASE;
        if (destinationOffset > sourceOffset
            && destinationOffset < sourceOffset + static_cast<m_long>(size)) {
            for (std::size_t index = size; index-- > 0;)
                storeWRAMByte(destinationOffset + static_cast<m_long>(index),
                              loadWRAMByte(sourceOffset + static_cast<m_long>(index)));
        } else {
            for (std::size_t index = 0; index < size; ++index)
                storeWRAMByte(destinationOffset + static_cast<m_long>(index),
                              loadWRAMByte(sourceOffset + static_cast<m_long>(index)));
        }
        return;
    }
    if (source < ROM_END && destination >= WORK_RAM_BASE
        && size <= ROM_END - source && size <= WORK_RAM_SIZE - (destination - WORK_RAM_BASE)) {
        std::shared_lock romLock(romMutex_);
        const m_long destinationOffset = destination - WORK_RAM_BASE;
        for (std::size_t index = 0; index < size; ++index)
            storeWRAMByte(destinationOffset + static_cast<m_long>(index), rom_[source + index]);
        return;
    }

    for (int index = 0; index < count; ++index)
        writeByte(to + static_cast<m_long>(index), readByte(from + static_cast<m_long>(index)));
}

void SystemMemory::copyWord(m_long from, m_long to) {
    writeWord(to, readWord(from));
}

void SystemMemory::copyLong(m_long from, m_long to) {
    writeLong(to, readLong(from));
}

void SystemMemory::copyToBuffer(m_long address, void *ptr, int count) {
    if (count <= 0)
        return;

    const m_long normalized = address & 0x00FFFFFFu;
    const auto size = static_cast<std::size_t>(count);
    if (normalized < ROM_END && size <= ROM_END - normalized) {
        std::shared_lock lock(romMutex_);
        std::memcpy(ptr, rom_ + normalized, size);
        return;
    }
    if (normalized >= WORK_RAM_BASE && size <= WORK_RAM_SIZE - (normalized - WORK_RAM_BASE)) {
        auto *destination = static_cast<m_byte *>(ptr);
        const m_long offset = normalized - WORK_RAM_BASE;
        for (std::size_t index = 0; index < size; ++index)
            destination[index] = loadWRAMByte(offset + static_cast<m_long>(index));
        return;
    }

    auto *destination = static_cast<m_byte *>(ptr);
    for (int index = 0; index < count; ++index)
        destination[index] = readByte(address + static_cast<m_long>(index));
}

void SystemMemory::writeFromBuffer(void *ptr, m_long address, int count) {
    if (count <= 0)
        return;

    const m_long normalized = address & 0x00FFFFFFu;
    const auto size = static_cast<std::size_t>(count);
    if (normalized >= WORK_RAM_BASE && size <= WORK_RAM_SIZE - (normalized - WORK_RAM_BASE)) {
        const auto *source = static_cast<const m_byte *>(ptr);
        const m_long offset = normalized - WORK_RAM_BASE;
        for (std::size_t index = 0; index < size; ++index)
            storeWRAMByte(offset + static_cast<m_long>(index), source[index]);
        return;
    }

    const auto *source = static_cast<const m_byte *>(ptr);
    for (int index = 0; index < count; ++index)
        writeByte(address + static_cast<m_long>(index), source[index]);
}
