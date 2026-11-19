#pragma once

#include "VDPState.hpp"
#include <array>
#include <cstdint>

/**
 * @file VDPMotion.hpp
 * @brief Where each output pixel came from, and how far it moved since an earlier frame (host-only).
 *
 * Frame generation (MetalFX frame interpolation on macOS) needs a motion vector per pixel. The VDP knows them
 * exactly for its planes (the scroll registers) and nearly for its sprites (their SAT entries), so the renderer
 * records, while compositing, which layer won each pixel, each line's scroll values and the sprites it drew;
 * computeMotion() then compares two such records.
 */
struct VDPFrameLayers {
    /// Source codes: a sprite is SpriteBase + its SAT index.
    enum : std::uint8_t {
        Backdrop   = 0,
        PlaneB     = 1,
        PlaneA     = 2,
        Window     = 3,
        Overlay    = 4, ///< changed by VDP::setFrameOverlay's host drawing
        SpriteBase = 8,
    };

    struct Line {
        std::int16_t                                    hscrollA = 0;
        std::int16_t                                    hscrollB = 0;
        bool                                            twoCellVscroll = false;
        std::array<std::int16_t, VDPState::VSRAM_ENTRIES> vsram{};
    };

    struct Sprite {
        bool          drawn = false;
        std::int16_t  x = 0, y = 0; ///< top-left in output pixels
        std::uint8_t  w = 0, h = 0; ///< in pixels
        std::uint16_t tile = 0;     ///< pattern index
        std::uint8_t  palette = 0;
        bool          hflip = false, vflip = false;
    };

    int width = 0;  ///< output picture, in pixels
    int height = 0;
    int planeWidth = 0; ///< scrolling planes, in pixels (they wrap)
    int planeHeight = 0;
    int centerOffsetX = 0; ///< Mode 48's extension of the picture on the left

    std::array<std::uint8_t, VDPState::MAX_SCREEN_W * VDPState::MAX_SCREEN_H> source{};
    std::array<Line, VDPState::MAX_SCREEN_H>                                  lines{};
    std::array<Sprite, VDPState::SAT_MAX_SPRITES>                              sprites{};

    /// Starts a new frame of @p w×@p h: no sprite drawn yet. Lines and sources are overwritten as drawn.
    void begin(int w, int h, int planeW, int planeH, int offsetX);
};

/// For each pixel of @p current, where it was in @p previous minus where it is now, in output pixels, as
/// MetalFX's motion texture wants it: @p motionXY gets current.width × current.height (x, y) pairs, row by row.
/// Planes move by their scroll difference on the pixel's line (and 2-cell column); a sprite moves to the nearest
/// sprite of the previous frame with the same pattern, size, palette and flips, or to the one at its own SAT index
/// with the same size, within 32 pixels; anything else (backdrop, window, host overlay, an unmatched sprite) is
/// still. A frame of another size has no motion.
void computeMotion(const VDPFrameLayers &previous, const VDPFrameLayers &current, float *motionXY);

/// Pose holds, for a frame generated halfway between @p previous and @p current that should not blend a sprite's
/// two poses: @p holds gets current.width × current.height (dx, dy, hold, 0) quads. Where hold is 1 the frame
/// shows @p previous at (x + dx, y + dy): a sprite whose pattern changed (a new pose), its old pixels moved half
/// way to the new position, over the pixels where either pose would be halfway; a sprite that appears or goes
/// stays as in @p previous. Sprites with the same pattern are left to the interpolation. False when nothing holds.
bool computePoseHolds(const VDPFrameLayers &previous, const VDPFrameLayers &current, float *holds);
