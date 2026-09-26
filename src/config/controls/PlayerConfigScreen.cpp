#include "PlayerConfigScreen.hpp"
#include <cstring>

static constexpr int kItemCount = 6; // connected, keyboard, gamepad, bind, test, back

PlayerConfigScreen::PlayerConfigScreen(int playerNum, PlayerConfig &config) : m_playerNum(playerNum), m_config(config) {
}

void PlayerConfigScreen::reset() {
    m_done      = false;
    m_sel       = 0;
    m_modalOpen = false;
    m_modalSel  = 0;
    m_result    = PlayerConfigResult::Back;
}

// ─── Gamepad modal ───────────────────────────────────────────────────────────

void PlayerConfigScreen::openModal() {
    m_gamepads.clear();
    int             count = 0;
    SDL_JoystickID *ids   = SDL_GetGamepads(&count);
    if (ids) {
        for (int i = 0; i < count; ++i) {
            const char *name = SDL_GetGamepadNameForID(ids[i]);
            SDL_GUID    guid = SDL_GetJoystickGUIDForID(ids[i]);
            char        buf[33];
            SDL_GUIDToString(guid, buf, sizeof(buf));
            m_gamepads.push_back({ids[i], buf, name ? name : "Unknown Gamepad"});
        }
        SDL_free(ids);
    }

    // Compute current selection: Off (0), Automatic (1), or a matching device.
    m_modalSel = m_config.gamepadEnabled ? 1 : 0;
    if (m_config.gamepadEnabled && !m_config.gamepadGuid.empty()) {
        for (int i = 0; i < (int)m_gamepads.size(); ++i) {
            if (m_gamepads[i].guid == m_config.gamepadGuid) {
                m_modalSel = i + 2; // +2 because indices 0/1 are Off/Automatic
                break;
            }
        }
    }

    m_modalOpen = true;
}

void PlayerConfigScreen::applyModal() {
    if (m_modalSel == 0) {
        m_config.gamepadEnabled = false;
        m_config.gamepadGuid.clear();
        m_config.gamepadId = 0;
        m_config.gamepadName.clear();
    } else if (m_modalSel == 1) {
        m_config.gamepadEnabled = true;
        m_config.gamepadGuid.clear();
        m_config.gamepadName.clear();
        // Live preview id only; the actual game session resolves gamepads
        // jointly across both players, giving player 1 priority.
        m_config.gamepadId = m_gamepads.empty() ? 0 : m_gamepads[0].id;
    } else {
        int idx = m_modalSel - 2;
        if (idx >= 0 && idx < (int)m_gamepads.size()) {
            m_config.gamepadEnabled = true;
            m_config.gamepadId      = m_gamepads[idx].id;
            m_config.gamepadGuid    = m_gamepads[idx].guid;
            m_config.gamepadName    = m_gamepads[idx].name;
        }
    }
    m_modalOpen = false;
}

void PlayerConfigScreen::handleModalEvent(const SDL_Event &e) {
    int total = 2 + static_cast<int>(m_gamepads.size()); // Off + Automatic + gamepads

    auto navigateModal = [&](int delta) {
        m_modalSel = (m_modalSel + delta + total) % total;
    };

    if (e.type == SDL_EVENT_KEY_DOWN) {
        switch (e.key.key) {
            case SDLK_UP:
                navigateModal(-1);
                break;
            case SDLK_DOWN:
                navigateModal(+1);
                break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                applyModal();
                break;
            case SDLK_ESCAPE:
            case SDLK_BACKSPACE:
                m_modalOpen = false;
                break;
            default:
                break;
        }
    }
    if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        switch (e.gbutton.button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP:
                navigateModal(-1);
                break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                navigateModal(+1);
                break;
            case SDL_GAMEPAD_BUTTON_EAST:
                applyModal();
                break;
            case SDL_GAMEPAD_BUTTON_SOUTH:
            case SDL_GAMEPAD_BUTTON_BACK:
                m_modalOpen = false;
                break;
            default:
                break;
        }
    }
}

void PlayerConfigScreen::renderModal(UIRenderer &ui) {
    // Dim overlay
    ui.fillRect(0, 0, CC_WIN_W, CC_WIN_H, CC_COL_OVERLAY);

    int total  = 2 + static_cast<int>(m_gamepads.size());
    int itemH  = CC_BTN_H;
    int modalW = 340;
    int modalH = 40 + total * (itemH + 6) + 20;
    int modalX = (CC_WIN_W - modalW) / 2;
    int modalY = (CC_WIN_H - modalH) / 2;

    // Modal background
    ui.fillRect(modalX, modalY, modalW, modalH, CC_COL_MODAL_BG);
    ui.drawRect(modalX, modalY, modalW, modalH, CC_COL_MODAL_BORDER);

    // Title
    ui.drawCenteredText(CC_WIN_W / 2, modalY + 12, "Select Gamepad", CC_COL_TEXT_YELLOW);

    // Items
    int itemX = modalX + 10;
    int itemW = modalW - 20;
    int itemY = modalY + 40;

    ui.drawButton(itemX, itemY, itemW, itemH, "Off", m_modalSel == 0);
    itemY += itemH + 6;
    ui.drawButton(itemX, itemY, itemW, itemH, "Automatic (first available)", m_modalSel == 1);
    itemY += itemH + 6;

    for (int i = 0; i < (int)m_gamepads.size(); ++i) {
        std::string label = "Gamepad: " + m_gamepads[i].name;
        int         maxChars = (itemW - 8) / CC_CHAR_W;
        if ((int)label.size() > maxChars)
            label = label.substr(0, maxChars - 3) + "...";
        ui.drawButton(itemX, itemY, itemW, itemH, label, m_modalSel == i + 2);
        itemY += itemH + 6;
    }

    ui.drawHint("Up/Down:Navigate  Enter/East:Select  Bksp/South:Cancel");
}

// ─── Helpers ─────────────────────────────────────────────────────────────────

std::string PlayerConfigScreen::gamepadDisplayName() const {
    if (!m_config.gamepadEnabled)
        return "Off";
    if (m_config.gamepadGuid.empty())
        return "Automatic";
    if (!m_config.gamepadName.empty())
        return m_config.gamepadName;
    return "Gamepad";
}

// ─── Navigation / confirm ────────────────────────────────────────────────────

bool PlayerConfigScreen::isItemEnabled(int item) const {
    // Keyboard toggle and gamepad picker require the player to be connected.
    if (item == 1 || item == 2)
        return m_config.connected;
    // Binding and testing require connected plus at least one active device.
    if (item == 3 || item == 4)
        return m_config.connected && (m_config.keyboardEnabled || m_config.gamepadEnabled);
    return true;
}

void PlayerConfigScreen::navigate(int delta) {
    int next = m_sel;
    do {
        next = (next + delta + kItemCount) % kItemCount;
    } while (!isItemEnabled(next));
    m_sel = next;
}

void PlayerConfigScreen::confirm() {
    switch (m_sel) {
        case 0: // Connected toggle
            m_config.connected = !m_config.connected;
            break;
        case 1: // Keyboard toggle (only if connected)
            if (m_config.connected)
                m_config.keyboardEnabled = !m_config.keyboardEnabled;
            break;
        case 2: // Gamepad picker (only if connected)
            if (m_config.connected)
                openModal();
            break;
        case 3: // Bind keys (only if connected and a device is active)
            if (isItemEnabled(3)) {
                m_result = PlayerConfigResult::BindKeys;
                m_done   = true;
            }
            break;
        case 4: // Test inputs (only if connected and a device is active)
            if (isItemEnabled(4)) {
                m_result = PlayerConfigResult::TestInputs;
                m_done   = true;
            }
            break;
        case 5: // Back
            m_result = PlayerConfigResult::Back;
            m_done   = true;
            break;
        default:
            break;
    }
}

void PlayerConfigScreen::handleEvent(const SDL_Event &e) {
    if (m_modalOpen) {
        handleModalEvent(e);
        return;
    }

    if (e.type == SDL_EVENT_KEY_DOWN) {
        switch (e.key.key) {
            case SDLK_UP:
                navigate(-1);
                break;
            case SDLK_DOWN:
                navigate(+1);
                break;
            case SDLK_RETURN:
            case SDLK_KP_ENTER:
                confirm();
                break;
            case SDLK_ESCAPE:
            case SDLK_BACKSPACE:
                m_result = PlayerConfigResult::Back;
                m_done   = true;
                break;
            default:
                break;
        }
    }
    if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        switch (e.gbutton.button) {
            case SDL_GAMEPAD_BUTTON_DPAD_UP:
                navigate(-1);
                break;
            case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                navigate(+1);
                break;
            case SDL_GAMEPAD_BUTTON_EAST:
                confirm();
                break;
            case SDL_GAMEPAD_BUTTON_SOUTH:
            case SDL_GAMEPAD_BUTTON_BACK:
                m_result = PlayerConfigResult::Back;
                m_done   = true;
                break;
            default:
                break;
        }
    }
}

// ─── Render ──────────────────────────────────────────────────────────────────

void PlayerConfigScreen::render(UIRenderer &ui) {
    ui.clear();

    // Title
    std::string title = "Player ";
    title += std::to_string(m_playerNum);
    title += " Configuration";
    ui.drawCenteredText(CC_WIN_W / 2, 20, title, CC_COL_TEXT_YELLOW);

    // All items share the same row geometry
    const int rowX   = CC_PAD;
    const int rowW   = CC_WIN_W - 2 * CC_PAD;
    const int rowH   = CC_ROW_H;
    const int ipad   = 8;                      // inner horizontal padding
    const int ty_off = (rowH - CC_CHAR_H) / 2; // vertical text offset inside row

    const int startY  = 65;
    const int spacing = rowH + 8;

    // Helper: draw row background + border (disabled = dimmed, not selectable)
    auto rowBg = [&](int y, bool sel, bool enabled = true) {
        SDL_Color bg  = !enabled ? CC_COL_BTN_DISABLED : sel ? CC_COL_BTN_SEL : CC_COL_BTN_NORMAL;
        SDL_Color brd = !enabled ? CC_COL_BORDER : sel ? CC_COL_BORDER_SEL : CC_COL_BORDER;
        ui.fillRect(rowX, y, rowW, rowH, bg);
        ui.drawRect(rowX, y, rowW, rowH, brd);
    };

    // ── Item 0: Connected toggle ──────────────────────────────────────────────
    {
        int  y   = startY;
        bool sel = (m_sel == 0);
        rowBg(y, sel);

        SDL_Color   fg  = sel ? CC_COL_TEXT_WHITE : CC_COL_TEXT_GRAY;
        std::string lbl = (m_config.connected ? "[X]" : "[ ]");
        lbl += " Connected";
        ui.drawText(rowX + ipad, y + ty_off, lbl, fg);

        const char *status = m_config.connected ? "ON" : "OFF";
        SDL_Color   sCol   = m_config.connected ? SDL_Color{90, 210, 90, 255} : CC_COL_TEXT_GRAY;
        int         sw     = ui.textWidth(status);
        ui.drawText(rowX + rowW - ipad - sw, y + ty_off, status, sCol);
    }

    // ── Item 1: Keyboard toggle ───────────────────────────────────────────────
    {
        int  y       = startY + spacing;
        bool sel     = (m_sel == 1);
        bool enabled = isItemEnabled(1);
        rowBg(y, sel, enabled);

        SDL_Color   fg  = !enabled ? CC_COL_TEXT_DISABLED : sel ? CC_COL_TEXT_WHITE : CC_COL_TEXT_GRAY;
        std::string lbl = (m_config.keyboardEnabled ? "[X]" : "[ ]");
        lbl += " Keyboard";
        ui.drawText(rowX + ipad, y + ty_off, lbl, fg);

        const char *status = m_config.keyboardEnabled ? "ON" : "OFF";
        SDL_Color   sCol   = !enabled          ? CC_COL_TEXT_DISABLED
                             : m_config.keyboardEnabled ? SDL_Color{90, 210, 90, 255}
                                                 : CC_COL_TEXT_GRAY;
        int sw = ui.textWidth(status);
        ui.drawText(rowX + rowW - ipad - sw, y + ty_off, status, sCol);
    }

    // ── Item 2: Gamepad row ───────────────────────────────────────────────────
    {
        int  y       = startY + 2 * spacing;
        bool sel     = (m_sel == 2);
        bool enabled = isItemEnabled(2);
        rowBg(y, sel, enabled);

        int ty = y + ty_off;

        const char *leftLbl = "Gamepad";
        SDL_Color   fgLbl   = !enabled ? CC_COL_TEXT_DISABLED : sel ? CC_COL_TEXT_WHITE : CC_COL_TEXT_GRAY;
        ui.drawText(rowX + ipad, ty, leftLbl, fgLbl);

        const char *rightBtn = "[Select]";
        SDL_Color   fgBtn    = !enabled ? CC_COL_TEXT_DISABLED : sel ? CC_COL_TEXT_YELLOW : CC_COL_TEXT_GRAY;
        int         rbW      = ui.textWidth(rightBtn);
        ui.drawText(rowX + rowW - ipad - rbW, ty, rightBtn, fgBtn);

        std::string name    = gamepadDisplayName();
        int         leftEnd = rowX + ipad + ui.textWidth(leftLbl) + 12;
        int         rightSt = rowX + rowW - ipad - rbW - 12;
        int         availW  = rightSt - leftEnd;
        int         nameW   = ui.textWidth(name);
        if (nameW > availW) {
            int maxCh = availW / CC_CHAR_W;
            name      = maxCh > 3 ? name.substr(0, maxCh - 3) + "..." : name.substr(0, maxCh);
            nameW     = ui.textWidth(name);
        }
        int       nameX  = leftEnd + (availW - nameW) / 2;
        SDL_Color fgName = !enabled ? CC_COL_TEXT_DISABLED : CC_COL_TEXT_WHITE;
        ui.drawText(nameX, ty, name, fgName);
    }

    // ── Separator before action items ─────────────────────────────────────────
    {
        int sepY = startY + 3 * spacing + 6;
        ui.drawHLine(rowX, rowX + rowW, sepY, CC_COL_BORDER);
    }

    // ── Item 3: Bind Keys / Buttons ───────────────────────────────────────────
    {
        int  y       = startY + 3 * spacing + 16;
        bool sel     = (m_sel == 3);
        bool enabled = isItemEnabled(3);
        rowBg(y, sel, enabled);

        SDL_Color fg = !enabled ? CC_COL_TEXT_DISABLED : sel ? CC_COL_TEXT_WHITE : CC_COL_TEXT_GRAY;
        ui.drawText(rowX + ipad, y + ty_off, "Bind Keys / Buttons", fg);

        SDL_Color arrowCol = !enabled ? CC_COL_TEXT_DISABLED : sel ? CC_COL_TEXT_YELLOW : CC_COL_TEXT_GRAY;
        int       aw       = ui.textWidth(">");
        ui.drawText(rowX + rowW - ipad - aw, y + ty_off, ">", arrowCol);
    }

    // ── Item 4: Test Inputs ───────────────────────────────────────────────────
    {
        int  y       = startY + 4 * spacing + 16;
        bool sel     = (m_sel == 4);
        bool enabled = isItemEnabled(4);
        rowBg(y, sel, enabled);

        SDL_Color fg = !enabled ? CC_COL_TEXT_DISABLED : sel ? CC_COL_TEXT_WHITE : CC_COL_TEXT_GRAY;
        ui.drawText(rowX + ipad, y + ty_off, "Test Inputs", fg);

        SDL_Color arrowCol = !enabled ? CC_COL_TEXT_DISABLED : sel ? CC_COL_TEXT_YELLOW : CC_COL_TEXT_GRAY;
        int       aw       = ui.textWidth(">");
        ui.drawText(rowX + rowW - ipad - aw, y + ty_off, ">", arrowCol);
    }

    // ── Item 5: Back ──────────────────────────────────────────────────────────
    {
        int  y   = startY + 5 * spacing + 16;
        bool sel = (m_sel == 5);
        rowBg(y, sel);

        SDL_Color fg = sel ? CC_COL_TEXT_WHITE : CC_COL_TEXT_GRAY;
        ui.drawText(rowX + ipad, y + ty_off, "Back", fg);

        SDL_Color arrowCol = sel ? CC_COL_TEXT_YELLOW : CC_COL_TEXT_GRAY;
        int       aw       = ui.textWidth("<");
        ui.drawText(rowX + rowW - ipad - aw, y + ty_off, "<", arrowCol);
    }

    ui.drawHint("Up/Down:Navigate  Enter/East:Select  Bksp/South/Esc:Back");

    if (m_modalOpen)
        renderModal(ui);
}
