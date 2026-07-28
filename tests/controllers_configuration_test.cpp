#include "system/controllers/Controllers.hpp"

#include <SDL3/SDL.h>

#include <cassert>
#include <string>
#include <utility>

namespace {

PlayerConfiguration keyboardPlayer(std::string binding) {
    PlayerConfiguration player;
    player.enabled = true;
    player.deviceType = InputDevice::Keyboard;
    player.bindings.push_back(std::move(binding));
    return player;
}

void pushKey(SDL_Keycode key, SDL_Scancode scancode, bool pressed) {
    SDL_Event event{};
    event.type = pressed ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.key = key;
    event.key.scancode = scancode;
    event.key.repeat = false;
    const bool pushed = SDL_PushEvent(&event);
    assert(pushed);
    (void)pushed;

    while (SDL_PollEvent(&event)) {
    }
}

void testRuntimeReconfiguration() {
    ControlsConfigStore configuration;
    configuration.player1 = keyboardPlayer("A@Z");
    configuration.player2 = {};

    Controllers controllers(nullptr, configuration);
    pushKey(SDLK_Z, SDL_SCANCODE_Z, true);
    assert(controllers.getCurrentState().player1.a);

    controllers.setPlayerConfiguration(1, keyboardPlayer("A@Q"));
    assert(!controllers.getCurrentState().player1.a);

    pushKey(SDLK_Z, SDL_SCANCODE_Z, true);
    assert(!controllers.getCurrentState().player1.a);

    pushKey(SDLK_Q, SDL_SCANCODE_Q, true);
    assert(controllers.getCurrentState().player1.a);
}

void testConfigurationPersistence() {
    ControlsConfigStore configuration;
    configuration.player1 = keyboardPlayer("Start@Return");
    configuration.player2 = keyboardPlayer("B@Space");

    Controllers controllers(nullptr);
    controllers.setConfigurationAndSave(configuration);

    ControlsConfigStore loaded;
    assert(loaded.player1.enabled);
    assert(loaded.player1.bindings.size() == 1);
    assert(loaded.player1.bindings[0] == "Start@Return");
    assert(loaded.player2.enabled);
    assert(loaded.player2.bindings.size() == 1);
    assert(loaded.player2.bindings[0] == "B@Space");
}

void testAvailableGamepadsWrapper() {
    const auto gamepads = Controllers::availableGamepads();
    for (const AvailableGamepad &gamepad : gamepads) {
        assert(gamepad.id != 0);
        assert(gamepad.guid.size() == 32);
    }
}

} // namespace

int main() {
    SDL_InitSubSystem(SDL_INIT_EVENTS | SDL_INIT_GAMEPAD);

    testRuntimeReconfiguration();
    testConfigurationPersistence();
    testAvailableGamepadsWrapper();
}
