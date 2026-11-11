// what changed in the open thread's folder since the last commit: the files, and one's diff
#include "app.hpp"

#include "imshell/button.hpp"
#include "imshell/components.hpp"
#include "imshell/resizable.hpp"
#include "imshell/themes.hpp"

#include <algorithm>
#include <format>
#include <utility>

namespace Theme = imshell::Theme;
namespace ui = imshell::ui;

void App::changesOpen(Thread& t) {
    changes.want = true;
    changes.host = t.host;
    changes.cwd = t.cwd;
    changes.list.clear();
    changes.lines.clear();
    changes.selected = -1;
    changes.reload = true;
}

void App::changesDraw() {
    Changes& c = changes;
    const auto& colors = Theme::Current()->colors;
    // the list, then the picked file's diff, each off the ui thread (a remote is slow)
    c.listOp.check([&](std::vector<Git::Change> got) {
        c.list = std::move(got);
        c.selected = c.list.empty() ? -1 : std::clamp(c.selected, 0, static_cast<int>(c.list.size()) - 1);
        c.diffFor.clear();
    });
    if (std::exchange(c.reload, false) && !c.listOp.isRunning()) {
        c.listOp.start([host = c.host, cwd = c.cwd] { return Git::changes(host, cwd); });
    }
    c.diffOp.check([&](std::pair<std::string, std::vector<Diff::Line>> got) {
        if (got.first == c.diffFor) {
            c.lines = std::move(got.second);
        }
    });
    if (c.selected >= 0 && c.selected < static_cast<int>(c.list.size()) && c.diffFor != c.list[c.selected].path &&
        !c.diffOp.isRunning()) {
        c.diffFor = c.list[c.selected].path;
        c.lines.clear();
        c.diffOp.start([host = c.host, cwd = c.cwd, change = c.list[c.selected]] {
            return std::pair{change.path, Git::fileDiff(host, cwd, change)};
        });
    }

    if (std::exchange(c.want, false)) {
        ImGui::OpenPopup("##changes");
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(1100.0f, vp->WorkSize.x - 32), std::min(720.0f, vp->WorkSize.y - 32)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, colors.mantle);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.surface1);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    const bool shown = ImGui::BeginPopupModal("##changes", nullptr,
                                              ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    if (!shown) {
        return;
    }
    const float button = ImGui::GetFrameHeight();
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(nullptr, 18.0f);
    ImGui::TextUnformatted("Changes");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    int added = 0, removed = 0;
    for (const auto& change : c.list) {
        added += std::max(0, change.added);
        removed += std::max(0, change.removed);
    }
    ImGui::TextColored(colors.overlay1, "%zu %s", c.list.size(), c.list.size() == 1 ? "file" : "files");
    ImGui::SameLine();
    ImGui::TextColored(colors.green, "+%d", added);
    ImGui::SameLine();
    ImGui::TextColored(colors.red, "-%d", removed);
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 2 * button - 6);
    if (imshell::IconButton(ICON_FA_ROTATE, imshell::ButtonVariant::Secondary, ImVec2(button, button))) {
        c.reload = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Check again");
    }
    ImGui::SameLine(0, 6);
    bool close = imshell::IconButton(ICON_FA_XMARK, imshell::ButtonVariant::Secondary, ImVec2(button, button));
    ImGui::Spacing();

    if (c.listOp.isRunning() && c.list.empty()) {
        ui::Progress(-1, "Reading the changes");
    } else if (c.list.empty()) {
        ImGui::TextColored(colors.overlay1, "Nothing has changed since the last commit.");
    } else if (imshell::BeginResizable("changes", {.line = ImGui::GetColorU32(colors.surface1),
                                                   .lit = ImGui::GetColorU32(colors.blue)})) {
        // the files at the left; up and down move through them
        if (imshell::ResizablePanel({.size = &c.listWidth, .min = 160, .fixed = true})) {
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
                const int step = (ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : 0) - (ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? 1 : 0);
                c.selected = std::clamp(c.selected + step, 0, static_cast<int>(c.list.size()) - 1);
            }
            for (int i = 0; i < static_cast<int>(c.list.size()); ++i) {
                const Git::Change& change = c.list[static_cast<size_t>(i)];
                ImGui::PushID(i);
                if (rowButton("##file", i == c.selected, ImGui::GetTextLineHeight() + 10)) {
                    c.selected = i;
                }
                const ImVec2 end = ImGui::GetItemRectMax(), start = ImGui::GetItemRectMin();
                ImDrawList* draw = ImGui::GetWindowDrawList();
                const ImVec4 tone = change.kind == '?' || change.kind == 'A' ? colors.green
                                    : change.kind == 'D'                    ? colors.red
                                                                            : colors.yellow;
                const char letter[2] = {change.kind == '?' ? 'U' : change.kind, 0};
                const float y = start.y + 5;
                draw->AddText(ImVec2(start.x + 8, y), ImGui::GetColorU32(tone), letter);
                // the name, then its folder dimmed, cut to the row
                const size_t slash = change.path.rfind('/');
                const std::string name = change.path.substr(slash == std::string::npos ? 0 : slash + 1);
                const std::string folder = slash == std::string::npos ? "" : change.path.substr(0, slash);
                draw->PushClipRect(start, ImVec2(end.x - 4, end.y), true);
                draw->AddText(ImVec2(start.x + 26, y), ImGui::GetColorU32(colors.text), name.c_str());
                draw->AddText(ImVec2(start.x + 34 + ImGui::CalcTextSize(name.c_str()).x, y),
                              ImGui::GetColorU32(colors.overlay0), folder.c_str());
                draw->PopClipRect();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", change.path.c_str());
                }
                ImGui::PopID();
            }
        }
        if (imshell::ResizablePanel({.min = 300})) {
            if (c.selected >= 0 && c.selected < static_cast<int>(c.list.size())) {
                const Git::Change& change = c.list[static_cast<size_t>(c.selected)];
                if (c.diffOp.isRunning() && c.lines.empty()) {
                    ui::Progress(-1, "Reading the diff");
                } else if (c.lines.empty()) {
                    ImGui::TextColored(colors.overlay1, change.kind == 'D' ? "Deleted." : "No text to show (a binary file?).");
                } else if (diffBlock(change.path, change.added, change.removed, c.lines, 0) && change.kind != 'D') {
                    showFile(change.path); // in the files panel, beside the chat
                    close = true;
                }
            }
        }
        if (imshell::EndResizable()) {
            save();
        }
    }
    close |= ImGui::IsKeyPressed(ImGuiKey_Escape);
    close |= ImGui::IsMouseClicked(0) &&
             !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if (close) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
