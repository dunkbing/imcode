#include "app.hpp"

#include "clipboard.hpp"
#include <fstream>
#include <cstring>
#include <chrono>
#include "imshell/markdown.hpp"
#include "imshell/button.hpp"
#include "imshell/file_dialog.hpp"
#include "imshell/components.hpp"
#include "imshell/themes.hpp"
#include <algorithm>
#include <cstdlib>
#include <format>
#include <functional>
#include <optional>

namespace Theme = imshell::Theme;
namespace ui = imshell::ui;

namespace {
    const acp::ConfigOption* optionOf(const Thread& t, const char* category) {
        for (const auto& option : t.config) {
            if (option.category == category && option.type == "select" &&
                option.currentValue.is_string()) {
                return &option;
            }
        }
        return nullptr;
    }

    std::string currentName(const acp::ConfigOption* option) {
        if (!option) {
            return "";
        }
        const std::string value = option->currentValue.get<std::string>();
        for (const auto& choice : option->options) {
            if (choice.value == value) {
                return choice.name;
            }
        }
        return value;
    }

    // a row of a picker list: padded, the whole row lit when chosen, a check at its right.
    // lead draws what goes before the label (an icon)
    bool pickRow(const std::string& id, const std::string& label, bool chosen,
                 const std::function<void()>& lead = nullptr) {
        const auto& colors = Theme::Current()->colors;
        const float line = ImGui::GetTextLineHeight();
        const float height = line + 8;
        const ImVec2 at = ImGui::GetCursorPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const bool clicked =
            rowButton(("##" + id).c_str(), chosen, height, ImGuiSelectableFlags_DontClosePopups);
        const ImVec2 next = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(at.x + 8, at.y + 4));
        if (lead) {
            lead();
            ImGui::SameLine(0, 8);
        }
        ImGui::TextUnformatted(label.c_str());
        if (chosen) {
            ImGui::SetCursorPos(ImVec2(at.x + width - 24, at.y + 4));
            ImGui::TextColored(colors.blue, ICON_FA_CHECK);
        }
        // imgui wants an item after a cursor move; placed so it adds no second row gap
        ImGui::SetCursorPos(ImVec2(next.x, next.y - ImGui::GetStyle().ItemSpacing.y));
        ImGui::Dummy(ImVec2(0, 0));
        return clicked;
    }

    // one setting as a list
    void choices(Thread& t, const acp::ConfigOption& option, const char* filter = "") {
        const std::string value = option.currentValue.get<std::string>();
        for (const auto& choice : option.options) {
            const auto found = std::ranges::search(choice.name, std::string(filter), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) ==
                       std::tolower(static_cast<unsigned char>(b));
            });
            if (filter[0] && found.empty()) {
                continue;
            }
            if (pickRow(option.id + "/" + choice.value, choice.name, choice.value == value)) {
                t.setConfig(option.id, choice.value);
            }
        }
    }
} // namespace

namespace {
    bool foldable(const Item& it) {
        return it.kind == Item::Kind::Tool && it.changes.empty() &&
               (it.toolStatus == "completed" || it.toolStatus == "failed");
    }
} // namespace

// the line standing for tool calls [from, to): how many, and what kinds. true when clicked
bool App::toolGroupRow(Thread& t, size_t from, size_t to, float width, bool shown) {
    const auto& colors = Theme::Current()->colors;
    std::vector<std::string> kinds; // the first word of each title, once each
    int failed = 0;
    for (size_t k = from; k < to; ++k) {
        const std::string& title = t.items[k].toolTitle;
        const std::string kind = title.substr(0, title.find(' '));
        if (!kind.empty() && std::ranges::find(kinds, kind) == kinds.end() && kinds.size() < 4) {
            kinds.push_back(kind);
        }
        failed += t.items[k].toolStatus == "failed";
    }
    std::string label = std::format("{}  {} tool calls", shown ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_RIGHT, to - from);
    for (size_t k = 0; k < kinds.size(); ++k) {
        label += (k == 0 ? "  ·  " : ", ") + kinds[k];
    }
    if (failed > 0) {
        label += std::format("  ·  {} failed", failed);
    }
    ImGui::PushID(static_cast<int>(from));
    ImGui::PushStyleColor(ImGuiCol_Text, failed > 0 ? colors.peach : colors.overlay1);
    const bool clicked = ImGui::Selectable((label + "##group").c_str(), false, 0,
                                           ImVec2(width, ImGui::GetTextLineHeight() + 10));
    ImGui::PopStyleColor();
    ImGui::PopID();
    ImGui::Spacing();
    return clicked;
}

// a file's diff in a dark box: its name (true when clicked), +added -removed, then the lines
// tinted. at most maxHeight tall (0: fills the room), scrolling inside
bool App::diffBlock(const std::string& path, int added, int removed, const std::vector<Diff::Line>& lines, float maxHeight) {
    const auto& colors = Theme::Current()->colors;
    bool clicked = false;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.crust);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.surface1);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 0));
    const float lineHeight = ImGui::GetTextLineHeight() + 4;
    const float height = maxHeight > 0 ? std::min(maxHeight, lineHeight * static_cast<float>(lines.size() + 1) + 20) + 16 : 0;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
    ImGui::BeginChild("change", ImVec2(0, height), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_HorizontalScrollbar);
    ImGui::PopStyleVar();
    // the file: click to read it in the viewer
    const std::string label = std::string(ICON_FA_FILE "  ") + path;
    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
    clicked = ImGui::Selectable(label.c_str(), false, 0, ImGui::CalcTextSize(label.c_str()));
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    if (added >= 0) {
        ImGui::SameLine();
        ImGui::TextColored(colors.green, "+%d", added);
        ImGui::SameLine();
        ImGui::TextColored(colors.red, "-%d", removed);
    }
    ImGui::Dummy(ImVec2(0, 4));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float width = std::max(ImGui::GetContentRegionAvail().x, 2000.0f);
    // only the lines in view are drawn: a long diff stays cheap
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(lines.size()), lineHeight);
    while (clipper.Step()) {
        for (int k = clipper.DisplayStart; k < clipper.DisplayEnd; ++k) {
            const Diff::Line& line = lines[static_cast<size_t>(k)];
            const ImVec2 at = ImGui::GetCursorScreenPos();
            if (line.kind == '+' || line.kind == '-') {
                ImVec4 tint = line.kind == '+' ? colors.green : colors.red;
                tint.w = 0.14f;
                draw->AddRectFilled(at, ImVec2(at.x + width, at.y + lineHeight), ImGui::GetColorU32(tint));
            }
            ImGui::SetCursorScreenPos(ImVec2(at.x + 4, at.y + 2));
            if (line.kind == '~') {
                ImGui::TextColored(colors.overlay0, "...");
            } else {
                ImGui::TextColored(line.kind == '+' ? colors.green : line.kind == '-' ? colors.red : colors.subtext0,
                                   "%c %s", line.kind, line.text.c_str());
            }
            ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + lineHeight));
            ImGui::Dummy(ImVec2(0, 0)); // imgui wants an item after a cursor move
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    return clicked;
}

void App::transcriptItem(Thread& t, Item& it, int index, float width) {
    const auto& colors = Theme::Current()->colors;
    ImGui::PushID(index);
    // everything but the user's bubble sits in the reading column: cards and the markdown
    // renderer fill the window they are in
    const bool boxed = it.kind != Item::Kind::User;
    ImVec2 bubbleMin, bubbleMax; // the user's bubble, for its right-click
    if (boxed) {
        ImGui::BeginChild("item", ImVec2(width, 0), ImGuiChildFlags_AutoResizeY);
    }
    switch (it.kind) {
    case Item::Kind::User: {
        // a bubble at the right, as wide as its text up to most of the column
        const float pad = 12;
        const ImVec2 size = ImGui::CalcTextSize(it.text.c_str(), nullptr, false, width * 0.75f);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const ImVec2 min(at.x + width - size.x - 2 * pad, at.y);
        const ImVec2 max(at.x + width, at.y + size.y + 2 * pad);
        ImGui::GetWindowDrawList()->AddRectFilled(min, max, ImGui::GetColorU32(colors.surface0), 14);
        bubbleMin = min;
        bubbleMax = max;
        ImGui::SetCursorScreenPos(ImVec2(min.x + pad, min.y + pad));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + size.x + 1);
        ImGui::TextUnformatted(it.text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::SetCursorScreenPos(ImVec2(at.x, max.y));
        ImGui::Dummy(ImVec2(width, it.files.empty() ? Theme::Spacing::M : 2));
        // what went with it, under the bubble at the right
        for (const auto& file : it.files) {
            const std::string label = std::string(ICON_FA_PAPERCLIP "  ") + file;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + width - ImGui::CalcTextSize(label.c_str()).x);
            ImGui::TextColored(colors.overlay1, "%s", label.c_str());
        }
        break;
    }
    case Item::Kind::Assistant:
        imshell::Markdown::Render(it.text, std::format("m{}", index));
        break;
    case Item::Kind::Thought:
        ImGui::PushStyleColor(ImGuiCol_Text, colors.overlay1);
        ImGui::TextWrapped("%s", it.text.c_str());
        ImGui::PopStyleColor();
        break;
    case Item::Kind::Tool: {
        // taller than its text, so the hover band clears the badge
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 7));
        const bool open = ImGui::TreeNodeEx(
            "tool", ImGuiTreeNodeFlags_FramePadding | (it.toolOutput.empty() ? ImGuiTreeNodeFlags_Leaf : 0), "%s",
            it.toolTitle.empty() ? "Tool" : it.toolTitle.c_str());
        ImGui::PopStyleVar();
        // what follows on the row sits at its top: moved down to the row's middle
        const float rowTop = ImGui::GetItemRectMin().y, rowHeight = ImGui::GetItemRectSize().y;
        const auto centred = [&](float height) {
            ImGui::SameLine();
            ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, rowTop + (rowHeight - height) * 0.5f));
        };
        centred(ImGui::GetTextLineHeight() + imshell::Theme::Spacing::XS * 2); // the badge's height
        ui::Badge(it.toolStatus.empty()            ? "pending"
                  : it.toolStatus == "in_progress" ? "running"
                                                   : it.toolStatus.c_str(),
                  it.toolStatus == "failed"      ? ui::Tone::Danger
                  : it.toolStatus == "completed" ? ui::Tone::Success
                                                 : ui::Tone::Info);
        if (!it.toolPath.empty()) {
            centred(rowHeight); // from the row's top: a small button already drops to the text's line
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Text, colors.overlay1);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
            if (ImGui::SmallButton(ICON_FA_ARROW_UP_RIGHT_FROM_SQUARE)) {
                showFile(it.toolPath);
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(2);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Open %s", it.toolPath.c_str());
            }
        }
        if (open) {
            // an edit shows as its diff; the text only says "diff: <path>" then
            if (!it.toolOutput.empty() && it.changes.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                ImGui::TextWrapped("%s", it.toolOutput.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::TreePop();
        }
        for (size_t c = 0; c < it.changes.size(); ++c) {
            const Item::Change& change = it.changes[c];
            ImGui::PushID(static_cast<int>(c));
            if (diffBlock(change.path, change.added, change.removed, change.lines, 320) && !change.path.starts_with("/")) {
                showFile(change.path);
            }
            ImGui::PopID();
        }
        break;
    }
    case Item::Kind::Plan:
        ui::BeginCard("plan", "Plan");
        for (const auto& entry : it.plan) {
            const bool done = entry.status == "completed";
            const bool doing = entry.status == "in_progress";
            ImGui::TextColored(done ? colors.green : doing ? colors.yellow : colors.overlay0, "%s",
                               done ? ICON_FA_CIRCLE_CHECK : doing ? ICON_FA_CIRCLE_HALF_STROKE : ICON_FA_CIRCLE);
            ImGui::SameLine(0, 10);
            ImGui::PushStyleColor(ImGuiCol_Text, done ? colors.subtext0 : colors.text);
            ImGui::TextWrapped("%s", entry.content.c_str());
            ImGui::PopStyleColor();
        }
        ui::EndCard();
        break;
    case Item::Kind::Permission:
        ui::BeginCard("permission", "Permission");
        ImGui::TextWrapped("%s", it.permission.title.c_str());
        if (it.answered) {
            ImGui::TextColored(colors.subtext0, "-> %s", it.choice.c_str());
        }
        for (size_t i = 0; !it.answered && i < it.permission.options.size(); ++i) {
            const auto& option = it.permission.options[i];
            if (i > 0) {
                ImGui::SameLine(0, Theme::Spacing::S);
            }
            if (imshell::SmallButton(std::format("{}##p{}", option.name, i).c_str(),
                                    option.kind == "allow_once" ? imshell::ButtonVariant::Primary
                                                                : imshell::ButtonVariant::Secondary)) {
                t.answerPermission(it, option);
            }
        }
        ui::EndCard();
        break;
    case Item::Kind::Auth: {
        ui::BeginCard("auth", "Sign in required");
        // a copy: signing in may add an item, which moves the one `it` refers to
        std::optional<acp::AuthMethod> picked;
        if (it.answered) {
            ImGui::TextColored(colors.subtext0, "-> %s", it.choice.c_str());
            if (it.terminalSignIn) {
                ImGui::TextWrapped("Finish signing in in the terminal below, then continue.");
                if (imshell::SmallButton("Continue", imshell::ButtonVariant::Primary)) {
                    t.continueAfterTerminalSignIn(it);
                }
            }
        }
        for (size_t i = 0; !it.answered && i < it.authMethods.size(); ++i) {
            if (i > 0) {
                ImGui::SameLine(0, Theme::Spacing::S);
            }
            if (imshell::SmallButton(std::format("{}##a{}", it.authMethods[i].name, i).c_str(),
                                    i == 0 ? imshell::ButtonVariant::Primary
                                           : imshell::ButtonVariant::Secondary)) {
                picked = it.authMethods[i];
            }
        }
        ui::EndCard();
        if (picked) {
            t.signIn(it, *picked);
        }
        break;
    }
    case Item::Kind::Info:
        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
        ImGui::TextWrapped("%s", it.text.c_str());
        ImGui::PopStyleColor();
        break;
    case Item::Kind::Error:
        ui::Callout("Error", it.text.c_str(), ui::Tone::Danger);
        if (it.resumeChoice && !it.answered) {
            if (imshell::Button("Try again", imshell::ButtonVariant::Primary)) {
                it.answered = true;
                t.retryResume();
            }
            ImGui::SameLine();
            if (imshell::Button("Start a new session")) {
                it.answered = true;
                t.startOver();
            }
        }
        break;
    case Item::Kind::TurnChanges: {
        // what the turn did to the folder: click for the files and their diffs
        const std::string label = std::format("{}  {} {} changed", ICON_FA_CODE_COMPARE, it.filesChanged,
                                              it.filesChanged == 1 ? "file" : "files");
        const std::string counts = std::format("+{}  -{}", it.linesAdded, it.linesRemoved);
        const ImVec2 size(ImGui::CalcTextSize(label.c_str()).x + ImGui::CalcTextSize(counts.c_str()).x + 32,
                          ImGui::GetFrameHeight());
        const bool clicked = ImGui::InvisibleButton("##turn", size);
        const ImVec2 at = ImGui::GetItemRectMin();
        const bool hot = ImGui::IsItemHovered();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRect(at, ImVec2(at.x + size.x, at.y + size.y), ImGui::GetColorU32(hot ? colors.surface2 : colors.surface1),
                      ImGui::GetStyle().FrameRounding + 3);
        const float y = at.y + (size.y - ImGui::GetTextLineHeight()) * 0.5f;
        draw->AddText(ImVec2(at.x + 10, y), ImGui::GetColorU32(hot ? colors.text : colors.subtext0), label.c_str());
        const float x = at.x + 10 + ImGui::CalcTextSize(label.c_str()).x + 12;
        const std::string plus = std::format("+{}", it.linesAdded);
        draw->AddText(ImVec2(x, y), ImGui::GetColorU32(colors.green), plus.c_str());
        draw->AddText(ImVec2(x + ImGui::CalcTextSize((plus + "  ").c_str()).x, y), ImGui::GetColorU32(colors.red),
                      std::format("-{}", it.linesRemoved).c_str());
        if (hot) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        if (clicked) {
            changesOpen(t, it.treeFrom, it.treeTo);
        }
        break;
    }
    }
    if (boxed) {
        ImGui::EndChild();
        bubbleMin = ImGui::GetItemRectMin();
        bubbleMax = ImGui::GetItemRectMax();
    }
    // right-click a message: copy it as written (a reply as its markdown)
    if (it.kind == Item::Kind::User || it.kind == Item::Kind::Assistant) {
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
            ImGui::IsMouseHoveringRect(bubbleMin, bubbleMax)) {
            ImGui::OpenPopup("message");
        }
        if (ImGui::BeginPopup("message")) {
            if (ImGui::MenuItem(ICON_FA_COPY "  Copy")) {
                ImGui::SetClipboardText(it.text.c_str());
                ui::Toast("Copied", ui::Tone::Success, 2);
            }
            ImGui::EndPopup();
        }
    }
    ImGui::PopID();
    ImGui::Spacing();
}

// the input in a rounded box, with the agent/model picker and send inside it
// the input's height: three lines, growing with the wrapped text up to eight
static float composerInputHeight(const char* text, float width) {
    const float line = ImGui::GetTextLineHeight();
    const float wrap = width - 2 * ImGui::GetStyle().WindowPadding.x - 2 * ImGui::GetStyle().FramePadding.x;
    const float need = ImGui::CalcTextSize(text, nullptr, false, wrap).y + line; // a spare line: no jump on wrap
    return std::clamp(need, line * 3, line * 8);
}

void App::composer(Thread& t, float width) {
    const auto& colors = Theme::Current()->colors;
    const AcpAgentDef* def = AcpAgents::find(t.agentId);
    const float inputHeight = composerInputHeight(input, width);
    const float button = ImGui::GetFrameHeight();
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.mantle);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.surface1);
    // a thin outline: the theme gives child windows none. read when the window begins
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1);
    ImGui::BeginChild("composer", ImVec2(width, inputHeight + button + 34),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    if (std::exchange(focusInput, false)) {
        ImGui::SetKeyboardFocusHere();
    }
    // enter sends, ctrl+enter is a new line
    // the box around it is the frame: none of its own
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    const ImVec2 inputAt = ImGui::GetCursorPos();
    const ImVec2 boxAt = ImGui::GetWindowPos();
    // paste: a picture or copied files on the clipboard become attachments. the field would
    // also paste a copied file's name as text: its text is put back
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V)) {
        const auto tempDir = std::filesystem::temp_directory_path() / "imcode-pasted";
        auto pasted = clipboardFiles(tempDir, std::getenv("IMCODE_PASTEBOARD")); // a named one: ui checks
        // a wall of text (a log, a file) goes along as a file, not into the message
        const char* text = ImGui::GetClipboardText();
        if (pasted.empty() && text && std::strlen(text) > 4000) {
            std::error_code ec;
            std::filesystem::create_directories(tempDir, ec);
            const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
            const auto file = tempDir / ("pasted-" + std::to_string(stamp) + ".txt");
            if (std::ofstream(file, std::ios::binary) << text) {
                pasted.push_back(file.string());
            }
        }
        if (!pasted.empty()) {
            attachments.insert(attachments.end(), pasted.begin(), pasted.end());
            picker.replace = std::string(input);
        }
    }
    bool send = ImGui::InputTextMultiline(
        "##input", input, sizeof(input), ImVec2(-FLT_MIN, inputHeight),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine |
            ImGuiInputTextFlags_CallbackAlways | ImGuiInputTextFlags_WordWrap,
        [](ImGuiInputTextCallbackData* data) {
            // the only way to change a field's text while it is being edited
            auto* picker = static_cast<Picker*>(data->UserData);
            if (picker->replace) {
                data->DeleteChars(0, data->BufTextLen);
                data->InsertChars(0, picker->replace->c_str());
                picker->replace.reset();
            }
            return 0;
        },
        &picker);
    ImGui::PopStyleColor(3);
    // up/down in an empty input (or one still showing a recalled prompt): the prompts sent
    // in this thread, newest first
    {
        std::vector<const std::string*> sent;
        for (auto item = t.items.rbegin(); item != t.items.rend(); ++item) {
            if (item->kind == Item::Kind::User && !item->text.empty()) {
                sent.push_back(&item->text);
            }
        }
        const bool showing = recall >= 0 && recall < static_cast<int>(sent.size()) && *sent[recall] == input;
        if (!showing) {
            recall = -1;
        }
        if (ImGui::IsItemActive() && (showing || input[0] == 0)) {
            const int to = recall + (ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? 1 : 0) -
                           (ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : 0);
            if (to != recall && to >= -1 && to < static_cast<int>(sent.size())) {
                recall = to;
                picker.replace = to < 0 ? std::string() : sent[to]->substr(0, sizeof(input) - 1);
            }
        }
    }

    // "/" at the start: the agent's commands. "@" starting the last word: the project's files
    {
        const std::string text = input;
        const size_t word = text.find_last_of(" \n") + 1; // 0 when there is no space
        const bool commandMode = text.starts_with("/") && word == 0;
        if (commandMode) {
            t.wake(); // the commands come from the agent
        }
        const bool fileMode = !commandMode && word < text.size() && text[word] == '@';
        struct Row {
            std::string label, detail, insert;
        };
        std::vector<Row> rows;
        auto matches = [](const std::string& hay, const std::string& needle) {
            const auto found = std::ranges::search(hay, needle, [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            });
            return needle.empty() || !found.empty();
        };
        if (text != picker.dismissed && commandMode) {
            // names that start with what was typed, then names that contain it
            for (const bool prefix : {true, false}) {
                for (const auto& command : t.commands) {
                    const bool starts = command.name.starts_with(text.substr(1));
                    if (starts == prefix && matches(command.name, text.substr(1)) && rows.size() < 8) {
                        rows.push_back({"/" + command.name, command.description, "/" + command.name + " "});
                    }
                }
            }
        } else if (text != picker.dismissed && fileMode) {
            const std::string key = t.host + "\n" + t.cwd;
            picker.op.check([&](std::pair<std::string, std::vector<std::string>> got) {
                picker.filesFor = std::move(got.first);
                picker.files = std::move(got.second);
            });
            if (picker.filesFor != key && !picker.op.isRunning()) {
                picker.op.start([key, host = t.host, cwd = t.cwd] { return std::pair{key, projectFiles(host, cwd)}; });
            }
            for (const auto& file : picker.filesFor == key ? picker.files : std::vector<std::string>{}) {
                if (matches(file, text.substr(word + 1)) && rows.size() < 8) {
                    // shortcut: a mention is sent as "@path" in the text, which agents read;
                    // a resource link block when one needs the file attached
                    rows.push_back({file.substr(file.find_last_of('/') + 1), file, text.substr(0, word) + "@" + file + " "});
                }
            }
        }
        if (!rows.empty()) {
            picker.selected = std::clamp(picker.selected + (ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : 0) -
                                             (ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? 1 : 0),
                                         0, static_cast<int>(rows.size()) - 1);
            int accept = send ? picker.selected : -1; // enter takes the row, it does not send
            send = false;
            if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                picker.dismissed = text;
            }
            // a list over the box's top edge; it must not take the keyboard from the input
            const float line = ImGui::GetTextLineHeight();
            const float height =
                static_cast<float>(rows.size()) * (line + 10 + ImGui::GetStyle().ItemSpacing.y) + 14;
            ImGui::SetNextWindowPos(ImVec2(boxAt.x, boxAt.y - height - 6));
            ImGui::SetNextWindowSize(ImVec2(width, height));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, colors.mantle);
            ImGui::PushStyleColor(ImGuiCol_Border, colors.surface1);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1, 1)); // one row is a short window
            ImGui::Begin("##picker", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                             ImGuiWindowFlags_NoMove);
            for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
                const ImVec2 at = ImGui::GetCursorPos();
                if (rowButton(("##pick" + std::to_string(i)).c_str(), i == picker.selected, line + 8)) {
                    accept = i;
                }
                const ImVec2 next = ImGui::GetCursorPos();
                ImGui::SetCursorPos(ImVec2(at.x + 10, at.y + 4));
                ImGui::TextUnformatted(rows[i].label.c_str());
                ImGui::SameLine(0, 14);
                ImGui::TextColored(colors.overlay1, "%s", rows[i].detail.substr(0, 70).c_str());
                ImGui::SetCursorPos(ImVec2(next.x, next.y - ImGui::GetStyle().ItemSpacing.y + 2));
                ImGui::Dummy(ImVec2(0, 0)); // imgui wants an item after a cursor move
            }
            ImGui::End();
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(2);
            if (accept >= 0) {
                picker.replace = rows[accept].insert;
                picker.selected = 0;
                focusInput = true; // a click on a row took the keyboard
            }
        } else {
            picker.selected = 0;
        }
    }
    if (!input[0]) {
        const ImVec2 after = ImGui::GetCursorPos();
        const ImVec2 pad = ImGui::GetStyle().FramePadding;
        ImGui::SetCursorPos(ImVec2(inputAt.x + pad.x, inputAt.y + pad.y));
        ImGui::TextColored(colors.overlay1, "Ask anything");
        ImGui::SetCursorPos(after);
    }

    // attach: pictures and files, picked here or dropped on the window
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    if (ImGui::Button(ICON_FA_PAPERCLIP, ImVec2(button, button))) {
        imshell::OpenFileDialog({{"Any file", "*"}}, [this](const std::string& path) { attachments.push_back(path); });
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Attach a picture or a file");
    }
    ImGui::SameLine();
    // how full the agent's context is, at the left of the row
    if (t.contextSize > 0) {
        const int percent = static_cast<int>(100 * t.contextUsed / t.contextSize);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(percent >= 80 ? colors.yellow : colors.overlay1, "%d%% context", percent);
        if (ImGui::IsItemHovered()) {
            const std::string spent = t.cost >= 0 ? std::format("\n${:.2f} so far", t.cost) : "";
            ImGui::SetTooltip("%lldk of %lldk tokens%s", t.contextUsed / 1000, t.contextSize / 1000, spent.c_str());
        }
        ImGui::SameLine();
    }
    // "<agent> <model> <effort>": the picker
    const acp::ConfigOption* model = optionOf(t, "model");
    const acp::ConfigOption* effort = optionOf(t, "thought_level");
    std::string label = model ? currentName(model) : def ? def->name : t.agentId;
    if (effort) {
        label += "  " + currentName(effort);
    }
    // the model button: its padding, the agent's mark, a gap, the label
    constexpr float chipPad = 7, chipIcon = 16, chipGap = 6;
    const float labelWidth = ImGui::CalcTextSize(label.c_str()).x + 2 * chipPad + chipIcon + chipGap;
    // the permission mode, beside the picker: it decides what the agent may do unasked.
    // the ones that let it do anything stand out
    const acp::ConfigOption* mode = optionOf(t, "mode");
    std::string modeLabel = mode ? currentName(mode) : "";
    const bool risky = mode && (riskyMode(modeLabel) || riskyMode(mode->currentValue.get<std::string>()));
    if (!modeLabel.empty()) {
        const std::string icon = risky ? ICON_FA_TRIANGLE_EXCLAMATION : ICON_FA_SHIELD_HALVED;
        // where there is no room for its name (a narrow window, the context meter): the icon only
        const float free = ImGui::GetWindowContentRegionMax().x - ImGui::GetCursorPosX() - labelWidth - button - 40;
        modeLabel = ImGui::CalcTextSize((icon + "  " + modeLabel).c_str()).x + 8 > free ? icon : icon + "  " + modeLabel;
    }
    const float modeWidth = modeLabel.empty() ? 0 : ImGui::CalcTextSize(modeLabel.c_str()).x + 8 +
                                                        ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - labelWidth - button -
                         ImGui::GetStyle().ItemSpacing.x - modeWidth);
    if (!modeLabel.empty()) {
        // level with the buttons beside it: the text drawn in the middle of a button-high
        // area (a selectable would follow the line's text offset and sit lower)
        const ImVec2 chipSize(modeWidth - ImGui::GetStyle().ItemSpacing.x, button);
        const bool clicked = ImGui::InvisibleButton("##mode", chipSize);
        const ImVec2 chipAt = ImGui::GetItemRectMin();
        const bool chipHot = ImGui::IsItemHovered();
        if (chipHot) {
            ImGui::GetWindowDrawList()->AddRectFilled(chipAt, ImVec2(chipAt.x + chipSize.x, chipAt.y + chipSize.y),
                                                      ImGui::GetColorU32(colors.surface0), 6);
        }
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(chipAt.x + 4, chipAt.y + (chipSize.y - ImGui::GetTextLineHeight()) * 0.5f),
            ImGui::GetColorU32(risky ? colors.peach : chipHot ? colors.text : colors.overlay1), modeLabel.c_str());
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s: permission mode (shift+tab to change)", currentName(mode).c_str());
        }
        if (clicked) {
            devOpenModels = true; // the picker, where modes are listed
        }
        ImGui::SameLine();
    }
    // shift+tab in the input: the next mode
    if (mode && !mode->options.empty() && ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
        ImGui::IsKeyChordPressed(ImGuiMod_Shift | ImGuiKey_Tab)) {
        // the modes that allow everything are left out: only the picker turns them on
        std::vector<const acp::ConfigOptionValue*> safe;
        for (const auto& choice : mode->options) {
            if (!riskyMode(choice.name) && !riskyMode(choice.value)) {
                safe.push_back(&choice);
            }
        }
        const std::string now = mode->currentValue.get<std::string>();
        size_t at = 0;
        while (at < safe.size() && safe[at]->value != now) {
            ++at;
        }
        if (!safe.empty()) { // from a risky mode (not in the list): the first safe one
            t.setConfig(mode->id, safe[at < safe.size() ? (at + 1) % safe.size() : 0]->value);
        }
    }
    // the agent's mark and the model: plain until hovered (or open), then a rounded background
    {
        const ImVec2 chip(labelWidth, button);
        const bool clicked = ImGui::InvisibleButton("##models", chip);
        const ImVec2 at = ImGui::GetItemRectMin();
        const bool lit = ImGui::IsItemHovered() || ImGui::IsPopupOpen("models");
        ImDrawList* draw = ImGui::GetWindowDrawList();
        if (lit) {
            draw->AddRectFilled(at, ImVec2(at.x + chip.x, at.y + chip.y), ImGui::GetColorU32(colors.surface0),
                                ImGui::GetStyle().FrameRounding + 3);
        }
        agentIconAt(t.agentId, ImVec2(at.x + chipPad, std::floor(at.y + (button - chipIcon) * 0.5f)), chipIcon);
        draw->AddText(ImVec2(at.x + chipPad + chipIcon + chipGap, at.y + (button - ImGui::GetTextLineHeight()) * 0.5f),
                      ImGui::GetColorU32(lit ? colors.text : colors.subtext1), label.c_str());
        if (clicked) {
            t.wake(); // the models and modes come from the agent
            ImGui::OpenPopup("models");
        }
    }
    if (std::exchange(devOpenModels, false)) {
        t.wake();
        ImGui::OpenPopup("models");
    }
    // above the button, right edges aligned; plain panes, not the composer's rounded box
    // kept inside the window when it is narrower than the popup
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float popupWidth = std::min(600.0f, viewport->WorkSize.x - 16);
    ImGui::SetNextWindowSize(ImVec2(popupWidth, 0));
    ImGui::SetNextWindowPos(ImVec2(std::clamp(ImGui::GetItemRectMax().x, viewport->WorkPos.x + 8 + popupWidth,
                                              viewport->WorkPos.x + viewport->WorkSize.x - 8),
                                   ImGui::GetItemRectMin().y - 8),
                            ImGuiCond_Always, ImVec2(1, 1));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, colors.mantle);
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6); // three lists side by side: thin bars
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 12));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 12);
    const bool modelsOpen = ImGui::BeginPopup("models");
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    if (modelsOpen) {
        static char filter[64] = {};
        // agents; models of this thread's agent; its other settings
        ImGui::BeginChild("agents", ImVec2(ImGui::GetContentRegionAvail().x * 0.27f, 300));
        ImGui::TextColored(colors.subtext0, "Agent");
        for (const AcpAgentDef* agent : agentsOn()) {
            if (pickRow(agent->id, agent->name, agent->id == t.agentId,
                        [&] { agentIcon(agent->id); }) &&
                agent->id != t.agentId) {
                switchAgentTo = agent->id; // a thread keeps its agent: this starts a new one
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("models", ImVec2(ImGui::GetContentRegionAvail().x * 0.58f, 300));
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##filter", ICON_FA_MAGNIFYING_GLASS "  Search models", filter,
                                 sizeof(filter));
        if (model) {
            choices(t, *model, filter);
        } else {
            ImGui::TextColored(colors.overlay1, "%s",
                               t.starting() ? "Starting the agent..." : "This agent offers no models");
        }
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("settings", ImVec2(0, 300));
        for (const auto& option : t.config) {
            if (&option == model || option.type != "select" || !option.currentValue.is_string()) {
                continue;
            }
            ImGui::TextColored(colors.subtext0, "%s", option.name.c_str());
            choices(t, option);
            ImGui::Spacing();
        }
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::SameLine();
    // send (or stop): a filled circle, lit when there is something to send
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##send", ImVec2(button, button));
    const bool ready = t.busy() || input[0];
    const ImVec4 fill = t.busy() ? colors.red : ready ? colors.blue : colors.surface1;
    const ImVec2 centre(at.x + button * 0.5f, at.y + button * 0.5f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddCircleFilled(centre, button * 0.5f,
                          ImGui::GetColorU32(ImGui::IsItemHovered() && ready
                                                 ? ImVec4(fill.x, fill.y, fill.z, 0.85f)
                                                 : fill));
    const char* glyph = t.busy() ? ICON_FA_STOP : ICON_FA_ARROW_UP;
    const ImVec2 size = ImGui::CalcTextSize(glyph);
    draw->AddText(ImVec2(centre.x - size.x * 0.5f, centre.y - size.y * 0.5f),
                  ImGui::GetColorU32(ready ? colors.crust : colors.overlay1), glyph);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", t.busy() ? "Stop" : "Send (Enter)");
    }
    if (pressed && t.busy()) {
        t.cancel();
    }
    send |= pressed && !t.busy();
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    if (send && (input[0] || !attachments.empty())) { // while a turn runs this queues
        shownRevision = -3; // what was sent comes into view
        t.send(input, std::exchange(attachments, {}));
        input[0] = 0;
        focusInput = true;
    }
}

// what will go with the next message, each removable
void App::attachedRows(float left) {
    const auto& colors = Theme::Current()->colors;
    int drop = -1;
    for (size_t i = 0; i < attachments.size(); ++i) {
        ImGui::SetCursorPosX(left + 8);
        ImGui::PushID(static_cast<int>(1000 + i));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, colors.overlay1);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
        if (ImGui::SmallButton(ICON_FA_XMARK)) {
            drop = static_cast<int>(i);
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        ImGui::PopID();
        ImGui::SameLine();
        ImGui::TextColored(colors.subtext0, ICON_FA_PAPERCLIP "  %s",
                           std::filesystem::path(attachments[i]).filename().string().c_str());
    }
    if (drop >= 0) {
        attachments.erase(attachments.begin() + drop);
    }
}

void App::statusLine(Thread& t, float left) {
    const auto& colors = Theme::Current()->colors;
    // the repository's state, refreshed while this thread is shown
    const std::string key = t.host + "\n" + t.cwd;
    const long long now = static_cast<long long>(ImGui::GetTime() * 1000);
    gitOp.check([&](std::pair<std::string, Git::Status> got) {
        gitFor = got.first;
        git = got.second;
    });
    if (!gitOp.isRunning() && (gitFor != key || now - gitAt > 5000)) {
        gitAt = now;
        gitOp.start([key, host = t.host, cwd = t.cwd] { return std::pair{key, Git::status(host, cwd)}; });
    }
    // where it works: click to pick another project
    ImGui::SetCursorPosX(left);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.surface0);
    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
    // the folder, short: ~ for the home folder, the last two parts when it is still long
    std::string where = t.cwd;
    if (const char* home = std::getenv("HOME"); t.host.empty() && home && where.starts_with(home)) {
        where.replace(0, std::string(home).size(), "~");
    }
    if (where.size() > 44) {
        const size_t cut = where.rfind('/', where.rfind('/') - 1);
        where = ".../" + where.substr(cut + 1);
    }
    if (ImGui::Button((std::string(t.host.empty() ? ICON_FA_FOLDER : ICON_FA_SERVER) + "  " +
                       (t.host.empty() ? "" : t.host + ":") + where + "  " ICON_FA_CHEVRON_DOWN "##where")
                          .c_str())) {
        ImGui::OpenPopup("projects");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", t.cwd.c_str());
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    projectPopup();
    if (gitFor == key && git.repo) {
        ImGui::SameLine(0, 16);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(colors.subtext0, ICON_FA_CODE_BRANCH "  %s", git.branch.c_str());
        if (git.changed > 0) {
            // click: what changed, file by file
            ImGui::SameLine(0, 10);
            ImGui::PushStyleColor(ImGuiCol_Text, colors.yellow);
            if (ImGui::Selectable(std::format("{} changed##changes", git.changed).c_str(), false, 0,
                                  ImGui::CalcTextSize(std::format("{} changed", git.changed).c_str()))) {
                changesOpen(t);
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("See what changed");
            }
        }
        // before the first message: work in this checkout, or in a worktree of its own
        if (t.items.empty() && t.root.empty()) {
            // on the same line when there is room for it, else under the folder
            const char* label = t.wantWorktree ? ICON_FA_CODE_FORK "  New worktree" : ICON_FA_CODE_FORK "  Current checkout";
            const float room = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x - ImGui::GetItemRectMax().x;
            if (room > ImGui::CalcTextSize(label).x + 40) {
                ImGui::SameLine(0, 16);
            } else {
                ImGui::SetCursorPosX(left);
            }
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.surface0);
            ImGui::PushStyleColor(ImGuiCol_Text, t.wantWorktree ? colors.blue : colors.subtext0);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
            if (ImGui::Button(label)) {
                t.wantWorktree = !t.wantWorktree;
            }
            ImGui::PopStyleVar();
            ImGui::PopStyleColor(3);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", t.wantWorktree
                                            ? "The first message makes a worktree on a new branch and works there"
                                            : "Works in the project's folder. Click to use a worktree of its own");
            }
        } else if (!t.root.empty()) {
            ImGui::SameLine(0, 16);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(colors.overlay1, ICON_FA_CODE_FORK "  worktree");
        }
    }
}

// the thread in a window of its own that never scrolls: only the transcript inside it does, so
// the composer and the line under it stay put
void App::threadView(Thread& t) {
    ImGui::BeginChild("thread-view", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    threadBody(t);
    ImGui::EndChild();
}

void App::threadBody(Thread& t) {
    const auto& colors = Theme::Current()->colors;
    const AcpAgentDef* def = AcpAgents::find(t.agentId);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float width = std::min(760.0f, avail.x - 2 * Theme::Spacing::M); // the reading column
    const float left = (avail.x - width) * 0.5f;
    const float composerHeight = composerInputHeight(input, width) + ImGui::GetFrameHeight() + 34;

    if (t.items.empty() && !t.busy()) {
        // nothing said yet: the question and the composer in the middle
        const std::string name = std::filesystem::path(t.cwd).filename().string();
        const std::string ask = "What should we build in " + name + "?";
        ImGui::PushFont(nullptr, 24.0f);
        const float askWidth = ImGui::CalcTextSize(ask.c_str()).x;
        // a third of the way down, or centred when the area is short (a tall terminal)
        const float block = composerHeight + 90;
        ImGui::SetCursorPos(ImVec2((avail.x - askWidth) * 0.5f,
                                   std::max(0.0f, std::min(avail.y * 0.36f, (avail.y - block) * 0.5f))));
        ImGui::TextUnformatted(ask.c_str());
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, Theme::Spacing::L));
        attachedRows(left);
        ImGui::SetCursorPosX(left);
        composer(t, width);
        statusLine(t, left);
        if (t.starting()) {
            ImGui::SetCursorPosX(left + 8);
            ImGui::TextColored(colors.overlay1, "Starting %s...",
                               def ? def->name.c_str() : t.agentId.c_str());
        }
        return;
    }

    const float queued =
        static_cast<float>(t.queue.size() + attachments.size()) * ImGui::GetFrameHeightWithSpacing();
    // cmd+f: a find bar over the transcript; enter and shift+enter step through the hits
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_F)) {
        find.open = find.focus = true;
    }
    if (find.open) {
        const auto lower = [](std::string s) {
            std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
            return s;
        };
        const std::string q = lower(find.query);
        find.hits.clear();
        for (size_t i = 0; !q.empty() && i < t.items.size(); ++i) {
            const Item& it = t.items[i];
            if (lower(it.text).find(q) != std::string::npos || lower(it.toolTitle).find(q) != std::string::npos ||
                lower(it.toolOutput).find(q) != std::string::npos) {
                find.hits.push_back(i);
            }
        }
        ImGui::Indent(left);
        if (std::exchange(find.focus, false)) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(std::min(320.0f, width - 140));
        const bool typed = ImGui::InputTextWithHint("##find", ICON_FA_MAGNIFYING_GLASS "  Find in thread", find.query,
                                                    sizeof(find.query));
        const bool active = ImGui::IsItemActive() || ImGui::IsItemFocused();
        const int count = static_cast<int>(find.hits.size());
        if (typed) {
            find.at = 0;
            find.jump = count > 0;
        }
        if (active && count > 0 && ImGui::IsKeyPressed(ImGuiKey_Enter)) {
            find.at = (find.at + (ImGui::GetIO().KeyShift ? count - 1 : 1)) % count;
            find.jump = true;
            find.focus = true; // enter leaves the field: back into it
        }
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (q.empty()) {
            ImGui::TextColored(colors.overlay0, " ");
        } else if (count == 0) {
            ImGui::TextColored(colors.peach, "No match");
        } else {
            ImGui::TextColored(colors.overlay1, "%d of %d", std::min(find.at, count - 1) + 1, count);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_XMARK "##find-close") || (active && ImGui::IsKeyPressed(ImGuiKey_Escape))) {
            find.open = false;
            find.query[0] = 0;
            find.hits.clear();
        }
        ImGui::Unindent(left);
        // a hit inside folded tool calls opens them
        if (find.jump && find.at < count) {
            const size_t hit = find.hits[find.at];
            for (size_t i = 0; i < t.items.size();) {
                size_t end = i;
                while (end < t.items.size() && foldable(t.items[end])) {
                    ++end;
                }
                if (end - i >= 3 && hit >= i && hit < end) {
                    openGroups[&t].insert(i);
                }
                i = std::max(end, i + 1);
            }
        }
    } else {
        find.hits.clear();
    }
    ImGui::BeginChild("transcript", ImVec2(0, -(composerHeight + Theme::Spacing::L + queued +
                                                ImGui::GetFrameHeightWithSpacing())));
    find.itemY.assign(t.items.size(), -1);
    // the hit being shown: a bar at its left
    const auto mark = [&](size_t i) {
        if (find.open && find.at < static_cast<int>(find.hits.size()) && find.hits[find.at] == i) {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(at.x - 12, at.y),
                                                      ImVec2(at.x - 9, at.y + ImGui::GetTextLineHeight() * 1.5f),
                                                      ImGui::GetColorU32(colors.yellow), 2);
        }
    };
    ImGui::Dummy(ImVec2(0, Theme::Spacing::L));
    ImGui::Indent(left);
    ImGui::PushTextWrapPos(left + width);
    // by index: answering a sign-in can add an item and move the rest
    for (size_t i = 0; i < t.items.size(); ++i) {
        // finished tool calls in a row, with nothing to look at (no edits), fold into one line
        size_t end = i;
        while (end < t.items.size() && foldable(t.items[end])) {
            ++end;
        }
        if (end - i >= 3) {
            auto& open = openGroups[&t];
            const bool shown = open.contains(i);
            if (toolGroupRow(t, i, end, width, shown)) {
                if (shown) {
                    open.erase(i);
                } else {
                    open.insert(i);
                }
            }
            for (size_t k = i; k < end; ++k) {
                find.itemY[k] = ImGui::GetCursorPosY(); // folded: the line stands for them
                if (shown) {
                    mark(k);
                    transcriptItem(t, t.items[k], static_cast<int>(k), width);
                }
            }
            i = end - 1;
            continue;
        }
        find.itemY[i] = ImGui::GetCursorPosY();
        mark(i);
        transcriptItem(t, t.items[i], static_cast<int>(i), width);
    }
    // how long the turn has run
    std::string working = "Working";
    if (t.busy() && !t.starting()) {
        const auto since = turnSince.try_emplace(&t, std::chrono::steady_clock::now()).first->second;
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - since).count();
        working += secs < 60 ? std::format("  ·  {}s", secs) : std::format("  ·  {}m {:02}s", secs / 60, secs % 60);
    } else {
        turnSince.erase(&t);
    }
    if (t.busy() || t.starting()) {
        ui::Progress(-1, t.starting() ? "Starting" : working.c_str(), 160);
        ImGui::Dummy(ImVec2(0, Theme::Spacing::M)); // clear of the child's bottom edge
    }
    ImGui::PopTextWrapPos();
    ImGui::Unindent(left);
    if (shownRevision < 0 || t.replaying()) {
        ImGui::SetScrollHereY(1.0f);
        // a few frames: what was just added is laid out by then; history still arriving restarts it
        shownRevision = t.replaying() ? -3 : shownRevision + 1;
    } else if (find.jump && find.at < static_cast<int>(find.hits.size()) && find.itemY[find.hits[find.at]] >= 0) {
        ImGui::SetScrollY(std::max(0.0f, find.itemY[find.hits[find.at]] - 60));
        find.jump = false;
    } else if (!find.open && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 2) {
        ImGui::SetScrollHereY(1.0f); // at the end: a reply that grows stays in view
    }
    const bool away = ImGui::GetScrollY() < ImGui::GetScrollMaxY() - 80;
    const ImVec2 viewAt = ImGui::GetWindowPos(), viewSize = ImGui::GetWindowSize();
    ImGui::EndChild();
    if (away && shownRevision >= 0) {
        // further up while there is more below: a way back to the newest. a small child of
        // its own over the transcript, so the text under it does not take the click
        const ImVec2 back = ImGui::GetCursorScreenPos();
        const float size = 30;
        ImGui::SetCursorScreenPos(ImVec2(viewAt.x + left + (width - size) * 0.5f, viewAt.y + viewSize.y - size - 10));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
        ImGui::BeginChild("to-newest", ImVec2(size, size), 0, ImGuiWindowFlags_NoScrollbar);
        ImGui::PushStyleColor(ImGuiCol_Button, colors.surface1);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, size * 0.5f);
        if (ImGui::Button(ICON_FA_ARROW_DOWN, ImVec2(size, size))) {
            shownRevision = -3;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("To the newest");
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::SetCursorScreenPos(back); // what follows goes where it would without the button
    }
    // what waits for the running turn, each removable
    int drop = -1, now = -1;
    for (size_t i = 0; i < t.queue.size(); ++i) {
        ImGui::SetCursorPosX(left + 8);
        ImGui::PushID(static_cast<int>(i));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0)); // a quiet x, lit on hover
        ImGui::PushStyleColor(ImGuiCol_Text, colors.overlay1);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
        if (ImGui::SmallButton(ICON_FA_XMARK)) {
            drop = static_cast<int>(i);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Remove from the queue");
        }
        ImGui::SameLine(0, 2);
        if (ImGui::SmallButton(ICON_FA_ARROW_UP)) {
            now = static_cast<int>(i);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Send now: stops the running turn");
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        ImGui::PopID();
        ImGui::SameLine();
        ImGui::TextColored(colors.overlay1, "Queued: %s",
                           t.queue[i].text.substr(0, std::min(t.queue[i].text.find('\n'), size_t{90})).c_str());
    }
    if (drop >= 0) {
        t.queue.erase(t.queue.begin() + drop);
    } else if (now >= 0) {
        t.sendNow(static_cast<size_t>(now));
    }
    attachedRows(left);
    ImGui::SetCursorPosX(left);
    composer(t, width);
    statusLine(t, left);
}
