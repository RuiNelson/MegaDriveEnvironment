#include "config/controls/KeyBindScreen.hpp"

#include <SDL3/SDL.h>
#include <cassert>

namespace {

SDL_Event gamepadButtonDown(SDL_GamepadButton button) {
    SDL_Event event{};
    event.type           = SDL_EVENT_GAMEPAD_BUTTON_DOWN;
    event.gbutton.button = button;
    return event;
}

void testGamepadEastSavesFromTester() {
    PlayerConfig config;
    config.connected  = true;
    config.deviceType = DeviceType::Gamepad;

    KeyBindScreen screen(1, config);
    screen.resetToTest();
    screen.handleEvent(gamepadButtonDown(SDL_GAMEPAD_BUTTON_EAST));

    assert(screen.isDone());
    assert(!screen.wasCancelled());
}

void testGamepadBackStillSavesFromTester() {
    PlayerConfig config;
    config.connected  = true;
    config.deviceType = DeviceType::Gamepad;

    KeyBindScreen screen(1, config);
    screen.resetToTest();
    screen.handleEvent(gamepadButtonDown(SDL_GAMEPAD_BUTTON_BACK));

    assert(screen.isDone());
    assert(!screen.wasCancelled());
}

} // namespace

int main() {
    testGamepadEastSavesFromTester();
    testGamepadBackStillSavesFromTester();
    return 0;
}
