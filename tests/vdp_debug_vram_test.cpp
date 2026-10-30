#include "system/graphics/Framebuffer.hpp"
#include "system/graphics/VDPRendererDebug.hpp"
#include "system/graphics/VDPState.hpp"
#include "system/graphics/VDPTile.hpp"
#include "util/image/Image.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

struct Rgb {
    m_byte r, g, b;
};

constexpr int PlaneABase = 0xC000;
constexpr int SatBase    = 0x4000;
constexpr int TileSheetX = 32;
constexpr int TileSize   = 8;

void writeWord(VDPState &state, int address, m_word value) {
    state.vram_[address & 0xFFFF]       = static_cast<m_byte>(value >> 8);
    state.vram_[(address + 1) & 0xFFFF] = static_cast<m_byte>(value);
}

void configure(VDPState &state) {
    state.regs_[0x00] = 0x04; // full colour palette
    state.regs_[0x01] = 0x44; // display on, 224 lines
    state.regs_[0x02] = 0x30; // Plane A $C000
    state.regs_[0x04] = 0x07; // Plane B $E000
    state.regs_[0x05] = 0x20; // SAT $4000
    state.regs_[0x0C] = 0x81; // H40
    state.regs_[0x10] = 0x01; // 64 x 32 cells

    // Every test tile is solid colour index 1.
    for (int tile = 1; tile < 64; ++tile)
        for (int i = 0; i < 32; ++i)
            state.vram_[tile * 32 + i] = 0x11;

    // Palette 0 colour 1 is red, palette 1 colour 1 green, palette 2 colour 1
    // blue, palette 3 colour 1 white; CRAM layout is 3-bit BGR (bits 9-11 B).
    state.cram_[1]        = 0x000E;
    state.cram_[16 + 1]   = 0x00E0;
    state.cram_[2 * 16 + 1] = 0x0E00;
    state.cram_[3 * 16 + 1] = 0x0EEE;
}

Rgb tileSheetPixel(Image &image, int tileIndex) {
    const int       x      = (tileIndex % TileSheetX) * TileSize + 3;
    const int       y      = (tileIndex / TileSheetX) * TileSize + 3;
    const ImageSize size   = image.getSize();
    const auto     *buffer = static_cast<const m_byte *>(image.getRawBuffer());
    const int       index  = (y * size.width + x) * 3;
    return {buffer[index], buffer[index + 1], buffer[index + 2]};
}

bool same(Rgb pixel, Rgb expected) {
    return pixel.r == expected.r && pixel.g == expected.g && pixel.b == expected.b;
}

// A tile referenced by a plane entry is drawn with that entry's palette.
void testPlanePaletteColoursTile() {
    VDPState    state;
    Framebuffer framebuffer;
    VDPTile     tile(state);
    configure(state);

    // Cell (3, 2) of plane A shows tile 5 through palette 2.
    writeWord(state, PlaneABase + (2 * 64 + 3) * 2, static_cast<m_word>((2 << 13) | 5));

    VDPRendererDebug debug(state, tile, framebuffer);
    Image            image = debug.makeVramTilesImage(true);

    assert(same(tileSheetPixel(image, 5), {0, 0, 255}));
}

// A tile nothing references falls back to palette 0.
void testUnusedTileUsesPaletteZero() {
    VDPState    state;
    Framebuffer framebuffer;
    VDPTile     tile(state);
    configure(state);

    VDPRendererDebug debug(state, tile, framebuffer);
    Image            image = debug.makeVramTilesImage(true);

    assert(same(tileSheetPixel(image, 7), {255, 0, 0}));
}

// When one tile is used through more than one palette, the most used wins.
void testMostUsedPaletteWins() {
    VDPState    state;
    Framebuffer framebuffer;
    VDPTile     tile(state);
    configure(state);

    writeWord(state, PlaneABase + 0 * 2, static_cast<m_word>((3 << 13) | 9));
    writeWord(state, PlaneABase + 1 * 2, static_cast<m_word>((3 << 13) | 9));
    writeWord(state, PlaneABase + 2 * 2, static_cast<m_word>((1 << 13) | 9));

    VDPRendererDebug debug(state, tile, framebuffer);
    Image            image = debug.makeVramTilesImage(true);

    assert(same(tileSheetPixel(image, 9), {255, 255, 255}));
}

// A sprite-only tile is coloured with the sprite entry's palette.
void testSpritePaletteColoursTile() {
    VDPState    state;
    Framebuffer framebuffer;
    VDPTile     tile(state);
    configure(state);

    // Sprite 0: tile $0A through palette 1, linked list ends there.
    writeWord(state, SatBase + 4, static_cast<m_word>((1 << 13) | 0x0A));
    state.sat_[2] = 0x00; // 1 x 1 tiles
    state.sat_[3] = 0x00; // link: end of list

    VDPRendererDebug debug(state, tile, framebuffer);
    Image            image = debug.makeVramTilesImage(true);

    assert(same(tileSheetPixel(image, 0x0A), {0, 255, 0}));
}

// Without full range the 3-bit components are expanded to 8 bits.
void testReducedRangeExpandsToFullBytes() {
    VDPState    state;
    Framebuffer framebuffer;
    VDPTile     tile(state);
    configure(state);
    state.cram_[1] = 0x0008; // red component 4 of 7

    VDPRendererDebug debug(state, tile, framebuffer);
    Image            image = debug.makeVramTilesImage(false);

    assert(same(tileSheetPixel(image, 7), {146, 0, 0}));
}

} // namespace

int main() {
    testPlanePaletteColoursTile();
    testUnusedTileUsesPaletteZero();
    testMostUsedPaletteWins();
    testSpritePaletteColoursTile();
    testReducedRangeExpandsToFullBytes();
    std::puts("vdp_debug_vram_test passed");
    return 0;
}
