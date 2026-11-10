#include "app.hpp"

#include "markdown.hpp"
#include "mydear/button.hpp"
#include "mydear/components.hpp"
#include "mydear/themes.hpp"
#include <algorithm>
#include <format>
#include <optional>

namespace Theme = mydear::Theme;
namespace ui = mydear::ui;

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

    // one setting as a list; true when a choice was clicked
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
            const bool current = choice.value == value;
            if (ImGui::Selectable(
                    std::format("{}{}##{}", choice.name, current ? "   " ICON_FA_CHECK : "", option.id)
                        .c_str(),
                    current, ImGuiSelectableFlags_DontClosePopups)) {
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
        if (open) {
            if (!it.toolOutput.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                ImGui::TextWrapped("%s", it.toolOutput.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::TreePop();
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
            if (mydear::SmallButton(std::format("{}##p{}", option.name, i).c_str(),
                                    option.kind == "allow_once" ? mydear::ButtonVariant::Primary
                                                                : mydear::ButtonVariant::Secondary)) {
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
                if (mydear::SmallButton("Continue", mydear::ButtonVariant::Primary)) {
                    t.continueAfterTerminalSignIn(it);
                }
            }
        }
        for (size_t i = 0; !it.answered && i < it.authMethods.size(); ++i) {
            if (i > 0) {
                ImGui::SameLine(0, Theme::Spacing::S);
            }
            if (mydear::SmallButton(std::format("{}##a{}", it.authMethods[i].name, i).c_str(),
                                    mydear::ButtonVariant::Primary)) {
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
    ImGui::AlignTextToFramePadding();
    agentIcon(t.agentId);
    ImGui::SameLine(0, 6);
    if (mydear::Button((label + "##models").c_str(), mydear::ButtonVariant::Secondary,
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
            agentIcon(agent->id);
            ImGui::SameLine(0, 8);
            if (ImGui::Selectable(agent->name.c_str(), agent->id == t.agentId,
                                  ImGuiSelectableFlags_DontClosePopups) &&
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
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (t.busy()) {
        if (mydear::IconButton(ICON_FA_STOP, mydear::ButtonVariant::Danger, ImVec2(button, button))) {
            t.cancel();
        }
    } else {
        send |= mydear::IconButton(ICON_FA_ARROW_UP, mydear::ButtonVariant::Primary,
                                   ImVec2(button, button));
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
    if (send && !t.busy() && input[0]) {
        t.send(input);
        input[0] = 0;
        focusInput = true;
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
        ImGui::SetCursorPos(ImVec2((avail.x - askWidth) * 0.5f, avail.y * 0.36f));
        ImGui::TextUnformatted(ask.c_str());
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, Theme::Spacing::L));
        ImGui::SetCursorPosX(left);
        composer(t, width);
        ImGui::SetCursorPosX(left + 8);
        ImGui::TextColored(colors.overlay1, ICON_FA_FOLDER "  %s%s%s", t.host.c_str(),
                           t.host.empty() ? "" : ":", t.cwd.c_str());
        if (t.starting()) {
            ImGui::SetCursorPosX(left + 8);
            ImGui::TextColored(colors.overlay1, "Starting %s...",
                               def ? def->name.c_str() : t.agentId.c_str());
        }
        return;
    }

    ImGui::BeginChild("transcript", ImVec2(0, -(composerHeight + Theme::Spacing::L)));
    ImGui::Dummy(ImVec2(0, Theme::Spacing::L));
    ImGui::Indent(left);
    ImGui::PushTextWrapPos(left + width);
    // by index: answering a sign-in can add an item and move the rest
    for (size_t i = 0; i < t.items.size(); ++i) {
        transcriptItem(t, t.items[i], static_cast<int>(i), width);
    }
    if (t.busy() || t.starting()) {
        ui::Progress(-1, t.starting() ? "Starting" : "Working", 160);
    }
    ImGui::PopTextWrapPos();
    ImGui::Unindent(left);
    if (shownRevision != t.revision()) {
        shownRevision = t.revision();
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::SetCursorPosX(left);
    composer(t, width);
}
