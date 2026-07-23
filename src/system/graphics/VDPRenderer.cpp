/**
 * @file VDPRenderer.cpp
 * @brief VDP scanline renderer implementation.
 */

#include "VDPRenderer.hpp"
#include <algorithm>
#include <cstring>

namespace {

struct BGRColor {
    m_byte b;
    m_byte g;
    m_byte r;
};

inline m_byte readTileRowPixel(const m_byte *vram, int rowAddress, int pixelX) {
    const m_byte packed = vram[rowAddress + (pixelX >> 1)];
    return (pixelX & 1) == 0 ? static_cast<m_byte>(packed >> 4)
                             : static_cast<m_byte>(packed & 0x0F);
}

} // namespace

/// Initializes renderer with references to VDP state, tile decoder, and framebuffer.
VDPRenderer::VDPRenderer(VDPState &state, VDPTile &tile, Framebuffer &fb) : state_(state), tile_(tile), fb_(fb) {
}

/// Renders the active visible frame if display is enabled; otherwise clears framebuffer to black.
void VDPRenderer::renderFrame() {
    fb_.clear();

    if (!state_.displayEnabled()) {
        return;
    }

    for (int line = 0; line < state_.activeHeight(); ++line) {
        state_.vCounter_ = static_cast<m_word>(line);
        renderScanline(line);
    }
}

/// Renders a single scanline (Y-coordinate): evaluates each pixel's plane B, plane A/window, and sprite layers, then
/// composites them using priority rules.
void VDPRenderer::renderScanline(int line) {
    VDPState &s = state_;
    if (line < 0 || line >= s.activeHeight())
        return;

    int hsBase = s.hscrollBase();

    // ── Read HScroll values ─────────────────────────────────────────────
    int hscrollA = 0, hscrollB = 0;
    switch (s.hscrollMode()) {
        case 0: {
            int addr = hsBase;
            hscrollA = static_cast<int16_t>((s.vram_[addr] << 8) | s.vram_[addr + 1]);
            hscrollB = static_cast<int16_t>((s.vram_[addr + 2] << 8) | s.vram_[addr + 3]);
            break;
        }
        case 2: {
            int row  = (line / 8) * 8;
            int addr = (hsBase + row * 4) & 0xFFFF;
            hscrollA = static_cast<int16_t>((s.vram_[addr] << 8) | s.vram_[addr + 1]);
            hscrollB = static_cast<int16_t>((s.vram_[addr + 2] << 8) | s.vram_[addr + 3]);
            break;
        }
        case 3: {
            int addr = (hsBase + line * 4) & 0xFFFF;
            hscrollA = static_cast<int16_t>((s.vram_[addr] << 8) | s.vram_[addr + 1]);
            hscrollB = static_cast<int16_t>((s.vram_[addr + 2] << 8) | s.vram_[addr + 3]);
            break;
        }
        default:
            break;
    }

    const bool twoCell  = s.vscrollMode() == 1;
    const int  vscrollA = static_cast<int16_t>(s.vsram_[0]);
    const int  vscrollB = static_cast<int16_t>(s.vsram_[1]);

    const int  planeAAddr       = s.planeABase();
    const int  planeBAddr       = s.planeBBase();
    const int  activeWidth      = s.activeWidth();
    const int  planeWidthCells  = s.planeWidthCells();
    const int  planeHeightCells = s.planeHeightCells();
    const bool interlace2       = s.interlaceMode() == 2;
    const bool interlaced       = s.interlaced();

    // Decode each CRAM entry once per line. The previous pixel path repeated
    // the shifts, masks, palette-mode check, and function call for every pixel.
    BGRColor paletteColors[VDPState::CRAM_ENTRIES];
    const m_word componentMask = s.fullColorPaletteEnabled() ? 0x07 : 0x01;
    for (int index = 0; index < VDPState::CRAM_ENTRIES; ++index) {
        const m_word entry = s.cram_[index];
        paletteColors[index] = {
            static_cast<m_byte>((entry >> 9) & componentMask),
            static_cast<m_byte>((entry >> 5) & componentMask),
            static_cast<m_byte>((entry >> 1) & componentMask),
        };
    }
    const BGRColor background = paletteColors[s.bgColorPalette() * 16 + s.bgColorIndex()];

    // Build both scrolling planes in sequential scanline buffers. Nametable
    // entries and tile rows are cached until a tile boundary (or a two-cell
    // VScroll boundary) is crossed.
    buildPlaneLine(planeBLine_,
                   planeBAddr,
                   hscrollB,
                   vscrollB,
                   1,
                   twoCell,
                   line,
                   activeWidth,
                   planeWidthCells,
                   planeHeightCells,
                   interlace2);
    buildPlaneLine(planeALine_,
                   planeAAddr,
                   hscrollA,
                   vscrollA,
                   0,
                   twoCell,
                   line,
                   activeWidth,
                   planeWidthCells,
                   planeHeightCells,
                   interlace2);

    // Window activation is constant vertically and has at most one horizontal
    // boundary, so calculate its span once rather than testing every pixel.
    const int  screenCellY    = line / 8;
    const int  windowCellX    = s.windowHPos() * 2;
    const int  windowCellY    = s.windowVPos();
    const bool windowVertical = windowCellY > 0
        && (s.windowDown() ? screenCellY >= windowCellY : screenCellY < windowCellY);
    int windowStart = 0;
    int windowEnd   = 0;
    if (windowVertical) {
        windowEnd = activeWidth;
    } else if (windowCellX > 0) {
        const int boundary = std::min(activeWidth, windowCellX * 8);
        if (s.windowRight()) {
            windowStart = boundary;
            windowEnd   = activeWidth;
        } else {
            windowEnd = boundary;
        }
    }
    if (windowStart < windowEnd)
        overlayWindowLine(planeALine_, line, windowStart, windowEnd, interlace2);

    // ── Sprite layer (evaluated once for the whole scanline) ─────────────
    buildSpriteLine(line);

    // ── Per-pixel priority resolution and direct framebuffer write ───────
    const int outputY = interlaced ? line * 2 : line;
    auto *output = static_cast<m_byte *>(fb_.getRawPointer()) + outputY * Framebuffer::PITCH;
    const bool shadowHighlight = s.shadowHighlightEnabled();

    for (int x = 0; x < activeWidth; ++x) {
        const PixelResult &planeB = planeBLine_[x];
        const PixelResult &planeA = planeALine_[x];
        const PixelResult &sprite = spriteLine_[x];
        BGRColor color = background;

        if (shadowHighlight && sprite.opaque && sprite.palette == 3
            && (sprite.colorIndex == 14 || sprite.colorIndex == 15)) {
            const PixelResult *under = nullptr;
            if (planeA.opaque && planeA.priority)
                under = &planeA;
            else if (planeB.opaque && planeB.priority)
                under = &planeB;
            else if (planeA.opaque)
                under = &planeA;
            else if (planeB.opaque)
                under = &planeB;

            if (under != nullptr)
                color = paletteColors[under->palette * 16 + under->colorIndex];

            if (sprite.colorIndex == 14) {
                color.b = static_cast<m_byte>(std::min<int>(7, color.b + 3));
                color.g = static_cast<m_byte>(std::min<int>(7, color.g + 3));
                color.r = static_cast<m_byte>(std::min<int>(7, color.r + 3));
            } else {
                color.b = static_cast<m_byte>(color.b >> 1);
                color.g = static_cast<m_byte>(color.g >> 1);
                color.r = static_cast<m_byte>(color.r >> 1);
            }
        } else {
            const PixelResult *front = nullptr;
            if (sprite.opaque && sprite.priority)
                front = &sprite;
            else if (planeA.opaque && planeA.priority)
                front = &planeA;
            else if (planeB.opaque && planeB.priority)
                front = &planeB;
            else if (sprite.opaque)
                front = &sprite;
            else if (planeA.opaque)
                front = &planeA;
            else if (planeB.opaque)
                front = &planeB;

            if (front != nullptr)
                color = paletteColors[front->palette * 16 + front->colorIndex];
        }

        output[x * Framebuffer::BPP + 0] = color.b;
        output[x * Framebuffer::BPP + 1] = color.g;
        output[x * Framebuffer::BPP + 2] = color.r;
    }

    if (interlaced)
        std::memcpy(output + Framebuffer::PITCH, output, static_cast<std::size_t>(activeWidth * Framebuffer::BPP));
}

// ── Layer evaluation ────────────────────────────────────────────────────────

void VDPRenderer::buildPlaneLine(PixelResult *destination,
                                 int          planeBase,
                                 int          hscroll,
                                 int          baseVscroll,
                                 int          vsramOffset,
                                 bool         twoCellVscroll,
                                 int          line,
                                 int          activeWidth,
                                 int          planeWidthCells,
                                 int          planeHeightCells,
                                 bool         interlace2) const {
    const VDPState &s = state_;
    const int planeXMask = planeWidthCells * 8 - 1;
    const int planeYMask = planeHeightCells * 8 - 1;
    const int tileShift  = interlace2 ? 4 : 3;
    int planeX = (-hscroll) & planeXMask;

    int    cachedEntryAddress = -1;
    int    cachedPixelY       = -1;
    m_word cachedEntry        = 0;
    int    cachedRowAddress   = 0;

    for (int x = 0; x < activeWidth; ++x) {
        int vscroll = baseVscroll;
        if (twoCellVscroll) {
            const int index = (x >> 4) * 2 + vsramOffset;
            if (index < VDPState::VSRAM_ENTRIES)
                vscroll = static_cast<int16_t>(s.vsram_[index]);
        }

        const int planeY  = (line + vscroll) & planeYMask;
        const int cellX   = planeX >> 3;
        const int cellY   = planeY >> tileShift;
        const int pixelX  = planeX & 7;
        const int pixelY  = planeY & (interlace2 ? 15 : 7);
        const int address = (planeBase + (cellY * planeWidthCells + cellX) * 2) & 0xFFFF;

        if (address != cachedEntryAddress) {
            cachedEntry = static_cast<m_word>((s.vram_[address] << 8) | s.vram_[(address + 1) & 0xFFFF]);
            cachedEntryAddress = address;
            cachedPixelY = -1;
        }
        if (pixelY != cachedPixelY) {
            int tileIndex = cachedEntry & 0x07FF;
            int tilePixelY = pixelY;
            if (interlace2) {
                tileIndex = ((cachedEntry & 0x03FF) << 1) | (tilePixelY >> 3);
                tilePixelY &= 7;
            }
            if ((cachedEntry & 0x1000) != 0)
                tilePixelY = 7 - tilePixelY;
            cachedRowAddress = tileIndex * 32 + tilePixelY * 4;
            cachedPixelY = pixelY;
        }

        const int tilePixelX = (cachedEntry & 0x0800) != 0 ? 7 - pixelX : pixelX;
        const m_byte colorIndex = readTileRowPixel(s.vram_, cachedRowAddress, tilePixelX);
        destination[x] = {
            colorIndex,
            static_cast<m_byte>((cachedEntry >> 13) & 0x03),
            (cachedEntry & 0x8000) != 0,
            colorIndex != 0,
        };
        planeX = (planeX + 1) & planeXMask;
    }
}

void VDPRenderer::overlayWindowLine(
    PixelResult *destination, int line, int xStart, int xEnd, bool interlace2) const {
    const VDPState &s = state_;
    const int windowWidth = s.h40Mode() ? 64 : 32;
    const int tileShift = interlace2 ? 4 : 3;
    const int pixelY = line & (interlace2 ? 15 : 7);
    const int cellY = line >> tileShift;
    const int windowBase = s.windowBase();

    int    cachedCellX     = -1;
    m_word cachedEntry     = 0;
    int    cachedRowAddress = 0;

    for (int x = xStart; x < xEnd; ++x) {
        const int cellX = x >> 3;
        if (cellX != cachedCellX) {
            const int address = (windowBase + (cellY * windowWidth + cellX) * 2) & 0xFFFF;
            cachedEntry = static_cast<m_word>((s.vram_[address] << 8) | s.vram_[(address + 1) & 0xFFFF]);

            int tileIndex = cachedEntry & 0x07FF;
            int tilePixelY = pixelY;
            if (interlace2) {
                tileIndex = ((cachedEntry & 0x03FF) << 1) | (tilePixelY >> 3);
                tilePixelY &= 7;
            }
            if ((cachedEntry & 0x1000) != 0)
                tilePixelY = 7 - tilePixelY;
            cachedRowAddress = tileIndex * 32 + tilePixelY * 4;
            cachedCellX = cellX;
        }

        const int pixelX = x & 7;
        const int tilePixelX = (cachedEntry & 0x0800) != 0 ? 7 - pixelX : pixelX;
        const m_byte colorIndex = readTileRowPixel(s.vram_, cachedRowAddress, tilePixelX);
        destination[x] = {
            colorIndex,
            static_cast<m_byte>((cachedEntry >> 13) & 0x03),
            (cachedEntry & 0x8000) != 0,
            colorIndex != 0,
        };
    }
}

/// Checks if window plane is active at cell coordinates (cellX, cellY) based on window position and direction settings.
bool VDPRenderer::isWindowActive(int cellX, int cellY) const {
    const VDPState &s    = state_;
    int             hpos = s.windowHPos() * 2;
    int             vpos = s.windowVPos();

    if (vpos > 0) {
        if (s.windowDown()) {
            if (cellY >= vpos)
                return true;
        } else {
            if (cellY < vpos)
                return true;
        }
    }

    if (hpos > 0) {
        if (s.windowRight()) {
            return cellX >= hpos;
        } else {
            return cellX < hpos;
        }
    }

    return false;
}

/// Evaluates plane pixel at screen coordinates (screenX, screenY) with given scroll offsets.
/// Applies wrapping based on plane dimensions. Decodes nametable entry and looks up tile pixel.
VDPRenderer::PixelResult
VDPRenderer::getPlanePixel(int planeBase, int hscroll, int vscroll, int screenX, int screenY) const {
    const VDPState &s  = state_;
    int             pw = s.planeWidthCells();
    int             ph = s.planeHeightCells();

    int planeX = (screenX - hscroll) % (pw * 8);
    if (planeX < 0)
        planeX += pw * 8;

    int planeY = (screenY + vscroll) % (ph * 8);
    if (planeY < 0)
        planeY += ph * 8;

    int cellX        = planeX / 8;
    int cellY        = planeY / (s.interlaceMode() == 2 ? 16 : 8);
    int pixelInTileX = planeX % 8;
    int pixelInTileY = planeY % (s.interlaceMode() == 2 ? 16 : 8);

    int    entryAddr = (planeBase + (cellY * pw + cellX) * 2) & 0xFFFF;
    m_word entry     = static_cast<m_word>((s.vram_[entryAddr] << 8) | s.vram_[entryAddr + 1]);

    bool   priority  = (entry & 0x8000) != 0;
    m_byte palette   = static_cast<m_byte>((entry >> 13) & 0x03);
    bool   vflip     = (entry & 0x1000) != 0;
    bool   hflip     = (entry & 0x0800) != 0;
    int tileIndex = entry & 0x07FF;
    if (s.interlaceMode() == 2) {
        tileIndex = ((entry & 0x03FF) << 1) | (pixelInTileY >> 3);
        pixelInTileY &= 0x07;
    }

    m_byte colorIdx = tile_.getTilePixel(tileIndex * 32, pixelInTileX, pixelInTileY, hflip, vflip);

    return {colorIdx, palette, priority, colorIdx != 0};
}

/// Evaluates window plane pixel at screen coordinates (screenX, screenY). Window nametable is 64×32 in H40 mode.
VDPRenderer::PixelResult VDPRenderer::getWindowPixel(int screenX, int screenY) const {
    const VDPState &s      = state_;
    int             wBase  = s.windowBase();
    int             wWidth = s.h40Mode() ? 64 : 32;

    int cellX        = screenX / 8;
    int cellY        = screenY / (s.interlaceMode() == 2 ? 16 : 8);
    int pixelInTileX = screenX % 8;
    int pixelInTileY = screenY % (s.interlaceMode() == 2 ? 16 : 8);

    int    entryAddr = (wBase + (cellY * wWidth + cellX) * 2) & 0xFFFF;
    m_word entry     = static_cast<m_word>((s.vram_[entryAddr] << 8) | s.vram_[entryAddr + 1]);

    bool   priority  = (entry & 0x8000) != 0;
    m_byte palette   = static_cast<m_byte>((entry >> 13) & 0x03);
    bool   vflip     = (entry & 0x1000) != 0;
    bool   hflip     = (entry & 0x0800) != 0;
    int tileIndex = entry & 0x07FF;
    if (s.interlaceMode() == 2) {
        tileIndex = ((entry & 0x03FF) << 1) | (pixelInTileY >> 3);
        pixelInTileY &= 0x07;
    }

    m_byte colorIdx = tile_.getTilePixel(tileIndex * 32, pixelInTileX, pixelInTileY, hflip, vflip);

    return {colorIdx, palette, priority, colorIdx != 0};
}

/// Evaluates topmost non-transparent sprite pixel at screen coordinates (screenX, screenY) by scanning SAT.
/// Sprite pixel lookup. Returns first opaque pixel found.
/// Also detects sprite-sprite collisions: when a second sprite's opaque pixel lands on the same screen
/// position as a prior opaque pixel, sets the SCOL flag (status bit 5). Once SCOL is already set for the
/// frame, reverts to early-return semantics to keep rendering cost identical to a no-collision scan.
VDPRenderer::PixelResult VDPRenderer::getSpritePixel(int screenX, int screenY) {
    VDPState            &s                    = state_;
    const int            hwSpritesPerLine     = s.h40Mode() ? 20 : 16;
    const int            maxSpritesPerLine    = hardwareSpriteLimits_ ? hwSpritesPerLine : 1000;

    int         base          = s.satBase();
    int         spriteIdx     = 0;
    int         spritesOnLine = 0;
    bool        masked        = false;
    bool        sawNonZeroX   = false;
    PixelResult result        = {0, 0, false, false};

    for (int count = 0; count < VDPState::SAT_MAX_SPRITES; ++count) {
        int satAddr   = base + spriteIdx * 8;
        int satShadow = spriteIdx * 8; // offset into sat_[] shadow
        if (satAddr + 7 >= VDPState::VRAM_SIZE)
            break;

        // Y, size, link read from shadow (frozen at last VRAM write — immune to mid-frame SAT changes).
        // SAT word 1 is size in the high byte and link in the low byte.
        int yRaw  = ((s.sat_[satShadow] & 0x03) << 8) | s.sat_[satShadow + 1];
        int link  = s.sat_[satShadow + 3] & 0x7F;
        int sizeW = ((s.sat_[satShadow + 2] >> 2) & 0x03) + 1;
        int sizeH = (s.sat_[satShadow + 2] & 0x03) + 1;

        // Tile word and X position read live from VRAM (matches GPX behaviour)
        m_word tileWord = static_cast<m_word>((s.vram_[satAddr + 4] << 8) | s.vram_[satAddr + 5]);

        int xRaw = ((s.vram_[satAddr + 6] & 0x01) << 8) | s.vram_[satAddr + 7];

        int spriteX      = xRaw - 128;
        int spriteY      = yRaw - 128;
        int spritePixelW = sizeW * 8;
        int spritePixelH = sizeH * 8;

        bool onScanline = (screenY >= spriteY && screenY < spriteY + spritePixelH);

        if (onScanline) {
            // Sprite masking: a sprite at X=0 only hides subsequent ones once a prior
            // on-scanline sprite has had nonzero X (matches hardware/GPGX behaviour;
            // unused sprites parked at X=0 at the head of the chain must not mask).
            if (xRaw != 0)
                sawNonZeroX = true;
            else if (sawNonZeroX)
                masked = true;

            if (!masked) {
                spritesOnLine++;
                if (spritesOnLine == hwSpritesPerLine + 1)
                    s.status_ |= 0x40; // SOVR
                if (spritesOnLine > maxSpritesPerLine)
                    break;

                if (screenX >= spriteX && screenX < spriteX + spritePixelW) {
                    bool   priority = (tileWord & 0x8000) != 0;
                    m_byte palette  = static_cast<m_byte>((tileWord >> 13) & 0x03);
                    bool   vflip    = (tileWord & 0x1000) != 0;
                    bool   hflip    = (tileWord & 0x0800) != 0;
                    int    baseTile = tileWord & 0x07FF;

                    int px = screenX - spriteX;
                    int py = screenY - spriteY;
                    if (hflip)
                        px = spritePixelW - 1 - px;
                    if (vflip)
                        py = spritePixelH - 1 - py;

                    int tileCol    = px / 8;
                    int tileRow    = py / (s.interlaceMode() == 2 ? 16 : 8);
                    int pixInTileX = px % 8;
                    int pixInTileY = py % (s.interlaceMode() == 2 ? 16 : 8);
                    int tileIdx    = baseTile + tileCol * sizeH + tileRow;
                    if (s.interlaceMode() == 2) {
                        tileIdx = ((tileIdx & 0x03FF) << 1) | (pixInTileY >> 3);
                        pixInTileY &= 0x07;
                    }

                    m_byte colorIdx = tile_.getTilePixel(tileIdx * 32, pixInTileX, pixInTileY, false, false);

                    if (colorIdx != 0) {
                        if (!result.opaque) {
                            result = {colorIdx, palette, priority, true};
                            if (s.status_ & 0x0020) {
                                return result;
                            }
                        } else {
                            s.status_ |= 0x0020;
                            return result;
                        }
                    }
                }
            }
        }

        // link is a 7-bit field (0-127) but sat_[] shadow only holds SAT_MAX_SPRITES (80) entries; a link
        // beyond that range must stop the chain, otherwise satShadow indexes past sat_[] into unrelated
        // VDPState fields (matches GPGX, which breaks when link is out of range).
        if (link == 0 || link >= VDPState::SAT_MAX_SPRITES)
            break;
        spriteIdx = link;
    }

    return result;
}

/// Evaluates the sprite layer for one scanline into spriteLine_. This replaces the per-pixel getSpritePixel()
/// scan: the SAT link chain is walked a single time per line (O(sprites + width) instead of O(width × sprites)).
/// Masking, the per-line sprite limit / SOVR, topmost-wins ordering, and sprite-sprite collision (SCOL) are
/// preserved exactly — for each sprite the row is fixed, so only the horizontal span is iterated, and the first
/// opaque sprite in chain order claims each pixel (a later opaque pixel landing on a taken slot sets SCOL).
void VDPRenderer::buildSpriteLine(int line) {
    VDPState            &s                    = state_;
    const int            hwSpritesPerLine     = s.h40Mode() ? 20 : 16;
    const int            maxSpritesPerLine    = hardwareSpriteLimits_ ? hwSpritesPerLine : 1000;
    const int            activeWidth          = s.activeWidth();
    const int            maxSpritePixels      = hardwareSpriteLimits_ ? activeWidth : 1000000;
    const bool           interlace2           = s.interlaceMode() == 2;
    const int            spriteTileHeight     = interlace2 ? 16 : 8;
    int                  spritePixelsOnLine   = 0;

    std::fill_n(spriteLine_, activeWidth, PixelResult{0, 0, false, false});

    int  base          = s.satBase();
    int  spriteIdx     = 0;
    int  spritesOnLine = 0;
    bool masked        = false;
    bool sawNonZeroX   = false;

    for (int count = 0; count < VDPState::SAT_MAX_SPRITES; ++count) {
        int satAddr   = base + spriteIdx * 8;
        int satShadow = spriteIdx * 8; // offset into sat_[] shadow
        if (satAddr + 7 >= VDPState::VRAM_SIZE)
            break;

        // Y, size, link read from shadow (frozen at last VRAM write — immune to mid-frame SAT changes).
        // SAT word 1 is size in the high byte and link in the low byte.
        int yRaw  = ((s.sat_[satShadow] & 0x03) << 8) | s.sat_[satShadow + 1];
        int link  = s.sat_[satShadow + 3] & 0x7F;
        int sizeW = ((s.sat_[satShadow + 2] >> 2) & 0x03) + 1;
        int sizeH = (s.sat_[satShadow + 2] & 0x03) + 1;

        // Tile word and X position read live from VRAM (matches GPX behaviour)
        m_word tileWord = static_cast<m_word>((s.vram_[satAddr + 4] << 8) | s.vram_[satAddr + 5]);
        int    xRaw     = ((s.vram_[satAddr + 6] & 0x01) << 8) | s.vram_[satAddr + 7];

        int spriteX      = xRaw - 128;
        int spriteY      = yRaw - 128;
        int spritePixelW = sizeW * 8;
        int spritePixelH = sizeH * 8;

        bool onScanline = (line >= spriteY && line < spriteY + spritePixelH);

        if (onScanline) {
            // Sprite masking: a sprite at X=0 only hides subsequent ones once a prior on-scanline sprite has
            // had nonzero X (matches hardware/GPGX behaviour; unused sprites parked at X=0 at the head of the
            // chain must not mask).
            if (xRaw != 0)
                sawNonZeroX = true;
            else if (sawNonZeroX)
                masked = true;

            if (!masked) {
                spritesOnLine++;
                if (spritesOnLine == hwSpritesPerLine + 1)
                    s.status_ |= 0x40; // SOVR
                if (spritesOnLine > maxSpritesPerLine)
                    break;

                bool   priority = (tileWord & 0x8000) != 0;
                m_byte palette  = static_cast<m_byte>((tileWord >> 13) & 0x03);
                bool   vflip    = (tileWord & 0x1000) != 0;
                bool   hflip    = (tileWord & 0x0800) != 0;
                int    baseTile = tileWord & 0x07FF;
                spritePixelsOnLine += spritePixelW;

                // The row within the sprite is constant across the span — resolve it once.
                int py = line - spriteY;
                if (vflip)
                    py = spritePixelH - 1 - py;
                int tileRow    = py / spriteTileHeight;
                int pixInTileY = py % spriteTileHeight;

                // Clip the horizontal span to the visible screen so only on-screen pixels are touched.
                int xStart = spriteX < 0 ? 0 : spriteX;
                int xEnd   = spriteX + spritePixelW;
                if (xEnd > activeWidth)
                    xEnd = activeWidth;
                if (hardwareSpriteLimits_ && spritePixelsOnLine > maxSpritePixels) {
                    xEnd -= spritePixelsOnLine - maxSpritePixels;
                    if (xEnd < xStart)
                        xEnd = xStart;
                    s.status_ |= 0x40;
                }

                for (int screenX = xStart; screenX < xEnd; ++screenX) {
                    int px = screenX - spriteX;
                    if (hflip)
                        px = spritePixelW - 1 - px;

                    int tileCol    = px / 8;
                    int pixInTileX = px % 8;
                    int tileIdx    = baseTile + tileCol * sizeH + tileRow;
                    int tilePixY   = pixInTileY;
                    if (interlace2) {
                        tileIdx = ((tileIdx & 0x03FF) << 1) | (tilePixY >> 3);
                        tilePixY &= 0x07;
                    }

                    const int rowAddress = tileIdx * 32 + tilePixY * 4;
                    const m_byte colorIdx = rowAddress <= VDPState::VRAM_SIZE - 4
                        ? readTileRowPixel(s.vram_, rowAddress, pixInTileX)
                        : 0;
                    if (colorIdx != 0) {
                        if (!spriteLine_[screenX].opaque) {
                            spriteLine_[screenX] = {colorIdx, palette, priority, true};
                        } else {
                            s.status_ |= 0x0020; // SCOL: two opaque sprite pixels overlap
                        }
                    }
                }
                if (hardwareSpriteLimits_ && spritePixelsOnLine >= maxSpritePixels) {
                    break;
                }
            }
        }

        // link is a 7-bit field (0-127) but sat_[] shadow only holds SAT_MAX_SPRITES (80) entries; a link
        // beyond that range must stop the chain, otherwise satShadow indexes past sat_[] into unrelated
        // VDPState fields (matches GPGX, which breaks when link is out of range).
        if (link == 0 || link >= VDPState::SAT_MAX_SPRITES)
            break;
        spriteIdx = link;
    }
}

// ── Compositing ─────────────────────────────────────────────────────────────

/// Composites plane B, plane A/window, and sprite pixels using priority rules. Returns final RGB color or invalid if
/// all layers transparent.
VDPRenderer::CompositeResult VDPRenderer::resolvePixel(
    PixelResult planeBPx, PixelResult planeAPx, PixelResult spritePx, m_byte bgR, m_byte bgG, m_byte bgB) const {
    CompositeResult cr{0, 0, 0, false};

    if (state_.shadowHighlightEnabled() && spritePx.opaque && spritePx.palette == 3
        && (spritePx.colorIndex == 14 || spritePx.colorIndex == 15)) {
        auto pickUnderSprite = [&]() {
            CompositeResult under{bgR, bgG, bgB, true};
            if (planeAPx.opaque && planeAPx.priority) {
                tile_.cramToRGB(planeAPx.palette, planeAPx.colorIndex, under.r, under.g, under.b);
            } else if (planeBPx.opaque && planeBPx.priority) {
                tile_.cramToRGB(planeBPx.palette, planeBPx.colorIndex, under.r, under.g, under.b);
            } else if (planeAPx.opaque) {
                tile_.cramToRGB(planeAPx.palette, planeAPx.colorIndex, under.r, under.g, under.b);
            } else if (planeBPx.opaque) {
                tile_.cramToRGB(planeBPx.palette, planeBPx.colorIndex, under.r, under.g, under.b);
            }
            return under;
        };

        cr = pickUnderSprite();
        if (spritePx.colorIndex == 14) {
            cr.r = static_cast<m_byte>(std::min<int>(7, cr.r + 3));
            cr.g = static_cast<m_byte>(std::min<int>(7, cr.g + 3));
            cr.b = static_cast<m_byte>(std::min<int>(7, cr.b + 3));
        } else {
            cr.r = static_cast<m_byte>(cr.r >> 1);
            cr.g = static_cast<m_byte>(cr.g >> 1);
            cr.b = static_cast<m_byte>(cr.b >> 1);
        }
        return cr;
    }

    if (!planeAPx.opaque && !planeBPx.opaque && !spritePx.opaque) {
        return cr; // valid=false → use background
    }

    // Front-to-back priority resolution
    //  1. High-priority sprite
    //  2. High-priority Plane A / Window
    //  3. High-priority Plane B
    //  4. Low-priority sprite
    //  5. Low-priority Plane A / Window
    //  6. Low-priority Plane B

    if (spritePx.opaque && spritePx.priority) {
        tile_.cramToRGB(spritePx.palette, spritePx.colorIndex, cr.r, cr.g, cr.b);
        cr.valid = true;
    } else if (planeAPx.opaque && planeAPx.priority) {
        tile_.cramToRGB(planeAPx.palette, planeAPx.colorIndex, cr.r, cr.g, cr.b);
        cr.valid = true;
    } else if (planeBPx.opaque && planeBPx.priority) {
        tile_.cramToRGB(planeBPx.palette, planeBPx.colorIndex, cr.r, cr.g, cr.b);
        cr.valid = true;
    } else if (spritePx.opaque && !spritePx.priority) {
        tile_.cramToRGB(spritePx.palette, spritePx.colorIndex, cr.r, cr.g, cr.b);
        cr.valid = true;
    } else if (planeAPx.opaque && !planeAPx.priority) {
        tile_.cramToRGB(planeAPx.palette, planeAPx.colorIndex, cr.r, cr.g, cr.b);
        cr.valid = true;
    } else if (planeBPx.opaque && !planeBPx.priority) {
        tile_.cramToRGB(planeBPx.palette, planeBPx.colorIndex, cr.r, cr.g, cr.b);
        cr.valid = true;
    }

    return cr;
}
