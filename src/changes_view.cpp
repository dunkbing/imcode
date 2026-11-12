// what changed in the open thread's folder since the last commit: the files, and one's diff
#include "app.hpp"

#include "acp_host/process_runner.hpp"
#include "imshell/button.hpp"
#include "imshell/components.hpp"
#include "imshell/resizable.hpp"
#include "imshell/themes.hpp"

#include <algorithm>
#include <chrono>
#include <format>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace Theme = imshell::Theme;
namespace ui = imshell::ui;

namespace {
    // a file in the list: where in the changes, and which side of the index it shows
    struct ChangeRow {
        int index;
        bool staged;
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

void App::changesDraw() {
    Changes& c = changes;
    const auto& colors = Theme::Current()->colors;
    // the list, then the picked file's diff, each off the ui thread (a remote is slow). on this
    // machine a read is held for a moment right after it starts: most finish in that time, so
    // the dialog never shows a loader for them; a big one falls back to it
    const auto quick = std::chrono::milliseconds(c.host.empty() ? 80 : 0);
    const auto quickList = std::chrono::milliseconds(c.host.empty() ? 250 : 0); // a big repository takes ~150 ms
    const bool working = c.from.empty(); // the folder against the last commit: staged files and the rest
    const auto gotList = [&](std::vector<Git::Change> got) {
        c.list = std::move(got);
        c.diffFor.clear();
    };
    const auto gotDiff = [&](std::pair<std::string, std::vector<Diff::Line>> got) {
        if (got.first == c.diffFor) {
            c.lines = std::move(got.second);
        }
    };
    c.listOp.check(gotList);
    if (std::exchange(c.reload, false) && !c.listOp.isRunning()) {
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
    // the rows: the staged files, then the rest (a partly staged file is in both); a turn's are one list
    std::vector<ChangeRow> rows;
    for (int pass = 0; pass < (working ? 2 : 1); ++pass) {
        for (int i = 0; i < static_cast<int>(c.list.size()); ++i) {
            const Git::Change& ch = c.list[static_cast<size_t>(i)];
            if (!working || (pass == 0 ? ch.staged() : ch.unstaged())) {
                rows.push_back({i, working && pass == 0});
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
    if (std::exchange(c.want, false)) {
        ImGui::OpenPopup("##changes");
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(1100.0f, vp->WorkSize.x - 32), std::min(720.0f, vp->WorkSize.y - 32)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, colors.mantle);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.surface1);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Round::Dialog);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    const bool shown = ImGui::BeginPopupModal("##changes", nullptr,
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
    ImGui::TextUnformatted(!c.title.empty() ? c.title.c_str() : c.from.empty() ? "Changes" : "Changes in this turn");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    int added = 0, removed = 0;
    for (const auto& change : c.list) {
        added += std::max(0, change.added);
        removed += std::max(0, change.removed);
    }
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
    bool close = imshell::IconButton(ICON_FA_XMARK, imshell::ButtonVariant::Secondary, ImVec2(button, button));
    ImGui::Spacing();

    if (c.listOp.isRunning() && c.list.empty()) {
        ui::Progress(-1, "Reading the changes");
    } else if (c.list.empty()) {
        ImGui::TextColored(colors.overlay1, c.from.empty() ? "Nothing has changed since the last commit." : "Nothing changed.");
    } else if (imshell::BeginResizable("changes", {.size = ImVec2(0, -(ImGui::GetFrameHeightWithSpacing() + 10)),
                                                   .line = ImGui::GetColorU32(colors.surface1),
                                                   .lit = ImGui::GetColorU32(colors.blue)})) {
        // the files at the left; up and down move through them. in the folder's own view they are
        // two sections, the staged ones and the others, with a button to move each file between
        if (imshell::ResizablePanel({.size = &c.listWidth, .min = 160, .fixed = true})) {
            if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && c.asking.empty()) {
                const int step = (ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : 0) - (ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? 1 : 0);
                if (step != 0) {
                    select(std::clamp(c.selected + step, 0, static_cast<int>(rows.size()) - 1));
                }
            }
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
            for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
                const ChangeRow& row = rows[static_cast<size_t>(i)];
                const Git::Change& change = c.list[static_cast<size_t>(row.index)];
                if (working && (i == 0 || rows[static_cast<size_t>(i) - 1].staged != row.staged)) {
                    ImGui::PushID(row.staged ? "staged" : "others");
                    if (i > 0) {
                        ImGui::Dummy(ImVec2(0, 4));
                    }
                    const std::vector<Git::Change> section = filesOf(row.staged);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextColored(colors.overlay1, "%s  %zu", row.staged ? "Staged" : "Changes", section.size());
                    ImGui::SameLine(ImGui::GetContentRegionMax().x - ImGui::CalcTextSize(row.staged ? "Unstage all" : "Stage all").x - 2 * ImGui::GetStyle().FramePadding.x - 8);
                    ImGui::BeginDisabled(!idle);
                    if (ImGui::SmallButton(row.staged ? "Unstage all" : "Stage all")) {
                        edit(row.staged ? 'u' : 's', section);
                    }
                    ImGui::EndDisabled();
                    ImGui::PopID();
                }
                ImGui::PushID(i);
                if (rowButton("##file", i == c.selected, ImGui::GetTextLineHeight() + 10)) {
                    select(i);
                }
                const ImVec2 end = ImGui::GetItemRectMax(), start = ImGui::GetItemRectMin();
                const bool over = ImGui::IsItemHovered() || (idle && ImGui::IsMouseHoveringRect(start, end) &&
                                                             ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows));
                ImDrawList* draw = ImGui::GetWindowDrawList();
                const char state = !working ? change.kind : row.staged ? change.index : change.work;
                const ImVec4 tone = state == '?' || state == 'A' ? colors.green : state == 'D' ? colors.red : colors.yellow;
                const char letter[2] = {state == '?' ? 'U' : state, 0};
                const float y = start.y + (end.y - start.y - ImGui::GetTextLineHeight()) / 2;
                draw->AddText(ImVec2(start.x + 8, y), ImGui::GetColorU32(tone), letter);
                // the name, then its folder dimmed, cut to the row (and short of the buttons)
                const float buttons = working && over && idle ? (row.staged ? 26.0f : 54.0f) : 0.0f;
                const size_t slash = change.path.rfind('/');
                const std::string name = change.path.substr(slash == std::string::npos ? 0 : slash + 1);
                const std::string folder = slash == std::string::npos ? "" : change.path.substr(0, slash);
                draw->PushClipRect(start, ImVec2(end.x - 4 - buttons, end.y), true);
                draw->AddText(ImVec2(start.x + 26, y), ImGui::GetColorU32(colors.text), name.c_str());
                draw->AddText(ImVec2(start.x + 34 + ImGui::CalcTextSize(name.c_str()).x, y),
                              ImGui::GetColorU32(colors.overlay0), folder.c_str());
                draw->PopClipRect();
                if (buttons > 0) {
                    // on the row's own line, as full-height buttons: no cursor moves to get them there
                    const float rowHeight = end.y - start.y - 4;
                    ImGui::SameLine(end.x - buttons - 12 - (ImGui::GetWindowPos().x - ImGui::GetScrollX()), 0);
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.surface1);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Round::Row);
                    if (!row.staged) {
                        if (ImGui::Button(ICON_FA_ROTATE_LEFT "##discard", ImVec2(26, rowHeight))) {
                            c.asking = {change};
                            c.askWant = true;
                        }
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip(change.work == '?' ? "Delete the file" : "Discard the changes");
                        }
                        ImGui::SameLine(0, 2);
                    }
                    if (ImGui::Button(row.staged ? ICON_FA_MINUS "##unstage" : ICON_FA_PLUS "##stage", ImVec2(26, rowHeight))) {
                        edit(row.staged ? 'u' : 's', {change});
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip(row.staged ? "Unstage" : "Stage");
                    }
                    ImGui::PopStyleVar(2);
                    ImGui::PopStyleColor(2);
                } else if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", change.path.c_str());
                }
                ImGui::PopID();
            }
        }
        if (imshell::ResizablePanel({.min = 300})) {
            if (c.selected >= 0 && c.selected < static_cast<int>(rows.size())) {
                const Git::Change& change = c.list[static_cast<size_t>(rows[static_cast<size_t>(c.selected)].index)];
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
    // throwing changes away has no way back: asked first
    if (std::exchange(c.askWant, false)) {
        ImGui::OpenPopup("##discard");
    }
    if (ImGui::BeginPopupModal("##discard", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
        const size_t fresh = static_cast<size_t>(std::ranges::count(c.asking, '?', &Git::Change::work));
        if (c.asking.size() == 1) {
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
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (imshell::Button(c.asking.size() == 1 && fresh ? "Delete" : "Discard", imshell::ButtonVariant::Primary)) {
            edit('d', std::exchange(c.asking, {}));
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    // the commit bar
    const bool busy = c.commitOp.isRunning();
    const int stagedCount = static_cast<int>(std::ranges::count_if(c.list, [](const Git::Change& ch) { return ch.staged(); }));
    if (c.from.empty() && (!c.list.empty() || c.pushed)) { // committing is for the folder, not a turn
        ImGui::Dummy(ImVec2(0, 4));
        // after a push with nothing left: the pull request alone
        bool enter = false;
        if (c.pushed && c.list.empty()) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(colors.green, ICON_FA_CHECK "  Pushed");
        } else {
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 250 - 8);
            ImGui::BeginDisabled(busy);
            enter = ImGui::InputTextWithHint("##message", stagedCount > 0 ? "Commit message (the staged files)" : "Commit message (all the changes)", c.message, sizeof(c.message),
                                             ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::EndDisabled();
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
        ImGui::SameLine(0, 8);
        if (c.pushed && c.list.empty()) {
            ImGui::BeginDisabled(busy);
            if (imshell::Button(ICON_FA_CODE_PULL_REQUEST "  Open a pull request", imshell::ButtonVariant::Primary)) {
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
            ImGui::BeginDisabled(!canCommit);
            if (imshell::Button("Commit") || (enter && canCommit)) {
                start(false);
            }
            ImGui::SameLine(0, 6);
            if (imshell::Button(ICON_FA_ARROW_UP "  Commit and push", imshell::ButtonVariant::Primary)) {
                start(true);
            }
            ImGui::EndDisabled();
        }
        if (busy) {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(colors.overlay1, "Working...");
        }
        if (!c.said.empty()) {
            ImGui::TextColored(colors.red, "%s", c.said.c_str());
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
