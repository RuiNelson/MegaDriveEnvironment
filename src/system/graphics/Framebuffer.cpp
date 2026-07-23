/**
 * @file Framebuffer.cpp
 * @brief 320×480 BGR framebuffer for the VDP emulator.
 */

#include "Framebuffer.hpp"
#include <algorithm>

/// Initializes framebuffer and clears to black.
Framebuffer::Framebuffer() {
    clear();
}

/// Returns mutable pointer to pixel data for direct read/write access.
void *Framebuffer::getRawPointer() {
    return pixels_;
}

/// Returns const pointer to pixel data for read-only access.
const void *Framebuffer::getRawPointer() const {
    return pixels_;
}

/// Sets pixel at (X, Y) to BGR values (each 3-bit: 0–7, stored in low 3 bits). Silently clips out-of-bounds writes.
void Framebuffer::setPixel(int x, int y, m_byte b, m_byte g, m_byte r) {
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT)
        return;
    int offset          = (y * WIDTH + x) * BPP;
    pixels_[offset + 0] = b;
    pixels_[offset + 1] = g;
    pixels_[offset + 2] = r;
}

/// Reads pixel at (X, Y) into BGR references. Returns black (0, 0, 0) if out of bounds.
void Framebuffer::getPixel(int x, int y, m_byte &b, m_byte &g, m_byte &r) const {
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) {
        b = g = r = 0;
        return;
    }
    int offset = (y * WIDTH + x) * BPP;
    b          = pixels_[offset + 0];
    g          = pixels_[offset + 1];
    r          = pixels_[offset + 2];
}

/// Fills entire framebuffer with black (all bytes = 0).
void Framebuffer::clear() {
    std::memset(pixels_, 0, SIZE);
}

/// Creates new framebuffer with 3-bit values expanded to 8-bit using lookup table.
/// Maps 3-bit channel [0..7] → 8-bit [0, 36, 73, 109, 146, 182, 219, 255] for linear expansion.
Framebuffer Framebuffer::convertTo8BitsPerPixel() const {
    static constexpr m_byte lut[8] = {0, 36, 73, 109, 146, 182, 219, 255};

    Framebuffer result;
    auto       *dst = static_cast<m_byte *>(result.getRawPointer());
    for (int i = 0; i < SIZE; ++i) {
        dst[i] = lut[pixels_[i] & 0x07];
    }
    return result;
}

/// Uploads native pixels after expanding 3-bit channels directly into the locked streaming texture.
/// Only the active image is converted: progressive H40 output is 320×224 rather than the framebuffer's
/// interlace-capable 320×480 allocation. This also avoids constructing and clearing a 450 KiB temporary.
void Framebuffer::uploadToTexture(SDL_Texture *tex, int width, int height) const {
    if (tex == nullptr)
        return;

    width  = std::clamp(width, 0, WIDTH);
    height = std::clamp(height, 0, HEIGHT);
    if (width == 0 || height == 0)
        return;

    void *texturePixels = nullptr;
    int   texturePitch  = 0;
    if (!SDL_LockTexture(tex, nullptr, &texturePixels, &texturePitch))
        return;

    static constexpr m_byte lut[8] = {0, 36, 73, 109, 146, 182, 219, 255};
    auto *destination = static_cast<m_byte *>(texturePixels);
    for (int y = 0; y < height; ++y) {
        const m_byte *sourceRow = pixels_ + y * PITCH;
        m_byte       *targetRow = destination + y * texturePitch;
        for (int byte = 0; byte < width * BPP; ++byte)
            targetRow[byte] = lut[sourceRow[byte] & 0x07];
    }
    SDL_UnlockTexture(tex);
}
