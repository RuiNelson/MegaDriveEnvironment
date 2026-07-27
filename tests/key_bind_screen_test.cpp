#define private public
#include "config/controls/KeyBindScreen.hpp"
#undef private

#include <SDL3/SDL.h>
#include <cassert>

namespace {

constexpr Uint64 kMS = 1'000'000ull;

SDL_Event gamepadButtonEvent(SDL_EventType type, SDL_GamepadButton button, Uint64 timestampNS) {
    SDL_Event event{};
    event.type           = type;
    event.gbutton.timestamp = timestampNS;
    event.gbutton.button = button;
    return event;
}

SDL_Event gamepadButtonDown(SDL_GamepadButton button) {
    return gamepadButtonEvent(SDL_EVENT_GAMEPAD_BUTTON_DOWN, button, SDL_GetTicksNS());
}

SDL_Event timerEvent(Uint64 timestampNS) {
    SDL_Event event{};
    event.type             = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    event.gaxis.timestamp  = timestampNS;
    event.gaxis.axis       = SDL_GAMEPAD_AXIS_LEFTX;
    return event;
}

void holdButton(KeyBindScreen &screen, SDL_GamepadButton button, Uint64 startNS) {
    screen.handleEvent(gamepadButtonEvent(SDL_EVENT_GAMEPAD_BUTTON_DOWN, button, startNS));
    screen.handleEvent(timerEvent(startNS + 400 * kMS));
    screen.handleEvent(timerEvent(startNS + 1000 * kMS));
}

void testGamepadBindingRequiresHoldBeforeAdvance() {
    PlayerConfig config;
    config.connected  = true;
    config.deviceType = DeviceType::Gamepad;

    KeyBindScreen screen(1, config);
    screen.reset();
    screen.handleEvent(gamepadButtonEvent(SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_GAMEPAD_BUTTON_SOUTH, 1 * kMS));

    screen.handleEvent(timerEvent(400 * kMS));
    assert(screen.m_bindIdx == 0);
    assert(screen.m_temp.bindings[int(MDButton::A)].gpButton == SDL_GAMEPAD_BUTTON_INVALID);

    screen.handleEvent(timerEvent(401 * kMS));
    assert(screen.m_bindIdx == 0);
    assert(screen.m_temp.bindings[int(MDButton::A)].gpButton == SDL_GAMEPAD_BUTTON_SOUTH);

    screen.handleEvent(timerEvent(1000 * kMS));
    assert(screen.m_bindIdx == 0);

    screen.handleEvent(timerEvent(1001 * kMS));
    assert(screen.m_bindIdx == 1);
}

void testGamepadBindingCancelsShortPress() {
    PlayerConfig config;
    config.connected  = true;
    config.deviceType = DeviceType::Gamepad;

    KeyBindScreen screen(1, config);
    screen.reset();
    screen.handleEvent(gamepadButtonEvent(SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_GAMEPAD_BUTTON_SOUTH, 1 * kMS));
    screen.handleEvent(gamepadButtonEvent(SDL_EVENT_GAMEPAD_BUTTON_UP, SDL_GAMEPAD_BUTTON_SOUTH, 300 * kMS));
    screen.handleEvent(timerEvent(1200 * kMS));

    assert(screen.m_bindIdx == 0);
    assert(screen.m_temp.bindings[int(MDButton::A)].gpButton == SDL_GAMEPAD_BUTTON_INVALID);
}

void testGamepadBindingAdvancesToTesterAfterHeldButtons() {
    PlayerConfig config;
    config.connected  = true;
    config.deviceType = DeviceType::Gamepad;

    KeyBindScreen screen(1, config);
    screen.reset();

    holdButton(screen, SDL_GAMEPAD_BUTTON_SOUTH, 1 * kMS);
    holdButton(screen, SDL_GAMEPAD_BUTTON_EAST, 1100 * kMS);
    holdButton(screen, SDL_GAMEPAD_BUTTON_WEST, 2200 * kMS);
    holdButton(screen, SDL_GAMEPAD_BUTTON_START, 3300 * kMS);
    holdButton(screen, SDL_GAMEPAD_BUTTON_NORTH, 4400 * kMS);
    holdButton(screen, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 5500 * kMS);
    holdButton(screen, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 6600 * kMS);
    holdButton(screen, SDL_GAMEPAD_BUTTON_MISC1, 7700 * kMS);

    assert(screen.m_phase == KeyBindScreen::Phase::Testing);
    screen.handleEvent(gamepadButtonDown(SDL_GAMEPAD_BUTTON_EAST));
    assert(screen.isDone());
    assert(config.bindings[int(MDButton::A)].gpButton == SDL_GAMEPAD_BUTTON_SOUTH);
    assert(config.bindings[int(MDButton::Mode)].gpButton == SDL_GAMEPAD_BUTTON_MISC1);
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
    testGamepadBindingRequiresHoldBeforeAdvance();
    testGamepadBindingCancelsShortPress();
    testGamepadBindingAdvancesToTesterAfterHeldButtons();
    testGamepadEastSavesFromTester();
    testGamepadBackStillSavesFromTester();
    return 0;
}
