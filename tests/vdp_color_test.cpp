#include "system/graphics/VDPState.hpp"
#include "system/graphics/VDPTile.hpp"

#include <cassert>

namespace {

void expectColor(const VDPTile &tile,
                 m_byte palette,
                 m_byte colorIndex,
                 m_byte expectedR,
                 m_byte expectedG,
                 m_byte expectedB) {
    m_byte r = 0;
    m_byte g = 0;
    m_byte b = 0;
    tile.cramToRGB(palette, colorIndex, r, g, b);
    assert(r == expectedR);
    assert(g == expectedG);
    assert(b == expectedB);
}

} // namespace

int main() {
    VDPState state;
    VDPTile tile{state};

    state.cram_[1] = 0x0EEE;
    state.cram_[2] = 0x0008;
    state.cram_[3] = 0x0002;

    // Register $00 bit 2 exposes all three CRAM bits per color component.
    state.regs_[0x00] = 0x04;
    assert(state.fullColorPaletteEnabled());
    expectColor(tile, 0, 1, 7, 7, 7);
    expectColor(tile, 0, 2, 4, 0, 0);
    expectColor(tile, 0, 3, 1, 0, 0);

    // With palette select clear, the VDP outputs only each component's LSB.
    state.regs_[0x00] = 0x00;
    assert(!state.fullColorPaletteEnabled());
    expectColor(tile, 0, 1, 1, 1, 1);
    expectColor(tile, 0, 2, 0, 0, 0);
    expectColor(tile, 0, 3, 1, 0, 0);

    m_byte r = 0;
    m_byte g = 0;
    m_byte b = 0;
    tile.cramToRGB_FullRange(0, 1, r, g, b);
    assert(r == 36 && g == 36 && b == 36);
}
