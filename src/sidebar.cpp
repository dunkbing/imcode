#include "app.hpp"

#include "imshell/button.hpp"
#include "imshell/components.hpp"
#include "imshell/themes.hpp"
#include <algorithm>

namespace Theme = imshell::Theme;
namespace ui = imshell::ui;

void App::sidebar() {
    const auto& colors = Theme::Current()->colors;
    const float button = ImGui::GetFrameHeight();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;

    // search, add project, new thread
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 3 * (button + spacing));
    ImGui::InputTextWithHint("##search", ICON_FA_MAGNIFYING_GLASS "  Search", search, sizeof(search));
    ImGui::SameLine();
    // which project's threads are listed; picking one also makes it where new threads go
    // lit while one project is picked
    ImGui::PushStyleColor(ImGuiCol_Text, filterProject >= 0 ? colors.blue : colors.text);
    const bool openFilter =
        imshell::IconButton(ICON_FA_BARS_STAGGERED, imshell::ButtonVariant::Secondary, ImVec2(button, button));
    ImGui::PopStyleColor();
    if (openFilter) {
        projectSearch[0] = 0;
        ImGui::OpenPopup("filter");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Projects");
    }
    // under its button, left edges aligned
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 6));
    ImGui::SetNextWindowSize(ImVec2(300, 0));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, colors.mantle);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 10);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    const bool filtering = ImGui::BeginPopup("filter");
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    if (filtering) {
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##projects", ICON_FA_MAGNIFYING_GLASS "  Search projects...", projectSearch,
                                 sizeof(projectSearch));
        ImGui::Spacing();
        const float line = ImGui::GetTextLineHeight();
        // one row: its mark, its name, a check when it is the one listed
        auto row = [&](int index, const std::string& name) {
            const ImVec2 at = ImGui::GetCursorPos();
            const float width = ImGui::GetContentRegionAvail().x;
            const bool clicked = rowButton(("##p" + std::to_string(index)).c_str(), index == filterProject, line + 12);
            const ImVec2 next = ImGui::GetCursorPos();
            ImGui::SetCursorPos(ImVec2(at.x + 10, at.y + 6));
            if (index < 0) {
                ImGui::TextColored(colors.subtext0, ICON_FA_FOLDER_OPEN);
            } else {
                projectBadge(name);
            }
            ImGui::SameLine(at.x + 42);
            ImGui::TextUnformatted(name.c_str());
            if (index == filterProject) {
                ImGui::SetCursorPos(ImVec2(at.x + width - 24, at.y + 6));
                ImGui::TextColored(colors.blue, ICON_FA_CHECK);
            }
            ImGui::SetCursorPos(ImVec2(next.x, next.y - ImGui::GetStyle().ItemSpacing.y + 2));
            ImGui::Dummy(ImVec2(0, 0)); // imgui wants an item after a cursor move
            return clicked;
        };
        const std::string query = projectSearch;
        auto matches = [&](const std::string& name) {
            const auto found = std::ranges::search(name, query, [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
            });
            return query.empty() || !found.empty();
        };
        if (matches("All projects") && row(-1, "All projects")) {
            filterProject = -1;
            ImGui::CloseCurrentPopup();
        }
        for (int i = 0; i < static_cast<int>(data.projects.size()); ++i) {
            const std::string name = std::filesystem::path(data.projects[i].cwd).filename().string();
            if (matches(name) && row(i, name)) {
                filterProject = i;
                // new threads go there; a thread not yet started moves with it
                if (current < 0 || data.threads[current]->title.empty()) {
                    switchProjectTo = i;
                } else {
                    project = i;
                }
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (imshell::IconButton(ICON_FA_FOLDER_PLUS, imshell::ButtonVariant::Secondary,
                           ImVec2(button, button))) {
        paletteOpen();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Add project");
    }
    ImGui::SameLine();
    if (imshell::IconButton(ICON_FA_PEN_TO_SQUARE, imshell::ButtonVariant::Secondary,
                           ImVec2(button, button))) {
        newThread(project);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("New thread");
    }
    ImGui::Spacing();

    // threads, newest first; a draft (nothing sent yet) is not listed
    std::vector<int> order;
    for (int i = 0; i < static_cast<int>(data.threads.size()); ++i) {
        const Thread& t = *data.threads[i];
        const std::string query = search;
        const auto found = std::ranges::search(t.title, query, [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
        const bool listed = filterProject < 0 || filterProject >= static_cast<int>(data.projects.size()) ||
                            (t.host == data.projects[filterProject].host &&
                             t.cwd == data.projects[filterProject].cwd);
        if (!t.title.empty() && listed && (query.empty() || !found.empty())) {
            order.push_back(i);
        }
    }
    std::ranges::stable_sort(order, [&](int a, int b) {
        return data.threads[a]->updatedAt > data.threads[b]->updatedAt;
    });

    int open = -1, remove = -1;
    ImGui::BeginChild("threads", ImVec2(0, -(button + ImGui::GetStyle().ItemSpacing.y)));
    const float line = ImGui::GetTextLineHeight();
    for (const int i : order) {
        const Thread& t = *data.threads[i];
        ImGui::PushID(i);
        const ImVec2 at = ImGui::GetCursorPos();
        const float width = ImGui::GetContentRegionAvail().x;
        if (rowButton("##thread", i == current, line * 3 + 14)) {
            open = i;
        }
        if (ImGui::BeginPopupContextItem("menu")) {
            if (ImGui::MenuItem("Remove from list")) {
                remove = i;
            }
            ImGui::EndPopup();
        }
        const ImVec2 next = ImGui::GetCursorPos();
        // the project and how long ago; the title; where and with which agent
        ImGui::SetCursorPos(ImVec2(at.x + 8, at.y + 4));
        const std::string projectName = std::filesystem::path(t.cwd).filename().string();
        projectBadge(projectName, 14);
        ImGui::SameLine(0, 6);
        ImGui::TextColored(colors.subtext0, "%s", projectName.c_str());
        const std::string when = ago(t.updatedAt);
        ImGui::SameLine(at.x + width - ImGui::CalcTextSize(when.c_str()).x - 8);
        ImGui::TextColored(colors.overlay1, "%s", when.c_str());
        ImGui::SetCursorPos(ImVec2(at.x + 8, at.y + 4 + line + 3));
        ImGui::PushTextWrapPos(at.x + width - 8);
        ImGui::TextUnformatted(t.title.substr(0, 34).c_str());
        ImGui::PopTextWrapPos();
        ImGui::SetCursorPos(ImVec2(at.x + 8, at.y + 4 + 2 * (line + 3)));
        const AcpAgentDef* def = AcpAgents::find(t.agentId);
        ImGui::PushStyleColor(ImGuiCol_Text, colors.overlay1);
        agentIcon(t.agentId, 13);
        ImGui::SameLine(0, 6);
        ImGui::Text("%s%s%s", def ? def->name.c_str() : t.agentId.c_str(),
                    t.host.empty() ? "" : "  on ", t.host.c_str());
        ImGui::PopStyleColor();
        if (t.needsAnswer() || t.busy()) {
            ImGui::SameLine();
            ui::Badge(t.needsAnswer() ? "needs you" : "working",
                      t.needsAnswer() ? ui::Tone::Warning : ui::Tone::Info);
        }
        ImGui::SetCursorPos(next);
        ImGui::Dummy(ImVec2(0, 0)); // imgui wants an item after a cursor move
        ImGui::PopID();
    }
    if (order.empty()) {
        ImGui::TextColored(colors.overlay1, "%s",
                           search[0] ? "Nothing found" : filterProject >= 0 ? "No threads in this project yet"
                                                                            : "No threads yet");
    }
    ImGui::EndChild();

    if (imshell::IconButton(ICON_FA_GEAR, imshell::ButtonVariant::Secondary, ImVec2(button, button))) {
        settingsWant = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Settings");
    }

    if (remove >= 0) {
        const bool wasCurrent = remove == current;
        terminals.erase(data.threads[remove].get());
        data.threads.erase(data.threads.begin() + remove); // stops its agent
        current = wasCurrent ? -1 : current - (remove < current ? 1 : 0);
        save();
    } else if (open >= 0) {
        select(open);
    }
}
