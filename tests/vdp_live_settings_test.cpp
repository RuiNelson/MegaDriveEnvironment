#include "system/graphics/VDP.hpp"

#include <SDL3/SDL.h>

#include <atomic>
#include <cassert>
#include <thread>

// VDP::applyLive from another thread, as a game's settings screen calls it,
// with the main thread pumping events as boot() does.
int main() {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    {
        VDP vdp(nullptr, VDP::InternalTimer, VDP::Integer, VDP::HardwareSpriteLimit);
        assert(vdp.synchronization() == VDP::InternalTimer);
        assert(!vdp.widescreen());
        assert(!vdp.integerScaling());

        VDP::LiveSettings settings;
        settings.synchronization = VDP::VSync2;
        settings.widescreen      = true;
        settings.integerScaling  = true;

        std::atomic<bool> done{false};
        std::thread       game([&] {
            vdp.applyLive(settings);
            done.store(true);
        });
        while (!done.load())
            SDL_PumpEvents();
        game.join();

        assert(vdp.synchronization() == VDP::VSync2);
        assert(vdp.widescreen());
        assert(vdp.integerScaling());
        assert(!vdp.fullscreen());

        settings.synchronization = VDP::InternalTimer;
        settings.widescreen      = false;
        settings.integerScaling  = false;
        done.store(false);
        std::thread back([&] {
            vdp.applyLive(settings);
            done.store(true);
        });
        while (!done.load())
            SDL_PumpEvents();
        back.join();

        assert(vdp.synchronization() == VDP::InternalTimer);
        assert(!vdp.widescreen());
        assert(!vdp.integerScaling());
    }
    SDL_Quit();
    return 0;
}
