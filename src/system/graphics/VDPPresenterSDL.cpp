/**
 * @file VDPPresenterSDL.cpp
 * @brief VDP presentation through SDL_Renderer (every platform but macOS, and macOS's fallback).
 */

#include "VDPPresenter.hpp"
#include "VDP.hpp"

#include <algorithm>

namespace {

class SDLPresenter final : public VDPPresenter {
    public:
    SDLPresenter(SDL_Renderer *renderer, SDL_Texture *texture) : renderer_(renderer), texture_(texture) {}

    ~SDLPresenter() override {
        SDL_DestroyTexture(intermediate_);
        SDL_DestroyTexture(texture_);
        SDL_DestroyRenderer(renderer_);
    }

    const char *name() const override {
        return "sdl";
    }

    void upload(const Framebuffer &framebuffer, int width, int height) override {
        framebuffer.uploadToTexture(texture_, width, height);
    }

    bool supports(VDP::ScalingFilter filter) const override {
        return filter != VDP::MetalFX;
    }

    void present(int width,
                 int height,
                 const m_byte background[3],
                 bool integerScaling,
                 VDP::ScalingFilter filter,
                 const VDPFrameGeneration &) override {
        if (width <= 0 || height <= 0)
            return;
        int outputW = 0;
        int outputH = 0;
        if (!SDL_GetRenderOutputSize(renderer_, &outputW, &outputH) || outputW <= 0 || outputH <= 0)
            return;
        const SDL_FRect src{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)};
        const SDL_FRect dst = VDP::presentationRect(outputW, outputH, width, height, integerScaling);

        // Hybrid: nearest-neighbour to the next whole multiple, then bilinear down to the rectangle.
        SDL_Texture *scaled = nullptr;
        if (filter == VDP::Hybrid) {
            int factor = VDP::hybridFactor(dst.w, width);
            if (static_cast<float>(factor * width) - dst.w > 0.5f) {
                const int widest = maxTextureSize_ / std::max(width, height);
                factor           = std::clamp(factor, 1, std::max(1, widest));
                scaled           = intermediate(factor * width, factor * height);
            }
            if (scaled) {
                setLinear(false);
                SDL_SetRenderTarget(renderer_, scaled);
                SDL_RenderTexture(renderer_, texture_, &src, nullptr);
                SDL_SetRenderTarget(renderer_, nullptr);
            }
        }

        SDL_SetRenderDrawColor(renderer_, background[0], background[1], background[2], SDL_ALPHA_OPAQUE);
        SDL_RenderClear(renderer_);
        if (scaled) {
            SDL_RenderTexture(renderer_, scaled, nullptr, &dst);
        } else {
            setLinear(filter == VDP::Bilinear);
            SDL_RenderTexture(renderer_, texture_, &src, &dst);
        }
        SDL_RenderPresent(renderer_);
    }

    private:
    void setLinear(bool linear) {
        if (linear == linear_)
            return;
        SDL_SetTextureScaleMode(texture_, linear ? SDL_SCALEMODE_LINEAR : SDL_SCALEMODE_NEAREST);
        linear_ = linear;
    }

    /// The hybrid pass's target, w×h, bilinear; remade when the size changes. Null when it cannot be made.
    SDL_Texture *intermediate(int w, int h) {
        if (intermediate_ && intermediateW_ == w && intermediateH_ == h)
            return intermediate_;
        SDL_DestroyTexture(intermediate_);
        intermediate_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
        intermediateW_ = intermediate_ ? w : 0;
        intermediateH_ = intermediate_ ? h : 0;
        if (intermediate_)
            SDL_SetTextureScaleMode(intermediate_, SDL_SCALEMODE_LINEAR);
        return intermediate_;
    }

    SDL_Renderer *renderer_;
    SDL_Texture  *texture_;
    bool          linear_        = true; // SDL's default scale mode
    SDL_Texture  *intermediate_  = nullptr;
    int           intermediateW_ = 0;
    int           intermediateH_ = 0;
    int           maxTextureSize_ = static_cast<int>(
        SDL_GetNumberProperty(SDL_GetRendererProperties(renderer_), SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 4096));
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
