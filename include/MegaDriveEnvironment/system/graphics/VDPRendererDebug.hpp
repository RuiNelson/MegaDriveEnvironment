#pragma once

#include "Framebuffer.hpp"
#include "VDPState.hpp"
#include "VDPTile.hpp"
#include "data_types.hpp"
#include "util/image/Image.hpp"
#include <string>

/**
 * @file VDPRendererDebug.hpp
 * @brief Debug PNG export functions for VDPRenderer.
 */

class VDPRendererDebug {
    public:
    /// Initializes debug renderer with references to VDP state, tile decoder, and framebuffer.
    explicit VDPRendererDebug(VDPState &state, VDPTile &tile, Framebuffer &fb);

    // ── Individual debug images ────────────────────────────────────────────

    /// Creates image from current framebuffer pixels.
    Image makeFinalOutputImage(bool fullRange) const;

    /// Creates table showing all 64 CRAM palette entries (4 palettes × 16 colors).
    Image makePalletesTable(bool fullRange) const;

    /// Creates table showing all 40 VSRAM entries.
    Image makeVSRAMImage() const;

    /// Creates image showing only the window layer.
    Image makeWindowLayerImage(bool fullRange) const;

    /// Creates image showing sprite layer (SAT).
    Image makeSpriteTablesImage(bool fullRange) const;

    /// Creates a screen-sized image containing only the visible sprite plane.
    Image makeSpriteLayerImage(bool fullRange) const;

    /// Creates image of the whole plane B name table, with the visible screen area outlined.
    Image makeBackgroundLayerImage(bool fullRange) const;

    /// Creates image of the whole plane A name table, with the visible screen area outlined.
    Image makeForegroundLayerImage(bool fullRange) const;

    /// Creates grid showing all VRAM tiles (16×8 pixel each).
    Image makeVramTilesImage(bool fullRange) const;

    /// Creates table showing all 24 VDP registers.
    Image makeRegistersImage() const;

    // ── PNG export ─────────────────────────────────────────────────────────

    /// Exports current framebuffer to PNG.
    void dumpFrameBufferToPNG(const std::string &path, bool fullRange) const;

    /// Exports all debug images tiled in a single PNG.
    void dumpEverythingToPNG(const std::string &path, bool fullRange) const;

    private:
    VDPState    &state_;
    VDPTile     &tile_;
    Framebuffer &fb_;

    // ── Helpers ─────────────────────────────────────────────────────────────

    /// Converts 3-bit BGR to full 8-bit RGB using lookup table.
    static uint8_t expand3to8(uint8_t val);

    /// Creates a colored pixel at coordinates.
    void setImagePixel(Image &img, int x, int y, m_byte b, m_byte g, m_byte r) const;

    /// Renders a single palette row (16 colors) as a horizontal strip.
    Image renderPaletteRow(int paletteIdx, bool fullRange) const;

    /// Horizontal scroll the renderer uses for a plane (0 = A, 1 = B) on a screen line.
    int planeHScroll(int scrollIndex, int line) const;

    /// Vertical scroll the renderer uses for a plane (0 = A, 1 = B) at a screen column.
    int planeVScroll(int scrollIndex, int screenX) const;

    /// Renders a whole plane name table (Plane A or Plane B) and outlines the screen area it shows.
    Image renderPlaneLayer(int planeBase, int scrollIndex, bool fullRange) const;

    /// Renders the sprite layer to an image.
    Image renderSpriteLayer(bool fullRange) const;

    /// Renders the linked SAT sprites at their screen positions.
    Image renderSpritePlane(bool fullRange) const;

    /// Renders the window layer to an image.
    Image renderWindowLayer(bool fullRange) const;
};
