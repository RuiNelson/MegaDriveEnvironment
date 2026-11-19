/**
 * @file VDPPresenterSDL.cpp
 * @brief VDP presentation through SDL_Renderer (every platform but macOS, and macOS's fallback).
 */

#include "VDPPresenter.hpp"
#include "VDP.hpp"

namespace {

class SDLPresenter final : public VDPPresenter {
    public:
    SDLPresenter(SDL_Renderer *renderer, SDL_Texture *texture) : renderer_(renderer), texture_(texture) {}

    ~SDLPresenter() override {
        SDL_DestroyTexture(texture_);
        SDL_DestroyRenderer(renderer_);
    }

    const char *name() const override {
        return "sdl";
    }

    void upload(const Framebuffer &framebuffer, int width, int height) override {
        framebuffer.uploadToTexture(texture_, width, height);
    }

    void present(int width, int height, const m_byte background[3], bool integerScaling, bool linear) override {
        if (width <= 0 || height <= 0)
            return;
        if (linear != linear_) {
            SDL_SetTextureScaleMode(texture_, linear ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
            linear_ = linear;
        }
        SDL_SetRenderDrawColor(renderer_, background[0], background[1], background[2], SDL_ALPHA_OPAQUE);
        SDL_RenderClear(renderer_);

        int outputW = 0;
        int outputH = 0;
        if (!SDL_GetRenderOutputSize(renderer_, &outputW, &outputH) || outputW <= 0 || outputH <= 0)
            return;

        const SDL_FRect src{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)};
        const SDL_FRect dst = VDP::presentationRect(outputW, outputH, width, height, integerScaling);
        SDL_RenderTexture(renderer_, texture_, &src, &dst);
        SDL_RenderPresent(renderer_);
    }

    private:
    SDL_Renderer *renderer_;
    SDL_Texture  *texture_;
    bool          linear_ = true; // SDL's default scale mode
};

} // namespace

std::unique_ptr<VDPPresenter> createSDLPresenter(SDL_Window *window) {
    SDL_Renderer *renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer)
        return nullptr;
    // Every mode presents in sync with the display, so nothing tears. InternalTimer still paces itself: it
    // queues presents without waiting and drops frames while one is pending, so this neither caps turbo nor
    // ties the game's speed to the monitor, and a VRR monitor follows the timer's rate.
    SDL_SetRenderVSync(renderer, 1);
    SDL_Texture *texture = SDL_CreateTexture(renderer,
                                             SDL_PIXELFORMAT_BGR24,
                                             SDL_TEXTUREACCESS_STREAMING,
                                             VDPState::MAX_SCREEN_W,
                                             VDPState::MAX_SCREEN_H);
    if (!texture) {
        SDL_DestroyRenderer(renderer);
        return nullptr;
    }
    return std::make_unique<SDLPresenter>(renderer, texture);
}
