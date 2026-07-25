#include "system/MegaDriveEnvironment.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

class CooperativeHSyncEnvironment final : public MegaDriveEnvironment {
  public:
    CooperativeHSyncEnvironment()
        : MegaDriveEnvironment(
              VDP::InternalTimer, VDP::Scale1x, VDP::HardwareSpriteLimit, 0) {}

    std::array<std::uint8_t, 3> pixelAt(int x, int y) const {
        const std::size_t offset =
            static_cast<std::size_t>(y) * snapshot_.pitch +
            static_cast<std::size_t>(x) * 3;
        return {
            snapshot_.pixels[offset],
            snapshot_.pixels[offset + 1],
            snapshot_.pixels[offset + 2],
        };
    }

    const std::vector<int> &hSyncLines() const {
        return hSyncLines_;
    }

  private:
    void writeRegister(std::uint8_t reg, std::uint8_t value) {
        vdp().writeControlPort(
            static_cast<std::uint16_t>(0x8000u | (reg << 8) | value));
    }

    void writeBackdrop(std::uint16_t color) {
        vdp().writeControlPort(0xC000);
        vdp().writeControlPort(0x0000);
        vdp().writeDataPort(color);
    }

    void run() override {
        writeRegister(0x00, 0x14); // full CRAM palette + HINT
        writeRegister(0x01, 0x74); // display + DMA + Mode 5 + VINT
        writeRegister(0x07, 0x00); // backdrop palette 0, entry 0
        writeRegister(0x0A, 0x07); // one HINT every eight scanlines
        writeRegister(0x0C, 0x81); // H40, non-interlaced
        writeBackdrop(0x0000);

        while (completedFrames_ < 3) {
            runVDPInterrupts();
            pace();
        }
        snapshot_ = vdp().framebufferSnapshot();
    }

    void hSync(int scanline) override {
        hSyncLines_.push_back(scanline);
        switch (scanline) {
            case 7:
                writeBackdrop(0x000E); // red
                break;
            case 15:
                writeBackdrop(0x00E0); // green
                break;
            case 23:
                writeBackdrop(0x0E00); // blue
                break;
            default:
                break;
        }
    }

    void vSync() override {
        ++completedFrames_;
        writeBackdrop(0x0000);
    }

    unsigned                 completedFrames_ = 0;
    VDP::FramebufferSnapshot snapshot_;
    std::vector<int>         hSyncLines_;
};

class LockFreeHSyncEnvironment final : public MegaDriveEnvironment {
  public:
    LockFreeHSyncEnvironment()
        : MegaDriveEnvironment(
              VDP::InternalTimer, VDP::Scale1x, VDP::HardwareSpriteLimit, 0) {}

    unsigned handledHSyncs() const {
        return handledHSyncs_;
    }

  private:
    void writeRegister(std::uint8_t reg, std::uint8_t value) {
        vdp().writeControlPort(
            static_cast<std::uint16_t>(0x8000u | (reg << 8) | value));
    }

    void run() override {
        writeRegister(0x00, 0x14);
        writeRegister(0x01, 0x74);
        writeRegister(0x0A, 0x07);
        writeRegister(0x0C, 0x81);

        while (gameUptimeFrames() < 2) {
            waitForInterrupt();
            const int level = irqLevel();
            if (level != 0) {
                clearInterrupt(level);
                if (level == 4) {
                    ++handledHSyncs_;
                }
            }
        }
    }

    unsigned handledHSyncs_ = 0;
};

} // namespace

int main() {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");

    CooperativeHSyncEnvironment environment;
    environment.boot();

    const auto &lines = environment.hSyncLines();
    constexpr std::array expectedLines{7, 15, 23};
    assert(std::search(lines.begin(),
                       lines.end(),
                       expectedLines.begin(),
                       expectedLines.end()) != lines.end());

    // Framebuffer pixels are packed BGR with native three-bit components.
    assert((environment.pixelAt(4, 4) == std::array<std::uint8_t, 3>{0, 0, 0}));
    assert((environment.pixelAt(4, 12) == std::array<std::uint8_t, 3>{0, 0, 7}));
    assert((environment.pixelAt(4, 20) == std::array<std::uint8_t, 3>{0, 7, 0}));
    assert((environment.pixelAt(4, 28) == std::array<std::uint8_t, 3>{7, 0, 0}));

    // Recompiled consumers acknowledge the same mandatory ticket when they
    // enter the lock-free IRQ4 path instead of dispatching hSync() callbacks.
    LockFreeHSyncEnvironment lockFreeEnvironment;
    lockFreeEnvironment.boot();
    assert(lockFreeEnvironment.handledHSyncs() >= 28);
}
