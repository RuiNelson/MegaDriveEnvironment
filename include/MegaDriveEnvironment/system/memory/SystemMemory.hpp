#pragma once

#include "data_types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <shared_mutex>
#include <string>

class MegaDriveEnvironment;

/**
 * @file SystemMemory.hpp
 * @brief 68K address-space memory for a MegaDriveEnvironment.
 *
 * Owns the cartridge ROM (0x000000-0x3FFFFF) and the 68000 work RAM
 * (0xFF0000-0xFFFFFF) and exposes a unified read/write interface over the
 * 24-bit address space. This was previously a set of global free functions
 * in memory.cpp; it is now an instance owned by MegaDriveEnvironment so that
 * each environment has its own address space and subsystems reach it through
 * the parent (e.g. `env->memory()`).
 *
 * All multi-byte values are read/written in big-endian (Motorola) byte order,
 * matching the native format of the 68000 CPU.
 *
 * Threading: cartridge ROM may be patched by remote automation after loadROM().
 * Normal ROM reads are lock-free atomic loads; a publication sequence keeps
 * multi-byte reads within one patch generation. Bulk ROM dumps/patches also use
 * a whole-image shared mutex. WRAM is stored as atomic 16-bit bus words, so
 * game-thread and remote accesses do not need a lock on every operation.
 * Memory-mapped hardware is routed to self-synchronizing subsystems.
 */
class SystemMemory {
    public:
    /// Allocates and zero-initializes ROM and work RAM.
    /// @p env (optional) lets memory-mapped hardware accesses (VDP, I/O ports,
    /// Z80, TMSS) route to the owning environment's subsystems; when null those
    /// regions read as 0 and ignore writes.
    explicit SystemMemory(MegaDriveEnvironment *env = nullptr);

    /// Releases ROM and work RAM.
    ~SystemMemory();

    SystemMemory(const SystemMemory &)            = delete;
    SystemMemory &operator=(const SystemMemory &) = delete;

    /// Re-zeroes ROM and work RAM. Kept for parity with the old initRAM() entry point.
    void initRAM();

    /// Clears only 68000 work RAM, preserving the complete cartridge image.
    void resetWorkRAM();

    /// Loads a cartridge ROM image from @p path into the ROM region
    /// (0x000000-0x3FFFFF). Reads at most ROM_SIZE (4 MiB) bytes; a larger file
    /// is truncated to that limit. On a missing or unreadable file, writes a
    /// clear error to stderr and leaves the ROM region unchanged (it is not a
    /// silent success).
    /// @return true when the complete requested image was loaded.
    bool loadROM(const std::string &path);

    /// Patches the in-memory cartridge image. Unlike normal bus writes, which
    /// preserve ROM hardware semantics, these helpers intentionally permit ROM
    /// modification for remote testing. The backing ROM file is never changed.
    void patchBytes(m_long address, const void *data, std::size_t count);

    /// Reads a single byte from the given 68K address.
    inline m_byte readByte(m_long address) {
        const m_long normalized = address & ADDRESS_MASK;
        if (normalized >= WORK_RAM_BASE) [[likely]] {
            return loadWRAMByte(normalized & WORK_RAM_MASK);
        }
        if (normalized < ROM_SIZE) [[likely]]
            return loadROMByte(normalized);
        return hwReadByte(normalized);
    }

    /// Reads a 16-bit word (big-endian) from the given 68K address.
    inline m_word readWord(m_long address) {
        const m_long normalized = address & ADDRESS_MASK;
        if (normalized >= WORK_RAM_BASE) [[likely]] {
            const m_long offset = normalized & WORK_RAM_MASK;
            if ((offset & 1u) == 0)
                return loadWRAMWord(offset);
            return static_cast<m_word>((static_cast<m_word>(loadWRAMByte(offset)) << 8)
                                     | loadWRAMByte((offset + 1) & WORK_RAM_MASK));
        }
        if (normalized < ROM_SIZE) [[likely]]
            return loadROMWord(normalized);
        return hwReadWord(normalized);
    }

    /// Reads a 32-bit long word (big-endian) from the given 68K address.
    inline m_long readLong(m_long address) {
        const m_long normalized = address & ADDRESS_MASK;
        if (normalized >= WORK_RAM_BASE) [[likely]] {
            const m_long offset = normalized & WORK_RAM_MASK;
            return (static_cast<m_long>(readWRAMWord(offset)) << 16)
                 | static_cast<m_long>(readWRAMWord((offset + 2) & WORK_RAM_MASK));
        }
        if (normalized < ROM_SIZE) [[likely]]
            return loadROMLong(normalized);
        return hwReadLong(normalized);
    }

    /// Writes a single byte to the given 68K address.
    inline void writeByte(m_long address, m_byte value) {
        const m_long normalized = address & ADDRESS_MASK;
        if (normalized >= WORK_RAM_BASE) [[likely]] {
            storeWRAMByte(normalized & WORK_RAM_MASK, value);
            return;
        }
        if (normalized < ROM_SIZE) [[likely]]
            return;
        hwWriteByte(normalized, value);
    }

    /// Writes a 16-bit word (big-endian) to the given 68K address.
    inline void writeWord(m_long address, m_word value) {
        const m_long normalized = address & ADDRESS_MASK;
        if (normalized >= WORK_RAM_BASE) [[likely]] {
            writeWRAMWord(normalized & WORK_RAM_MASK, value);
            return;
        }
        if (normalized < ROM_SIZE) [[likely]]
            return;
        hwWriteWord(normalized, value);
    }

    /// Writes a 32-bit long word (big-endian) to the given 68K address.
    inline void writeLong(m_long address, m_long value) {
        const m_long normalized = address & ADDRESS_MASK;
        if (normalized >= WORK_RAM_BASE) [[likely]] {
            const m_long offset = normalized & WORK_RAM_MASK;
            writeWRAMWord(offset, static_cast<m_word>(value >> 16));
            writeWRAMWord((offset + 2) & WORK_RAM_MASK, static_cast<m_word>(value));
            return;
        }
        if (normalized < ROM_SIZE) [[likely]]
            return;
        hwWriteLong(normalized, value);
    }

    /// Waits until a byte equals @p desiredValue. While it differs, invokes
    /// @p waitForProgress, which may block and must allow the producer of the
    /// desired value to run. Returning false cancels the wait. This is useful
    /// for replacing emulated polling loops without embedding scheduler or
    /// interrupt policy in memory.
    m_byte waitForByteValue(m_long address,
                            m_byte desiredValue,
                            const std::function<bool()> &waitForProgress);

    /// Waits until `(readWord(address) & mask) == (expected & mask)`. While the
    /// condition differs, invokes @p waitForProgress so an emulated producer
    /// can advance without a host-side busy loop. Returning false cancels.
    m_word waitForWordBits(m_long address,
                           m_word mask,
                           m_word expected,
                           const std::function<bool()> &waitForProgress);

    /// Copies one byte from @p from to @p to within the 68K address space.
    void copyByte(m_long from, m_long to);

    /// Copies @p count bytes from @p from to @p to within the 68K address space.
    void copyBytes(m_long from, m_long to, int count);

    /// Copies one word from @p from to @p to within the 68K address space.
    void copyWord(m_long from, m_long to);

    /// Copies one long word from @p from to @p to within the 68K address space.
    void copyLong(m_long from, m_long to);

    /// Copies @p count bytes from emulated memory into a host buffer.
    void copyToBuffer(m_long address, void *ptr, int count);

    /// Copies @p count bytes from a host buffer into emulated memory.
    void writeFromBuffer(void *ptr, m_long address, int count);

    private:
    static_assert(std::atomic_ref<m_byte>::is_always_lock_free,
                  "SystemMemory requires lock-free byte atomics for ROM reads");
    static_assert(std::atomic<m_word>::is_always_lock_free,
                  "SystemMemory requires lock-free 16-bit atomics for work RAM");

    static constexpr m_long ADDRESS_MASK  = 0x00FFFFFFu;
    static constexpr m_long ROM_SIZE      = 0x00400000u;
    static constexpr m_long WORK_RAM_BASE = 0x00FF0000u;
    static constexpr m_long WORK_RAM_MASK = 0x0000FFFFu;
    static constexpr std::size_t WORK_RAM_WORDS = 0x00008000u;

    inline m_word loadWRAMWord(m_long evenOffset) const {
        return wram_[(evenOffset & WORK_RAM_MASK) >> 1].load(std::memory_order_relaxed);
    }
    inline m_byte loadWRAMByte(m_long offset) const {
        const m_word word = loadWRAMWord(offset & ~1u);
        return static_cast<m_byte>((offset & 1u) != 0 ? word : word >> 8);
    }
    inline void storeWRAMWord(m_long evenOffset, m_word value) {
        wram_[(evenOffset & WORK_RAM_MASK) >> 1].store(value, std::memory_order_relaxed);
    }
    inline void storeWRAMByte(m_long offset, m_byte value) {
        auto &word = wram_[(offset & WORK_RAM_MASK) >> 1];
        m_word observed = word.load(std::memory_order_relaxed);
        m_word desired;
        do {
            desired = (offset & 1u) != 0
                    ? static_cast<m_word>((observed & 0xFF00u) | value)
                    : static_cast<m_word>((observed & 0x00FFu) | (static_cast<m_word>(value) << 8));
        } while (!word.compare_exchange_weak(observed,
                                             desired,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed));
    }
    inline m_word readWRAMWord(m_long offset) const {
        if ((offset & 1u) == 0)
            return loadWRAMWord(offset);
        return static_cast<m_word>((static_cast<m_word>(loadWRAMByte(offset)) << 8)
                                 | loadWRAMByte((offset + 1) & WORK_RAM_MASK));
    }
    inline void writeWRAMWord(m_long offset, m_word value) {
        if ((offset & 1u) == 0) {
            storeWRAMWord(offset, value);
            return;
        }
        storeWRAMByte(offset, static_cast<m_byte>(value >> 8));
        storeWRAMByte((offset + 1) & WORK_RAM_MASK, static_cast<m_byte>(value));
    }

    /// Lock-free ROM reads. Byte storage is accessed through atomic_ref so a
    /// rare remote patch cannot race the emulated CPU. Word/long reads use the
    /// sequence counter to reject a value spanning two patch generations.
    inline m_byte loadROMByte(m_long offset) {
        return std::atomic_ref<m_byte>(rom_[offset]).load(std::memory_order_relaxed);
    }
    inline std::uint32_t beginROMRead() {
        for (;;) {
            const std::uint32_t sequence = romSequence_.load(std::memory_order_acquire);
            if ((sequence & 1u) == 0)
                return sequence;
            romSequence_.wait(sequence, std::memory_order_acquire);
        }
    }
    inline m_word loadROMWord(m_long offset) {
        for (;;) {
            const std::uint32_t sequence = beginROMRead();
            const m_word value = static_cast<m_word>(
                (static_cast<m_word>(loadROMByte(offset)) << 8) | loadROMByte(offset + 1));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (romSequence_.load(std::memory_order_relaxed) == sequence)
                return value;
        }
    }
    inline m_long loadROMLong(m_long offset) {
        for (;;) {
            const std::uint32_t sequence = beginROMRead();
            const m_long value = (static_cast<m_long>(loadROMByte(offset)) << 24)
                               | (static_cast<m_long>(loadROMByte(offset + 1)) << 16)
                               | (static_cast<m_long>(loadROMByte(offset + 2)) << 8)
                               | static_cast<m_long>(loadROMByte(offset + 3));
            std::atomic_thread_fence(std::memory_order_acquire);
            if (romSequence_.load(std::memory_order_relaxed) == sequence)
                return value;
        }
    }
    void publishROMBytes(std::size_t offset, const m_byte *data, std::size_t count);
    void clearROM();

    // ── Memory-mapped hardware (VDP / I/O / Z80 / TMSS) ──────────────────────
    // Each subsystem self-synchronizes; the VDP's DMA can read back through
    // this memory without a cross-subsystem lock.
    m_byte      hwReadByte(m_long address);
    m_word      hwReadWord(m_long address);
    m_long      hwReadLong(m_long address);
    void        hwWriteByte(m_long address, m_byte value);
    void        hwWriteWord(m_long address, m_word value);
    void        hwWriteLong(m_long address, m_long value);

    MegaDriveEnvironment *env_ = nullptr; ///< For memory-mapped hardware routing.

    m_byte *rom_ = nullptr; ///< 0x000000-0x3FFFFF (4 MiB)
    std::atomic<m_word> *wram_ = nullptr; ///< 0xFF0000-0xFFFFFF as 32K bus words
    mutable std::shared_mutex romMutex_; ///< Serializes ROM publications and bulk snapshots.
    std::atomic<std::uint32_t> romSequence_{0}; ///< Odd while a ROM image patch is being published.
};
