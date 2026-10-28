#include "system/graphics/Framebuffer.hpp"
#include "system/graphics/VDP.hpp"
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
            const int index = (std::max(0, x - state.centerOffsetX()) / 16) * 2;
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

    const int outputLine = line - state.topCropLines();
    const int outputY = state.interlaced() ? outputLine * 2 : outputLine;
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

bool isPixel(const Framebuffer &framebuffer, int x, int y, m_byte b, m_byte g, m_byte r) {
    m_byte actualB = 0;
    m_byte actualG = 0;
    m_byte actualR = 0;
    framebuffer.getPixel(x, y, actualB, actualG, actualR);
    return actualB == b && actualG == g && actualR == r;
}

void setSprite(VDPState &state, int index, int xRaw, int yRaw, int link, m_word tileWord) {
    state.sat_[index * 8 + 0] = static_cast<m_byte>(yRaw >> 8);
    state.sat_[index * 8 + 1] = static_cast<m_byte>(yRaw);
    state.sat_[index * 8 + 2] = 0;
    state.sat_[index * 8 + 3] = static_cast<m_byte>(link);
    const int address = state.satBase() + index * 8;
    state.vram_[address + 4] = static_cast<m_byte>(tileWord >> 8);
    state.vram_[address + 5] = static_cast<m_byte>(tileWord);
    state.vram_[address + 6] = static_cast<m_byte>(xRaw >> 8);
    state.vram_[address + 7] = static_cast<m_byte>(xRaw);
}

/// Tile 1 is solid colour 1 (blue), tile 2 solid colour 2 (green), tile 3 solid colour 3 (red).
void setupSolidTiles(VDPState &state) {
    std::fill(std::begin(state.vram_) + 32, std::begin(state.vram_) + 64, 0x11);
    std::fill(std::begin(state.vram_) + 64, std::begin(state.vram_) + 96, 0x22);
    std::fill(std::begin(state.vram_) + 96, std::begin(state.vram_) + 128, 0x33);
    state.cram_[1] = 0x0E00;
    state.cram_[2] = 0x00E0;
    state.cram_[3] = 0x000E;
}

void testModeDimensions() {
    VDPState state;
    state.regs_[0x0C] = 0x01;
    assert(state.activeWidth() == 320 && state.activeOutputHeight() == 224 && state.topCropLines() == 0);
    assert(state.spriteOriginX() == 128 && state.centerOffsetX() == 0);

    state.widescreen_ = true;
    assert(state.widescreenActive());
    assert(state.activeWidth() == 384 && state.activeHeight() == 224 && state.activeOutputHeight() == 216);
    assert(state.hardwareWidth() == 320 && state.topCropLines() == 8);
    assert(state.spriteOriginX() == 96 && state.centerOffsetX() == 32);

    // Mode 48 only replaces H40: H32 stays untouched.
    state.regs_[0x0C] = 0x00;
    assert(!state.widescreenActive());
    assert(state.activeWidth() == 256 && state.activeOutputHeight() == 224);
}

void testWidescreenPlanes() {
    VDPState state;
    Framebuffer framebuffer;
    VDPTile tile(state);
    VDPRenderer renderer(state, tile, framebuffer);
    fillDeterministicState(state);

    state.widescreen_ = true;
    state.regs_[0x00] = 0x04;
    state.regs_[0x01] = 0x40;
    state.regs_[0x02] = 0x30;
    state.regs_[0x04] = 0x07;
    state.regs_[0x05] = 0x50;
    state.regs_[0x07] = 0x21;
    state.regs_[0x0B] = 0x07;
    state.regs_[0x0C] = 0x01;
    state.regs_[0x0D] = 0x2C;
    state.regs_[0x10] = 0x01;
    for (int line : {8, 9, 73, 150, 223}) {
        const auto expected = referenceScanline(state, tile, renderer, line, false);
        assert(expected.size() == static_cast<std::size_t>(384 * Framebuffer::BPP));
        expectRenderedLine(state, framebuffer, renderer, expected, line);
    }

    // The first tile row is not output.
    framebuffer.clear();
    for (int line = 0; line < 8; ++line)
        renderer.renderScanline(line);
    const auto *pixels = static_cast<const m_byte *>(framebuffer.getRawPointer());
    assert(std::all_of(pixels, pixels + Framebuffer::SIZE, [](m_byte value) { return value == 0; }));
}

/// Mode 48 extends the hardware picture by 32 pixels on each side, so plane pixel 0 lands where sprite X 128
/// does (output column 32) and the planes stay aligned with the sprites.
void testWidescreenPlanesAreCentred() {
    VDPState state;
    Framebuffer framebuffer;
    VDPTile tile(state);
    VDPRenderer renderer(state, tile, framebuffer);

    state.regs_[0x00] = 0x04; // full-colour palette
    state.regs_[0x01] = 0x40;
    state.regs_[0x02] = 0x30; // Plane A at $C000
    state.regs_[0x04] = 0x07;
    state.regs_[0x05] = 0x70; // SAT at $E000
    state.regs_[0x0C] = 0x01;
    state.regs_[0x0D] = 0x3E; // h-scroll table at $F800 (zero: no scroll)
    state.regs_[0x10] = 0x01; // 64x32
    setupSolidTiles(state);
    // Plane A: cell 0 of every row is green, cell 63 red, the rest blue.
    for (int row = 0; row < 32; ++row)
        for (int cell = 0; cell < 64; ++cell)
            state.vram_[0xC000 + (row * 64 + cell) * 2 + 1] = cell == 0 ? 2 : (cell == 63 ? 3 : 1);

    renderer.renderScanline(100);
    assert(isPixel(framebuffer, 0, 100, 0, 7, 0));
    assert(isPixel(framebuffer, 8, 100, 7, 0, 0));

    state.widescreen_ = true;
    framebuffer.clear();
    renderer.renderScanline(100);
    const int row = 100 - 8;
    // Plane pixels -32..-1 are the plane's last four cells (it wraps), then pixel 0 at output column 32.
    assert(isPixel(framebuffer, 23, row, 7, 0, 0));
    assert(isPixel(framebuffer, 24, row, 0, 0, 7));
    assert(isPixel(framebuffer, 31, row, 0, 0, 7));
    assert(isPixel(framebuffer, 32, row, 0, 7, 0));
    assert(isPixel(framebuffer, 39, row, 0, 7, 0));
    assert(isPixel(framebuffer, 40, row, 7, 0, 0));

    // A sprite at X 128 covers the same output columns as plane pixels 0-7.
    setSprite(state, 0, 128, 128 + 100, 0, 3);
    framebuffer.clear();
    renderer.renderScanline(100);
    assert(isPixel(framebuffer, 23, row, 7, 0, 0));
    assert(isPixel(framebuffer, 32, row, 0, 0, 7));
    assert(isPixel(framebuffer, 39, row, 0, 0, 7));
    assert(isPixel(framebuffer, 40, row, 7, 0, 0));
}

void testWidescreenWindowIsCentered() {
    VDPState state;
    Framebuffer framebuffer;
    VDPTile tile(state);
    VDPRenderer renderer(state, tile, framebuffer);

    state.widescreen_ = true;
    state.regs_[0x00] = 0x04; // full-colour palette
    state.regs_[0x01] = 0x40;
    state.regs_[0x02] = 0x30; // Plane A at $C000
    state.regs_[0x03] = 0x34; // window at $D000
    state.regs_[0x04] = 0x07;
    state.regs_[0x0C] = 0x01;
    state.regs_[0x10] = 0x01; // 64x32
    state.regs_[0x12] = 0x1F; // window covers the whole height
    setupSolidTiles(state);
    for (int cell = 0; cell < 64 * 32; ++cell) {
        state.vram_[0xC000 + cell * 2 + 1] = 2;
        state.vram_[0xD000 + cell * 2 + 1] = 1;
    }

    renderer.renderScanline(100);
    const int row = 100 - 8;
    assert(isPixel(framebuffer, 0, row, 0, 7, 0));
    assert(isPixel(framebuffer, 31, row, 0, 7, 0));
    assert(isPixel(framebuffer, 32, row, 7, 0, 0));
    assert(isPixel(framebuffer, 351, row, 7, 0, 0));
    assert(isPixel(framebuffer, 352, row, 0, 7, 0));
    assert(isPixel(framebuffer, 383, row, 0, 7, 0));

    // Horizontal split: window columns 0-9 of the original picture, i.e. output columns 32-111.
    state.regs_[0x11] = 0x05;
    state.regs_[0x12] = 0x00;
    renderer.renderScanline(100);
    assert(isPixel(framebuffer, 31, row, 0, 7, 0));
    assert(isPixel(framebuffer, 32, row, 7, 0, 0));
    assert(isPixel(framebuffer, 111, row, 7, 0, 0));
    assert(isPixel(framebuffer, 112, row, 0, 7, 0));

    // A transparent (colour 0) window pixel shows plane A in Mode 48 and replaces it in Mode 40.
    state.regs_[0x11] = 0x00;
    state.regs_[0x12] = 0x1F;
    for (int cell = 0; cell < 64 * 32; ++cell)
        state.vram_[0xD000 + cell * 2 + 1] = 0;
    renderer.renderScanline(100);
    assert(isPixel(framebuffer, 100, row, 0, 7, 0));
}

void testWidescreenSprites() {
    VDPState state;
    Framebuffer framebuffer;
    VDPTile tile(state);
    VDPRenderer renderer(state, tile, framebuffer);

    state.regs_[0x00] = 0x04; // full-colour palette
    state.regs_[0x01] = 0x40;
    state.regs_[0x02] = 0x30;
    state.regs_[0x04] = 0x07;
    state.regs_[0x05] = 0x70; // SAT at $E000
    state.regs_[0x0C] = 0x01;
    setupSolidTiles(state);

    const int yRaw = 128 + 40;
    // Sprite X 128 is screen column 0 on hardware and column 32 in Mode 48; X 100 and X 472 only exist in Mode 48.
    setSprite(state, 0, 128, yRaw, 1, 3);
    setSprite(state, 1, 100, yRaw, 2, 3);
    setSprite(state, 2, 472, yRaw, 0, 3);

    renderer.renderScanline(44);
    assert(isPixel(framebuffer, 0, 44, 0, 0, 7));
    assert(isPixel(framebuffer, 7, 44, 0, 0, 7));
    assert(isPixel(framebuffer, 8, 44, 0, 0, 0));

    state.widescreen_ = true;
    framebuffer.clear();
    renderer.renderScanline(44);
    const int row = 44 - 8;
    assert(isPixel(framebuffer, 31, row, 0, 0, 0));
    assert(isPixel(framebuffer, 32, row, 0, 0, 7));
    assert(isPixel(framebuffer, 39, row, 0, 0, 7));
    assert(isPixel(framebuffer, 40, row, 0, 0, 0));
    assert(isPixel(framebuffer, 3, row, 0, 0, 0));
    assert(isPixel(framebuffer, 4, row, 0, 0, 7));
    assert(isPixel(framebuffer, 11, row, 0, 0, 7));
    assert(isPixel(framebuffer, 12, row, 0, 0, 0));
    assert(isPixel(framebuffer, 375, row, 0, 0, 0));
    assert(isPixel(framebuffer, 376, row, 0, 0, 7));
    assert(isPixel(framebuffer, 383, row, 0, 0, 7));
}

void testPresentationRect() {
    // 320x224 in 1920x1080: 4x (1280x896), centred, instead of filling the height.
    SDL_FRect whole = VDP::presentationRect(1920, 1080, 320, 224, true);
    assert(whole.w == 1280.0f && whole.h == 896.0f);
    assert(whole.x == 320.0f && whole.y == 92.0f);

    SDL_FRect fit = VDP::presentationRect(1920, 1080, 320, 224, false);
    assert(fit.h == 1080.0f);

    // Mode 48 (384x216) in 1920x1080 is an exact 5x either way.
    SDL_FRect wide = VDP::presentationRect(1920, 1080, 384, 216, true);
    assert(wide.x == 0.0f && wide.y == 0.0f && wide.w == 1920.0f && wide.h == 1080.0f);

    // Smaller than 1x: still fitted.
    SDL_FRect small = VDP::presentationRect(160, 112, 320, 224, true);
    assert(small.w == 160.0f && small.h == 112.0f);
}

} // namespace

int main() {
    testPresentationRect();
    testPlanesAndWindowAgainstReference();
    testSpriteLineAgainstReference();
    testModeDimensions();
    testWidescreenPlanes();
    testWidescreenPlanesAreCentred();
    testWidescreenWindowIsCentered();
    testWidescreenSprites();
}
