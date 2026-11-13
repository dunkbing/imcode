#include "app.hpp"
#include "text.hpp"
#include "ssh.hpp"

#include "imshell/button.hpp"
#include "imshell/components.hpp"
#include "colors.hpp"
#include <algorithm>
#include <format>

using Theme = imshell::Theme;
namespace ui = imshell::ui;

// a search field with the glass drawn inside, the text starting clear of it
static void searchField(const char* id, const char* hint, char* buffer, size_t size, float width) {
    const float icon = ImGui::GetFontSize() + 10;
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x + icon, ImGui::GetStyle().FramePadding.y));
    ImGui::SetNextItemWidth(width);
    ImGui::InputTextWithHint(id, hint, buffer, size);
    ImGui::PopStyleVar();
    const ImVec2 min = ImGui::GetItemRectMin();
    ImGui::GetWindowDrawList()->AddText(ImVec2(min.x + ImGui::GetStyle().FramePadding.x, min.y + ImGui::GetStyle().FramePadding.y),
                                        ImGui::GetColorU32(themeColors().subtext0), ICON_FA_MAGNIFYING_GLASS);
}

void App::sidebar() {
    const Colors colors = themeColors();
    const float button = ImGui::GetFrameHeight();
    const float spacing = ImGui::GetStyle().ItemSpacing.x;

    // search, add project, new thread
    // three buttons after it, the last clear of the divider beside the list
    searchField("##search", "Search", search, sizeof(search), ImGui::GetContentRegionAvail().x - 3 * (button + spacing) - 6);
    ImGui::SameLine();
    // which project's threads are listed; picking one also makes it where new threads go
    // lit while one project is picked
    // the picked project's mark replaces the glyph
    const bool picked = filterProject >= 0 && filterProject < static_cast<int>(data.projects.size());
    ImGui::PushStyleColor(ImGuiCol_Text, picked ? ImVec4(0, 0, 0, 0) : colors.text);
    const bool openFilter =
        imshell::IconButton(ICON_FA_BARS_STAGGERED, imshell::ButtonVariant::Secondary, ImVec2(button, button));
    ImGui::PopStyleColor();
    if (picked) {
        const float mark = 14;
        const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
        drawProjectBadge(ImGui::GetWindowDrawList(),
                         ImVec2(std::floor((min.x + max.x - mark - 4) * 0.5f), std::floor((min.y + max.y - mark) * 0.5f)),
                         std::filesystem::path(data.projects[filterProject].cwd).filename().string(), mark);
    }
    if (openFilter) {
        projectSearch[0] = 0;
        ImGui::OpenPopup("filter");
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", filterProject >= 0 && filterProject < static_cast<int>(data.projects.size())
                                    ? std::filesystem::path(data.projects[filterProject].cwd).filename().string().c_str()
                                    : "All projects");
    }
    // under its button, left edges aligned
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 6));
    ImGui::SetNextWindowSize(ImVec2(300, 0));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, colors.mantle);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, Round::Dialog);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    const bool filtering = ImGui::BeginPopup("filter");
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    if (filtering) {
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        searchField("##projects", "Search projects...", projectSearch, sizeof(projectSearch), -FLT_MIN);
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
            data.filter = {};
            save();
            ImGui::CloseCurrentPopup();
        }
        for (int i = 0; i < static_cast<int>(data.projects.size()); ++i) {
            const std::string name = std::filesystem::path(data.projects[i].cwd).filename().string();
            if (matches(name) && row(i, name)) {
                filterProject = i;
                data.filter = data.projects[i];
                save();
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
    int archivedCount = 0;
    for (int i = 0; i < static_cast<int>(data.threads.size()); ++i) {
        const Thread& t = *data.threads[i];
        const std::string query = search;
        const auto found = std::ranges::search(t.title, query, [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
        const bool listed = filterProject < 0 || filterProject >= static_cast<int>(data.projects.size()) ||
                            (t.host == data.projects[filterProject].host &&
                             t.projectDir() == data.projects[filterProject].cwd);
        archivedCount += !t.title.empty() && listed && t.archived;
        if (!t.title.empty() && listed && t.archived == showArchived && (query.empty() || !found.empty())) {
            order.push_back(i);
        }
    }
    // what waits for the user first, then the newest
    std::ranges::stable_sort(order, [&](int a, int b) {
        const Thread& x = *data.threads[a];
        const Thread& y = *data.threads[b];
        return x.needsAnswer() != y.needsAnswer() ? x.needsAnswer() : x.updatedAt > y.updatedAt;
    });

    int open = -1, remove = -1, toggleArchive = -1, renamed = -1;
    static int focusedFor = -1; // the row whose field was given the keyboard
    bool renameFocus = renaming >= 0 && focusedFor != renaming;
    focusedFor = renaming;
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
            if (ImGui::MenuItem("Rename")) {
                renaming = i;
                renameText = t.title;
            }
            if (ImGui::MenuItem(t.archived ? "Unarchive" : "Archive")) {
                toggleArchive = i;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Remove from list")) {
                remove = i;
            }
            // only when git lets it go: a worktree with uncommitted work stays, and so does the thread
            if (!t.root.empty() && ImGui::MenuItem("Remove with its worktree")) {
                std::string error;
                // shortcut: blocks the ui for git (and ssh), like making the worktree does
                if (Git::removeWorktree(t.host, t.root, t.cwd, error)) {
                    remove = i;
                } else {
                    ui::Toast(("The worktree was kept: " + error).c_str(), ui::Tone::Warning, 8);
                }
            }
            ImGui::EndPopup();
        }
        const ImVec2 next = ImGui::GetCursorPos();
        // the project and how long ago; the title; where and with which agent
        ImGui::SetCursorPos(ImVec2(at.x + 8, at.y + 4));
        const std::string projectName = std::filesystem::path(t.projectDir()).filename().string();
        projectBadge(projectName, 14);
        ImGui::SameLine(0, 6);
        ImGui::TextColored(colors.subtext0, "%s", projectName.c_str());
        const std::string when = ago(t.updatedAt);
        ImGui::SameLine(at.x + width - ImGui::CalcTextSize(when.c_str()).x - 8);
        ImGui::TextColored(colors.overlay1, "%s", when.c_str());
        ImGui::SetCursorPos(ImVec2(at.x + 8, at.y + 4 + line + 3));
        if (renaming == i) {
            // the name, edited in place: enter keeps it, escape or a click elsewhere drops it
            ImGui::SetCursorPos(ImVec2(at.x + 4, at.y + line + 3));
            ImGui::SetNextItemWidth(width - 8);
            if (ImGui::IsWindowAppearing() || renameFocus) {
                ImGui::SetKeyboardFocusHere();
                renameFocus = false;
            }
            const bool keep = ui::TextField("##rename", renameText,
                                            {.width = width - 8, .flags = ImGuiInputTextFlags_EnterReturnsTrue |
                                                                          ImGuiInputTextFlags_AutoSelectAll});
            if (keep && !renameText.empty()) {
                renamed = i;
            } else if (ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsItemDeactivated()) {
                renaming = -1;
            }
        } else {
            ImGui::PushTextWrapPos(at.x + width - 8);
            ImGui::TextUnformatted(cut(t.title, 34).c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::SetCursorPos(ImVec2(at.x + 8, at.y + 4 + 2 * (line + 3)));
        const AcpAgentDef* def = AcpAgents::find(t.agentId);
        ImGui::PushStyleColor(ImGuiCol_Text, colors.overlay1);
        agentIcon(t.agentId, 13);
        ImGui::SameLine(0, 6);
        ImGui::TextUnformatted(def ? def->name.c_str() : t.agentId.c_str());
        if (!t.host.empty()) {
            // the host, with a dot: green while the app holds a connection to it
            ImGui::SameLine(0, 8);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float r = 3;
            const bool live = Ssh::connected(t.host);
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(at.x + r, at.y + ImGui::GetTextLineHeight() * 0.5f), r,
                                                        ImGui::GetColorU32(live ? colors.green : colors.surface2));
            ImGui::Dummy(ImVec2(2 * r, 0));
            ImGui::SameLine(0, 5);
            ImGui::TextUnformatted(t.host.c_str());
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip(live ? "Connected to %s" : "Not connected to %s: opening the thread connects", t.host.c_str());
            }
        }
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
    if (renamed >= 0) {
        data.threads[renamed]->title = cut(renameText, 80);
        data.threads[renamed]->renamed = true;
        renaming = -1;
        save();
    }
    if (toggleArchive >= 0) {
        data.threads[toggleArchive]->archived = !data.threads[toggleArchive]->archived;
        if (toggleArchive == current) {
            current = -1; // out of the list it was opened from
        }
        save();
    }
    if (order.empty()) {
        ImGui::TextColored(colors.overlay1, "%s",
                           search[0] ? "Nothing found" : showArchived ? "Nothing archived" : filterProject >= 0 ? "No threads in this project yet"
                                                                            : "No threads yet");
    }
    ImGui::EndChild();

    // the archive: a count at the right of the bottom row; click to list it instead
    if (archivedCount > 0 || showArchived) {
        const std::string label = showArchived ? "Back to threads" : std::format("Archived  {}", archivedCount);
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(label.c_str()).x - 16);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
        if (ImGui::Button(label.c_str())) {
            showArchived = !showArchived;
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetStyle().WindowPadding.x);
    }
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
