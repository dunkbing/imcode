#include "app.hpp"

#include "markdown.hpp"
#include "imshell/button.hpp"
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

void App::transcriptItem(Thread& t, Item& it, int index, float width) {
    const auto& colors = Theme::Current()->colors;
    ImGui::PushID(index);
    // everything but the user's bubble sits in the reading column: cards and the markdown
    // renderer fill the window they are in
    const bool boxed = it.kind != Item::Kind::User;
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
        ImGui::SetCursorScreenPos(ImVec2(min.x + pad, min.y + pad));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + size.x + 1);
        ImGui::TextUnformatted(it.text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::SetCursorScreenPos(ImVec2(at.x, max.y));
        ImGui::Dummy(ImVec2(width, Theme::Spacing::M));
        break;
    }
    case Item::Kind::Assistant:
        Markdown::render(it.text, std::format("m{}", index));
        break;
    case Item::Kind::Thought:
        ImGui::PushStyleColor(ImGuiCol_Text, colors.overlay1);
        ImGui::TextWrapped("%s", it.text.c_str());
        ImGui::PopStyleColor();
        break;
    case Item::Kind::Tool: {
        const bool open =
            ImGui::TreeNodeEx("tool", it.toolOutput.empty() ? ImGuiTreeNodeFlags_Leaf : 0, "%s",
                              it.toolTitle.empty() ? "Tool" : it.toolTitle.c_str());
        ImGui::SameLine();
        ui::Badge(it.toolStatus.empty()            ? "pending"
                  : it.toolStatus == "in_progress" ? "running"
                                                   : it.toolStatus.c_str(),
                  it.toolStatus == "failed"      ? ui::Tone::Danger
                  : it.toolStatus == "completed" ? ui::Tone::Success
                                                 : ui::Tone::Info);
        if (!it.toolPath.empty()) {
            ImGui::SameLine();
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
            ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.crust);
            ImGui::PushStyleColor(ImGuiCol_Border, colors.surface1);
            ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6);
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 0));
            const float lineHeight = ImGui::GetTextLineHeight() + 4;
            const float height = std::min(320.0f, lineHeight * static_cast<float>(change.lines.size() + 1) + 20);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
            ImGui::BeginChild("change", ImVec2(0, height + 16),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                              ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::PopStyleVar();
            // the file, then how much was added and removed
            // the file: click to read it in the viewer
            ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
            if (ImGui::Selectable((std::string(ICON_FA_FILE "  ") + change.path).c_str(), false, 0,
                                  ImGui::CalcTextSize((std::string(ICON_FA_FILE "  ") + change.path).c_str())) &&
                !change.path.starts_with("/")) {
                showFile(change.path);
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            ImGui::SameLine();
            ImGui::TextColored(colors.green, "+%d", change.added);
            ImGui::SameLine();
            ImGui::TextColored(colors.red, "-%d", change.removed);
            ImGui::Dummy(ImVec2(0, 4));
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const float width = std::max(ImGui::GetContentRegionAvail().x, 2000.0f);
            for (const auto& line : change.lines) {
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
                    ImGui::TextColored(line.kind == '+'   ? colors.green
                                       : line.kind == '-' ? colors.red
                                                          : colors.subtext0,
                                       "%c %s", line.kind, line.text.c_str());
                }
                ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + lineHeight));
                ImGui::Dummy(ImVec2(0, 0)); // imgui wants an item after a cursor move
            }
            ImGui::EndChild();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(2);
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
                ImGui::TextWrapped("Finish signing in in the Terminal window, then continue.");
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
        break;
    }
    if (boxed) {
        ImGui::EndChild();
    }
    ImGui::PopID();
    ImGui::Spacing();
}

// the input in a rounded box, with the agent/model picker and send inside it
void App::composer(Thread& t, float width) {
    const auto& colors = Theme::Current()->colors;
    const AcpAgentDef* def = AcpAgents::find(t.agentId);
    const float inputHeight = ImGui::GetTextLineHeight() * 3;
    const float button = ImGui::GetFrameHeight();
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.mantle);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.surface1);
    ImGui::BeginChild("composer", ImVec2(width, inputHeight + button + 34),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (std::exchange(focusInput, false)) {
        ImGui::SetKeyboardFocusHere();
    }
    // enter sends, ctrl+enter is a new line
    // the box around it is the frame: none of its own
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    const ImVec2 inputAt = ImGui::GetCursorPos();
    bool send = ImGui::InputTextMultiline(
        "##input", input, sizeof(input), ImVec2(-FLT_MIN, inputHeight),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine);
    ImGui::PopStyleColor(3);
    if (!input[0]) {
        const ImVec2 after = ImGui::GetCursorPos();
        const ImVec2 pad = ImGui::GetStyle().FramePadding;
        ImGui::SetCursorPos(ImVec2(inputAt.x + pad.x, inputAt.y + pad.y));
        ImGui::TextColored(colors.overlay1, "Ask anything");
        ImGui::SetCursorPos(after);
    }

    // "<agent> <model> <effort>": the picker
    const acp::ConfigOption* model = optionOf(t, "model");
    const acp::ConfigOption* effort = optionOf(t, "thought_level");
    std::string label = model ? currentName(model) : def ? def->name : t.agentId;
    if (effort) {
        label += "  " + currentName(effort);
    }
    const float labelWidth = ImGui::CalcTextSize(label.c_str()).x + 24;
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - labelWidth - button - 16 - 6 -
                         ImGui::GetStyle().ItemSpacing.x);
    agentIcon(t.agentId, 16, button);
    ImGui::SameLine(0, 6);
    if (imshell::Button((label + "##models").c_str(), imshell::ButtonVariant::Secondary,
                       ImVec2(labelWidth, 0))) {
        ImGui::OpenPopup("models");
    }
    if (std::exchange(devOpenModels, false)) {
        ImGui::OpenPopup("models");
    }
    // above the button, right edges aligned; plain panes, not the composer's rounded box
    ImGui::SetNextWindowSize(ImVec2(std::min(600.0f, ImGui::GetMainViewport()->WorkSize.x - 16), 0));
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMax().x, ImGui::GetItemRectMin().y - 8),
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
    if (send && input[0]) { // while a turn runs this queues
        t.send(input);
        input[0] = 0;
        focusInput = true;
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
            ImGui::SameLine(0, 10);
            ImGui::TextColored(colors.yellow, "%d changed", git.changed);
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

void App::threadView(Thread& t) {
    const auto& colors = Theme::Current()->colors;
    const AcpAgentDef* def = AcpAgents::find(t.agentId);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float width = std::min(760.0f, avail.x - 2 * Theme::Spacing::M); // the reading column
    const float left = (avail.x - width) * 0.5f;
    const float composerHeight = ImGui::GetTextLineHeight() * 3 + ImGui::GetFrameHeight() + 34;

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

    const float queued = static_cast<float>(t.queue.size()) * ImGui::GetFrameHeightWithSpacing();
    ImGui::BeginChild("transcript", ImVec2(0, -(composerHeight + Theme::Spacing::L + queued +
                                                ImGui::GetFrameHeightWithSpacing())));
    ImGui::Dummy(ImVec2(0, Theme::Spacing::L));
    ImGui::Indent(left);
    ImGui::PushTextWrapPos(left + width);
    // by index: answering a sign-in can add an item and move the rest
    for (size_t i = 0; i < t.items.size(); ++i) {
        transcriptItem(t, t.items[i], static_cast<int>(i), width);
    }
    if (t.busy() || t.starting()) {
        ui::Progress(-1, t.starting() ? "Starting" : "Working", 160);
        ImGui::Dummy(ImVec2(0, Theme::Spacing::M)); // clear of the child's bottom edge
    }
    ImGui::PopTextWrapPos();
    ImGui::Unindent(left);
    if (shownRevision != t.revision()) {
        shownRevision = t.revision();
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    // what waits for the running turn, each removable
    int drop = -1;
    for (size_t i = 0; i < t.queue.size(); ++i) {
        ImGui::SetCursorPosX(left + 8);
        ImGui::PushID(static_cast<int>(i));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0)); // a quiet x, lit on hover
        ImGui::PushStyleColor(ImGuiCol_Text, colors.overlay1);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
        if (ImGui::SmallButton(ICON_FA_XMARK)) {
            drop = static_cast<int>(i);
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        ImGui::PopID();
        ImGui::SameLine();
        ImGui::TextColored(colors.overlay1, "Queued: %s",
                           t.queue[i].substr(0, std::min(t.queue[i].find('\n'), size_t{90})).c_str());
    }
    if (drop >= 0) {
        t.queue.erase(t.queue.begin() + drop);
    }
    ImGui::SetCursorPosX(left);
    composer(t, width);
    statusLine(t, left);
}
