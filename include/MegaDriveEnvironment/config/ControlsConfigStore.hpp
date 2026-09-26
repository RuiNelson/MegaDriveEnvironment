#pragma once
#include <string>
#include <vector>

/// @brief Persistent configuration for a single player.
///
/// A player may have a keyboard binding set, a gamepad binding set, both, or
/// neither, independently of one another. When both are enabled, either
/// device can drive the same Mega Drive button at the same time.
///
/// Bindings are encoded as strings of the form "MdButton@SDLCode", e.g.
/// "A@Z" (keyboard) or "A@a" (gamepad), "Up@auto" (gamepad auto-direction).
/// The caller is responsible for parsing these strings and resolving SDL
/// key/button codes at runtime.
///
/// SDL_JoystickID is omitted — it is runtime-assigned and must be resolved
/// from gamepadGuid by the caller after loading. An empty gamepadGuid with
/// gamepadEnabled set means "automatically use the first gamepad available",
/// resolved with priority given to player 1 over player 2.
struct PlayerConfiguration {
    bool                     enabled         = false;
    bool                     keyboardEnabled = false;
    bool                     gamepadEnabled  = false;
    std::string              gamepadGuid; ///< SDL_GUIDToString of the assigned gamepad; empty = automatic.
    std::string              gamepadName; ///< Human-readable gamepad name (for display).
    std::vector<std::string> keyboardBindings; ///< "MdButton@SDLKeyName" tuples.
    std::vector<std::string> gamepadBindings;  ///< "MdButton@SDLButtonName" tuples, or "MdButton@auto".
};

/// @brief Persistence layer for controls configuration.
///
/// Loads from and saves to @c controls.yaml in the current working directory.
/// Falls back to built-in defaults if the file is absent or malformed. Files
/// written by older versions of this library (a single "device" plus
/// "bindings" list per player) are migrated transparently: the recorded
/// bindings become the corresponding device's list, and the other device is
/// backfilled with its own built-in defaults so the player ends up with both
/// a keyboard and a gamepad configuration.
class ControlsConfigStore {
    public:
    ControlsConfigStore(); ///< Loads from controls.yaml; applies defaults on failure.
    void save();           ///< Saves to controls.yaml.

    PlayerConfiguration player1;
    PlayerConfiguration player2;
};
