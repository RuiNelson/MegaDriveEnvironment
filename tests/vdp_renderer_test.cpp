#include "system/graphics/Framebuffer.hpp"
#include "system/graphics/VDPRenderer.hpp"
#include "system/graphics/VDPState.hpp"
#include "system/graphics/VDPTile.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

using Pixel = VDPRenderer::PixelResult;

std::vector<m_byte> referenceScanline(
    VDPState &state, VDPTile &tile, VDPRenderer &renderer, int line, bool includeSprites) {
    const int width = state.activeWidth();
    std::vector<m_byte> result(static_cast<std::size_t>(width * Framebuffer::BPP));

    const int scrollBase = state.hscrollBase();
    int hscrollA = 0;
    int hscrollB = 0;
    int scrollAddress = scrollBase;
    if (state.hscrollMode() == 2)
        scrollAddress = (scrollBase + (line / 8) * 8 * 4) & 0xFFFF;
    else if (state.hscrollMode() == 3)
        scrollAddress = (scrollBase + line * 4) & 0xFFFF;
    if (state.hscrollMode() == 0 || state.hscrollMode() == 2 || state.hscrollMode() == 3) {
        hscrollA = static_cast<int16_t>((state.vram_[scrollAddress] << 8) | state.vram_[scrollAddress + 1]);
        hscrollB =
            static_cast<int16_t>((state.vram_[scrollAddress + 2] << 8) | state.vram_[scrollAddress + 3]);
    }

    m_byte bgR = 0;
    m_byte bgG = 0;
    m_byte bgB = 0;
    tile.cramToRGB(static_cast<m_byte>(state.bgColorPalette()),
                   static_cast<m_byte>(state.bgColorIndex()),
                   bgR,
                   bgG,
                   bgB);

    for (int x = 0; x < width; ++x) {
        int vscrollA = static_cast<int16_t>(state.vsram_[0]);
        int vscrollB = static_cast<int16_t>(state.vsram_[1]);
        if (state.vscrollMode() == 1) {
            const int index = (x / 16) * 2;
            if (index + 1 < VDPState::VSRAM_ENTRIES) {
                vscrollA = static_cast<int16_t>(state.vsram_[index]);
                vscrollB = static_cast<int16_t>(state.vsram_[index + 1]);
            }
        }

        const Pixel planeB =
            renderer.getPlanePixel(state.planeBBase(), hscrollB, vscrollB, x, line);
        const Pixel planeA = renderer.isWindowActive(x / 8, line / 8)
            ? renderer.getWindowPixel(x, line)
            : renderer.getPlanePixel(state.planeABase(), hscrollA, vscrollA, x, line);
        const Pixel sprite =
            includeSprites ? renderer.getSpritePixel(x, line) : Pixel{0, 0, false, false};
        const auto composite = renderer.resolvePixel(planeB, planeA, sprite, bgR, bgG, bgB);

        result[x * Framebuffer::BPP + 0] = composite.valid ? composite.b : bgB;
        result[x * Framebuffer::BPP + 1] = composite.valid ? composite.g : bgG;
        result[x * Framebuffer::BPP + 2] = composite.valid ? composite.r : bgR;
    }
    return result;
}

void expectRenderedLine(VDPState &state,
                        Framebuffer &framebuffer,
                        VDPRenderer &renderer,
                        const std::vector<m_byte> &expected,
                        int line) {
    framebuffer.clear();
    renderer.renderScanline(line);

    const int outputY = state.interlaced() ? line * 2 : line;
    const auto *actual =
        static_cast<const m_byte *>(framebuffer.getRawPointer()) + outputY * Framebuffer::PITCH;
    for (std::size_t index = 0; index < expected.size(); ++index) {
        if (actual[index] != expected[index]) {
            std::fprintf(stderr,
                         "scanline mismatch at line=%d byte=%zu: actual=%u expected=%u\n",
                         line,
                         index,
                         static_cast<unsigned>(actual[index]),
                         static_cast<unsigned>(expected[index]));
            assert(false);
        }
    }

    if (state.interlaced()) {
        const auto *duplicate = actual + Framebuffer::PITCH;
        assert(std::equal(expected.begin(), expected.end(), duplicate));
    }
}

void fillDeterministicState(VDPState &state) {
    std::uint32_t value = 0x13579BDFu;
    for (auto &byte : state.vram_) {
        value = value * 1664525u + 1013904223u;
        byte = static_cast<m_byte>(value >> 24);
    }
    for (auto &color : state.cram_) {
        value = value * 1664525u + 1013904223u;
        color = static_cast<m_word>(value & 0x0EEEu);
    }
    for (auto &scroll : state.vsram_) {
        value = value * 1664525u + 1013904223u;
        scroll = static_cast<m_word>(value);
    }
    std::fill(std::begin(state.sat_), std::end(state.sat_), 0);
}

void testPlanesAndWindowAgainstReference() {
    VDPState state;
    Framebuffer framebuffer;
    VDPTile tile(state);
    VDPRenderer renderer(state, tile, framebuffer);
    fillDeterministicState(state);

    state.regs_[0x00] = 0x04;
    state.regs_[0x01] = 0x40;
    state.regs_[0x02] = 0x30;
    state.regs_[0x03] = 0x34;
    state.regs_[0x04] = 0x07;
    state.regs_[0x05] = 0x50;
    state.regs_[0x07] = 0x21;
    state.regs_[0x0D] = 0x2C;

    struct Scenario {
        m_byte scrollMode;
        m_byte displayMode;
        m_byte planeSize;
        m_byte windowH;
        m_byte windowV;
        int line;
    };
    constexpr Scenario scenarios[] = {
        {0x00, 0x01, 0x00, 0x00, 0x00, 3},
        {0x07, 0x01, 0x11, 0x08, 0x08, 73},
        {0x03, 0x00, 0x33, 0x88, 0x00, 127},
        {0x06, 0x01, 0x01, 0x04, 0x84, 191},
        {0x07, 0x05, 0x10, 0x86, 0x05, 111},
    };

    for (const Scenario &scenario : scenarios) {
        state.regs_[0x0B] = scenario.scrollMode;
        state.regs_[0x0C] = scenario.displayMode;
        state.regs_[0x10] = scenario.planeSize;
        state.regs_[0x11] = scenario.windowH;
        state.regs_[0x12] = scenario.windowV;
        const auto expected = referenceScanline(state, tile, renderer, scenario.line, false);
        expectRenderedLine(state, framebuffer, renderer, expected, scenario.line);
    }
}

void testSpriteLineAgainstReference() {
    VDPState state;
    Framebuffer framebuffer;
    VDPTile tile(state);
    VDPRenderer renderer(state, tile, framebuffer);

    state.regs_[0x00] = 0x04;
    state.regs_[0x01] = 0x40;
    state.regs_[0x02] = 0x30;
    state.regs_[0x04] = 0x07;
    state.regs_[0x05] = 0x68;
    state.regs_[0x0C] = 0x01;

    std::fill(std::begin(state.vram_) + 32, std::begin(state.vram_) + 64, 0x11);
    state.cram_[17] = 0x0E00;

    const int spriteY = 40;
    const int spriteX = 20;
    state.sat_[0] = static_cast<m_byte>((spriteY + 128) >> 8);
    state.sat_[1] = static_cast<m_byte>(spriteY + 128);
    state.sat_[2] = 0;
    state.sat_[3] = 0;
    const int sat = state.satBase();
    state.vram_[sat + 4] = 0xA0;
    state.vram_[sat + 5] = 0x01;
    state.vram_[sat + 6] = static_cast<m_byte>((spriteX + 128) >> 8);
    state.vram_[sat + 7] = static_cast<m_byte>(spriteX + 128);

    const auto expected = referenceScanline(state, tile, renderer, spriteY + 3, true);
    expectRenderedLine(state, framebuffer, renderer, expected, spriteY + 3);

    // Shadow/highlight sprite operator over an opaque Plane A pixel.
    state.regs_[0x0C] = 0x09;
    std::fill(std::begin(state.vram_) + 32, std::begin(state.vram_) + 64, 0xEE);
    std::fill(std::begin(state.vram_) + 64, std::begin(state.vram_) + 96, 0x44);
    for (int offset = 0; offset < 32 * 32 * 2; offset += 2) {
        state.vram_[state.planeABase() + offset] = 0;
        state.vram_[state.planeABase() + offset + 1] = 2;
    }
    state.cram_[4] = 0x0222;
    state.vram_[sat + 4] = 0x60;
    state.vram_[sat + 5] = 0x01;

    const auto highlightExpected = referenceScanline(state, tile, renderer, spriteY + 3, true);
    expectRenderedLine(state, framebuffer, renderer, highlightExpected, spriteY + 3);
}

} // namespace

int main() {
    testPlanesAndWindowAgainstReference();
    testSpriteLineAgainstReference();
}
