/**
 * @file VDPMotion.cpp
 * @brief Motion vectors from two VDP layer records (see VDPMotion.hpp).
 */

#include "VDPMotion.hpp"

#include <algorithm>
#include <cstdlib>
#include <limits>

void VDPFrameLayers::begin(int w, int h, int planeW, int planeH, int offsetX) {
    width         = w;
    height        = h;
    planeWidth    = planeW;
    planeHeight   = planeH;
    centerOffsetX = offsetX;
    for (Sprite &sprite : sprites)
        sprite.drawn = false;
}

namespace {

/// The shortest signed distance between two scroll positions on a plane that wraps every @p size pixels.
int wrapped(int delta, int size) {
    if (size <= 0)
        return delta;
    delta %= size;
    if (delta > size / 2)
        delta -= size;
    else if (delta < -size / 2)
        delta += size;
    return delta;
}

int vscrollAt(const VDPFrameLayers::Line &line, int x, int offsetX, int layer) {
    // As VDPRenderer::buildPlaneLine: 2-cell columns from the hardware picture's left edge.
    int index = layer;
    if (line.twoCellVscroll) {
        index = (std::max(0, x - offsetX) >> 4) * 2 + layer;
        if (index >= VDPState::VSRAM_ENTRIES)
            index = layer;
    }
    return line.vsram[static_cast<std::size_t>(index)];
}

constexpr int kSpriteReach = 32;

/// The previous frame's position of current sprite @p index, or false when it has no match.
bool matchSprite(const VDPFrameLayers &previous, const VDPFrameLayers &current, int index, int &dx, int &dy) {
    const VDPFrameLayers::Sprite &now  = current.sprites[static_cast<std::size_t>(index)];
    int                           best = std::numeric_limits<int>::max();
    for (const VDPFrameLayers::Sprite &then : previous.sprites) {
        if (!then.drawn || then.tile != now.tile || then.w != now.w || then.h != now.h || then.palette != now.palette ||
            then.hflip != now.hflip || then.vflip != now.vflip)
            continue;
        const int distance = std::abs(then.x - now.x) + std::abs(then.y - now.y);
        if (distance < best) {
            best = distance;
            dx   = then.x - now.x;
            dy   = then.y - now.y;
        }
    }
    if (best <= kSpriteReach * 2)
        return true;
    // A new animation frame has a new pattern: the same SAT slot and size is most likely the same piece.
    const VDPFrameLayers::Sprite &slot = previous.sprites[static_cast<std::size_t>(index)];
    if (slot.drawn && slot.w == now.w && slot.h == now.h && std::abs(slot.x - now.x) <= kSpriteReach &&
        std::abs(slot.y - now.y) <= kSpriteReach) {
        dx = slot.x - now.x;
        dy = slot.y - now.y;
        return true;
    }
    return false;
}

} // namespace

void computeMotion(const VDPFrameLayers &previous, const VDPFrameLayers &current, float *motionXY) {
    const int  width     = current.width;
    const int  height    = current.height;
    const bool sameFrame = previous.width == width && previous.height == height &&
                           previous.planeWidth == current.planeWidth && previous.planeHeight == current.planeHeight;
    std::fill_n(motionXY, static_cast<std::size_t>(width) * height * 2, 0.0f);
    if (!sameFrame)
        return;

    // Each sprite once, not once per pixel.
    std::array<int, VDPState::SAT_MAX_SPRITES> spriteDX{};
    std::array<int, VDPState::SAT_MAX_SPRITES> spriteDY{};
    for (int index = 0; index < VDPState::SAT_MAX_SPRITES; ++index) {
        if (current.sprites[static_cast<std::size_t>(index)].drawn &&
            !matchSprite(previous, current, index, spriteDX[static_cast<std::size_t>(index)], spriteDY[static_cast<std::size_t>(index)])) {
            spriteDX[static_cast<std::size_t>(index)] = 0;
            spriteDY[static_cast<std::size_t>(index)] = 0;
        }
    }

    for (int y = 0; y < height; ++y) {
        const VDPFrameLayers::Line &now  = current.lines[static_cast<std::size_t>(y)];
        const VDPFrameLayers::Line &then = previous.lines[static_cast<std::size_t>(y)];
        // Screen X = plane X + hscroll and screen Y = plane Y - vscroll, so a plane pixel was hscroll's change
        // to the side and vscroll's change the other way.
        const int dxA = wrapped(then.hscrollA - now.hscrollA, current.planeWidth);
        const int dxB = wrapped(then.hscrollB - now.hscrollB, current.planeWidth);
        float    *out = motionXY + static_cast<std::size_t>(y) * width * 2;
        for (int x = 0; x < width; ++x) {
            const std::uint8_t source = current.source[static_cast<std::size_t>(y) * VDPState::MAX_SCREEN_W + x];
            int                dx = 0;
            int                dy = 0;
            if (source == VDPFrameLayers::PlaneA || source == VDPFrameLayers::PlaneB) {
                const int layer = source == VDPFrameLayers::PlaneA ? 0 : 1;
                dx              = layer == 0 ? dxA : dxB;
                dy = wrapped(vscrollAt(now, x, current.centerOffsetX, layer) - vscrollAt(then, x, current.centerOffsetX, layer),
                             current.planeHeight);
            } else if (source >= VDPFrameLayers::SpriteBase) {
                const int index = source - VDPFrameLayers::SpriteBase;
                if (index < VDPState::SAT_MAX_SPRITES) {
                    dx = spriteDX[static_cast<std::size_t>(index)];
                    dy = spriteDY[static_cast<std::size_t>(index)];
                }
            }
            out[x * 2]     = static_cast<float>(dx);
            out[x * 2 + 1] = static_cast<float>(dy);
        }
    }
}
