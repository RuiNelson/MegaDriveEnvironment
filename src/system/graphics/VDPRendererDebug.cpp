/**
 * @file VDPRendererDebug.cpp
 * @brief Debug PNG export functions for VDPRenderer.
 */

#include "VDPRendererDebug.hpp"
#include <algorithm>
#include <array>
#include <iomanip>
#include <sstream>

VDPRendererDebug::VDPRendererDebug(VDPState &state, VDPTile &tile, Framebuffer &fb)
    : state_(state), tile_(tile), fb_(fb) {
}

// ── Helper implementations ────────────────────────────────────────────────────

uint8_t VDPRendererDebug::expand3to8(uint8_t val) {
    static const uint8_t lut[8] = {0, 36, 73, 109, 146, 182, 219, 255};
    return lut[val & 0x07];
}

void VDPRendererDebug::setImagePixel(Image &img, int x, int y, m_byte b, m_byte g, m_byte r) const {
    ImageSize size = img.getSize();
    if (x < 0 || x >= size.width || y < 0 || y >= size.height)
        return;
    uint8_t *buf = static_cast<uint8_t *>(img.getRawBuffer());
    int      idx = (y * size.width + x) * 3;
    buf[idx]     = r; // Image buffer is RGB
    buf[idx + 1] = g;
    buf[idx + 2] = b;
}

Image VDPRendererDebug::renderPaletteRow(int paletteIdx, bool fullRange) const {
    const int colorH = 16;
    const int colorW = 16;
    Color     black  = {0, 0, 0, 255};

    Image colors;
    for (int c = 0; c < 16; ++c) {
        m_byte r8, g8, b8;
        if (fullRange)
            tile_.cramToRGB_FullRange(static_cast<m_byte>(paletteIdx), static_cast<m_byte>(c), r8, g8, b8);
        else
            tile_.cramToRGB(static_cast<m_byte>(paletteIdx), static_cast<m_byte>(c), r8, g8, b8);

        colors.addOnRight(Image(ImageSize{colorW, colorH}, Color{r8, g8, b8, 255}), 0, black);
    }

    std::ostringstream label;
    label << "P" << paletteIdx;
    return colors.addLabelOnBottom(label.str(), 0, black, Color{255, 255, 0, 255});
}

int VDPRendererDebug::planeHScroll(int scrollIndex, int line) const {
    // Same table lookup as VDPRenderer::renderScanline: full screen, per 8 lines, or per line.
    int address = state_.hscrollBase();
    switch (state_.hscrollMode()) {
        case 2:
            address += (line / 8) * 8 * 4;
            break;
        case 3:
            address += line * 4;
            break;
        case 0:
            break;
        default:
            return 0;
    }
    address = (address + scrollIndex * 2) & 0xFFFF;
    return static_cast<int16_t>((state_.vram_[address] << 8) | state_.vram_[(address + 1) & 0xFFFF]);
}

int VDPRendererDebug::planeVScroll(int scrollIndex, int screenX) const {
    // Same VSRAM selection as VDPRenderer::buildPlaneLine: one word per plane, or one per 2-cell column.
    int index = scrollIndex;
    if (state_.vscrollMode() == 1) {
        const int columnIndex = (std::max(0, screenX - state_.centerOffsetX()) >> 4) * 2 + scrollIndex;
        if (columnIndex < VDPState::VSRAM_ENTRIES)
            index = columnIndex;
    }
    return static_cast<int16_t>(state_.vsram_[index]);
}

Image VDPRendererDebug::renderPlaneLayer(int planeBase, int scrollIndex, bool fullRange) const {
    Color black   = {0, 0, 0, 255};
    Color outline = {255, 0, 255, 255};

    const bool interlace2  = state_.interlaceMode() == 2;
    const int  cellHeight  = interlace2 ? 16 : 8;
    const int  pw          = state_.planeWidthCells();
    const int  ph          = state_.planeHeightCells();
    const int  planeWidth  = pw * 8;
    const int  planeHeight = ph * cellHeight;

    // The whole name table, not just the screen-sized corner at cell (0, 0).
    Image result(ImageSize{planeWidth, planeHeight}, black);

    int nonZero = 0;
    for (int i = 0; i < pw * ph; ++i) {
        int    ea = (planeBase + i * 2) & 0xFFFF;
        m_word e  = static_cast<m_word>((state_.vram_[ea] << 8) | state_.vram_[(ea + 1) & 0xFFFF]);
        if (e != 0)
            nonZero++;
    }

    for (int planeY = 0; planeY < planeHeight; ++planeY) {
        for (int planeX = 0; planeX < planeWidth; ++planeX) {
            int cellX        = planeX / 8;
            int cellY        = planeY / cellHeight;
            int pixelInTileX = planeX % 8;
            int pixelInTileY = planeY % cellHeight;

            int    entryAddr = (planeBase + (cellY * pw + cellX) * 2) & 0xFFFF;
            m_word entry     = static_cast<m_word>((state_.vram_[entryAddr] << 8) | state_.vram_[(entryAddr + 1) & 0xFFFF]);

            m_byte palette   = static_cast<m_byte>((entry >> 13) & 0x03);
            bool   vflip     = (entry & 0x1000) != 0;
            bool   hflip     = (entry & 0x0800) != 0;
            int tileIndex = entry & 0x07FF;
            if (interlace2) {
                tileIndex = ((entry & 0x03FF) << 1) | (pixelInTileY >> 3);
                pixelInTileY &= 0x07;
            }

            m_byte colorIdx = tile_.getTilePixel(tileIndex * 32, pixelInTileX, pixelInTileY, hflip, vflip);

            if (colorIdx != 0) {
                m_byte r8, g8, b8;
                if (fullRange)
                    tile_.cramToRGB_FullRange(palette, colorIdx, r8, g8, b8);
                else
                    tile_.cramToRGB(palette, colorIdx, r8, g8, b8);
                setImagePixel(result, planeX, planeY, b8, g8, r8);
            }
        }
    }

    // Outline the screen's border pixels where the renderer samples them, using
    // its scroll math (and plane wrap), so per-line and per-column scroll show as
    // a stepped or wavy outline rather than a rectangle.
    const int screenWidth  = state_.activeWidth();
    const int screenHeight = state_.activeHeight();
    const int firstLine    = state_.topCropLines();
    const int scrollXMask  = planeWidth - 1;
    // VDPRenderer wraps plane Y at ph * 8 even in interlace mode 2; match it.
    const int scrollYMask  = ph * 8 - 1;
    for (int line = firstLine; line < screenHeight; ++line) {
        const int  hscroll = planeHScroll(scrollIndex, line);
        const bool edgeRow = line == firstLine || line == screenHeight - 1;
        for (int x = 0; x < screenWidth; x += edgeRow ? 1 : screenWidth - 1) {
            const int planeX = (x - state_.centerOffsetX() - hscroll) & scrollXMask;
            const int planeY = (line + planeVScroll(scrollIndex, x)) & scrollYMask;
            setImagePixel(result, planeX, planeY, outline.b, outline.g, outline.r);
        }
    }

    const int viewX = (-state_.centerOffsetX() - planeHScroll(scrollIndex, firstLine)) & scrollXMask;
    const int viewY = planeVScroll(scrollIndex, 0) & scrollYMask;

    std::ostringstream base;
    base << "$" << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << planeBase;
    Image info = Image(base.str(), black, Color{255, 255, 0, 255});
    info.addOnRight(Image(std::to_string(pw) + "x" + std::to_string(ph), black, Color{200, 200, 200, 255}), 4, black);
    info.addOnRight(Image(std::to_string(nonZero) + " tiles", black, Color{0, 200, 255, 255}), 4, black);
    info.addOnRight(Image("view " + std::to_string(viewX) + "," + std::to_string(viewY), black, outline), 4, black);
    result.addOnTop(info, 0, black);

    return result;
}

Image VDPRendererDebug::renderSpriteLayer(bool fullRange) const {
    Color     black = {0, 0, 0, 255};
    Color     white = {200, 200, 200, 255};
    const int gap   = 8;

    Image result = Image("#", black, white);
    result.addOnRight(Image("Preview", black, white), gap, black);
    result.addOnRight(Image("X", black, white), gap, black);
    result.addOnRight(Image("Y", black, white), gap, black);
    result.addOnRight(Image("Sz", black, white), gap, black);
    result.addOnRight(Image("Tile", black, white), gap, black);
    result.addOnRight(Image("Link", black, white), gap, black);

    int                                         base          = state_.satBase();
    int                                         spriteIdx     = 0;
    std::array<bool, VDPState::SAT_MAX_SPRITES> visited{};

    for (int row = 0; row < VDPState::SAT_MAX_SPRITES; ++row) {
        if (spriteIdx >= VDPState::SAT_MAX_SPRITES)
            break;
        if (visited[static_cast<size_t>(spriteIdx)])
            break;
        visited[static_cast<size_t>(spriteIdx)] = true;

        int satAddr = base + spriteIdx * 8;
        if (satAddr + 7 >= VDPState::VRAM_SIZE)
            break;

        int satShadow = spriteIdx * 8;

        int yRaw     = ((state_.sat_[satShadow] & 0x03) << 8) | state_.sat_[satShadow + 1];
        int link     = state_.sat_[satShadow + 3] & 0x7F;
        int sizeCode = state_.sat_[satShadow + 2];
        int sizeW    = ((sizeCode >> 2) & 0x03) + 1;
        int sizeH    = (sizeCode & 0x03) + 1;

        m_word tileWord = static_cast<m_word>((state_.vram_[satAddr + 4] << 8) | state_.vram_[satAddr + 5]);
        int    xRaw     = ((state_.vram_[satAddr + 6] & 0x01) << 8) | state_.vram_[satAddr + 7];

        int spriteX = xRaw - 128;
        int spriteY = yRaw - 128;

        char buf[32];

        Image preview = Image(ImageSize{32, 32}, black);
        {
            m_byte palette  = static_cast<m_byte>((tileWord >> 13) & 0x03);
            int    baseTile = tileWord & 0x07FF;
            bool   vflip    = (tileWord & 0x1000) != 0;
            bool   hflip    = (tileWord & 0x0800) != 0;
            int    tileH    = state_.interlaceMode() == 2 ? 16 : 8;

            int padX = (32 - sizeW * 8) / 2;
            int padY = (32 - sizeH * tileH) / 2;

            Image    spriteImg = Image(ImageSize{sizeW * 8, sizeH * tileH}, black);
            uint8_t *spriteBuf = static_cast<uint8_t *>(spriteImg.getRawBuffer());

            for (int destY = 0; destY < sizeH * tileH; ++destY) {
                for (int destX = 0; destX < sizeW * 8; ++destX) {
                    int px = hflip ? sizeW * 8 - 1 - destX : destX;
                    int py = vflip ? sizeH * tileH - 1 - destY : destY;
                    int tileCol = px / 8;
                    int tileRow = py / tileH;
                    int tileIdx = baseTile + tileCol * sizeH + tileRow;
                    int tileY = py % tileH;
                    if (state_.interlaceMode() == 2) {
                        tileIdx = ((tileIdx & 0x03FF) << 1) | (tileY >> 3);
                        tileY &= 0x07;
                    }
                    m_byte colorIdx = tile_.getTilePixel(tileIdx * 32, px % 8, tileY, false, false);
                    if (colorIdx != 0) {
                        m_byte r8, g8, b8;
                        if (fullRange)
                            tile_.cramToRGB_FullRange(palette, colorIdx, r8, g8, b8);
                        else
                            tile_.cramToRGB(palette, colorIdx, r8, g8, b8);
                        int idx            = (destY * sizeW * 8 + destX) * 3;
                        spriteBuf[idx]     = r8; // Image buffer is RGB
                        spriteBuf[idx + 1] = g8;
                        spriteBuf[idx + 2] = b8;
                    }
                }
            }

            Image centered = spriteImg;

            if (padX > 0) {
                Image leftPad  = Image(ImageSize{padX, sizeH * tileH}, black);
                Image rightPad = Image(ImageSize{padX, sizeH * tileH}, black);
                centered.addOnLeft(leftPad, 0, black);
                centered.addOnRight(rightPad, 0, black);
            }

            if (padY > 0) {
                Image topPad    = Image(ImageSize{32, padY}, black);
                Image bottomPad = Image(ImageSize{32, padY}, black);
                centered.addOnTop(topPad, 0, black);
                centered.addOnBottom(bottomPad, 0, black);
            }

            ImageSize sz = centered.getSize();
            if (sz.width > 32 || sz.height > 32) {
                Image    cropped = Image(ImageSize{32, 32}, black);
                uint8_t *src     = static_cast<uint8_t *>(centered.getRawBuffer());
                uint8_t *dst     = static_cast<uint8_t *>(cropped.getRawBuffer());
                for (int y = 0; y < 32; ++y) {
                    for (int x = 0; x < 32; ++x) {
                        int si      = (y * sz.width + x) * 3;
                        int di      = (y * 32 + x) * 3;
                        dst[di]     = src[si];
                        dst[di + 1] = src[si + 1];
                        dst[di + 2] = src[si + 2];
                    }
                }
                centered = cropped;
            }

            preview = centered;
        }

        snprintf(buf, sizeof(buf), "%d", spriteIdx);
        Image col0 = Image(buf, black, white);

        snprintf(buf, sizeof(buf), "%d", spriteX);
        Image col1 = Image(buf, black, white);

        snprintf(buf, sizeof(buf), "%d", spriteY);
        Image col2 = Image(buf, black, white);

        snprintf(buf, sizeof(buf), "%dx%d", sizeW, sizeH);
        Image col3 = Image(buf, black, white);

        snprintf(buf, sizeof(buf), "$%X", tileWord & 0x07FF);
        Image col4 = Image(buf, black, white);

        snprintf(buf, sizeof(buf), "%d", link);
        Image col5 = Image(buf, black, white);

        Image rowImg = col0;
        rowImg.addOnRight(preview, gap, black);
        rowImg.addOnRight(col1, gap, black);
        rowImg.addOnRight(col2, gap, black);
        rowImg.addOnRight(col3, gap, black);
        rowImg.addOnRight(col4, gap, black);
        rowImg.addOnRight(col5, gap, black);

        result.addOnBottom(rowImg, gap, black);

        if (link == 0 || link >= VDPState::SAT_MAX_SPRITES)
            break;
        spriteIdx = link;
    }

    return result;
}

Image VDPRendererDebug::renderSpritePlane(bool fullRange) const {
    Color black = {0, 0, 0, 255};
    Image result(ImageSize{state_.activeWidth(), state_.activeOutputHeight()}, black);
    const int logicalHeight = state_.interlaceMode() == 2 ? state_.activeOutputHeight() / 2
                                                          : state_.activeOutputHeight();

    const int firstLine = state_.topCropLines();
    for (int screenY = firstLine; screenY < firstLine + logicalHeight; ++screenY) {
        int  spriteIdx     = 0;
        int  spritesOnLine = 0;
        bool masked       = false;
        bool sawNonZeroX  = false;
        std::array<bool, VDPState::SAT_MAX_SPRITES> visited{};
        std::array<bool, VDPState::MAX_SCREEN_W> occupied{};

        for (int count = 0; count < VDPState::SAT_MAX_SPRITES; ++count) {
            if (spriteIdx >= VDPState::SAT_MAX_SPRITES || visited[static_cast<size_t>(spriteIdx)])
                break;
            visited[static_cast<size_t>(spriteIdx)] = true;

            int satAddr = state_.satBase() + spriteIdx * 8;
            if (satAddr + 7 >= VDPState::VRAM_SIZE)
                break;
            int satShadow = spriteIdx * 8;
            int yRaw      = ((state_.sat_[satShadow] & 0x03) << 8) | state_.sat_[satShadow + 1];
            int link      = state_.sat_[satShadow + 3] & 0x7F;
            int sizeW     = ((state_.sat_[satShadow + 2] >> 2) & 0x03) + 1;
            int sizeH     = (state_.sat_[satShadow + 2] & 0x03) + 1;
            m_word tileWord = static_cast<m_word>((state_.vram_[satAddr + 4] << 8) | state_.vram_[satAddr + 5]);
            int xRaw = ((state_.vram_[satAddr + 6] & 0x01) << 8) | state_.vram_[satAddr + 7];
            int spriteX = xRaw - state_.spriteOriginX();
            int spriteY = yRaw - 128;
            int spriteW = sizeW * 8;
            int spriteH = sizeH * 8;

            if (screenY >= spriteY && screenY < spriteY + spriteH) {
                if (xRaw != 0)
                    sawNonZeroX = true;
                else if (sawNonZeroX)
                    masked = true;

                if (!masked && ++spritesOnLine <= (state_.h40Mode() ? 20 : 16)) {
                    m_byte palette  = static_cast<m_byte>((tileWord >> 13) & 0x03);
                    bool   vflip    = (tileWord & 0x1000) != 0;
                    bool   hflip    = (tileWord & 0x0800) != 0;
                    int    baseTile = tileWord & 0x07FF;
                    int    py       = screenY - spriteY;
                    if (vflip)
                        py = spriteH - 1 - py;
                    int tileH = state_.interlaceMode() == 2 ? 16 : 8;
                    int tileRow = py / tileH;
                    int tileY = py % tileH;

                    for (int screenX = std::max(0, spriteX);
                         screenX < std::min(state_.activeWidth(), spriteX + spriteW); ++screenX) {
                        if (occupied[static_cast<size_t>(screenX)])
                            continue;
                        int px = screenX - spriteX;
                        if (hflip)
                            px = spriteW - 1 - px;
                        int tileIdx = baseTile + (px / 8) * sizeH + tileRow;
                        int pixelY = tileY;
                        if (state_.interlaceMode() == 2) {
                            tileIdx = ((tileIdx & 0x03FF) << 1) | (pixelY >> 3);
                            pixelY &= 0x07;
                        }
                        m_byte colorIdx = tile_.getTilePixel(tileIdx * 32, px % 8, pixelY, false, false);
                        if (colorIdx == 0)
                            continue;
                        m_byte r8, g8, b8;
                        if (fullRange)
                            tile_.cramToRGB_FullRange(palette, colorIdx, r8, g8, b8);
                        else
                            tile_.cramToRGB(palette, colorIdx, r8, g8, b8);
                        const int outputLine = screenY - firstLine;
                        int outputY = state_.interlaceMode() == 2 ? outputLine * 2 : outputLine;
                        setImagePixel(result, screenX, outputY, b8, g8, r8);
                        if (state_.interlaceMode() == 2)
                            setImagePixel(result, screenX, outputY + 1, b8, g8, r8);
                        occupied[static_cast<size_t>(screenX)] = true;
                    }
                }
            }

            if (link == 0 || link >= VDPState::SAT_MAX_SPRITES)
                break;
            spriteIdx = link;
        }
    }
    return result;
}

Image VDPRendererDebug::renderWindowLayer(bool fullRange) const {
    Image result(ImageSize{state_.activeWidth(), state_.activeOutputHeight()}, Color{0, 0, 0, 255});

    int wBase  = state_.windowBase();
    int wWidth = state_.h40Mode() ? 64 : 32;

    // Mode 48 centres the window plane and crops its first tile row; map output pixels back to window ones.
    for (int y = 0; y < state_.activeOutputHeight(); ++y) {
        for (int x = 0; x < state_.activeWidth(); ++x) {
            const int windowX = x - state_.centerOffsetX();
            const int windowY = y + state_.topCropLines();
            if (windowX < 0 || windowX >= state_.hardwareWidth())
                continue;
            int cellX = windowX / 8;
            int cellY = windowY / (state_.interlaceMode() == 2 ? 16 : 8);

            int  hpos   = state_.windowHPos() * 2;
            int  vpos   = state_.windowVPos();
            bool active = false;

            if (vpos > 0) {
                if (state_.windowDown()) {
                    if (cellY >= vpos)
                        active = true;
                } else {
                    if (cellY < vpos)
                        active = true;
                }
            }

            if (hpos > 0 && !active) {
                if (state_.windowRight()) {
                    if (cellX >= hpos)
                        active = true;
                } else {
                    if (cellX < hpos)
                        active = true;
                }
            }

            if (active) {
                int pixelInTileX = windowX % 8;
                int pixelInTileY = windowY % (state_.interlaceMode() == 2 ? 16 : 8);

                int    entryAddr = (wBase + (cellY * wWidth + cellX) * 2) & 0xFFFF;
                m_word entry     = static_cast<m_word>((state_.vram_[entryAddr] << 8) | state_.vram_[entryAddr + 1]);

                m_byte palette   = static_cast<m_byte>((entry >> 13) & 0x03);
                bool   vflip     = (entry & 0x1000) != 0;
                bool   hflip     = (entry & 0x0800) != 0;
                int tileIndex = entry & 0x07FF;
                if (state_.interlaceMode() == 2) {
                    tileIndex = ((entry & 0x03FF) << 1) | (pixelInTileY >> 3);
                    pixelInTileY &= 0x07;
                }

                m_byte colorIdx = tile_.getTilePixel(tileIndex * 32, pixelInTileX, pixelInTileY, hflip, vflip);

                if (colorIdx != 0) {
                    m_byte r8, g8, b8;
                    if (fullRange)
                        tile_.cramToRGB_FullRange(palette, colorIdx, r8, g8, b8);
                    else
                        tile_.cramToRGB(palette, colorIdx, r8, g8, b8);
                    setImagePixel(result, x, y, b8, g8, r8);
                }
            }
        }
    }

    return result;
}

// ── Public debug image methods ────────────────────────────────────────────────

Image VDPRendererDebug::makeFinalOutputImage(bool fullRange) const {
    Image result(ImageSize{state_.activeWidth(), state_.activeOutputHeight()}, Color{0, 0, 0, 255});

    const m_byte *fbData = static_cast<const m_byte *>(fb_.getRawPointer());
    for (int y = 0; y < state_.activeOutputHeight(); ++y) {
        for (int x = 0; x < state_.activeWidth(); ++x) {
            int    fbIdx = (y * Framebuffer::PITCH + x * 3);
            m_byte b     = fbData[fbIdx];
            m_byte g     = fbData[fbIdx + 1];
            m_byte r     = fbData[fbIdx + 2];
            if (fullRange) {
                r = expand3to8(r);
                g = expand3to8(g);
                b = expand3to8(b);
            }
            setImagePixel(result, x, y, b, g, r);
        }
    }

    return result;
}

Image VDPRendererDebug::makePalletesTable(bool fullRange) const {
    Color black = {0, 0, 0, 255};

    Image rows[4];
    for (int p = 0; p < 4; ++p)
        rows[p] = renderPaletteRow(p, fullRange);

    Image result = rows[0];
    result.addOnBottom(rows[1], 2, black);
    result.addOnBottom(rows[2], 2, black);
    result.addOnBottom(rows[3], 2, black);
    return result;
}

Image VDPRendererDebug::makeVSRAMImage() const {
    Color     black    = {0, 0, 0, 255};
    Color     fg       = {200, 200, 200, 255};
    const int colCount = 10;
    const int rowCount = 4;
    const int gap      = 1;

    Image result;
    for (int row = 0; row < rowCount; ++row) {
        Image rowImg;
        for (int col = 0; col < colCount; ++col) {
            int                idx = row * colCount + col;
            std::ostringstream oss;
            oss << std::hex << std::uppercase << std::setfill('0') << std::setw(4) << state_.vsram_[idx];
            rowImg.addOnRight(Image(oss.str(), black, fg), gap, black);
        }
        result.addOnBottom(rowImg, gap, black);
    }
    return result;
}

Image VDPRendererDebug::makeWindowLayerImage(bool fullRange) const {
    return renderWindowLayer(fullRange);
}

Image VDPRendererDebug::makeSpriteTablesImage(bool fullRange) const {
    return renderSpriteLayer(fullRange);
}

Image VDPRendererDebug::makeSpriteLayerImage(bool fullRange) const {
    return renderSpritePlane(fullRange);
}

Image VDPRendererDebug::makeBackgroundLayerImage(bool fullRange) const {
    return renderPlaneLayer(state_.planeBBase(), 1, fullRange);
}

Image VDPRendererDebug::makeForegroundLayerImage(bool fullRange) const {
    return renderPlaneLayer(state_.planeABase(), 0, fullRange);
}

Image VDPRendererDebug::makeVramTilesImage(bool fullRange) const {
    constexpr int tileBytes = 32;
    constexpr int tileCount = VDPState::VRAM_SIZE / tileBytes; // 2048 tiles
    constexpr int tileSize  = 8;

    // A tile in VRAM carries no palette of its own: the plane name-table entry,
    // window cell or sprite entry that uses it selects one of the four CRAM
    // palettes. Count how often each palette is used with each tile and draw the
    // tile with its most used palette; tiles nothing references fall back to
    // palette 0. This is the only faithful way to colour a raw tile sheet: the
    // same tile can be shown through different palettes in different places
    // (most often palette 0 for a reused piece of art, but not always).
    std::array<std::array<uint32_t, 4>, tileCount> paletteVotes{};

    auto vote = [&paletteVotes](int tileIndex, int palette) {
        if (tileIndex < 0 || tileIndex >= tileCount)
            return;
        paletteVotes[static_cast<size_t>(tileIndex)][static_cast<size_t>(palette & 0x03)]++;
    };

    const bool interlace2 = state_.interlaceMode() == 2;
    auto       voteEntry  = [&](m_word entry) {
        const int palette = (entry >> 13) & 0x03;
        if (interlace2) {
            // Interlace 2 turns one entry into a 16-pixel-tall tile: the two 8×8
            // halves at even/odd addresses, both with the entry's palette.
            const int base = (entry & 0x03FF) << 1;
            vote(base, palette);
            vote(base + 1, palette);
        } else {
            vote(entry & 0x07FF, palette);
        }
    };

    auto voteNameTable = [&](int base, int widthCells, int heightCells) {
        for (int cell = 0; cell < widthCells * heightCells; ++cell) {
            const int    address = (base + cell * 2) & 0xFFFF;
            const m_word entry   = static_cast<m_word>((state_.vram_[address] << 8) | state_.vram_[(address + 1) & 0xFFFF]);
            voteEntry(entry);
        }
    };

    voteNameTable(state_.planeABase(), state_.planeWidthCells(), state_.planeHeightCells());
    voteNameTable(state_.planeBBase(), state_.planeWidthCells(), state_.planeHeightCells());

    // The window uses the name-table format too but is always 64 cells wide in
    // H40 (32 in H32) and 32 cells tall; skip it when it covers no screen area.
    if (state_.windowHPos() > 0 || state_.windowVPos() > 0)
        voteNameTable(state_.windowBase(), state_.h40Mode() ? 64 : 32, 32);

    // Linked SAT sprites: each entry's tile word names the palette for every
    // tile of the sprite, laid out down each column first (as the renderer does).
    {
        std::array<bool, VDPState::SAT_MAX_SPRITES> visited{};
        int                                         spriteIndex = 0;
        for (int sprite = 0; sprite < VDPState::SAT_MAX_SPRITES; ++sprite) {
            if (spriteIndex >= VDPState::SAT_MAX_SPRITES || visited[static_cast<size_t>(spriteIndex)])
                break;
            visited[static_cast<size_t>(spriteIndex)] = true;

            const int satAddress = state_.satBase() + spriteIndex * 8;
            const int shadow     = spriteIndex * 8;
            if (satAddress + 7 >= VDPState::VRAM_SIZE)
                break;

            const m_word tileWord = static_cast<m_word>((state_.vram_[satAddress + 4] << 8) |
                                                        state_.vram_[satAddress + 5]);
            const int    palette  = (tileWord >> 13) & 0x03;
            const int    baseTile = tileWord & 0x07FF;
            const int    sizeCode = state_.sat_[shadow + 2];
            const int    sizeW    = ((sizeCode >> 2) & 0x03) + 1;
            const int    sizeH    = (sizeCode & 0x03) + 1;

            for (int col = 0; col < sizeW; ++col) {
                for (int row = 0; row < sizeH; ++row) {
                    int tile = baseTile + col * sizeH + row;
                    if (interlace2)
                        tile = (tile & 0x03FF) << 1;
                    vote(tile, palette);
                    if (interlace2)
                        vote(tile + 1, palette);
                }
            }

            const int link = state_.sat_[shadow + 3] & 0x7F;
            if (link == 0 || link >= VDPState::SAT_MAX_SPRITES)
                break;
            spriteIndex = link;
        }
    }

    std::array<m_byte, tileCount> tilePalette{};
    for (int tile = 0; tile < tileCount; ++tile) {
        int best = 0;
        for (int palette = 1; palette < 4; ++palette)
            if (paletteVotes[static_cast<size_t>(tile)][static_cast<size_t>(palette)] >
                paletteVotes[static_cast<size_t>(tile)][static_cast<size_t>(best)])
                best = palette;
        tilePalette[static_cast<size_t>(tile)] = static_cast<m_byte>(best);
    }

    // Show the WHOLE VRAM as a tile sheet (64 KB / 32 bytes = 2048 tiles), not
    // just the first 128. A 32-wide grid of 64 rows covers every tile so the
    // city/character art at higher addresses is visible, not only the font that
    // lives near address 0.
    const int tilesX = 32;
    const int tilesY = tileCount / tilesX; // 64 rows

    Color black = {0, 0, 0, 255};
    Image result(ImageSize{tilesX * tileSize, tilesY * tileSize}, black);

    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            const int    tileAddr = (ty * tilesX + tx) * tileBytes;
            const m_byte palette  = tilePalette[static_cast<size_t>(tileAddr / tileBytes)];
            for (int py = 0; py < tileSize; ++py) {
                for (int px = 0; px < tileSize; ++px) {
                    const m_byte colorIdx = tile_.getTilePixel(tileAddr, px, py, false, false);
                    m_byte       r8, g8, b8;
                    if (fullRange) {
                        tile_.cramToRGB_FullRange(palette, colorIdx, r8, g8, b8);
                    } else {
                        tile_.cramToRGB(palette, colorIdx, r8, g8, b8);
                        r8 = expand3to8(r8);
                        g8 = expand3to8(g8);
                        b8 = expand3to8(b8);
                    }
                    setImagePixel(result, tx * tileSize + px, ty * tileSize + py, b8, g8, r8);
                }
            }
        }
    }

    return result;
}

Image VDPRendererDebug::makeRegistersImage() const {
    Color     black = {0, 0, 0, 255};
    Color     white = {200, 200, 200, 255};
    Color     cyan  = {0, 200, 200, 255};
    const int gap   = 1;

    struct RegInfo {
        const char *name;
        int         idx;
    };
    static const RegInfo regs[] = {
        {"Mode Set 1", 0},
        {"Mode Set 2", 1},
        {"Plane A", 2},
        {"Window", 3},
        {"Plane B", 4},
        {"Sprite Attr", 5},
        {"BG Color", 7},
        {"Reset", 10},
        {"Scroll A H", 0x0B},
        {"Scroll B H", 0x0C},
        {"Scroll H", 0x0D},
        {"Scroll V", 0x0E},
        {"Auto Inc", 0x0F},
        {"Plane Sz", 0x10},
        {"Window H", 0x11},
        {"Window V", 0x12},
    };

    auto padRight = [](std::string s, int len) -> std::string {
        while ((int)s.size() < len)
            s += ' ';
        return s;
    };

    Image header = Image(padRight("Reg", 5), black, white);
    header.addOnRight(Image(padRight("Name", 15), black, white), gap, black);
    header.addOnRight(Image(padRight("Val", 4), black, white), gap, black);

    Image result = header;
    for (int row = 0; row < VDPState::REG_COUNT; ++row) {
        std::ostringstream addrStr;
        addrStr << "$" << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << row;

        const char *name = "";
        for (auto &r : regs)
            if (r.idx == row) {
                name = r.name;
                break;
            }

        std::ostringstream valStr;
        valStr << std::hex << std::uppercase << std::setfill('0') << std::setw(2) << int(state_.regs_[row]);

        Image rowImg = Image(padRight(addrStr.str(), 5), black, cyan);
        rowImg.addOnRight(Image(padRight(name, 15), black, white), gap, black);
        rowImg.addOnRight(Image(padRight(valStr.str(), 4), black, cyan), gap, black);
        result.addOnBottom(rowImg, gap, black);
    }
    return result;
}

// ── PNG export ────────────────────────────────────────────────────────────────

void VDPRendererDebug::dumpFrameBufferToPNG(const std::string &path, bool fullRange) const {
    makeFinalOutputImage(fullRange).printToPNG(path);
}

void VDPRendererDebug::dumpEverythingToPNG(const std::string &path, bool fullRange) const {
    Color black = {0, 0, 0, 255};
    Color green = {0, 255, 0, 255};

    Image finalOutput     = makeFinalOutputImage(fullRange);
    Image colorRAM        = makePalletesTable(fullRange);
    Image vsRAM           = makeVSRAMImage();
    Image vramTiles       = makeVramTilesImage(fullRange);
    Image registers       = makeRegistersImage();
    Image backgroundLayer = makeBackgroundLayerImage(fullRange);
    Image foregroundLayer = makeForegroundLayerImage(fullRange);
    Image windowLayer     = makeWindowLayerImage(fullRange);
    Image spriteTables    = makeSpriteTablesImage(fullRange);
    Image spriteLayer     = makeSpriteLayerImage(fullRange);

    finalOutput = finalOutput.addLabelOnTop("FinalOutput", 2, black, green);
    vramTiles = vramTiles.addLabelOnTop("VRAM Tiles (palette per use)", 2, black, green);
    colorRAM = colorRAM.addLabelOnTop("CRAM Palettes", 2, black, green);
    vsRAM = vsRAM.addLabelOnTop("VSRAM", 2, black, green);
    registers = registers.addLabelOnTop("Registers", 2, black, green);
    backgroundLayer = backgroundLayer.addLabelOnTop("Plane B", 2, black, green);
    foregroundLayer = foregroundLayer.addLabelOnTop("Plane A", 2, black, green);
    windowLayer = windowLayer.addLabelOnTop("Window", 2, black, green);
    spriteTables = spriteTables.addLabelOnTop("SAT Sprites", 2, black, green);
    spriteLayer = spriteLayer.addLabelOnTop("Sprite Plane", 2, black, green);

    Image col0 = registers;

    Image col1 = finalOutput;
    col1.addOnBottom(colorRAM, 2, black);
    col1.addOnBottom(vsRAM, 2, black);

    Image col2 = vramTiles;

    Image col3 = backgroundLayer;
    col3.addOnBottom(foregroundLayer, 2, black);
    col3.addOnBottom(windowLayer, 2, black);
    col3.addOnBottom(spriteLayer, 2, black);

    Image col4 = spriteTables;

    Image all = col0;
    all.addOnRight(col1, 2, black);
    all.addOnRight(col2, 2, black);
    all.addOnRight(col3, 2, black);
    all.addOnRight(col4, 2, black);

    all.printToPNG(path);
}
