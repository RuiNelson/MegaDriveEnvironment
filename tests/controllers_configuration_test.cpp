#include "system/controllers/Controllers.hpp"

#include <SDL3/SDL.h>

#include <cassert>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
#include <utility>

namespace {

PlayerConfiguration keyboardPlayer(std::string binding) {
    PlayerConfiguration player;
    player.enabled         = true;
    player.keyboardEnabled = true;
    player.keyboardBindings.push_back(std::move(binding));
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

/// Drains the SDL event queue, pumping a few times so any pending virtual
/// joystick state change (button, connect, disconnect) is turned into a real
/// event and delivered to registered event watches (e.g. Controllers).
void pumpEvents() {
    SDL_Event event;
    for (int i = 0; i < 10; ++i) {
        while (SDL_PollEvent(&event)) {
        }
        SDL_Delay(2);
    }
}

/// RAII wrapper around an SDL virtual joystick presented as a gamepad, used
/// to exercise real gamepad event delivery without physical hardware.
class VirtualGamepad {
    public:
    explicit VirtualGamepad(const char *name) {
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type     = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes    = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name     = name;

        id_ = SDL_AttachVirtualJoystick(&desc);
        assert(id_ != 0);
        joystick_ = SDL_OpenJoystick(id_);
        assert(joystick_ != nullptr);
        pumpEvents();
    }

    ~VirtualGamepad() {
        detach();
    }

    VirtualGamepad(const VirtualGamepad &)            = delete;
    VirtualGamepad &operator=(const VirtualGamepad &) = delete;

    SDL_JoystickID id() const {
        return id_;
    }

    void press(SDL_GamepadButton button, bool down) {
        SDL_SetJoystickVirtualButton(joystick_, button, down);
        pumpEvents();
    }

    void detach() {
        if (joystick_) {
            SDL_CloseJoystick(joystick_);
            joystick_ = nullptr;
        }
        if (id_ != 0) {
            SDL_DetachVirtualJoystick(id_);
            id_ = 0;
            pumpEvents();
        }
    }

    private:
    SDL_JoystickID id_       = 0;
    SDL_Joystick  *joystick_ = nullptr;
};

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
    assert(loaded.player1.keyboardBindings.size() == 1);
    assert(loaded.player1.keyboardBindings[0] == "Start@Return");
    assert(loaded.player2.enabled);
    assert(loaded.player2.keyboardBindings.size() == 1);
    assert(loaded.player2.keyboardBindings[0] == "B@Space");
}

void testDefaultsEnableKeyboardAndGamepadForBothPlayers() {
    std::remove("controls.yaml"); // guarantee the "file missing" fallback path

    ControlsConfigStore configuration;

    assert(configuration.player1.enabled);
    assert(configuration.player1.keyboardEnabled);
    assert(!configuration.player1.keyboardBindings.empty());
    assert(configuration.player1.gamepadEnabled);
    assert(!configuration.player1.gamepadBindings.empty());
    assert(configuration.player1.gamepadGuid.empty()); // automatic: first gamepad found

    assert(configuration.player2.enabled);
    assert(!configuration.player2.keyboardEnabled);
    assert(configuration.player2.gamepadEnabled);
    assert(!configuration.player2.gamepadBindings.empty());
    assert(configuration.player2.gamepadGuid.empty()); // automatic: second gamepad found
}

void testLegacyYamlIsMigratedToDualDeviceSchema() {
    {
        std::ofstream out("controls.yaml");
        out << "player1:\n"
               "  enabled: true\n"
               "  device: 1\n"
               "  gamepad_guid: \"deadbeef\"\n"
               "  gamepad_name: \"Old Pad\"\n"
               "  bindings:\n"
               "    - \"A@a\"\n"
               "player2:\n"
               "  enabled: false\n"
               "  device: 0\n"
               "  bindings:\n"
               "    - \"A@Z\"\n";
    }

    ControlsConfigStore configuration;

    // Legacy Gamepad device: recorded binding and GUID are kept, keyboard is
    // backfilled with the built-in defaults.
    assert(configuration.player1.enabled);
    assert(configuration.player1.gamepadEnabled);
    assert(configuration.player1.gamepadGuid == "deadbeef");
    assert(configuration.player1.gamepadBindings.size() == 1);
    assert(configuration.player1.gamepadBindings[0] == "A@a");
    assert(configuration.player1.keyboardEnabled);
    assert(!configuration.player1.keyboardBindings.empty());

    // Legacy Keyboard device: recorded binding is kept, gamepad is backfilled
    // with the built-in defaults and automatic assignment (no GUID).
    assert(!configuration.player2.enabled); // the legacy "enabled" flag itself is preserved as-is
    assert(configuration.player2.keyboardEnabled);
    assert(configuration.player2.keyboardBindings.size() == 1);
    assert(configuration.player2.keyboardBindings[0] == "A@Z");
    assert(configuration.player2.gamepadEnabled);
    assert(configuration.player2.gamepadGuid.empty());
    assert(!configuration.player2.gamepadBindings.empty());

    std::remove("controls.yaml");
}

void testKeyboardInputCapture() {
    Controllers controllers(nullptr);
    assert(!controllers.consumeCapturedInput().has_value());

    controllers.beginInputCapture();
    assert(controllers.inputCapturePending());
    controllers.cancelInputCapture();
    assert(!controllers.inputCapturePending());

    pushKey(SDLK_Z, SDL_SCANCODE_Z, true);
    assert(!controllers.consumeCapturedInput().has_value());

    controllers.beginInputCapture();
    pushKey(SDLK_Z, SDL_SCANCODE_Z, true);
    assert(!controllers.inputCapturePending());

    std::optional<CapturedInput> captured = controllers.consumeCapturedInput();
    assert(captured.has_value());
    assert(captured->deviceType == InputDevice::Keyboard);
    assert(captured->key == SDLK_Z);
    assert(captured->scancode == SDL_SCANCODE_Z);
    assert(captured->sdlName == "Z");
    assert(!controllers.consumeCapturedInput().has_value());

    controllers.beginInputCapture(20);
    pushKey(SDLK_X, SDL_SCANCODE_X, true);
    pushKey(SDLK_X, SDL_SCANCODE_X, false);
    assert(controllers.inputCapturePending());
    assert(!controllers.consumeCapturedInput().has_value());

    pushKey(SDLK_C, SDL_SCANCODE_C, true);
    SDL_Delay(25);
    captured = controllers.consumeCapturedInput();
    assert(captured.has_value());
    assert(captured->deviceType == InputDevice::Keyboard);
    assert(captured->key == SDLK_C);
    assert(captured->scancode == SDL_SCANCODE_C);
    assert(captured->sdlName == "C");
    assert(!controllers.inputCapturePending());
    pushKey(SDLK_C, SDL_SCANCODE_C, false);
}

void testAvailableGamepadsWrapper() {
    const auto gamepads = Controllers::availableGamepads();
    for (const AvailableGamepad &gamepad : gamepads) {
        assert(gamepad.id != 0);
        assert(gamepad.guid.size() == 32);
    }
}

void testKeyboardAndGamepadDriveTheSameButtonIndependently() {
    VirtualGamepad pad("Test Pad");

    PlayerConfiguration player;
    player.enabled           = true;
    player.keyboardEnabled   = true;
    player.keyboardBindings  = {"A@Z"};
    player.gamepadEnabled    = true;
    player.gamepadBindings   = {"A@a"}; // "a" is SDL's canonical name for SDL_GAMEPAD_BUTTON_SOUTH

    ControlsConfigStore configuration;
    configuration.player1 = player;
    configuration.player2 = {};

    Controllers controllers(nullptr, configuration);

    pushKey(SDLK_Z, SDL_SCANCODE_Z, true);
    assert(controllers.getCurrentState().player1.a);

    pad.press(SDL_GAMEPAD_BUTTON_SOUTH, true);
    assert(controllers.getCurrentState().player1.a);

    // Releasing one device must not clear a button the other device still holds.
    pushKey(SDLK_Z, SDL_SCANCODE_Z, false);
    assert(controllers.getCurrentState().player1.a);

    pad.press(SDL_GAMEPAD_BUTTON_SOUTH, false);
    assert(!controllers.getCurrentState().player1.a);
}

void testGamepadDisconnectPreservesKeyboardInput() {
    VirtualGamepad pad("Test Pad");

    PlayerConfiguration player;
    player.enabled          = true;
    player.keyboardEnabled  = true;
    player.keyboardBindings = {"A@Z"};
    player.gamepadEnabled   = true;
    player.gamepadBindings  = {"A@a"};

    ControlsConfigStore configuration;
    configuration.player1 = player;
    configuration.player2 = {};

    Controllers controllers(nullptr, configuration);

    pushKey(SDLK_Z, SDL_SCANCODE_Z, true);
    pad.press(SDL_GAMEPAD_BUTTON_SOUTH, true);
    assert(controllers.getCurrentState().player1.a);
    assert(controllers.getCurrentState().player1.connected);

    pad.detach();
    assert(controllers.getCurrentState().player1.a);         // keyboard Z is still held
    assert(controllers.getCurrentState().player1.connected); // keyboard alone keeps the player connected

    pushKey(SDLK_Z, SDL_SCANCODE_Z, false);
}

void testAutomaticGamepadAssignmentPrefersPlayerOne() {
    VirtualGamepad pad("Test Pad");

    PlayerConfiguration player;
    player.enabled         = true;
    player.gamepadEnabled  = true;
    player.gamepadBindings = {"A@a"};
    // gamepadGuid left empty on both players: automatic assignment.

    ControlsConfigStore configuration;
    configuration.player1 = player;
    configuration.player2 = player;

    Controllers controllers(nullptr, configuration);

    pad.press(SDL_GAMEPAD_BUTTON_SOUTH, true);
    const auto state = controllers.getCurrentState();
    assert(state.player1.a);
    assert(!state.player2.a); // the only gamepad is claimed by player 1, not shared

    pad.press(SDL_GAMEPAD_BUTTON_SOUTH, false);
}

void testAutomaticGamepadAssignmentGivesEachPlayerADifferentDevice() {
    VirtualGamepad padA("Test Pad A");
    VirtualGamepad padB("Test Pad B");

    // Enumeration order determines automatic-assignment priority (player 1
    // gets the first entry); resolve it here instead of assuming attach order.
    const auto avail = Controllers::availableGamepads();
    assert(avail.size() == 2);
    VirtualGamepad &first  = (avail[0].id == padA.id()) ? padA : padB;
    VirtualGamepad &second = (avail[0].id == padA.id()) ? padB : padA;

    PlayerConfiguration player;
    player.enabled         = true;
    player.gamepadEnabled  = true;
    player.gamepadBindings = {"A@a"};

    ControlsConfigStore configuration;
    configuration.player1 = player;
    configuration.player2 = player;

    Controllers controllers(nullptr, configuration);

    first.press(SDL_GAMEPAD_BUTTON_SOUTH, true);
    auto state = controllers.getCurrentState();
    assert(state.player1.a);
    assert(!state.player2.a);
    first.press(SDL_GAMEPAD_BUTTON_SOUTH, false);

    second.press(SDL_GAMEPAD_BUTTON_SOUTH, true);
    state = controllers.getCurrentState();
    assert(!state.player1.a);
    assert(state.player2.a);
    second.press(SDL_GAMEPAD_BUTTON_SOUTH, false);
}

} // namespace

int main() {
    SDL_InitSubSystem(SDL_INIT_EVENTS | SDL_INIT_GAMEPAD);

    testRuntimeReconfiguration();
    testConfigurationPersistence();
    testDefaultsEnableKeyboardAndGamepadForBothPlayers();
    testLegacyYamlIsMigratedToDualDeviceSchema();
    testKeyboardInputCapture();
    testAvailableGamepadsWrapper();
    testKeyboardAndGamepadDriveTheSameButtonIndependently();
    testGamepadDisconnectPreservesKeyboardInput();
    testAutomaticGamepadAssignmentPrefersPlayerOne();
    testAutomaticGamepadAssignmentGivesEachPlayerADifferentDevice();
}
