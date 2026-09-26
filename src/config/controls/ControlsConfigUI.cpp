#include "ControlsConfigUI.hpp"
#include "../ControlsConfigStore.hpp"
#include "ControlsConfig.hpp"
#include "KeyBindScreen.hpp"
#include "MainMenuScreen.hpp"
#include "PlayerConfigScreen.hpp"
#include "UIRenderer.hpp"
#include <SDL3/SDL.h>
#include <string>
#include <vector>

// ─── UIState ─────────────────────────────────────────────────────────────────

enum class UIState { MainMenu, Player1Config, Player2Config, Player1KeyBind, Player2KeyBind, Exit };

// ─── Store ↔ runtime conversion ───────────────────────────────────────────────

static const char *MD_BUTTON_NAMES[] = {"Up", "Down", "Left", "Right", "A", "B", "C", "Start", "X", "Y", "Z", "Mode"};

static int mdButtonIndex(const std::string &name) {
    for (int i = 0; i < static_cast<int>(MDButton::COUNT); ++i)
        if (name == MD_BUTTON_NAMES[i])
            return i;
    return -1;
}

static SDL_JoystickID resolveGamepadByGuid(const std::string &guidStr) {
    if (guidStr.empty())
        return 0;
    int             count = 0;
    SDL_JoystickID *ids   = SDL_GetGamepads(&count);
    if (!ids)
        return 0;
    SDL_JoystickID found = 0;
    for (int i = 0; i < count; ++i) {
        SDL_GUID g = SDL_GetJoystickGUIDForID(ids[i]);
        char     buf[33];
        SDL_GUIDToString(g, buf, sizeof(buf));
        if (guidStr == buf) {
            found = ids[i];
            break;
        }
    }
    SDL_free(ids);
    return found;
}

/// Returns the first connected gamepad other than @p excludeId. Used to
/// preview "automatic" gamepad assignment in this UI; the actual game
/// session resolves both players' gamepads jointly the same way.
static SDL_JoystickID firstAvailableGamepad(SDL_JoystickID excludeId) {
    int             count = 0;
    SDL_JoystickID *ids   = SDL_GetGamepads(&count);
    if (!ids)
        return 0;
    SDL_JoystickID found = 0;
    for (int i = 0; i < count; ++i) {
        if (ids[i] != excludeId) {
            found = ids[i];
            break;
        }
    }
    SDL_free(ids);
    return found;
}

/// @param excludeId  A gamepad already claimed by the other player (0 if none),
///                    skipped when resolving an automatic ("no GUID") assignment.
static PlayerConfig toRuntime(const PlayerConfiguration &cfg, SDL_JoystickID excludeId) {
    PlayerConfig pc;
    pc.connected       = cfg.enabled;
    pc.keyboardEnabled = cfg.keyboardEnabled;
    pc.gamepadEnabled  = cfg.gamepadEnabled;
    pc.gamepadGuid     = cfg.gamepadGuid;
    pc.gamepadName     = cfg.gamepadName;

    if (cfg.gamepadEnabled) {
        pc.gamepadId = resolveGamepadByGuid(cfg.gamepadGuid);
        if (pc.gamepadId == 0)
            pc.gamepadId = firstAvailableGamepad(excludeId);
    }

    for (const auto &s : cfg.keyboardBindings) {
        auto at = s.find('@');
        if (at == std::string::npos)
            continue;
        int idx = mdButtonIndex(s.substr(0, at));
        if (idx < 0)
            continue;
        pc.bindings[idx].key = SDL_GetKeyFromName(s.substr(at + 1).c_str());
    }
    for (const auto &s : cfg.gamepadBindings) {
        auto at = s.find('@');
        if (at == std::string::npos)
            continue;
        std::string sdlPart = s.substr(at + 1);
        int         idx     = mdButtonIndex(s.substr(0, at));
        if (idx < 0)
            continue;
        if (sdlPart == "auto")
            pc.bindings[idx].isAutoDir = true;
        else
            pc.bindings[idx].gpButton = SDL_GetGamepadButtonFromString(sdlPart.c_str());
    }
    return pc;
}

static PlayerConfiguration fromRuntime(const PlayerConfig &pc) {
    PlayerConfiguration cfg;
    cfg.enabled         = pc.connected;
    cfg.keyboardEnabled = pc.keyboardEnabled;
    cfg.gamepadEnabled  = pc.gamepadEnabled;
    cfg.gamepadGuid     = pc.gamepadGuid; // preserves an explicit choice; empty stays automatic
    cfg.gamepadName     = pc.gamepadName;

    for (int i = 0; i < static_cast<int>(MDButton::COUNT); ++i) {
        const auto &b = pc.bindings[i];
        if (b.key == SDLK_UNKNOWN)
            continue;
        const char *kn = SDL_GetKeyName(b.key);
        if (kn && kn[0] != '\0')
            cfg.keyboardBindings.push_back(std::string(MD_BUTTON_NAMES[i]) + '@' + kn);
    }
    for (int i = 0; i < static_cast<int>(MDButton::COUNT); ++i) {
        const auto &b = pc.bindings[i];
        if (b.isAutoDir) {
            cfg.gamepadBindings.push_back(std::string(MD_BUTTON_NAMES[i]) + "@auto");
        } else if (b.gpButton != SDL_GAMEPAD_BUTTON_INVALID) {
            const char *gn = SDL_GetGamepadStringForButton(b.gpButton);
            if (gn && gn[0] != '\0')
                cfg.gamepadBindings.push_back(std::string(MD_BUTTON_NAMES[i]) + '@' + gn);
        }
    }
    return cfg;
}

// ─── runControlsConfig ───────────────────────────────────────────────────────

void runControlsConfig() {
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        SDL_Log("SDL init failed: %s", SDL_GetError());
        return;
    }

    SDL_Window *window = SDL_CreateWindow("Controls Configuration", CC_WIN_W, CC_WIN_H, 0);
    if (!window) {
        SDL_Log("Window creation failed: %s", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
        return;
    }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        SDL_Log("Renderer creation failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
        return;
    }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    // ── Open all connected gamepads so events reach every screen ──────────────
    // In SDL3, SDL_EVENT_GAMEPAD_BUTTON_DOWN is only dispatched for open gamepads.
    std::vector<SDL_Gamepad *> openGamepads;
    auto                       openAllGamepads = [&]() {
        int             count = 0;
        SDL_JoystickID *ids   = SDL_GetGamepads(&count);
        if (ids) {
            for (int i = 0; i < count; ++i) {
                if (!SDL_GetGamepadFromID(ids[i])) {
                    SDL_Gamepad *gp = SDL_OpenGamepad(ids[i]);
                    if (gp)
                        openGamepads.push_back(gp);
                }
            }
            SDL_free(ids);
        }
    };
    openAllGamepads();

    // ── Load persisted config ─────────────────────────────────────────────────
    ControlsConfigStore store;
    // Player 1 resolves first so it keeps priority over an automatic
    // assignment; player 2 excludes whatever gamepad player 1 just claimed.
    PlayerConfig p1 = toRuntime(store.player1, 0);
    PlayerConfig p2 = toRuntime(store.player2, p1.gamepadId);

    // ── Screens ───────────────────────────────────────────────────────────────
    UIRenderer         ui(renderer);
    MainMenuScreen     mainMenu;
    PlayerConfigScreen p1Config(1, p1);
    PlayerConfigScreen p2Config(2, p2);
    KeyBindScreen      p1Bind(1, p1);
    KeyBindScreen      p2Bind(2, p2);

    UIState state   = UIState::MainMenu;
    Screen *current = &mainMenu;

    // ── Main loop ─────────────────────────────────────────────────────────────
    bool quit = false;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) {
                quit = true;
                break;
            }
            if (e.type == SDL_EVENT_GAMEPAD_ADDED) {
                SDL_Gamepad *gp = SDL_OpenGamepad(e.gdevice.which);
                if (gp)
                    openGamepads.push_back(gp);
            }
            current->handleEvent(e);
        }

        // ── Render ────────────────────────────────────────────────────────────
        current->render(ui);
        SDL_RenderPresent(renderer);

        // ── State transitions ─────────────────────────────────────────────────
        if (current->isDone()) {
            current->resetDone();

            switch (state) {
                // ── Main menu ─────────────────────────────────────────────────
                case UIState::MainMenu: {
                    auto r = mainMenu.getResult();
                    if (r == MainMenuResult::Exit) {
                        store.player1 = fromRuntime(p1);
                        store.player2 = fromRuntime(p2);
                        store.save();
                        quit = true;
                    } else if (r == MainMenuResult::Player1) {
                        p1Config.reset();
                        state   = UIState::Player1Config;
                        current = &p1Config;
                    } else {
                        p2Config.reset();
                        state   = UIState::Player2Config;
                        current = &p2Config;
                    }
                    break;
                }

                // ── Player 1 config ───────────────────────────────────────────
                case UIState::Player1Config: {
                    auto r = p1Config.getResult();
                    if (r == PlayerConfigResult::Back) {
                        mainMenu.reset();
                        state   = UIState::MainMenu;
                        current = &mainMenu;
                    } else if (r == PlayerConfigResult::TestInputs) {
                        p1Bind.resetToTest();
                        state   = UIState::Player1KeyBind;
                        current = &p1Bind;
                    } else if (r == PlayerConfigResult::BindKeyboard) {
                        p1Bind.reset(KeyBindScreen::BindMode::Keyboard);
                        state   = UIState::Player1KeyBind;
                        current = &p1Bind;
                    } else {
                        p1Bind.reset(KeyBindScreen::BindMode::Gamepad);
                        state   = UIState::Player1KeyBind;
                        current = &p1Bind;
                    }
                    break;
                }

                // ── Player 2 config ───────────────────────────────────────────
                case UIState::Player2Config: {
                    auto r = p2Config.getResult();
                    if (r == PlayerConfigResult::Back) {
                        mainMenu.reset();
                        state   = UIState::MainMenu;
                        current = &mainMenu;
                    } else if (r == PlayerConfigResult::TestInputs) {
                        p2Bind.resetToTest();
                        state   = UIState::Player2KeyBind;
                        current = &p2Bind;
                    } else if (r == PlayerConfigResult::BindKeyboard) {
                        p2Bind.reset(KeyBindScreen::BindMode::Keyboard);
                        state   = UIState::Player2KeyBind;
                        current = &p2Bind;
                    } else {
                        p2Bind.reset(KeyBindScreen::BindMode::Gamepad);
                        state   = UIState::Player2KeyBind;
                        current = &p2Bind;
                    }
                    break;
                }

                // ── Player 1 key bind ─────────────────────────────────────────
                case UIState::Player1KeyBind: {
                    p1Config.reset();
                    state   = UIState::Player1Config;
                    current = &p1Config;
                    break;
                }

                // ── Player 2 key bind ─────────────────────────────────────────
                case UIState::Player2KeyBind: {
                    p2Config.reset();
                    state   = UIState::Player2Config;
                    current = &p2Config;
                    break;
                }

                default:
                    quit = true;
                    break;
            }
        }

        SDL_Delay(16);
    }

    for (SDL_Gamepad *gp : openGamepads)
        SDL_CloseGamepad(gp);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD);
}
