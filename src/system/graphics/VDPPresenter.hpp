#pragma once

#include "Framebuffer.hpp"
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
class VDPPresenter {
    public:
    virtual ~VDPPresenter() = default;

    /// Short name for the log ("metal", "sdl").
    virtual const char *name() const = 0;

    /// Copies the width×height active area of @p framebuffer (3-bit channels) for the next present().
    virtual void upload(const Framebuffer &framebuffer, int width, int height) = 0;

    /// Clears the window to @p background (8-bit RGB), draws the last upload, width×height, where
    /// VDP::presentationRect puts it, and presents in sync with the display. @p linear selects bilinear
    /// sampling, otherwise nearest-neighbour.
    virtual void present(int width, int height, const m_byte background[3], bool integerScaling, bool linear) = 0;
};

/// SDL_Renderer presenter; null when the renderer or its texture cannot be created.
std::unique_ptr<VDPPresenter> createSDLPresenter(SDL_Window *window);

#ifdef __APPLE__
/// Metal presenter for a window created with SDL_WINDOW_METAL; null when Metal is unavailable.
std::unique_ptr<VDPPresenter> createMetalPresenter(SDL_Window *window);
#endif
