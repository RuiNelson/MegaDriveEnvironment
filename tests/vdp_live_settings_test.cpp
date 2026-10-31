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

        // The overlay lands in the finished frame, and only with the display on.
        vdp.setFrameOverlay([](Framebuffer &framebuffer, int width, int height) {
            assert(width == 320 && height == 224);
            framebuffer.setPixel(3, 2, 1, 2, 7);
        });
        vdp.writeControlPort(0x8C81); // H40
        vdp.writeControlPort(0x8144); // display on
        vdp.start();
        for (int i = 0; i < 100000; ++i) {
            SDL_PumpEvents();
            const VDP::FramebufferSnapshot frame = vdp.framebufferSnapshot();
            if (frame.pixels[2u * frame.pitch + 3u * 3u + 2u] == 7)
                break;
            assert(i + 1 < 100000);
            SDL_Delay(1);
        }
        vdp.stop();
    }
    SDL_Quit();
    return 0;
}
