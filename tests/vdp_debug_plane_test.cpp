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

constexpr int PlaneABase   = 0xC000;
constexpr int HScrollBase  = 0xF400;
constexpr int PlaneWidth   = 512;
constexpr int PlaneHeight  = 256;
constexpr Rgb OutlineColor = {255, 0, 255};

void writeWord(VDPState &state, int address, m_word value) {
    state.vram_[address & 0xFFFF]       = static_cast<m_byte>(value >> 8);
    state.vram_[(address + 1) & 0xFFFF] = static_cast<m_byte>(value);
}

void configure(VDPState &state) {
    state.regs_[0x00] = 0x04;
    state.regs_[0x01] = 0x44; // display on, 224 lines
    state.regs_[0x02] = 0x30; // Plane A $C000
    state.regs_[0x04] = 0x07; // Plane B $E000
    state.regs_[0x0B] = 0x00; // full-screen scroll
    state.regs_[0x0C] = 0x81; // H40
    state.regs_[0x0D] = 0x3D; // h-scroll table $F400
    state.regs_[0x10] = 0x01; // 64 x 32 cells

    // Tile 1 is solid colour 1; palette 0 colour 1 is red.
    for (int i = 0; i < 32; ++i)
        state.vram_[32 + i] = 0x11;
    state.cram_[1] = 0x000E;
}

// Pixel at plane coordinates, skipping the info line above the plane.
Rgb planePixel(Image &image, int x, int y) {
    const ImageSize size   = image.getSize();
    const int       header = size.height - PlaneHeight;
    const auto     *buffer = static_cast<const m_byte *>(image.getRawBuffer());
    const int       index  = ((y + header) * size.width + x) * 3;
    return {buffer[index], buffer[index + 1], buffer[index + 2]};
}

bool isOutline(Image &image, int x, int y) {
    const Rgb pixel = planePixel(image, x, y);
    return pixel.r == OutlineColor.r && pixel.g == OutlineColor.g && pixel.b == OutlineColor.b;
}

void testWholePlaneIsRendered() {
    VDPState    state;
    Framebuffer framebuffer;
    VDPTile     tile(state);
    configure(state);

    // Cell (50, 30) lies outside the screen-sized corner the old view showed.
    writeWord(state, PlaneABase + (30 * 64 + 50) * 2, 0x0001);

    VDPRendererDebug debug(state, tile, framebuffer);
    Image            image = debug.makeForegroundLayerImage(true);
    const ImageSize  size  = image.getSize();
    assert(size.width == PlaneWidth);
    assert(size.height > PlaneHeight);

    const Rgb tilePixel = planePixel(image, 50 * 8 + 3, 30 * 8 + 3);
    assert(tilePixel.r == 255 && tilePixel.g == 0 && tilePixel.b == 0);
}

void testFullScreenScrollOutline() {
    VDPState    state;
    Framebuffer framebuffer;
    VDPTile     tile(state);
    configure(state);

    // Plane A scrolled left by 256 px and up by 16 px.
    writeWord(state, HScrollBase, static_cast<m_word>(-256));
    state.vsram_[0] = 16;

    VDPRendererDebug debug(state, tile, framebuffer);
    Image            image = debug.makeForegroundLayerImage(true);

    assert(isOutline(image, 256, 16));             // screen (0, 0)
    assert(isOutline(image, (256 + 319) & 511, 16)); // screen (319, 0), wrapped
    assert(isOutline(image, 256, 16 + 223));       // screen (0, 223)
    assert(isOutline(image, 300, 16));             // top edge
    assert(isOutline(image, 256, 100));            // left edge
    assert(!isOutline(image, 300, 100));           // inside the view
    assert(!isOutline(image, 100, 100));           // outside the view
}

void testPerLineScrollOutline() {
    VDPState    state;
    Framebuffer framebuffer;
    VDPTile     tile(state);
    configure(state);
    state.regs_[0x0B] = 0x03; // per-line h-scroll

    writeWord(state, HScrollBase + 100 * 4, static_cast<m_word>(-40));

    VDPRendererDebug debug(state, tile, framebuffer);
    Image            image = debug.makeForegroundLayerImage(true);

    assert(isOutline(image, 0, 99));   // unscrolled line
    assert(isOutline(image, 40, 100)); // line 100 starts 40 px into the plane
    assert(!isOutline(image, 0, 100));
}

} // namespace

int main() {
    testWholePlaneIsRendered();
    testFullScreenScrollOutline();
    testPerLineScrollOutline();
    std::puts("vdp_debug_plane_test passed");
    return 0;
}
