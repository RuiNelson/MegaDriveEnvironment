/**
 * @file VDPMotion.cpp
 * @brief Motion vectors from two VDP layer records (see VDPMotion.hpp).
 */

#include "VDPMotion.hpp"

#include <algorithm>
#include <cmath>
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

/// Where a sprite of the current frame was in the previous one.
struct SpriteMatch {
    enum Kind { None, SamePattern, SameSlot } kind = None; ///< SameSlot: a new pattern, so a new pose
    int previous = -1;                                     ///< its SAT index in the previous frame
    int dx = 0, dy = 0;                                    ///< previous position minus current
};

/// The nearest sprite of the previous frame with the same pattern, size, palette and flips within reach;
/// else the one in the same SAT slot with the same size, within reach (a new animation frame has a new pattern,
/// and the same slot and size is most likely the same piece); else none.
SpriteMatch matchSprite(const VDPFrameLayers &previous, const VDPFrameLayers &current, int index) {
    const VDPFrameLayers::Sprite &now  = current.sprites[static_cast<std::size_t>(index)];
    SpriteMatch                   match;
    int                           best = std::numeric_limits<int>::max();
    for (int candidate = 0; candidate < VDPState::SAT_MAX_SPRITES; ++candidate) {
        const VDPFrameLayers::Sprite &then = previous.sprites[static_cast<std::size_t>(candidate)];
        if (!then.drawn || then.tile != now.tile || then.w != now.w || then.h != now.h || then.palette != now.palette ||
            then.hflip != now.hflip || then.vflip != now.vflip)
            continue;
        const int distance = std::abs(then.x - now.x) + std::abs(then.y - now.y);
        if (distance < best && distance <= kSpriteReach * 2) {
            best  = distance;
            match = {SpriteMatch::SamePattern, candidate, then.x - now.x, then.y - now.y};
        }
    }
    if (match.kind != SpriteMatch::None)
        return match;
    const VDPFrameLayers::Sprite &slot = previous.sprites[static_cast<std::size_t>(index)];
    if (slot.drawn && slot.w == now.w && slot.h == now.h && std::abs(slot.x - now.x) <= kSpriteReach &&
        std::abs(slot.y - now.y) <= kSpriteReach)
        return {SpriteMatch::SameSlot, index, slot.x - now.x, slot.y - now.y};
    return match;
}

bool sameFrame(const VDPFrameLayers &previous, const VDPFrameLayers &current) {
    return previous.width == current.width && previous.height == current.height &&
           previous.planeWidth == current.planeWidth && previous.planeHeight == current.planeHeight;
}

} // namespace

void computeMotion(const VDPFrameLayers &previous, const VDPFrameLayers &current, float *motionXY) {
    const int  width     = current.width;
    const int  height    = current.height;
    std::fill_n(motionXY, static_cast<std::size_t>(width) * height * 2, 0.0f);
    if (!sameFrame(previous, current))
        return;

    // Each sprite once, not once per pixel.
    std::array<int, VDPState::SAT_MAX_SPRITES> spriteDX{};
    std::array<int, VDPState::SAT_MAX_SPRITES> spriteDY{};
    for (int index = 0; index < VDPState::SAT_MAX_SPRITES; ++index) {
        if (current.sprites[static_cast<std::size_t>(index)].drawn) {
            const SpriteMatch match                    = matchSprite(previous, current, index);
            spriteDX[static_cast<std::size_t>(index)] = match.dx;
            spriteDY[static_cast<std::size_t>(index)] = match.dy;
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

bool computePoseHolds(const VDPFrameLayers &previous, const VDPFrameLayers &current, float fraction, float *holds) {
    const int width  = current.width;
    const int height = current.height;
    std::fill_n(holds, static_cast<std::size_t>(width) * height * 4, 0.0f);
    if (!sameFrame(previous, current))
        return false;

    // Which sprites hold, and by how much: @p fraction of the way from the previous position to the current one.
    struct Hold {
        bool  held = false;
        float ox = 0.0f, oy = 0.0f; ///< where the frame between samples the previous one, from the shown pixel
        float nx = 0.0f, ny = 0.0f; ///< from the current position to the shown one (the rest of the way)
    };
    std::array<Hold, VDPState::SAT_MAX_SPRITES> currentHolds{};
    std::array<Hold, VDPState::SAT_MAX_SPRITES> previousHolds{};
    std::array<bool, VDPState::SAT_MAX_SPRITES> previousMatched{};
    for (int index = 0; index < VDPState::SAT_MAX_SPRITES; ++index) {
        if (!current.sprites[static_cast<std::size_t>(index)].drawn)
            continue;
        const SpriteMatch match = matchSprite(previous, current, index);
        if (match.previous >= 0)
            previousMatched[static_cast<std::size_t>(match.previous)] = true;
        if (match.kind == SpriteMatch::SamePattern)
            continue; // the same drawing: interpolated
        const Hold hold{true, match.dx * fraction, match.dy * fraction, match.dx * (1.0f - fraction),
                        match.dy * (1.0f - fraction)};
        currentHolds[static_cast<std::size_t>(index)] = hold;
        if (match.previous >= 0)
            previousHolds[static_cast<std::size_t>(match.previous)] = hold;
    }
    for (int index = 0; index < VDPState::SAT_MAX_SPRITES; ++index) {
        if (previous.sprites[static_cast<std::size_t>(index)].drawn && !previousMatched[static_cast<std::size_t>(index)])
            previousHolds[static_cast<std::size_t>(index)] = {true, 0.0f, 0.0f}; // gone: stays until the real frame
    }

    bool any  = false;
    auto mark = [&](int x, int y, const Hold &hold) {
        if (x < 0 || y < 0 || x >= width || y >= height)
            return;
        float *out = holds + (static_cast<std::size_t>(y) * width + x) * 4;
        if (out[2] != 0.0f)
            return;
        out[0] = hold.ox;
        out[1] = hold.oy;
        out[2] = 1.0f;
        any    = true;
    };
    // The old pose where it is at @p fraction (its previous position minus the offset), then the new pose's
    // pixels where it would be (its current position plus the rest of the way), so neither shows as a blend.
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::uint8_t source = previous.source[static_cast<std::size_t>(y) * VDPState::MAX_SCREEN_W + x];
            if (source < VDPFrameLayers::SpriteBase || source - VDPFrameLayers::SpriteBase >= VDPState::SAT_MAX_SPRITES)
                continue;
            const Hold &hold = previousHolds[static_cast<std::size_t>(source - VDPFrameLayers::SpriteBase)];
            if (hold.held)
                mark(static_cast<int>(std::lround(x - hold.ox)), static_cast<int>(std::lround(y - hold.oy)), hold);
        }
    }
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::uint8_t source = current.source[static_cast<std::size_t>(y) * VDPState::MAX_SCREEN_W + x];
            if (source < VDPFrameLayers::SpriteBase || source - VDPFrameLayers::SpriteBase >= VDPState::SAT_MAX_SPRITES)
                continue;
            const Hold &hold = currentHolds[static_cast<std::size_t>(source - VDPFrameLayers::SpriteBase)];
            if (hold.held)
                mark(static_cast<int>(std::lround(x + hold.nx)), static_cast<int>(std::lround(y + hold.ny)), hold);
        }
    }
    return any;
}
