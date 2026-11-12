// the history of the open thread's repository: commits with a branch graph; enter or a
// double click shows what a commit changed
#include "app.hpp"

#include "imshell/button.hpp"
#include "imshell/components.hpp"
#include "imshell/themes.hpp"

#include <algorithm>
#include <utility>

namespace Theme = imshell::Theme;
namespace ui = imshell::ui;

namespace {
    constexpr size_t pageSize = 300;
    constexpr float laneWidth = 14, rowHeight = 26;
    // what `git diff` calls the tree of no files, for a first commit to be shown against
    constexpr const char* emptyTree = "4b825dc642cb6eb9a060e54bf8d69288fbee4904"; // shortcut: sha-1 repositories only

    ImU32 laneColor(int lane) {
        const auto& c = Theme::Current()->colors;
        const ImVec4 tones[] = {c.blue, c.green, c.peach, c.purple, c.teal, c.yellow, c.rose, c.sapphire};
        return ImGui::GetColorU32(tones[lane % 8]);
    }
} // namespace

void App::graphOpen(Thread& t) {
    graph.want = true;
    graph.reload = true;
    graph.done = false;
    graph.host = t.host;
    graph.cwd = t.cwd;
    graph.commits.clear();
    graph.rows.clear();
    graph.lanes = 1;
    graph.selected = 0;
}

void App::graphDraw() {
    Graph& g = graph;
    const auto& colors = Theme::Current()->colors;
    const std::string key = g.host + "\n" + g.cwd;
    // a page of history, read off the ui thread (a remote is slow); the next one when the end is near
    g.op.check([&](Graph::Page page) {
        if (page.key != key || page.skip != g.commits.size()) {
            return; // for another repository, or already here
        }
        g.done = page.got.size() < pageSize;
        g.commits.insert(g.commits.end(), std::make_move_iterator(page.got.begin()), std::make_move_iterator(page.got.end()));
        g.rows = Git::layoutGraph(g.commits);
        g.lanes = 1;
        for (const auto& row : g.rows) {
            g.lanes = std::max(g.lanes, row.width);
        }
    });
    const auto more = [&] {
        if (!g.op.isRunning()) {
            g.op.start([host = g.host, cwd = g.cwd, key, skip = g.commits.size()] {
                return Graph::Page{key, skip, Git::log(host, cwd, pageSize, skip)};
            });
        }
    };
    if (std::exchange(g.reload, false)) {
        more();
    }
    if (std::exchange(g.want, false)) {
        ImGui::OpenPopup("##graph");
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(1100.0f, vp->WorkSize.x - 32), std::min(720.0f, vp->WorkSize.y - 32)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, colors.mantle);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.surface1);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    const bool shown = ImGui::BeginPopupModal("##graph", nullptr,
                                              ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    if (!shown) {
        return;
    }
    const float button = ImGui::GetFrameHeight();
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(nullptr, 18.0f);
    ImGui::TextUnformatted("History");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    if (!g.commits.empty()) {
        ImGui::TextColored(colors.overlay1, "%zu%s commits", g.commits.size(), g.done ? "" : "+");
    }
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 2 * button - 6);
    if (imshell::IconButton(ICON_FA_ROTATE, imshell::ButtonVariant::Secondary, ImVec2(button, button))) {
        g.commits.clear();
        g.rows.clear();
        g.done = false;
        g.reload = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Read again");
    }
    ImGui::SameLine(0, 6);
    bool close = imshell::IconButton(ICON_FA_XMARK, imshell::ButtonVariant::Secondary, ImVec2(button, button));
    ImGui::Spacing();

    const int count = static_cast<int>(g.commits.size());
    bool open = false;
    if (count == 0) {
        if (g.op.isRunning() || g.reload) {
            ui::Progress(-1, "Reading the history");
        } else {
            ImGui::TextColored(colors.overlay1, "No commits here.");
        }
    } else if (ImGui::BeginChild("##commits", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_None)) {
        const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        const int step = focused ? (ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : 0) - (ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? 1 : 0) : 0;
        const bool moved = step != 0;
        g.selected = std::clamp(g.selected + step, 0, count - 1);
        open = focused && (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter));
        const float graphWidth = 10 + static_cast<float>(g.lanes) * laneWidth;
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0)); // rows touch: the lines run on
        ImGuiListClipper clipper;
        clipper.Begin(count, rowHeight);
        if (moved) {
            clipper.IncludeItemByIndex(g.selected);
        }
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const Git::LogEntry& e = g.commits[static_cast<size_t>(i)];
                const Git::GraphRow& row = g.rows[static_cast<size_t>(i)];
                ImGui::PushID(i);
                if (rowButton("##commit", i == g.selected, rowHeight)) {
                    g.selected = i;
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {
                    open = true;
                }
                if (moved && i == g.selected) {
                    ImGui::SetScrollHereY(0.5f);
                }
                const ImVec2 start = ImGui::GetItemRectMin(), end = ImGui::GetItemRectMax();
                ImDrawList* draw = ImGui::GetWindowDrawList();
                draw->PushClipRect(start, end, true);
                const auto x = [&](int lane) { return start.x + 10 + static_cast<float>(lane) * laneWidth; };
                const float mid = start.y + rowHeight / 2;
                for (const int lane : row.pass) {
                    draw->AddLine(ImVec2(x(lane), start.y), ImVec2(x(lane), end.y), laneColor(lane), 1.5f);
                }
                for (const int lane : row.into) {
                    draw->AddLine(ImVec2(x(lane), start.y), ImVec2(x(row.col), mid), laneColor(lane), 1.5f);
                }
                for (const int lane : row.parents) {
                    draw->AddLine(ImVec2(x(row.col), mid), ImVec2(x(lane), end.y), laneColor(lane), 1.5f);
                }
                draw->AddCircleFilled(ImVec2(x(row.col), mid), 4.0f, laneColor(row.col));
                // refs as small chips, then the subject; author and age at the right
                float at = start.x + graphWidth + 8;
                const float right = end.x - 8 - 190;
                const float textY = start.y + (rowHeight - ImGui::GetTextLineHeight()) / 2;
                for (const std::string& ref : e.refs) {
                    const bool head = ref.starts_with("HEAD -> "), tag = ref.starts_with("tag: ");
                    const std::string label = head ? ref.substr(8) : tag ? ref.substr(5) : ref;
                    const ImVec4 tone = head ? colors.blue : tag ? colors.yellow : (label.starts_with("origin/") || label.starts_with("upstream/")) ? colors.overlay1 : colors.green;
                    const float width = ImGui::CalcTextSize(label.c_str()).x + 12;
                    if (at + width > right) {
                        break;
                    }
                    ImVec4 fill = tone;
                    fill.w = 0.18f;
                    draw->AddRectFilled(ImVec2(at, start.y + 4), ImVec2(at + width, end.y - 4), ImGui::GetColorU32(fill), 4);
                    draw->AddText(ImVec2(at + 6, textY), ImGui::GetColorU32(tone), label.c_str());
                    at += width + 6;
                }
                draw->PushClipRect(ImVec2(start.x, start.y), ImVec2(right, end.y), true);
                draw->AddText(ImVec2(at, textY), ImGui::GetColorU32(colors.text), e.subject.c_str());
                draw->PopClipRect();
                draw->PushClipRect(ImVec2(end.x - 190, start.y), ImVec2(end.x - 54, end.y), true);
                draw->AddText(ImVec2(end.x - 190, textY), ImGui::GetColorU32(colors.overlay1), e.author.c_str());
                draw->PopClipRect();
                draw->AddText(ImVec2(end.x - 46, textY), ImGui::GetColorU32(colors.overlay0), ago(e.time).c_str());
                draw->PopClipRect();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s  %s", e.hash.substr(0, 10).c_str(), e.subject.c_str());
                }
                ImGui::PopID();
            }
        }
        // near the end of what is loaded: the next page
        if (!g.done && clipper.DisplayEnd >= count - 30) {
            more();
        }
        ImGui::PopStyleVar();
        ImGui::EndChild();
    }
    if (open && g.selected >= 0 && g.selected < count) {
        const Git::LogEntry& e = g.commits[static_cast<size_t>(g.selected)];
        // the first parent's tree; a first commit against nothing
        changesOpenAt(g.host, g.cwd, e.parents.empty() ? emptyTree : e.hash + "^", e.hash,
                      e.hash.substr(0, 7) + "  " + e.subject.substr(0, e.subject.find('\n')));
        close = true;
    }
    close |= ImGui::IsKeyPressed(ImGuiKey_Escape);
    close |= ImGui::IsMouseClicked(0) &&
             !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if (close) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
