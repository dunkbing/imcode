#include "app.hpp"

#include "imgui_internal.h"
#include "mydear/frames.hpp"
#include "mydear/themes.hpp"
#include <algorithm>

namespace Theme = mydear::Theme;

namespace {
    // what is typed goes to the shell: text as it is, the rest through the key encoder
    void sendKeys(Terminal& term) {
        const ImGuiIO& io = ImGui::GetIO();
        // imgui swaps cmd and ctrl on macos: there KeyCtrl is cmd and KeySuper is ctrl
        const bool ctrl = io.ConfigMacOSXBehaviors ? io.KeySuper : io.KeyCtrl;
        const bool cmd = io.ConfigMacOSXBehaviors ? io.KeyCtrl : io.KeySuper;
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
            if (const char* text = ImGui::GetClipboardText(); text && *text) {
                term.paste(text);
            }
            return;
        }
        // shortcut: text skips the encoder, so programs that ask for every key as an escape
        // code (kitty keyboard, report-all) get plain letters; send key events with utf8 then
        for (int i = 0; !ctrl && !cmd && i < io.InputQueueCharacters.Size; ++i) {
            char utf8[5];
            ImTextCharToUtf8(utf8, io.InputQueueCharacters[i]);
            term.scroll(0);
            term.write(utf8);
        }
        const GhosttyMods mods = (io.KeyShift ? GHOSTTY_MODS_SHIFT : 0) | (ctrl ? GHOSTTY_MODS_CTRL : 0) |
                                 (io.KeyAlt ? GHOSTTY_MODS_ALT : 0);
        static const std::pair<ImGuiKey, GhosttyKey> special[] = {
            {ImGuiKey_Enter, GHOSTTY_KEY_ENTER},
            {ImGuiKey_KeypadEnter, GHOSTTY_KEY_NUMPAD_ENTER},
            {ImGuiKey_Backspace, GHOSTTY_KEY_BACKSPACE},
            {ImGuiKey_Tab, GHOSTTY_KEY_TAB},
            {ImGuiKey_Escape, GHOSTTY_KEY_ESCAPE},
            {ImGuiKey_UpArrow, GHOSTTY_KEY_ARROW_UP},
            {ImGuiKey_DownArrow, GHOSTTY_KEY_ARROW_DOWN},
            {ImGuiKey_LeftArrow, GHOSTTY_KEY_ARROW_LEFT},
            {ImGuiKey_RightArrow, GHOSTTY_KEY_ARROW_RIGHT},
            {ImGuiKey_Home, GHOSTTY_KEY_HOME},
            {ImGuiKey_End, GHOSTTY_KEY_END},
            {ImGuiKey_PageUp, GHOSTTY_KEY_PAGE_UP},
            {ImGuiKey_PageDown, GHOSTTY_KEY_PAGE_DOWN},
            {ImGuiKey_Insert, GHOSTTY_KEY_INSERT},
            {ImGuiKey_Delete, GHOSTTY_KEY_DELETE},
        };
        for (const auto& [from, to] : special) {
            if (ImGui::IsKeyPressed(from, true)) {
                term.key(to, mods);
            }
        }
        for (int i = 0; i < 12; ++i) {
            if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_F1 + i), true)) {
                term.key(static_cast<GhosttyKey>(GHOSTTY_KEY_F1 + i), mods);
            }
        }
        // ctrl+letter arrives as a key, not as text
        // shortcut: letters only; ctrl with [ \ ] / space is not sent, add them to the table
        for (int i = 0; ctrl && !cmd && i < 26; ++i) {
            if (ImGui::IsKeyPressed(static_cast<ImGuiKey>(ImGuiKey_A + i), true)) {
                term.key(static_cast<GhosttyKey>(GHOSTTY_KEY_A + i), mods, static_cast<uint32_t>('a' + i));
            }
        }
    }
} // namespace

void App::toggleTerminal() {
    terminalOpen = !terminalOpen;
    focusTerminal = terminalOpen;
    focusInput = !terminalOpen;
}

// the thread's terminal, filling what is left of the window below the chat
// shortcut: no selection or copy; libghostty has selection gestures and a formatter for it
void App::terminalPanel(Thread& t) {
    const auto& colors = Theme::Current()->colors;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.crust);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 6));
    // no nav: tab and the arrows are the shell's
    ImGui::BeginChild("terminal", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollbar |
                          ImGuiWindowFlags_NoScrollWithMouse);
    if (std::exchange(focusTerminal, false)) {
        ImGui::SetWindowFocus();
    }
    const bool focused = ImGui::IsWindowFocused();
    const ImVec2 cell(ImGui::CalcTextSize("M").x, ImGui::GetTextLineHeight());
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const int cols = std::max(2, static_cast<int>(avail.x / cell.x));
    const int rows = std::max(1, static_cast<int>(avail.y / cell.y));

    auto& term = terminals[&t];
    const bool restart = term && term->ended() && focused && ImGui::IsKeyPressed(ImGuiKey_Enter, false);
    if (restart) {
        term.reset();
    }
    if (!term) {
        term = std::make_unique<Terminal>(
            [host = t.host, cwd = t.cwd](int c, int r, std::string& error) -> std::unique_ptr<Pty> {
                if (host.empty()) {
                    return Terminal::local(cwd, {}, c, r, error);
                }
                Ssh::Error failed;
                const auto session = Ssh::session(host, "", failed);
                error = failed.message;
                return session ? session->shell(c, r, cwd, error) : nullptr;
            },
            cols, rows, [] { mydear::requestFrames(); });
    }
    term->resize(cols, rows);
    term->poll();
    if (focused && !restart && !term->ended()) { // that enter is not the new shell's
        sendKeys(*term);
    }
    if (const float wheel = ImGui::GetIO().MouseWheel; wheel != 0 && ImGui::IsWindowHovered()) {
        // a notch is three rows; small trackpad steps still move one
        term->scroll(-static_cast<int>(wheel * 3 + (wheel > 0 ? 0.999f : -0.999f)));
    }

    ghostty_render_state_update(term->render, term->vt);
    GhosttyRenderStateCursor cursor = GHOSTTY_INIT_SIZED(GhosttyRenderStateCursor);
    ghostty_render_state_get(term->render, GHOSTTY_RENDER_STATE_DATA_CURSOR, &cursor);
    const bool cursorShown = cursor.visible && cursor.viewport_has_value && !term->ended();
    ghostty_render_state_get(term->render, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &term->rows);
    const ImU32 textColor = ImGui::GetColorU32(colors.text);
    const ImU32 backColor = ImGui::GetColorU32(colors.crust);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    // backgrounds under all text: a wide character reaches into the next cell
    draw->ChannelsSplit(2);
    for (int y = 0; y < rows && ghostty_render_state_row_iterator_next(term->rows); ++y) {
        ghostty_render_state_row_get(term->rows, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &term->cells);
        for (int x = 0; x < cols && ghostty_render_state_row_cells_next(term->cells); ++x) {
            const ImVec2 at(origin.x + static_cast<float>(x) * cell.x,
                            origin.y + static_cast<float>(y) * cell.y);
            GhosttyStyle style{};
            ghostty_render_state_row_cells_get(term->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_STYLE,
                                               &style);
            // 16, 256 and rgb colours arrive resolved; no value = the theme's
            GhosttyColorRgb rgb{};
            ImU32 fg = textColor, bg = backColor;
            if (ghostty_render_state_row_cells_get(
                    term->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_FG_COLOR, &rgb) == GHOSTTY_SUCCESS) {
                fg = IM_COL32(rgb.r, rgb.g, rgb.b, 255);
            }
            bool fill = ghostty_render_state_row_cells_get(
                            term->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_BG_COLOR, &rgb) ==
                        GHOSTTY_SUCCESS;
            if (fill) {
                bg = IM_COL32(rgb.r, rgb.g, rgb.b, 255);
            }
            if (style.flags & GHOSTTY_STYLE_FLAG_INVERSE) {
                std::swap(fg, bg);
                fill = true;
            }
            if (focused && cursorShown && x == cursor.viewport_x && y == cursor.viewport_y) {
                fg = backColor; // the block cursor
                bg = textColor;
                fill = true;
            }
            if (style.flags & GHOSTTY_STYLE_FLAG_FAINT) {
                fg = (fg & ~IM_COL32_A_MASK) | (0x80u << IM_COL32_A_SHIFT);
            }
            const ImVec2 end(at.x + cell.x, at.y + cell.y);
            if (fill) {
                draw->ChannelsSetCurrent(0);
                draw->AddRectFilled(at, end, bg);
            }
            draw->ChannelsSetCurrent(1);
            uint8_t text[64];
            GhosttyBuffer buf{.ptr = text, .cap = sizeof(text), .len = 0};
            if (!(style.flags & GHOSTTY_STYLE_FLAG_INVISIBLE) &&
                ghostty_render_state_row_cells_get(
                    term->cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &buf) ==
                    GHOSTTY_SUCCESS &&
                buf.len > 0) {
                const char* begin = reinterpret_cast<const char*>(text);
                draw->AddText(at, fg, begin, begin + buf.len);
                // shortcut: one font face; bold is drawn twice, italic stays upright
                if (style.flags & GHOSTTY_STYLE_FLAG_BOLD) {
                    draw->AddText(ImVec2(at.x + 0.6f, at.y), fg, begin, begin + buf.len);
                }
            }
            if (GHOSTTY_STYLE_UNDERLINE(style)) {
                draw->AddLine(ImVec2(at.x, end.y - 1), ImVec2(end.x, end.y - 1), fg);
            }
        }
    }
    draw->ChannelsMerge();
    if (!focused && cursorShown) {
        const ImVec2 at(origin.x + static_cast<float>(cursor.viewport_x) * cell.x,
                        origin.y + static_cast<float>(cursor.viewport_y) * cell.y);
        draw->AddRect(at, ImVec2(at.x + cell.x, at.y + cell.y), textColor);
    }
    if (term->ended()) {
        const char* hint = "The shell ended. Click here and press Enter for a new one.";
        draw->AddText(ImVec2(origin.x, origin.y + avail.y - cell.y), ImGui::GetColorU32(colors.overlay1),
                      hint);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}
