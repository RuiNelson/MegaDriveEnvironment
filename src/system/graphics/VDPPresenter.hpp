#pragma once

#include "Framebuffer.hpp"
#include "VDP.hpp"
#include <SDL3/SDL.h>
#include <memory>

/**
 * @file VDPPresenter.hpp
 * @brief Puts the VDP's finished frames in its window (source-tree only).
 *
 * macOS presents through Metal directly (a CAMetalLayer on the SDL window), so
 * Metal-only work such as MetalFX can be added there; everywhere else, and on
 * macOS when Metal is unavailable (the `dummy` video driver of the tests),
 * through SDL_Renderer. Both run on the SDL main thread only.
 */
/// Frame generation's part of a present (see VDP::setFrameGeneration()).
struct VDPFrameGeneration {
    /// Keep this picture for the next interpolation; false drops what was kept.
    bool keep = false;
    /// When set, first draw a frame between the kept picture and this one: width×height (x, y) motion
    /// vectors in picture pixels (computeMotion()), the two @p interval seconds apart.
    const float *motion   = nullptr;
    double       interval = 0.0;
    /// With @p motion, where that frame shows the kept picture instead (computePoseHolds()); null for none.
    const float *holds = nullptr;
};

class VDPPresenter {
    public:
    virtual ~VDPPresenter() = default;

    /// Short name for the log ("metal", "sdl").
    virtual const char *name() const = 0;

    /// Copies the width×height active area of @p framebuffer (3-bit channels) for the next present().
    virtual void upload(const Framebuffer &framebuffer, int width, int height) = 0;

    /// Whether present() can generate frames (VDPFrameGeneration). Fixed after creation.
    virtual bool supportsFrameGeneration() const {
        return false;
    }

    /// Whether present() can draw @p filter. Fixed after creation.
    virtual bool supports(VDP::ScalingFilter filter) const = 0;

    /// Clears the window to @p background (8-bit RGB), draws the last upload, width×height, where
    /// VDP::presentationRect puts it, scaled with @p filter (one it supports), and presents in sync with the
    /// display.
    virtual void present(int                       width,
                         int                       height,
                         const m_byte              background[3],
                         bool                      integerScaling,
                         VDP::ScalingFilter        filter,
                         const VDPFrameGeneration &generation) = 0;
};

/// SDL_Renderer presenter; null when the renderer or its texture cannot be created.
std::unique_ptr<VDPPresenter> createSDLPresenter(SDL_Window *window);

#ifdef __APPLE__
/// Metal presenter for a window created with SDL_WINDOW_METAL; null when Metal is unavailable.
std::unique_ptr<VDPPresenter> createMetalPresenter(SDL_Window *window);
#endif
