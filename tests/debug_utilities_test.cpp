#include "system/MegaDriveEnvironment.hpp"

#include <SDL3/SDL.h>

#include <atomic>
#include <cassert>
#include <thread>

namespace {

class DebugUtilitiesEnvironment final : public MegaDriveEnvironment {
  public:
    DebugUtilitiesEnvironment()
        : MegaDriveEnvironment(VDP::InternalTimer, VDP::Scale1x, VDP::HardwareSpriteLimit, 0) {
        setDebugUtilities(false);
    }

    std::atomic<bool> running{false};
    std::atomic<unsigned> optionHotkeyCount{0};

  protected:
    void run() override {
        running.store(true, std::memory_order_release);
        while (!shouldQuit())
            pace();
    }

    void handleOptionHotkey(OptionHotkeyCode) override {
        optionHotkeyCount.fetch_add(1, std::memory_order_relaxed);
    }
};

void pushKey(SDL_Keycode key, SDL_Keymod modifiers) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = key;
    event.key.mod = modifiers;
    event.key.repeat = false;
    const bool pushed = SDL_PushEvent(&event);
    assert(pushed);
    (void)pushed;
}

} // namespace

int main() {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    DebugUtilitiesEnvironment environment;

    std::thread input([&] {
        while (!environment.running.load(std::memory_order_acquire))
            std::this_thread::yield();

        pushKey(SDLK_L, SDL_KMOD_ALT);
        pushKey(SDLK_Q, SDL_KMOD_CTRL);
    });

    environment.boot();
    input.join();

    assert(environment.shouldQuit());
    assert(environment.optionHotkeyCount.load(std::memory_order_relaxed) == 0);
}
