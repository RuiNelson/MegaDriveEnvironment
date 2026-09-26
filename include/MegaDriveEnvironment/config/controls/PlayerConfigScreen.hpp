#pragma once

#include "ControlsConfig.hpp"
#include "Screen.hpp"
#include <string>
#include <vector>

/// @brief Result enum for PlayerConfigScreen.
/// @see PlayerConfigScreen::getResult()
enum class PlayerConfigResult {
    BindKeys,   ///< User proceeded to key/button binding
    TestInputs, ///< User proceeded to input tester (skips binding phase)
    Back        ///< User returned to main menu
};

/// @brief Entry in the gamepad device list.
/// @see PlayerConfigScreen
struct GamepadEntry {
    SDL_JoystickID id;   ///< SDL joystick ID for this gamepad
    std::string    guid; ///< Persistent SDL GUID string for this gamepad
    std::string    name; ///< Human-readable device name
};

/// @brief Second screen: configure one player's input devices and launch key binding.
///
/// Presents a six-item menu:
/// 1. Connected toggle — Enable/disable player
/// 2. Keyboard toggle — Enable/disable keyboard input independently
/// 3. Gamepad — Open modal to choose Off, Automatic, or a specific gamepad
/// 4. Bind Keys / Buttons — Launch KeyBindScreen (disabled unless connected and
///    at least one device active)
/// 5. Test Inputs — same enable rule as item 4
/// 6. Back — Return to main menu
///
/// Keyboard and gamepad may both be active at once: either device then drives
/// the same Mega Drive buttons for this player.
/// @see Screen, PlayerConfig
class PlayerConfigScreen : public Screen {
    public:
    /// @brief Construct the player configuration screen.
    /// @param playerNum The player number (1 or 2) for display labels.
    /// @param config Reference to the PlayerConfig to edit.
    explicit PlayerConfigScreen(int playerNum, PlayerConfig &config);

    /// @brief Render the player configuration menu.
    /// @param ui Reference to UIRenderer for drawing.
    void render(UIRenderer &ui) override;

    /// @brief Handle keyboard and gamepad input events.
    /// @param e The SDL event to process.
    /// @note May dispatch to handleModalEvent() if the gamepad modal is open.
    void handleEvent(const SDL_Event &e) override;

    /// @brief Get the user's action result.
    /// @return PlayerConfigResult indicating next action (bind keys or back).
    PlayerConfigResult getResult() const {
        return m_result;
    }

    /// @brief Reset the screen state for reuse.
    /// @note Clears m_done flag, closes modal, and resets selection.
    void reset();

    private:
    int           m_playerNum; ///< Player number (1 or 2)
    PlayerConfig &m_config;    ///< Reference to the configuration being edited

    /// @brief Current menu selection (0=connected, 1=keyboard, 2=gamepad, 3=bind, 4=test, 5=back).
    int                m_sel    = 0;
    PlayerConfigResult m_result = PlayerConfigResult::Back;

    // ── Gamepad selection modal ───────────────────────────────────────────────
    /// @brief Whether the gamepad selection modal is currently open.
    bool m_modalOpen = false;

    /// @brief Currently selected item in modal (0=Off, 1=Automatic, 2+=gamepad index).
    int m_modalSel = 0;

    /// @brief List of connected gamepads retrieved at modal open time.
    std::vector<GamepadEntry> m_gamepads;

    /// @brief Open the gamepad selection modal and populate the device list.
    void openModal();

    /// @brief Apply the selected option from the modal to m_config.
    void applyModal();

    /// @brief Render the gamepad selection modal overlay.
    /// @param ui Reference to UIRenderer for drawing.
    void renderModal(UIRenderer &ui);

    /// @brief Handle events while the gamepad modal is open.
    /// @param e The SDL event to process.
    void handleModalEvent(const SDL_Event &e);

    // ── Navigation and confirmation ───────────────────────────────────────────
    /// @brief Navigate menu up or down, skipping disabled items.
    /// @param delta Movement direction (-1 up, +1 down). Wraps around at bounds.
    void navigate(int delta);

    /// @brief Handle confirmation of current menu item.
    void confirm();

    /// @brief Check if a menu item is currently enabled.
    /// @param item Menu item index (0-5).
    /// @return true if item is selectable.
    bool isItemEnabled(int item) const;

    /// @brief Get the display label for the current gamepad selection.
    /// @return "Off", "Automatic", or the assigned gamepad's name.
    std::string gamepadDisplayName() const;
};
