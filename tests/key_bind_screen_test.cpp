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

SDL_Event keyDownEvent(SDL_Keycode key) {
    SDL_Event event{};
    event.type       = SDL_EVENT_KEY_DOWN;
    event.key.key    = key;
    event.key.repeat = false;
    return event;
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
    config.gamepadEnabled = true;

    KeyBindScreen screen(1, config);
    screen.reset(KeyBindScreen::BindMode::Gamepad);
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
    config.gamepadEnabled = true;

    KeyBindScreen screen(1, config);
    screen.reset(KeyBindScreen::BindMode::Gamepad);
    screen.handleEvent(gamepadButtonEvent(SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_GAMEPAD_BUTTON_SOUTH, 1 * kMS));
    screen.handleEvent(gamepadButtonEvent(SDL_EVENT_GAMEPAD_BUTTON_UP, SDL_GAMEPAD_BUTTON_SOUTH, 300 * kMS));
    screen.handleEvent(timerEvent(1200 * kMS));

    assert(screen.m_bindIdx == 0);
    assert(screen.m_temp.bindings[int(MDButton::A)].gpButton == SDL_GAMEPAD_BUTTON_INVALID);
}

void testGamepadBackAloneCanBeBound() {
    PlayerConfig config;
    config.connected  = true;
    config.gamepadEnabled = true;

    KeyBindScreen screen(1, config);
    screen.reset(KeyBindScreen::BindMode::Gamepad);
    screen.handleEvent(gamepadButtonEvent(SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_GAMEPAD_BUTTON_BACK, 1 * kMS));
    screen.handleEvent(timerEvent(1200 * kMS));

    assert(!screen.isDone());
    assert(!screen.wasCancelled());
    assert(screen.m_bindIdx == 1);
    assert(screen.m_temp.bindings[int(MDButton::A)].gpButton == SDL_GAMEPAD_BUTTON_BACK);
}

void testGamepadSelectStartCancelsBinding() {
    PlayerConfig config;
    config.connected  = true;
    config.gamepadEnabled = true;

    KeyBindScreen screen(1, config);
    screen.reset(KeyBindScreen::BindMode::Gamepad);
    screen.handleEvent(gamepadButtonEvent(SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_GAMEPAD_BUTTON_BACK, 1 * kMS));
    screen.handleEvent(gamepadButtonEvent(SDL_EVENT_GAMEPAD_BUTTON_DOWN, SDL_GAMEPAD_BUTTON_START, 2 * kMS));

    assert(screen.isDone());
    assert(screen.wasCancelled());
}

void testGamepadStartAloneCanStillBeBound() {
    PlayerConfig config;
    config.connected  = true;
    config.gamepadEnabled = true;

    KeyBindScreen screen(1, config);
    screen.reset(KeyBindScreen::BindMode::Gamepad);
    holdButton(screen, SDL_GAMEPAD_BUTTON_START, 1 * kMS);

    assert(!screen.isDone());
    assert(!screen.wasCancelled());
    assert(screen.m_bindIdx == 1);
    assert(screen.m_temp.bindings[int(MDButton::A)].gpButton == SDL_GAMEPAD_BUTTON_START);
}

void testGamepadBindingAdvancesToTesterAfterHeldButtons() {
    PlayerConfig config;
    config.connected  = true;
    config.gamepadEnabled = true;

    KeyBindScreen screen(1, config);
    screen.reset(KeyBindScreen::BindMode::Gamepad);

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
    config.gamepadEnabled = true;

    KeyBindScreen screen(1, config);
    screen.resetToTest();
    screen.handleEvent(gamepadButtonDown(SDL_GAMEPAD_BUTTON_EAST));

    assert(screen.isDone());
    assert(!screen.wasCancelled());
}

void testGamepadBackStillSavesFromTester() {
    PlayerConfig config;
    config.connected  = true;
    config.gamepadEnabled = true;

    KeyBindScreen screen(1, config);
    screen.resetToTest();
    screen.handleEvent(gamepadButtonDown(SDL_GAMEPAD_BUTTON_BACK));

    assert(screen.isDone());
    assert(!screen.wasCancelled());
}

void testKeyboardOnlyBindingSessionNeverTouchesGamepadFields() {
    PlayerConfig config;
    config.connected       = true;
    config.keyboardEnabled = true;
    config.gamepadEnabled  = true; // also enabled, but this session must not require it

    KeyBindScreen screen(1, config);
    screen.reset(KeyBindScreen::BindMode::Keyboard);

    // A keyboard session lists exactly the 12 buttons, never gamepad targets,
    // so a player without a gamepad can finish it without ever waiting on a
    // button press that cannot happen.
    assert(screen.m_targets.size() == 12);
    assert(screen.m_targets[0] == MDButton::Up);

    static constexpr SDL_Keycode kKeyboardDefaults[12] = {
        SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT, SDLK_Z, SDLK_X, SDLK_C, SDLK_V, SDLK_A, SDLK_S, SDLK_D, SDLK_F,
    };
    for (SDL_Keycode key : kKeyboardDefaults)
        screen.handleEvent(keyDownEvent(key));

    assert(screen.m_phase == KeyBindScreen::Phase::Testing);
    assert(screen.m_temp.bindings[int(MDButton::Up)].key == SDLK_UP);
    assert(screen.m_temp.bindings[int(MDButton::A)].key == SDLK_Z);
    // No gamepad button was ever pressed during this session.
    assert(screen.m_temp.bindings[int(MDButton::A)].gpButton == SDL_GAMEPAD_BUTTON_INVALID);
}

void testGamepadOnlyBindingSessionPreservesExistingKeyboardBindings() {
    PlayerConfig config;
    config.connected                     = true;
    config.keyboardEnabled               = true;
    config.gamepadEnabled                = true;
    config.bindings[int(MDButton::A)].key = SDLK_Z; // bound in an earlier, separate keyboard session

    KeyBindScreen screen(1, config);
    screen.reset(KeyBindScreen::BindMode::Gamepad);

    // A gamepad session lists only the 8 face buttons (directions auto-assigned).
    assert(screen.m_targets.size() == 8);
    assert(screen.m_targets[0] == MDButton::A);

    holdButton(screen, SDL_GAMEPAD_BUTTON_SOUTH, 1 * kMS);
    holdButton(screen, SDL_GAMEPAD_BUTTON_EAST, 1100 * kMS);
    holdButton(screen, SDL_GAMEPAD_BUTTON_WEST, 2200 * kMS);
    holdButton(screen, SDL_GAMEPAD_BUTTON_START, 3300 * kMS);
    holdButton(screen, SDL_GAMEPAD_BUTTON_NORTH, 4400 * kMS);
    holdButton(screen, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 5500 * kMS);
    holdButton(screen, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 6600 * kMS);
    holdButton(screen, SDL_GAMEPAD_BUTTON_MISC1, 7700 * kMS);

    assert(screen.m_phase == KeyBindScreen::Phase::Testing);
    // Binding A on the gamepad must not disturb A's independent, already-set keyboard key.
    assert(screen.m_temp.bindings[int(MDButton::A)].key == SDLK_Z);
    assert(screen.m_temp.bindings[int(MDButton::A)].gpButton == SDL_GAMEPAD_BUTTON_SOUTH);
    assert(screen.m_temp.bindings[int(MDButton::Mode)].gpButton == SDL_GAMEPAD_BUTTON_MISC1);
}

void testCancellingGamepadSessionDiscardsOnlyThatSessionsChanges() {
    PlayerConfig config;
    config.connected                     = true;
    config.keyboardEnabled               = true;
    config.gamepadEnabled                = true;
    config.bindings[int(MDButton::A)].key = SDLK_Z; // already saved from an earlier keyboard session

    KeyBindScreen screen(1, config);
    screen.reset(KeyBindScreen::BindMode::Gamepad);
    holdButton(screen, SDL_GAMEPAD_BUTTON_SOUTH, 1 * kMS);
    screen.handleEvent(keyDownEvent(SDLK_ESCAPE));

    assert(screen.isDone());
    assert(screen.wasCancelled());
    // The authoritative config (already saved keyboard key) is untouched by the cancel.
    assert(config.bindings[int(MDButton::A)].key == SDLK_Z);
    assert(config.bindings[int(MDButton::A)].gpButton == SDL_GAMEPAD_BUTTON_INVALID);
}

} // namespace

int main() {
    testGamepadBindingRequiresHoldBeforeAdvance();
    testGamepadBindingCancelsShortPress();
    testGamepadBackAloneCanBeBound();
    testGamepadSelectStartCancelsBinding();
    testGamepadStartAloneCanStillBeBound();
    testGamepadBindingAdvancesToTesterAfterHeldButtons();
    testGamepadEastSavesFromTester();
    testGamepadBackStillSavesFromTester();
    testKeyboardOnlyBindingSessionNeverTouchesGamepadFields();
    testGamepadOnlyBindingSessionPreservesExistingKeyboardBindings();
    testCancellingGamepadSessionDiscardsOnlyThatSessionsChanges();
    return 0;
}
