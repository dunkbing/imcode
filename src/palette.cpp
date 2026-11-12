#include "app.hpp"
#include "home.hpp"
#include "text.hpp"

#include "imshell/button.hpp"
#include "imshell/components.hpp"
#include "imshell/themes.hpp"
#include <algorithm>
#include <climits>
#include <cstdlib>

namespace Theme = imshell::Theme;
namespace ui = imshell::ui;
namespace fs = std::filesystem;

namespace {
    struct Row {
        std::string icon, title, detail;
        bool live = false; // a host the app holds a connection to: a green dot
    };

    bool matches(const std::string& text, const std::string& query) {
        const auto it = std::ranges::search(text, query, [](char a, char b) {
                            return std::tolower(static_cast<unsigned char>(a)) ==
                                   std::tolower(static_cast<unsigned char>(b));
                        }).begin();
        return query.empty() || it != text.end();
    }

    std::vector<std::string> localDirs(const std::string& path, std::string& error) {
        std::vector<std::string> out;
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(path, ec)) {
            if (std::error_code e; entry.is_directory(e)) {
                out.push_back(entry.path().filename().string());
            }
        }
        if (ec) {
            error = "Could not open " + path;
        }
        std::ranges::sort(out);
        return out;
    }
} // namespace

void App::paletteOpen(bool actions) {
    palette.wantOpen = true;
    palette.step = actions ? Palette::Step::Actions : Palette::Step::Sources;
}

// list a folder of the host ("" = its home folder); a remote host is connected first
void App::paletteGo(const std::string& host, std::string path) {
    Palette& p = palette;
    if (p.op.isRunning()) {
        return;
    }
    p.error.clear();
    if (path.starts_with("~")) {
        path.replace(0, 1, p.home);
    }
    if (host.empty()) {
        const char* home = homeEnv();
        p.home = home ? home : "/";
        std::error_code ec;
        const fs::path abs = fs::weakly_canonical(path.empty() ? p.home : path, ec);
        std::string error;
        auto dirs = localDirs(abs.string(), error);
        if (!error.empty()) {
            p.error = error;
            return;
        }
        p.host.clear();
        p.dir = abs.string();
        p.dirs = std::move(dirs);
        p.step = Palette::Step::Browse;
        p.query.clear();
        p.selected = 0;
        return;
    }
    p.op.start([host, path, secret = p.secret] {
        Palette::Listing out;
        const auto session = Ssh::session(host, secret, out.error);
        if (!session) {
            return out;
        }
        out.path = session->realpath(path.empty() ? "." : path, out.error.message);
        if (!out.path.empty()) {
            out.dirs = session->listDirs(out.path, out.error.message);
            out.ok = out.error.message.empty();
        }
        return out;
    });
}

void App::paletteDraw() {
    Palette& p = palette;
    const auto& colors = Theme::Current()->colors;
    if (std::exchange(p.wantOpen, false)) {
        p.query.clear();
        p.error.clear();
        p.askSecret = false;
        p.selected = 0;
        p.focus = true;
        ImGui::OpenPopup("##palette");
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->GetCenter().x, vp->WorkPos.y + 90), ImGuiCond_Always,
                            ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(std::min(580.0f, vp->WorkSize.x - 32), 0));
    // darker than a row's highlight, so the selected row shows
    ImGui::PushStyleColor(ImGuiCol_PopupBg, colors.mantle);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.surface1);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14, 14));
    const bool shown = ImGui::BeginPopupModal(
        "##palette", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    if (!shown) {
        return;
    }
    p.op.check([&](Palette::Listing got) {
        p.error = got.error.message;
        p.askSecret = got.error.needsSecret;
        if (!got.ok) {
            return;
        }
        if (p.step == Palette::Step::Host) {
            p.home = got.path; // the first listing of a host is its home folder
        }
        p.dir = got.path;
        p.dirs = std::move(got.dirs);
        p.step = Palette::Step::Browse;
        p.query.clear();
        p.secret.clear();
        p.selected = 0;
        p.focus = true;
    });

    // the rows of this step, narrowed by what is typed
    std::vector<Row> rows;
    const char* hint = "Search...";
    const char* heading = "Sources";
    std::vector<int> targets; // Actions: a project (>= 0) or thread (-2 - index) per row
    constexpr int terminalRow = INT_MIN; // or the terminal toggle
    constexpr int settingsRow = INT_MIN + 1;
    if (p.step == Palette::Step::Actions) {
        hint = "Search commands, projects, and threads...";
        heading = "Actions";
        auto offer = [&](Row row, int target) {
            if (matches(row.title, p.query)) {
                rows.push_back(std::move(row));
                targets.push_back(target);
            }
        };
        offer({ICON_FA_FOLDER_PLUS, "Add project", "A folder here or on another machine"}, -1);
        offer({ICON_FA_GEAR, "Settings", "Agents: turn on, turn off, install"}, settingsRow);
        if (current >= 0) {
            offer({ICON_FA_TERMINAL, "Toggle terminal", "A shell in this thread's folder"}, terminalRow);
        }
        for (int i = 0; i < static_cast<int>(data.projects.size()); ++i) {
            const auto& project = data.projects[i];
            offer({ICON_FA_PEN_TO_SQUARE,
                   "New thread in " + fs::path(project.cwd).filename().string(),
                   project.host.empty() ? project.cwd : project.host + ":" + project.cwd},
                  i);
        }
        for (int i = 0; i < static_cast<int>(data.threads.size()); ++i) {
            const Thread& t = *data.threads[i];
            if (!t.title.empty()) {
                offer({ICON_FA_ARROW_UP, cut(t.title, 60),
                       fs::path(t.projectDir()).filename().string()},
                      -2 - i);
            }
        }
    } else if (p.step == Palette::Step::Sources) {
        for (const Row& row :
             {Row{ICON_FA_FOLDER_PLUS, "Local folder", "Browse a folder on this machine"},
              Row{ICON_FA_SERVER, "Remote project", "A folder on another machine, over ssh"}}) {
            if (matches(row.title, p.query)) {
                rows.push_back(row);
            }
        }
    } else if (p.step == Palette::Step::Host) {
        hint = "user@host, or a name from ~/.ssh/config";
        heading = "Hosts";
        std::vector<std::string> known;
        for (const auto& project : data.projects) {
            if (!project.host.empty() && std::ranges::find(known, project.host) == known.end()) {
                known.push_back(project.host);
            }
        }
        for (const auto& name : Ssh::configHosts()) {
            if (std::ranges::find(known, name) == known.end()) {
                known.push_back(name);
            }
        }
        for (const auto& name : known) {
            if (matches(name, p.query)) {
                const auto there = std::ranges::count(data.projects, name, &Store::Project::host);
                const bool live = Ssh::connected(name);
                std::string detail = live ? "Connected" : "";
                if (there > 0) {
                    detail += (detail.empty() ? "" : "  ·  ") + std::to_string(there) + (there == 1 ? " project" : " projects");
                }
                rows.push_back({ICON_FA_SERVER, name, detail, live});
            }
        }
    } else {
        hint = "Filter, or type a path";
        heading = "Directories";
        if (p.dir != "/" && p.query.empty()) { // while filtering, enter takes the first match
            rows.push_back({ICON_FA_TURN_UP, "..", ""});
        }
        for (const auto& name : p.dirs) {
            // hidden folders only when asked for by a leading dot
            if (matches(name, p.query) && (!name.starts_with(".") || p.query.starts_with("."))) {
                rows.push_back({ICON_FA_FOLDER, name, ""});
            }
        }
    }
    p.selected = std::clamp(p.selected, 0, std::max(0, static_cast<int>(rows.size()) - 1));

    // header: back, where we are, the input, and Add while browsing
    const bool browsing = p.step == Palette::Step::Browse;
    bool back = imshell::IconButton(ICON_FA_ARROW_LEFT);
    ImGui::SameLine();
    if (browsing) {
        std::string shown = p.dir == "/" ? "/" : p.dir + "/";
        if (!p.home.empty() && shown.starts_with(p.home)) {
            shown.replace(0, p.home.size(), "~");
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(colors.subtext0, "%s%s%s", p.host.c_str(), p.host.empty() ? "" : ":",
                           shown.c_str());
        ImGui::SameLine();
    }
    const float addWidth = browsing ? 90.0f : 0.0f;
    const std::string before = p.query;
    // a field being edited ignores changes to its text: when a step or a folder change
    // empties it, it becomes a new field
    static std::string shownFor;
    static int fieldId = 0;
    const std::string where = std::to_string(static_cast<int>(p.step)) + p.host + "\n" + p.dir;
    if (where != shownFor) {
        shownFor = where;
        ++fieldId;
        p.query.clear();
        p.focus = true;
    }
    ImGui::PushID(fieldId);
    if (std::exchange(p.focus, false)) {
        ImGui::SetKeyboardFocusHere();
    }
    bool enter = ui::TextField("##query", p.query,
                               {.placeholder = hint,
                                .width = ImGui::GetContentRegionAvail().x - addWidth -
                                         (browsing ? ImGui::GetStyle().ItemSpacing.x : 0),
                                .flags = ImGuiInputTextFlags_EnterReturnsTrue});
    ImGui::PopID();
    if (p.query != before) {
        p.selected = 0;
    }
    const ImGuiIO& io = ImGui::GetIO();
    bool add = false;
    if (browsing) {
        ImGui::SameLine();
        add = imshell::Button("Add", imshell::ButtonVariant::Primary, ImVec2(addWidth, 0));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Add this folder (Cmd/Ctrl+Enter)");
        }
        if (enter && (io.KeySuper || io.KeyCtrl)) {
            add = true;
            enter = false;
        }
    }
    if (p.step == Palette::Step::Host && p.askSecret) {
        enter |= ui::TextField("Password or key passphrase", p.secret,
                               {.password = true, .flags = ImGuiInputTextFlags_EnterReturnsTrue});
    }

    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
        ++p.selected;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
        --p.selected;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Backspace) && before.empty()) {
        back = true;
    }
    p.selected = std::clamp(p.selected, 0, std::max(0, static_cast<int>(rows.size()) - 1));

    ImGui::Spacing();
    ImGui::TextColored(colors.subtext0, "%s", heading);
    int picked = -1;
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 6);
    ImGui::BeginChild("rows", ImVec2(0, 300));
    ImGui::PopStyleVar();
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        ImGui::PushID(i);
        const float height = rows[i].detail.empty()
                                 ? ImGui::GetTextLineHeight() + 12
                                 : ImGui::GetTextLineHeight() * 2 + ImGui::GetStyle().ItemSpacing.y + 12;
        const ImVec2 at = ImGui::GetCursorPos();
        if (rowButton("##row", i == p.selected, height, ImGuiSelectableFlags_DontClosePopups)) {
            picked = i;
        }
        if (i == p.selected && (ImGui::IsKeyPressed(ImGuiKey_DownArrow) ||
                                ImGui::IsKeyPressed(ImGuiKey_UpArrow))) {
            ImGui::SetScrollHereY();
        }
        const ImVec2 next = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(at.x + 10, at.y + 6));
        // icons differ in width: the titles start in one column
        ImGui::TextUnformatted(rows[i].icon.c_str());
        ImGui::SameLine(at.x + 10 + 30);
        ImGui::TextUnformatted(rows[i].title.c_str());
        if (rows[i].live) {
            const ImVec2 end = ImGui::GetItemRectMax();
            const float r = 3.5f;
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(end.x + 10 + r, end.y - ImGui::GetTextLineHeight() * 0.5f), r,
                                                        ImGui::GetColorU32(colors.green));
        }
        if (!rows[i].detail.empty()) {
            ImGui::SetCursorPosX(at.x + 10 + 30);
            ImGui::TextColored(colors.overlay1, "%s", rows[i].detail.c_str());
        }
        ImGui::SetCursorPos(next);
        ImGui::Dummy(ImVec2(0, 0)); // imgui wants an item after a cursor move
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (enter && !rows.empty()) {
        picked = p.selected;
    }

    if (p.op.isRunning()) {
        ui::Progress(-1, p.step == Palette::Step::Host ? "Connecting" : "Loading");
    }
    if (!p.error.empty()) {
        ui::Callout(p.askSecret ? "Sign in" : "Could not open", p.error.c_str(),
                    p.askSecret ? ui::Tone::Warning : ui::Tone::Danger);
    }
    if (vp->WorkSize.x >= 560) { // a phone has no keys to hint at
        ImGui::Separator();
        ImGui::TextColored(colors.overlay1, "Up/Down navigate    Enter select    Backspace back    Esc close");
    }

    // act on what was chosen
    bool close = ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (p.step == Palette::Step::Actions) {
        close |= back && before.empty() && !ImGui::IsKeyPressed(ImGuiKey_Backspace);
        if (picked >= 0 && targets[picked] == -1) {
            p.step = Palette::Step::Sources;
            p.query.clear();
            p.focus = true;
        } else if (picked >= 0) {
            close = true;
            if (targets[picked] == terminalRow) {
                toggleTerminal();
            } else if (targets[picked] == settingsRow) {
                settingsWant = true;
            } else if (targets[picked] >= 0) {
                newThread(targets[picked]);
            } else {
                select(-2 - targets[picked]);
            }
        }
    } else if (p.step == Palette::Step::Sources) {
        close |= back;
        if (picked >= 0 && rows[picked].title == "Local folder") {
            paletteGo("", "");
            p.focus = true;
        } else if (picked >= 0) {
            p.step = Palette::Step::Host;
            p.query.clear();
            p.focus = true;
        }
    } else if (p.step == Palette::Step::Host) {
        if (back) {
            p.step = Palette::Step::Sources;
            p.query.clear();
            p.error.clear();
            p.focus = true;
        } else if (picked >= 0 || (enter && !p.query.empty())) {
            // a listed host, or exactly what was typed
            p.host = picked >= 0 && !(enter && rows.empty()) ? rows[picked].title : Ssh::destination(p.query);
            paletteGo(p.host, "");
        }
    } else {
        if (back) {
            p.step = p.host.empty() ? Palette::Step::Sources : Palette::Step::Host;
            p.query.clear();
            p.focus = true;
        } else if (add) {
            const int index = addProject(p.host, p.dir);
            close = true;
            newThread(index);
        } else if (enter && (p.query.starts_with("/") || p.query.starts_with("~"))) {
            paletteGo(p.host, p.query); // a typed path
            p.focus = true;
        } else if (picked >= 0) {
            const std::string& name = rows[picked].title;
            paletteGo(p.host, name == ".." ? fs::path(p.dir).parent_path().string()
                                           : (p.dir == "/" ? "" : p.dir) + "/" + name);
            p.focus = true;
        }
    }
    if (close) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
