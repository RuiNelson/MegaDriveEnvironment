/**
 * @file VDPPort.cpp
 * @brief VDP I/O port implementation.
 */

#include "VDPPort.hpp"
#include "system/MegaDriveEnvironment.hpp"
#include <algorithm>
#include <vector>

// ── Port I/O ─────────────────────────────────────────────────────────────────

void VDPPort::writeControlPort(m_word value) {
    VDPState &s = *state_;

    if (!s.pendingSecondWord_) {
        // Register write: bit 15=1, bit 14=0
        if ((value & 0xC000) == 0x8000) {
            processRegisterWrite(value);
            return;
        }

        // First word of address set
        s.firstWord_         = value;
        s.code_              = static_cast<uint8_t>((s.code_ & 0x3C) | ((value >> 14) & 0x03));
        s.address_           = static_cast<uint16_t>((s.address_ & 0xC000) | (value & 0x3FFF));
        s.pendingSecondWord_ = true;
    } else {
        // Second word of address set
        s.code_              = static_cast<uint8_t>(((value >> 2) & 0x3C) | (s.code_ & 0x03));
        s.address_           = static_cast<uint16_t>((s.address_ & 0x3FFF) | ((value & 0x03) << 14));
        s.pendingSecondWord_ = false;

        processAddressSet();
    }
}

m_word VDPPort::readControlPort() {
    VDPState &s          = *state_;
    updateDynamicStatus();
    s.pendingSecondWord_ = false;
    m_word result        = s.status_;
    s.status_ &= ~0x0060; // SCOL/SOVR are cleared on status read.
    return result;
}

void VDPPort::writeDataPort(m_word value) {
    VDPState &s          = *state_;
    s.pendingSecondWord_ = false;

    if (s.dmaFillPending_) {
        executeDMAFill(value);
        return;
    }

    switch (s.code_ & 0x0F) {
        case 0x01:
            writeVRAM(value);
            break;
        case 0x03:
            writeCRAM(value);
            break;
        case 0x05:
            writeVSRAM(value);
            break;
        default:
            break;
    }
}

m_word VDPPort::readDataPort() {
    VDPState &s          = *state_;
    s.pendingSecondWord_ = false;

    uint16_t result = s.readBuffer_;

    switch (s.code_ & 0x0F) {
        case 0x00:
            s.readBuffer_ = readVRAM();
            break;
        case 0x08:
            s.readBuffer_ = readCRAM();
            break;
        case 0x04:
            s.readBuffer_ = readVSRAM();
            break;
        default:
            s.readBuffer_ = 0;
            break;
    }

    return result;
}

m_word VDPPort::readHVCounter() {
    VDPState &s = *state_;
    updateDynamicStatus();
    int vCounter = s.vCounter_;
    if (s.interlaceMode() == 2) {
        vCounter <<= 1;
        vCounter = (vCounter & ~1) | ((vCounter >> 8) & 1);
    }
    return static_cast<m_word>(((vCounter & 0xFF) << 8) | (s.hCounter_ & 0xFF));
}

// ── Register write ─────────────────────────────────────────────────────────

/// Decodes register number and data from control port write (format: 1000RRRR RRDDDDD), writes to VDP register array.
void VDPPort::processRegisterWrite(m_word value) {
    VDPState &s    = *state_;
    m_byte    reg  = (value >> 8) & 0x1F;
    m_byte    data = value & 0xFF;
    if (reg < VDPState::REG_COUNT) {
        s.regs_[reg] = data;
    }
    s.pendingSecondWord_ = false;
}

/// Finalizes address set (completes two-word protocol). If code register has DMA bit set, triggers DMA execution.
void VDPPort::processAddressSet() {
    VDPState &s = *state_;
    if ((s.code_ & 0x20) && s.dmaEnabled()) {
        executeDMA();
    }
}

// ── DMA ─────────────────────────────────────────────────────────────────────

/// Dispatches DMA operation based on mode (0/1=copy, 2=fill, 3=VRAM copy) from register $17 bits 7–6.
void VDPPort::executeDMA() {
    VDPState &s    = *state_;
    m_byte    mode = (s.regs_[0x17] >> 6) & 0x03;

    switch (mode) {
        case 0:
        case 1:
            executeDMACopy();
            break;
        case 2:
            s.dmaFillPending_ = true;
            s.status_ |= 0x0002;
            break;
        case 3:
            executeDMAVRAMCopy();
            break;
    }
}

/// Executes DMA copy: transfers data from 68000 RAM (via copyToBuffer) to VRAM/CRAM/VSRAM based on code register.
/// Source address from registers $15–$17; length from registers $13–$14. Auto-increments address and updates source
/// registers.
void VDPPort::executeDMACopy() {
    VDPState &s = *state_;

    uint32_t srcAddr = static_cast<uint32_t>(s.regs_[0x15] | (s.regs_[0x16] << 8) | ((s.regs_[0x17] & 0x7F) << 16))
                    << 1;

    int length = s.regs_[0x13] | (s.regs_[0x14] << 8);
    int count  = (length == 0) ? 65536 : length;

    s.status_ |= 0x0002;
    const int slotsPerLine = s.h40Mode() ? 18 : 16;
    s.dmaEndCycle_ = currentMasterCycles()
                   + std::max<uint64_t>(1, (static_cast<uint64_t>(count) * VDPState::MASTER_CYCLES_PER_LINE)
                                               / static_cast<uint64_t>(slotsPerLine));

    std::vector<uint8_t> buf(static_cast<size_t>(count) * 2);
    if (env_) {
        env_->memory().copyToBuffer(srcAddr, buf.data(), count * 2);
    }

    for (int i = 0; i < count; ++i) {
        uint16_t word = (static_cast<uint16_t>(buf[i * 2]) << 8) | buf[i * 2 + 1];

        switch (s.code_ & 0x0F) {
            case 0x01:
                writeVRAM(word);
                break;
            case 0x03:
                writeCRAM(word);
                break;
            case 0x05:
                writeVSRAM(word);
                break;
            default:
                break;
        }
    }

    m_long finalSrc = (srcAddr + static_cast<m_long>(count) * 2) >> 1;
    s.regs_[0x15]   = static_cast<m_byte>(finalSrc & 0xFF);
    s.regs_[0x16]   = static_cast<m_byte>((finalSrc >> 8) & 0xFF);
    s.regs_[0x17]   = static_cast<m_byte>((s.regs_[0x17] & 0xC0) | ((finalSrc >> 16) & 0x7F));
}

/// Executes DMA fill: writes fillWord high byte to VRAM at current address, then fills remaining cells with that byte.
/// Length from registers $13–$14. Auto-increments address. Clears dmaFillPending_ flag.
void VDPPort::executeDMAFill(m_word fillWord) {
    VDPState &s       = *state_;
    s.dmaFillPending_ = false;

    s.vram_[s.address_ & 0xFFFE] = (fillWord >> 8) & 0xFF;
    s.vram_[s.address_ | 0x0001] = fillWord & 0xFF;
    updateSATShadow(s.address_ & 0xFFFE);
    updateSATShadow(s.address_ | 0x0001);

    m_byte fillByte = (fillWord >> 8) & 0xFF;
    int    length   = s.regs_[0x13] | (s.regs_[0x14] << 8);
    int    count    = (length == 0) ? 65536 : length;

    s.status_ |= 0x0002;
    const int slotsPerLine = s.h40Mode() ? 17 : 15;
    s.dmaEndCycle_ = currentMasterCycles()
                   + std::max<uint64_t>(1, (static_cast<uint64_t>(count) * VDPState::MASTER_CYCLES_PER_LINE)
                                               / static_cast<uint64_t>(slotsPerLine));

    s.address_ += static_cast<uint16_t>(s.autoIncrement());
    for (int i = 1; i < count; ++i) {
        s.vram_[s.address_ ^ 1] = fillByte;
        updateSATShadow(s.address_ ^ 1);
        s.address_ += static_cast<uint16_t>(s.autoIncrement());
    }
}

/// Executes DMA VRAM copy: transfers data from source VRAM address (registers $15–$16) to destination (address
/// register). Length from registers $13–$14. Auto-increments both addresses.
void VDPPort::executeDMAVRAMCopy() {
    VDPState &s       = *state_;
    uint16_t  srcAddr = static_cast<uint16_t>(s.regs_[0x15] | (s.regs_[0x16] << 8));
    int       length  = s.regs_[0x13] | (s.regs_[0x14] << 8);
    int       count   = (length == 0) ? 65536 : length;

    s.status_ |= 0x0002;
    const int slotsPerLine = s.h40Mode() ? 9 : 8;
    s.dmaEndCycle_ = currentMasterCycles()
                   + std::max<uint64_t>(1, (static_cast<uint64_t>(count) * VDPState::MASTER_CYCLES_PER_LINE)
                                               / static_cast<uint64_t>(slotsPerLine));

    for (int i = 0; i < count; ++i) {
        s.vram_[s.address_ ^ 1] = s.vram_[srcAddr ^ 1];
        updateSATShadow(s.address_ ^ 1);
        srcAddr++;
        s.address_ += static_cast<uint16_t>(s.autoIncrement());
    }
}

// ── Memory access ───────────────────────────────────────────────────────────

/// Mirrors one byte from vram_[] into sat_[] when byteAddr falls within the SAT region.
void VDPPort::updateSATShadow(m_word byteAddr) {
    VDPState &s      = *state_;
    int       offset = static_cast<int>(byteAddr) - s.satBase();
    if (offset >= 0 && offset < VDPState::SAT_SIZE) {
        s.sat_[offset] = s.vram_[byteAddr];
    }
}

/// Writes word to VRAM at current address (high byte to even addr, low byte to odd addr). Auto-increments address.
void VDPPort::writeVRAM(m_word value) {
    VDPState &s    = *state_;
    noteFIFOWrite(2);
    m_word    addr = s.address_;
    if (addr & 1) {
        value = ((value >> 8) | (value << 8)) & 0xFFFF;
    }
    s.vram_[addr & 0xFFFE] = (value >> 8) & 0xFF;
    s.vram_[addr | 0x0001] = value & 0xFF;
    updateSATShadow(addr & 0xFFFE);
    updateSATShadow(addr | 0x0001);
    s.address_ += static_cast<m_word>(s.autoIncrement());
}

/// Reads word from VRAM at current address (high byte from even addr, low byte from odd addr). Auto-increments address.
m_word VDPPort::readVRAM() {
    VDPState &s      = *state_;
    m_word    addr   = s.address_ & 0xFFFE;
    m_word    result = static_cast<m_word>((s.vram_[addr] << 8) | s.vram_[addr + 1]);
    s.address_ += static_cast<m_word>(s.autoIncrement());
    return result;
}

/// Writes word to CRAM at index derived from address register (address >> 1 & 0x3F). Masks to valid bits (0x0EEE).
/// Auto-increments address.
void VDPPort::writeCRAM(m_word value) {
    VDPState &s     = *state_;
    noteFIFOWrite(1);
    int       index = (s.address_ >> 1) & 0x3F;
    s.cram_[index]  = value & 0x0EEE;
    s.address_ += static_cast<m_word>(s.autoIncrement());
}

/// Reads word from CRAM at index derived from address register. Auto-increments address.
m_word VDPPort::readCRAM() {
    VDPState &s     = *state_;
    int       index = (s.address_ >> 1) & 0x3F;
    s.address_ += static_cast<m_word>(s.autoIncrement());
    return s.cram_[index];
}

/// Writes word to VSRAM at index derived from address register (address >> 1 & 0x3F, bounds checked). Masks to valid
/// bits (0x07FF). Auto-increments address.
void VDPPort::writeVSRAM(m_word value) {
    VDPState &s     = *state_;
    noteFIFOWrite(1);
    int       index = (s.address_ >> 1) & 0x3F;
    if (index < VDPState::VSRAM_ENTRIES) {
        s.vsram_[index] = value & 0x07FF;
    }
    s.address_ += static_cast<m_word>(s.autoIncrement());
}

/// Reads word from VSRAM at index derived from address register. Returns 0 if out of bounds. Auto-increments address.
m_word VDPPort::readVSRAM() {
    VDPState &s      = *state_;
    int       index  = (s.address_ >> 1) & 0x3F;
    m_word    result = 0;
    if (index < VDPState::VSRAM_ENTRIES) {
        result = s.vsram_[index];
    }
    s.address_ += static_cast<uint16_t>(s.autoIncrement());
    return result;
}

uint64_t VDPPort::currentMasterCycles() const {
    return env_ ? env_->current68KMasterCycles() : 0;
}

void VDPPort::updateDynamicStatus() {
    VDPState &s = *state_;
    const bool pal = env_ != nullptr && env_->isPal50Hz();
    const uint64_t cycles = currentMasterCycles();
    s.updateCountersFromCycles(cycles, pal);

    m_word dynamic = s.status_ & ~(0x030C);

    if (!s.displayEnabled() || s.vCounter_ >= s.activeHeight()) {
        dynamic |= 0x0008;
    }

    if (s.interlaced() && s.oddFrame_) {
        dynamic |= 0x0010;
    } else {
        dynamic &= ~0x0010;
    }

    const int hblankStart = s.h40Mode() ? 0xE8 : 0xDA;
    const int hblankEnd   = s.h40Mode() ? 0x08 : 0x10;
    if (s.hCounter_ >= hblankStart || s.hCounter_ < hblankEnd) {
        dynamic |= 0x0004;
    }

    const uint64_t newest = s.fifoDrainCycle_[(s.fifoIndex_ + 3) & 3];
    const uint64_t oldest = s.fifoDrainCycle_[s.fifoIndex_];
    if (cycles >= newest) {
        dynamic |= 0x0200;
    } else if (cycles < oldest) {
        dynamic |= 0x0100;
    }

    if (s.dmaEndCycle_ != 0 && cycles < s.dmaEndCycle_) {
        dynamic |= 0x0002;
    }

    s.status_ = dynamic;
}

void VDPPort::noteFIFOWrite(int accessSlots) {
    VDPState &s = *state_;
    const uint64_t cycles = currentMasterCycles();
    const int slotsPerLine = s.h40Mode() ? 18 : 16;
    const uint64_t slotCycles = VDPState::MASTER_CYCLES_PER_LINE / static_cast<uint64_t>(slotsPerLine);

    const int prev = (s.fifoIndex_ + 3) & 3;
    uint64_t start = std::max(cycles, s.fifoDrainCycle_[prev]);
    s.fifoDrainCycle_[s.fifoIndex_] = start + slotCycles * static_cast<uint64_t>(std::max(1, accessSlots));
    s.fifoIndex_ = (s.fifoIndex_ + 1) & 3;
}
