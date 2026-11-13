// what changed in the open thread's folder since the last commit: the files, and one's diff
#include "app.hpp"

#include "acp_host/process_runner.hpp"
#include "imshell/button.hpp"
#include "imshell/components.hpp"
#include "imshell/resizable.hpp"
#include "colors.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

using Theme = imshell::Theme;
namespace ui = imshell::ui;

namespace {
    // a file in the list: where in the changes, and which side of the index it shows
    struct ChangeRow {
        int index;
        bool staged;
        int section; // 0 staged, 1 tracked, 2 not tracked (a turn's list: 1)
    };

    // in the browser of this machine
    void openUrl(const std::string& url) {
#if defined(__APPLE__)
        ProcessRunner::run({.args = {"/usr/bin/open", url}, .timeout = std::chrono::seconds(5)});
#elif defined(_WIN32)
        ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
        ProcessRunner::run({.args = {"xdg-open", url}, .timeout = std::chrono::seconds(5)});
#endif
    }
} // namespace

void App::changesOpen(Thread& t, const std::string& from, const std::string& to) {
    changesOpenAt(t.host, t.cwd, from, to, "");
}

void App::changesOpenAt(const std::string& host, const std::string& cwd, const std::string& from,
                        const std::string& to, const std::string& title) {
    changes.want = true;
    changes.host = host;
    changes.cwd = cwd;
    changes.from = from;
    changes.to = to;
    changes.title = title;
    changes.pushed = false;
    changes.list.clear();
    changes.lines.clear();
    changes.selected = -1;
    changes.reload = true;
}

void App::dockOpen(Thread& t) {
    files.open = true;
    files.tab = 1;
    dock.reload = true;
    dock.host = t.host;
    dock.cwd = t.cwd;
    dock.selected = -1;
}

void App::changesDraw(Changes& c, bool docked) {
    const Colors colors = themeColors();
    if (docked && !ImGui::GetIO().AppFocusLost && !c.listOp.isRunning() && !c.reload && ImGui::GetTime() - c.readAt > 3) {
        c.reload = true; // the folder changes under the panel: agents, editors, the user
    }
    // the list, then the picked file's diff, each off the ui thread (a remote is slow). on this
    // machine a read is held for a moment right after it starts: most finish in that time, so
    // the dialog never shows a loader for them; a big one falls back to it
    const auto quick = std::chrono::milliseconds(c.host.empty() ? 80 : 0);
    const auto quickList = std::chrono::milliseconds(c.host.empty() ? 250 : 0); // a big repository takes ~150 ms
    const bool working = c.from.empty(); // the folder against the last commit: staged files and the rest
    const auto gotList = [&](std::vector<Git::Change> got) {
        // the diff is read again only when a file changed: a list read every few seconds must not blink it
        const auto same = [](const Git::Change& a, const Git::Change& b) {
            return a.path == b.path && a.kind == b.kind && a.index == b.index && a.work == b.work && a.added == b.added &&
                   a.removed == b.removed;
        };
        if (!std::ranges::equal(c.list, got, same)) {
            c.diffFor.clear();
        }
        c.list = std::move(got);
    };
    const auto gotDiff = [&](std::pair<std::string, std::vector<Diff::Line>> got) {
        if (got.first == c.diffFor) {
            c.lines = std::move(got.second);
        }
    };
    c.listOp.check(gotList);
    if (std::exchange(c.reload, false) && !c.listOp.isRunning()) {
        c.readAt = ImGui::GetTime();
        c.listOp.start([host = c.host, cwd = c.cwd, from = c.from, to = c.to] { return Git::changes(host, cwd, from, to); });
        c.listOp.check(gotList, quickList);
    }
    // staging, unstaging and discarding run off the ui thread; the list is read again after
    c.opOp.check([&](std::string error) {
        if (!error.empty()) {
            c.said = error;
        }
        c.reload = true;
        gitAt = 0; // the line under the composer counts again
    });
    const auto edit = [&](char what, std::vector<Git::Change> files) {
        if (c.opOp.isRunning() || files.empty()) {
            return;
        }
        c.said.clear();
        c.opOp.start([host = c.host, cwd = c.cwd, what, files = std::move(files)] {
            std::string error;
            what == 's' ? Git::stage(host, cwd, files, error) : what == 'u' ? Git::unstage(host, cwd, files, error) : Git::discard(host, cwd, files, error);
            return error;
        });
    };
    // one hunk of the picked file: its lines as shown, then the change itself off the ui thread
    const auto editHunk = [&](char what, const Git::Change& file, int n, std::vector<Diff::Line> seen) {
        if (c.opOp.isRunning()) {
            return;
        }
        c.said.clear();
        c.opOp.start([host = c.host, cwd = c.cwd, what, file, n, seen = std::move(seen)] {
            std::string error;
            Git::applyHunk(host, cwd, file, what == 's' ? Git::HunkAction::Stage : what == 'u' ? Git::HunkAction::Unstage : Git::HunkAction::Discard, n,
                           seen, error);
            return error;
        });
    };
    // the rows: the staged files, then the tracked ones with changes left, then the new ones (a
    // partly staged file is in more than one); a turn's are one list
    std::vector<ChangeRow> rows;
    for (int section = working ? 0 : 1; section < (working ? 3 : 2); ++section) {
        for (int i = 0; i < static_cast<int>(c.list.size()); ++i) {
            const Git::Change& ch = c.list[static_cast<size_t>(i)];
            if (!working || (section == 0 ? ch.staged() : ch.unstaged() && (section == 2) == (ch.work == '?'))) {
                rows.push_back({i, working && section == 0, section});
            }
        }
    }
    const auto keyOf = [&](const ChangeRow& r) {
        return c.list[static_cast<size_t>(r.index)].path + (working ? (r.staged ? "\1S" : "\1U") : "");
    };
    const auto select = [&](int i) {
        c.selected = i;
        c.selKey = keyOf(rows[static_cast<size_t>(i)]);
    };
    // the same file stays picked while the list changes under it
    if (rows.empty()) {
        c.selected = -1;
    } else {
        const auto at = std::ranges::find_if(rows, [&](const ChangeRow& r) { return keyOf(r) == c.selKey; });
        select(at != rows.end() ? static_cast<int>(at - rows.begin()) : std::clamp(c.selected, 0, static_cast<int>(rows.size()) - 1));
    }
    c.diffOp.check(gotDiff);
    if (c.selected >= 0 && c.diffFor != keyOf(rows[static_cast<size_t>(c.selected)]) && !c.diffOp.isRunning()) {
        const ChangeRow row = rows[static_cast<size_t>(c.selected)];
        c.diffFor = keyOf(row);
        c.lines.clear();
        c.diffOp.start([host = c.host, cwd = c.cwd, change = c.list[static_cast<size_t>(row.index)], from = c.from, to = c.to,
                        key = c.diffFor, working, staged = row.staged] {
            return std::pair{key, working ? Git::fileDiff(host, cwd, change, staged ? Git::Side::Staged : Git::Side::Unstaged)
                                          : Git::fileDiff(host, cwd, change, from, to)};
        });
        c.diffOp.check(gotDiff, quick);
    }

    c.commitOp.check([&](Changes::Done done) {
        c.said = done.ok ? "" : done.output;
        if (!done.ok) {
            return;
        }
        if (done.pull) {
            ui::Toast("Pull request opened", ui::Tone::Success, 4);
            openUrl(done.url);
            c.pushed = false;
            return;
        }
        c.message[0] = 0;
        c.pushed = done.push;
        ui::Toast(done.push ? "Committed and pushed" : "Committed", ui::Tone::Success, 3);
        c.reload = true;
        gitAt = 0; // the line under the composer counts again
    });
    int added = 0, removed = 0;
    for (const auto& change : c.list) {
        added += std::max(0, change.added);
        removed += std::max(0, change.removed);
    }
    const float button = ImGui::GetFrameHeight();
    bool close = false;
    if (!docked) { // the dialog has a header; in the panel the list's top line says it
        if (std::exchange(c.want, false)) {
            ImGui::OpenPopup("##changes");
        }
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(std::min(1100.0f, vp->WorkSize.x - 32), std::min(720.0f, vp->WorkSize.y - 32)));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, colors.mantle);
        ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Round::Dialog);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1);
        const bool shown = ImGui::BeginPopupModal("##changes", nullptr,
                                                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                                      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
        if (!shown) {
            return;
        }
        ImGui::AlignTextToFramePadding();
        ImGui::PushFont(nullptr, 18.0f);
        ImGui::TextUnformatted(!c.title.empty() ? c.title.c_str() : c.from.empty() ? "Changes" : "Changes in this turn");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (!c.list.empty()) {
            ImGui::TextColored(colors.overlay1, "%zu %s", c.list.size(), c.list.size() == 1 ? "file" : "files");
            ImGui::SameLine();
            ImGui::TextColored(colors.green, "+%d", added);
            ImGui::SameLine();
            ImGui::TextColored(colors.red, "-%d", removed);
        }
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - 2 * button - 6);
        if (imshell::IconButton(ICON_FA_ROTATE, imshell::ButtonVariant::Secondary, ImVec2(button, button))) {
            c.reload = true;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Check again");
        }
        ImGui::SameLine(0, 6);
        close = imshell::IconButton(ICON_FA_XMARK, imshell::ButtonVariant::Secondary, ImVec2(button, button));
        ImGui::Spacing();
    }

    // the commit message and its buttons, at the foot of the file list; committing is for the
    // folder, not a turn. after a push with nothing left: the pull request alone
    const bool busy = c.commitOp.isRunning();
    const int stagedCount = static_cast<int>(std::ranges::count_if(c.list, [](const Git::Change& ch) { return ch.staged(); }));
    const bool bar = c.from.empty() && (!c.list.empty() || c.pushed);
    const bool prOnly = c.pushed && c.list.empty();
    const float messageH = ImGui::GetTextLineHeight() * 3 + 16;
    const float barH = !bar ? 0.0f
                       : prOnly ? ImGui::GetFrameHeight()
                                : messageH + ImGui::GetFrameHeight() + 2 * ImGui::GetStyle().ItemSpacing.y +
                                      ((c.said.empty() && !busy) ? 0.0f : 2 * ImGui::GetTextLineHeightWithSpacing());
    const auto commitBlock = [&] {
        if (!bar) {
            return;
        }
        const bool canCommit = !busy && !c.list.empty() && c.message[0] != 0;
        const auto start = [&](bool push) {
            c.said.clear();
            c.commitOp.start([host = c.host, cwd = c.cwd, message = std::string(c.message), push, stagedOnly = stagedCount > 0] {
                Changes::Done done{.push = push};
                done.ok = Git::commit(host, cwd, message, push, done.output, stagedOnly);
                return done;
            });
        };
        ImGui::Indent(8);
        const float width = ImGui::GetContentRegionAvail().x - 8;
        if (prOnly) {
            ImGui::BeginDisabled(busy);
            if (imshell::Button(ICON_FA_CODE_PULL_REQUEST "  Open a pull request", imshell::ButtonVariant::Primary, ImVec2(width, 0))) {
                c.said.clear();
                c.commitOp.start([host = c.host, cwd = c.cwd] {
                    Changes::Done done{.pull = true};
                    done.url = Git::pullRequest(host, cwd, done.output);
                    done.ok = !done.url.empty();
                    return done;
                });
            }
            ImGui::EndDisabled();
        } else {
            // enter commits; ctrl or cmd with it starts a new line
            ImGui::BeginDisabled(busy);
            const bool enter = ImGui::InputTextMultiline("##message", c.message, sizeof(c.message), ImVec2(width, messageH),
                                                         ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine);
            // the field is a child window drawn over this one's list: the hint goes on the foreground
            if (!c.message[0] && !ImGui::IsItemActive() && !ImGui::IsPopupOpen("##discard")) {
                const ImVec2 at = ImGui::GetItemRectMin();
                ImGui::GetForegroundDrawList()->AddText(ImVec2(at.x + ImGui::GetStyle().FramePadding.x, at.y + ImGui::GetStyle().FramePadding.y),
                                                    ImGui::GetColorU32(colors.overlay1),
                                                    stagedCount > 0 ? "Commit message (the staged files)" : "Commit message (all the changes)");
            }
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!canCommit);
            const float commitWidth = 84;
            if (imshell::Button("Commit", imshell::ButtonVariant::Secondary, ImVec2(commitWidth, 0)) || (enter && canCommit)) {
                start(false);
            }
            ImGui::SameLine(0, 6);
            if (imshell::Button(ICON_FA_ARROW_UP "  Commit & push", imshell::ButtonVariant::Primary, ImVec2(width - commitWidth - 6, 0))) {
                start(true);
            }
            ImGui::EndDisabled();
        }
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
        if (busy) {
            ImGui::TextColored(colors.overlay1, "Working...");
        } else if (!c.said.empty()) {
            ImGui::TextColored(colors.red, "%s", c.said.c_str());
        }
        ImGui::PopTextWrapPos();
        ImGui::Unindent(8);
    };

    if (c.listOp.isRunning() && c.list.empty()) {
        ui::Progress(-1, "Reading the changes");
    } else if (c.list.empty()) {
        ImGui::TextColored(colors.overlay1, c.from.empty() ? "Nothing has changed since the last commit." : "Nothing changed.");
        commitBlock();
    } else if (imshell::BeginResizable("changes", {.axis = docked ? imshell::Axis::Vertical : imshell::Axis::Horizontal,
                                                   .size = ImVec2(0, docked ? -(barH > 0 ? barH + ImGui::GetStyle().ItemSpacing.y : 0) : 0),
                                                   .line = ImGui::GetColorU32(colors.surface1),
                                                   .lit = ImGui::GetColorU32(colors.blue)})) {
        // the files at the left; up and down move through them. in the folder's own view they are
        // two sections, the staged ones and the others, with a button to move each file between
        if (imshell::ResizablePanel({.size = docked ? &c.listHeight : &c.listWidth, .min = docked ? 80.0f : 160.0f, .fixed = true})) {
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && c.asking.empty() && !ImGui::GetIO().WantTextInput) {
                const int step = (ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : 0) - (ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? 1 : 0);
                if (step != 0) {
                    int to = c.selected + step; // past the rows of a folded section
                    while (to >= 0 && to < static_cast<int>(rows.size()) && working && c.folded[rows[static_cast<size_t>(to)].section]) {
                        to += step;
                    }
                    if (to >= 0 && to < static_cast<int>(rows.size())) {
                        select(to);
                    }
                }
            }
            // the files scroll above the commit message
            ImGui::BeginChild("##list", ImVec2(0, docked ? 0.0f : -(barH > 0 ? barH + ImGui::GetStyle().ItemSpacing.y : 0)));
            const bool idle = !c.opOp.isRunning();
            const auto filesOf = [&](bool staged) {
                std::vector<Git::Change> out;
                for (const ChangeRow& r : rows) {
                    if (r.staged == staged) {
                        out.push_back(c.list[static_cast<size_t>(r.index)]);
                    }
                }
                return out;
            };
            const auto filesOfSection = [&](int section) {
                std::vector<Git::Change> out;
                for (const ChangeRow& r : rows) {
                    if (r.section == section) {
                        out.push_back(c.list[static_cast<size_t>(r.index)]);
                    }
                }
                return out;
            };
            const float rowH = ImGui::GetTextLineHeight() + 12;
            const float listRight = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x; // where the boxes end
            // a small square: empty, or filled with a check; hit-tested by hand by its caller
            const auto checkbox = [&](ImDrawList* draw, ImVec2 min, bool on, bool hot) {
                const ImVec2 max(min.x + 16, min.y + 16);
                draw->AddRectFilled(min, max, ImGui::GetColorU32(on ? colors.blue : hot ? colors.surface1 : ImVec4(0, 0, 0, 0)), 4);
                draw->AddRect(min, max, ImGui::GetColorU32(on ? colors.blue : colors.overlay0), 4);
                if (on) {
                    ImGui::PushFont(nullptr, 10.0f);
                    const ImVec2 size = ImGui::CalcTextSize(ICON_FA_CHECK);
                    draw->AddText(ImVec2(min.x + (16 - size.x) / 2, min.y + (16 - size.y) / 2), ImGui::GetColorU32(colors.crust), ICON_FA_CHECK);
                    ImGui::PopFont();
                }
            };
            // a status mark: a square outline with a dot (changed), a plus (new) or a minus (deleted)
            const auto mark = [&](ImDrawList* draw, ImVec2 at, char state) {
                const ImU32 tone = ImGui::GetColorU32(state == '?' || state == 'A' ? colors.green : state == 'D' ? colors.red : colors.yellow);
                draw->AddRect(at, ImVec2(at.x + 14, at.y + 14), tone, 3, 0, 1.5f);
                const ImVec2 mid(at.x + 7, at.y + 7);
                if (state == 'D') {
                    draw->AddLine(ImVec2(mid.x - 3, mid.y), ImVec2(mid.x + 3, mid.y), tone, 1.5f);
                } else if (state == '?' || state == 'A') {
                    draw->AddLine(ImVec2(mid.x - 3, mid.y), ImVec2(mid.x + 3, mid.y), tone, 1.5f);
                    draw->AddLine(ImVec2(mid.x, mid.y - 3), ImVec2(mid.x, mid.y + 3), tone, 1.5f);
                } else {
                    draw->AddCircleFilled(mid, 2.5f, tone);
                }
            };
            // the top of the list: what the files add up to, and every change staged at once
            if (working) {
                ImGui::AlignTextToFramePadding();
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8);
                if (docked) {
                    ImGui::TextColored(colors.overlay1, "%zu %s", c.list.size(), c.list.size() == 1 ? "file" : "files");
                    ImGui::SameLine(0, 6);
                }
                ImGui::TextColored(colors.green, "+%d", added);
                ImGui::SameLine(0, 6);
                ImGui::TextColored(colors.red, "-%d", removed);
                const std::vector<Git::Change> unstaged = filesOf(false);
                if (!unstaged.empty()) {
                    ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize("Stage All").x - 2 * ImGui::GetStyle().FramePadding.x - 8);
                    ImGui::BeginDisabled(!idle);
                    if (ImGui::SmallButton("Stage All")) {
                        edit('s', unstaged);
                    }
                    ImGui::EndDisabled();
                }
                ImGui::Spacing();
            }
            const char* const sectionNames[] = {"Staged", "Tracked", "Untracked"};
            for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
                const ChangeRow& row = rows[static_cast<size_t>(i)];
                const Git::Change& change = c.list[static_cast<size_t>(row.index)];
                if (working && (i == 0 || rows[static_cast<size_t>(i) - 1].section != row.section)) {
                    // a section: fold it, or move all its files across with the box
                    ImGui::PushID(sectionNames[row.section]);
                    const std::vector<Git::Change> section = filesOfSection(row.section);
                    const bool open = !c.folded[row.section];
                    ImGui::InvisibleButton("##section", ImVec2(ImGui::GetContentRegionAvail().x, rowH));
                    const ImVec2 start = ImGui::GetItemRectMin(), end = ImGui::GetItemRectMax();
                    ImDrawList* draw = ImGui::GetWindowDrawList();
                    const float mid = (start.y + end.y) / 2;
                    const ImVec2 box(listRight - 8 - 16, mid - 8);
                    const bool onBox = ImGui::IsMouseHoveringRect(box, ImVec2(box.x + 16, box.y + 16));
                    if (ImGui::IsItemClicked()) {
                        if (onBox) {
                            edit(row.section == 0 ? 'u' : 's', section);
                        } else {
                            c.folded[row.section] = open;
                        }
                    }
                    ImGui::PushFont(nullptr, 10.0f);
                    draw->AddText(ImVec2(start.x + 8, mid - ImGui::GetTextLineHeight() / 2), ImGui::GetColorU32(colors.overlay1),
                                  open ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_RIGHT);
                    ImGui::PopFont();
                    draw->AddText(ImVec2(start.x + 26, mid - ImGui::GetTextLineHeight() / 2), ImGui::GetColorU32(colors.subtext0),
                                  sectionNames[row.section]);
                    // the count in a small tag
                    const std::string count = std::to_string(section.size());
                    const ImVec2 countSize = ImGui::CalcTextSize(count.c_str());
                    const float tagRight = box.x - 8;
                    draw->AddRectFilled(ImVec2(tagRight - countSize.x - 12, mid - 9), ImVec2(tagRight, mid + 9), ImGui::GetColorU32(colors.surface0), 5);
                    draw->AddText(ImVec2(tagRight - countSize.x - 6, mid - countSize.y / 2), ImGui::GetColorU32(colors.subtext0), count.c_str());
                    checkbox(draw, box, row.section == 0, onBox && idle);
                    ImGui::PopID();
                }
                if (working && c.folded[row.section]) {
                    continue;
                }
                ImGui::PushID(i);
                if (rowButton("##file", i == c.selected, rowH)) {
                    select(i);
                }
                const ImVec2 end = ImGui::GetItemRectMax(), start = ImGui::GetItemRectMin();
                const bool over = ImGui::IsItemHovered() || (idle && ImGui::IsMouseHoveringRect(start, end) &&
                                                             ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows));
                ImDrawList* draw = ImGui::GetWindowDrawList();
                const char state = !working ? change.kind : row.staged ? change.index : change.work;
                const float mid = (start.y + end.y) / 2;
                const float y = mid - ImGui::GetTextLineHeight() / 2;
                mark(draw, ImVec2(start.x + 8, mid - 7), state);
                // the right side: the staged box (working view), the counts, and on hover the discard button
                float right = listRight - 8;
                if (working) {
                    const ImVec2 box(right - 16, mid - 8);
                    const bool onBox = ImGui::IsMouseHoveringRect(box, ImVec2(box.x + 16, box.y + 16));
                    checkbox(draw, box, row.staged, onBox && idle);
                    if (onBox && idle) {
                        ImGui::SetTooltip("%s", row.staged ? "Unstage" : "Stage");
                        if (ImGui::IsMouseClicked(0)) {
                            edit(row.staged ? 'u' : 's', {change});
                        }
                    }
                    right = box.x - 8;
                    if (over && idle && !row.staged) { // a staged file is only unstaged here
                        const ImVec2 lo(right - 26, start.y + 2), hi(right, end.y - 2);
                        const bool hot = ImGui::IsMouseHoveringRect(lo, hi);
                        if (hot) {
                            draw->AddRectFilled(lo, hi, ImGui::GetColorU32(colors.surface1), Round::Row);
                            ImGui::SetTooltip("%s", change.work == '?' ? "Delete the file" : "Discard the changes");
                            if (ImGui::IsMouseClicked(0)) {
                                c.asking = {change};
                                c.askWant = true;
                            }
                        }
                        const ImVec2 size = ImGui::CalcTextSize(ICON_FA_ROTATE_LEFT);
                        draw->AddText(ImVec2(lo.x + (26 - size.x) / 2, mid - size.y / 2), ImGui::GetColorU32(colors.text), ICON_FA_ROTATE_LEFT);
                        right = lo.x - 4;
                    }
                }
                if (change.added >= 0) {
                    const std::string plus = "+" + std::to_string(change.added), minus = "-" + std::to_string(change.removed);
                    const float width = ImGui::CalcTextSize(plus.c_str()).x + 6 + ImGui::CalcTextSize(minus.c_str()).x;
                    draw->AddText(ImVec2(right - width, y), ImGui::GetColorU32(colors.green), plus.c_str());
                    draw->AddText(ImVec2(right - ImGui::CalcTextSize(minus.c_str()).x, y), ImGui::GetColorU32(colors.red), minus.c_str());
                    right -= width + 8;
                }
                // the name, and only the folder it is in (not the whole path); none for a file at the top
                std::string shown = change.path;
                const bool dir = shown.ends_with('/'); // a folder not tracked yet
                if (dir) {
                    shown.pop_back();
                }
                const size_t slash = shown.rfind('/');
                const std::string name = shown.substr(slash == std::string::npos ? 0 : slash + 1) + (dir ? "/" : "");
                std::string folder = slash == std::string::npos ? "" : shown.substr(0, slash);
                folder.erase(0, folder.rfind('/') == std::string::npos ? 0 : folder.rfind('/') + 1);
                draw->PushClipRect(start, ImVec2(right, end.y), true);
                draw->AddText(ImVec2(start.x + 30, y), ImGui::GetColorU32(colors.text), name.c_str());
                draw->AddText(ImVec2(start.x + 38 + ImGui::CalcTextSize(name.c_str()).x, y), ImGui::GetColorU32(colors.overlay0),
                              folder.c_str());
                draw->PopClipRect();
                if (ImGui::IsItemHovered() && !over) {
                    ImGui::SetTooltip("%s", change.path.c_str());
                }
                ImGui::PopID();
            }
            ImGui::EndChild();
            if (!docked) {
                commitBlock();
            }
        }
        if (imshell::ResizablePanel({.min = docked ? 120.0f : 300.0f})) {
            if (c.selected >= 0 && c.selected < static_cast<int>(rows.size())) {
                const Git::Change& change = c.list[static_cast<size_t>(rows[static_cast<size_t>(c.selected)].index)];
                const std::function<void(int, char)> hunkAction = [&](int n, char what) {
                    std::vector<Diff::Line> seen;
                    int at = 0;
                    for (const Diff::Line& l : c.lines) {
                        if (l.kind == '~') {
                            ++at;
                        } else if (at == n) {
                            seen.push_back(l);
                        }
                    }
                    if (what == 'd') { // no way back: asked first
                        c.hunkFile = change;
                        c.hunkIndex = n;
                        c.hunkLines = std::move(seen);
                        c.askWant = true;
                    } else {
                        editHunk(what, change, n, std::move(seen));
                    }
                };
                if (c.diffOp.isRunning() && c.lines.empty()) {
                    ui::Progress(-1, "Reading the diff");
                } else if (c.lines.empty()) {
                    ImGui::TextColored(colors.overlay1, change.kind == 'D' ? "Deleted." : "No text to show (a binary file?).");
                } else if (diffBlock(change.path, change.added, change.removed, c.lines, 0,
                                     // a text file the index knows: not a new one, a delete or a rename
                                     working && (change.kind == 'M' || change.kind == 'A') && change.work != '?' ? hunkAction : nullptr,
                                     rows[static_cast<size_t>(c.selected)].staged) &&
                           change.kind != 'D') {
                    showFile(change.path); // in the files panel, beside the chat
                    close = true;
                }
            }
        }
        if (imshell::EndResizable()) {
            save();
        }
        if (docked) {
            commitBlock();
        }
    }
    // throwing changes away has no way back: asked first
    if (std::exchange(c.askWant, false)) {
        ImGui::OpenPopup("##discard");
    }
    ImGui::PushStyleColor(ImGuiCol_PopupBg, colors.mantle);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Round::Dialog);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1);
    const bool asked = ImGui::BeginPopupModal("##discard", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    if (asked) {
        const size_t fresh = static_cast<size_t>(std::ranges::count(c.asking, '?', &Git::Change::work));
        if (c.asking.empty()) { // one hunk
            ImGui::TextUnformatted("Discard this change?");
            ImGui::TextColored(colors.overlay1, "%s", c.hunkFile.path.c_str());
        } else if (c.asking.size() == 1) {
            ImGui::TextUnformatted(fresh ? "Delete this file?" : "Discard the changes in this file?");
            ImGui::TextColored(colors.overlay1, "%s", c.asking[0].path.c_str());
        } else {
            ImGui::Text("Discard the changes in %zu files?", c.asking.size());
            if (fresh) {
                ImGui::TextColored(colors.overlay1, "%zu of them are new and are deleted.", fresh);
            }
        }
        ImGui::TextColored(colors.overlay1, "This cannot be undone.");
        ImGui::Spacing();
        if (imshell::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            c.asking.clear();
            c.hunkIndex = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (imshell::Button(c.asking.size() == 1 && fresh ? "Delete" : "Discard", imshell::ButtonVariant::Primary)) {
            if (c.asking.empty() && c.hunkIndex >= 0) {
                editHunk('d', c.hunkFile, std::exchange(c.hunkIndex, -1), std::move(c.hunkLines));
            }
            edit('d', std::exchange(c.asking, {}));
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (docked) {
        return;
    }
    close |= ImGui::IsKeyPressed(ImGuiKey_Escape);
    close |= ImGui::IsMouseClicked(0) &&
             !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if (close) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
