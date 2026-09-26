#include "ControlsConfigStore.hpp"
#include <fstream>
#include <yaml-cpp/yaml.h>

static constexpr const char *YAML_PATH = "controls.yaml";

// ─── Defaults ─────────────────────────────────────────────────────────────────

static const std::vector<std::string> &defaultKeyboardBindings() {
    static const std::vector<std::string> kDefaults = {
        "Up@Up", "Down@Down", "Left@Left", "Right@Right", "A@Z", "B@X", "C@C", "Start@V", "X@A", "Y@S", "Z@D", "Mode@F",
    };
    return kDefaults;
}

/// Face-button names follow SDL's Xbox-layout string form: "a"/"b"/"x"/"y"
/// correspond to SDL_GAMEPAD_BUTTON_SOUTH/EAST/WEST/NORTH. Directions use the
/// "auto" sentinel (D-pad OR left stick); Mode is intentionally left unbound.
static const std::vector<std::string> &defaultGamepadBindings() {
    static const std::vector<std::string> kDefaults = {
        "Up@auto",
        "Down@auto",
        "Left@auto",
        "Right@auto",
        "A@leftshoulder",
        "B@x",
        "C@a",
        "Start@start",
        "X@rightshoulder",
        "Y@y",
        "Z@b",
    };
    return kDefaults;
}

static void applyDefaults(PlayerConfiguration &p1, PlayerConfiguration &p2) {
    p1.enabled          = true;
    p1.keyboardEnabled  = true;
    p1.keyboardBindings = defaultKeyboardBindings();
    p1.gamepadEnabled   = true;
    p1.gamepadBindings  = defaultGamepadBindings();
    // p1.gamepadGuid stays empty: automatically assigned to the first
    // available gamepad, with priority over player 2.

    p2.enabled         = true;
    p2.keyboardEnabled = false;
    p2.gamepadEnabled  = true;
    p2.gamepadBindings = defaultGamepadBindings();
    // p2.gamepadGuid stays empty: automatically assigned to the second
    // available gamepad (the first is reserved for player 1).
}

// ─── YAML serialization ───────────────────────────────────────────────────────

static YAML::Node playerToNode(const PlayerConfiguration &p) {
    YAML::Node n;
    n["enabled"]          = p.enabled;
    n["keyboard_enabled"] = p.keyboardEnabled;
    n["gamepad_enabled"]  = p.gamepadEnabled;
    n["gamepad_guid"]     = p.gamepadGuid;
    n["gamepad_name"]     = p.gamepadName;

    YAML::Node keyboardBindings;
    for (const auto &b : p.keyboardBindings)
        keyboardBindings.push_back(b);
    n["keyboard_bindings"] = keyboardBindings;

    YAML::Node gamepadBindings;
    for (const auto &b : p.gamepadBindings)
        gamepadBindings.push_back(b);
    n["gamepad_bindings"] = gamepadBindings;

    return n;
}

/// Loads one player's configuration, migrating the pre-dual-device schema
/// (a single "device" plus one "bindings" list) transparently: the recorded
/// bindings become the matching device's list, and both devices end up
/// enabled with the missing side backfilled from the built-in defaults.
static PlayerConfiguration nodeToPlayer(const YAML::Node &n) {
    PlayerConfiguration p;
    p.enabled     = n["enabled"].as<bool>(false);
    p.gamepadGuid = n["gamepad_guid"].as<std::string>("");
    p.gamepadName = n["gamepad_name"].as<std::string>("");

    const bool hasNewSchema = n["keyboard_bindings"].IsDefined() || n["gamepad_bindings"].IsDefined() ||
                              n["keyboard_enabled"].IsDefined() || n["gamepad_enabled"].IsDefined();

    if (hasNewSchema) {
        p.keyboardEnabled = n["keyboard_enabled"].as<bool>(false);
        p.gamepadEnabled  = n["gamepad_enabled"].as<bool>(false);
        for (const auto &item : n["keyboard_bindings"])
            p.keyboardBindings.push_back(item.as<std::string>());
        for (const auto &item : n["gamepad_bindings"])
            p.gamepadBindings.push_back(item.as<std::string>());
    } else {
        std::vector<std::string> legacyBindings;
        for (const auto &item : n["bindings"])
            legacyBindings.push_back(item.as<std::string>());

        if (n["device"].as<int>(0) == 1) { // legacy Gamepad device
            p.gamepadEnabled  = true;
            p.gamepadBindings = legacyBindings;
        } else { // legacy Keyboard device
            p.keyboardEnabled   = true;
            p.keyboardBindings  = legacyBindings;
        }

        // The legacy format only ever configured one device; enable the
        // other too so the migrated player ends up with both.
        p.keyboardEnabled = true;
        p.gamepadEnabled  = true;
    }

    // Backfill built-in defaults for a device left enabled but unconfigured,
    // whether from migration above or a hand-edited file missing a list.
    if (p.keyboardEnabled && p.keyboardBindings.empty())
        p.keyboardBindings = defaultKeyboardBindings();
    if (p.gamepadEnabled && p.gamepadBindings.empty())
        p.gamepadBindings = defaultGamepadBindings();

    return p;
}

// ─── ControlsConfigStore ──────────────────────────────────────────────────────

ControlsConfigStore::ControlsConfigStore() {
    try {
        YAML::Node root = YAML::LoadFile(YAML_PATH);
        player1         = nodeToPlayer(root["player1"]);
        player2         = nodeToPlayer(root["player2"]);
    } catch (...) {
        applyDefaults(player1, player2);
    }
}

void ControlsConfigStore::save() {
    YAML::Node root;
    root["player1"] = playerToNode(player1);
    root["player2"] = playerToNode(player2);

    std::ofstream out(YAML_PATH);
    out << root;
}
