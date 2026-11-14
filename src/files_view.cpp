#include "app.hpp"
#include "git_local.hpp"
#include "home.hpp"
#include "imshell/resizable.hpp"

#include "imshell/image.hpp"

#include "acp_host/agents.hpp"
#include "acp_host/process_runner.hpp"
#include "imshell/button.hpp"
#include "imshell/components.hpp"
#include "imshell/frames.hpp"
#include "colors.hpp"
#include "code_colors.hpp"
#include <algorithm>
#include <chrono>
#include <deque>
#include <format>
#include <fstream>
#include <optional>

using Theme = imshell::Theme;
namespace ui = imshell::ui;
namespace fs = std::filesystem;
using Files = App::Files;

namespace {
    constexpr size_t textBytes = 1 << 20;
    constexpr int maxLineBytes = 2000;
    constexpr size_t maxIndexed = 20000;

    std::string under(const std::string& cwd, const std::string& path) {
        return path.empty() ? cwd : cwd + "/" + path;
    }

    // a file on a host saved in ~/Downloads under a free name; what to tell the user
    // shortcut: read whole into memory (capped), no progress; stream it in chunks for big files
    std::string download(const std::string& host, const std::string& full, const std::string& name) {
        constexpr size_t cap = 512u << 20;
        const char* home = homeEnv();
        if (!home) {
            return "Could not find the Downloads folder.";
        }
        Ssh::Error failed;
        const auto session = Ssh::session(host, "", failed);
        if (!session) {
            return failed.message;
        }
        std::string error;
        const std::string bytes = session->read(full, cap, error);
        if (!error.empty()) {
            return error;
        }
        if (bytes.size() >= cap) {
            return name + " is over 512 MB: not downloaded.";
        }
        const fs::path dir = fs::path(home) / "Downloads";
        std::error_code ec;
        fs::create_directories(dir, ec);
        const fs::path base(name);
        fs::path target = dir / base;
        for (int n = 1; fs::exists(target, ec); ++n) {
            target = dir / (base.stem().string() + std::format(" ({})", n) + base.extension().string());
        }
        if (!(std::ofstream(target, std::ios::binary) << bytes)) {
            return "Could not write " + target.string();
        }
        return "Saved to ~/Downloads/" + target.filename().string();
    }

    // files from this machine copied into a folder of the project, here or on its host, each under a
    // free name; what to tell the user. shortcut: read whole into memory (capped), no progress
    std::string copyIn(const std::string& host, const std::string& dir, const std::string& rel, const std::vector<std::string>& paths) {
        constexpr size_t cap = 512u << 20;
        std::shared_ptr<Ssh::Session> session;
        std::set<std::string> taken;
        std::error_code ec;
        if (!host.empty()) {
            Ssh::Error failed;
            session = Ssh::session(host, "", failed);
            if (!session) {
                return failed.message;
            }
            std::string error;
            for (const auto& entry : session->list(dir, error)) {
                taken.insert(entry.name);
            }
        }
        std::string last, notes;
        int done = 0;
        for (const auto& source : paths) {
            const fs::path from(source);
            if (fs::is_directory(from, ec)) {
                notes += " " + from.filename().string() + " is a folder: not uploaded.";
                continue;
            }
            std::string name = from.filename().string();
            for (int n = 1; host.empty() ? fs::exists(fs::path(dir) / name, ec) : taken.contains(name); ++n) {
                name = from.stem().string() + std::format(" ({})", n) + from.extension().string();
            }
            std::string error;
            if (host.empty()) {
                fs::copy_file(from, fs::path(dir) / name, ec);
                if (ec) {
                    error = ec.message();
                }
            } else {
                std::ifstream in(from, std::ios::binary);
                std::string bytes(std::istreambuf_iterator<char>(in), {});
                if (!in && !in.eof()) {
                    error = "Could not read " + source;
                } else if (bytes.size() >= cap) {
                    error = name + " is over 512 MB: not uploaded.";
                } else if (!session->write(dir + "/" + name, bytes, error) && error.empty()) {
                    error = "Could not write " + name;
                }
            }
            if (!error.empty()) {
                return error;
            }
            taken.insert(name);
            last = rel.empty() ? name : rel + "/" + name;
            ++done;
        }
        if (done == 0) {
            return notes.empty() ? "Nothing to upload." : notes.substr(1);
        }
        return (done == 1 ? "Uploaded to " + last : std::format("Uploaded {} files to {}", done, rel.empty() ? "the project root" : rel)) + notes;
    }

    // what is in a folder, here or over sftp: folders first, then by name
    std::vector<Ssh::Entry> list(const std::string& host, const std::string& path, std::string& error) {
        if (!host.empty()) {
            Ssh::Error failed;
            const auto session = Ssh::session(host, "", failed);
            error = failed.message;
            return session ? session->list(path, error) : std::vector<Ssh::Entry>{};
        }
        std::vector<Ssh::Entry> out;
        std::error_code ec;
        for (fs::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code ignored;
            out.push_back({it->path().filename().string(), it->is_directory(ignored)});
        }
        if (ec) {
            error = ec.message();
        }
        std::ranges::sort(out, [](const Ssh::Entry& a, const Ssh::Entry& b) {
            return a.dir != b.dir ? a.dir : a.name < b.name;
        });
        return out;
    }

    Files::Doc read(const std::string& host, const std::string& cwd, const std::string& path, int epoch) {
        Files::Doc doc;
        doc.epoch = epoch;
        doc.path = path;
        const std::string full = under(cwd, path);
        // shortcut: png only (what imshell decodes); other pictures still read as binary
        const bool picture = fs::path(path).extension() == ".png";
        const size_t maxBytes = picture ? 16u << 20 : textBytes;
        // one byte over the limit tells a larger file from one that just fits
        if (!host.empty()) {
            Ssh::Error failed;
            const auto session = Ssh::session(host, "", failed);
            doc.note = failed.message;
            if (session) {
                doc.text = session->read(full, maxBytes + 1, doc.note);
            }
        } else if (std::ifstream in(full, std::ios::binary); in) {
            doc.text.resize(maxBytes + 1);
            in.read(doc.text.data(), static_cast<std::streamsize>(doc.text.size()));
            doc.text.resize(static_cast<size_t>(in.gcount()));
        } else {
            doc.note = "Could not open " + full;
        }
        if (!doc.note.empty()) {
            doc.text.clear();
        } else if (picture && doc.text.size() > maxBytes) {
            doc.text.clear();
            doc.note = "A picture larger than 16 MB: not shown.";
        } else if (picture && doc.text.starts_with("\x89PNG")) {
            doc.image = std::move(doc.text);
            doc.text.clear();
        } else if (doc.text.substr(0, 8192).find('\0') != std::string::npos) {
            doc.text.clear();
            doc.note = "A binary file: not shown.";
        } else if (doc.text.size() > maxBytes) {
            const size_t cut = doc.text.rfind('\n', maxBytes);
            doc.text.resize(cut == std::string::npos ? maxBytes : cut);
            doc.note = "Larger than 1 MB: only the first part is shown.";
        }
        const Syntax::Lang lang = Syntax::langOf(fs::path(path).filename().string());
        std::vector<Syntax::Span> spans;
        bool carry = false;
        int widest = 0;
        for (size_t at = 0; at < doc.text.size();) {
            const size_t end = std::min(doc.text.find('\n', at), doc.text.size());
            doc.lines.push_back(static_cast<int>(at));
            doc.carry.push_back(carry);
            Syntax::line(lang, std::string_view(doc.text).substr(at, end - at), carry, spans);
            if (static_cast<int>(end - at) > widest) {
                widest = static_cast<int>(end - at);
                doc.widest = static_cast<int>(doc.lines.size()) - 1;
            }
            at = end + 1;
        }
        doc.lines.push_back(static_cast<int>(doc.text.size()) + 1);
        return doc;
    }

    // what git tracks or would track: the project's own ignore rules, for free. nullopt
    // when the folder is not a repository (or, on a host, git is not there)
    std::optional<std::vector<std::string>> gitFiles(const std::string& host, const std::string& cwd) {
        if (host.empty()) { // libgit2
            return Git::Local::files(cwd, maxIndexed);
        }
        std::vector<std::string> out;
        Ssh::Error sshError;
        const auto session = Ssh::session(host, "", sshError);
        std::string quoted = "'";
        for (const char c : cwd) {
            quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
        }
        std::string error;
        auto lines = session ? session->exec("cd " + quoted + "' && git ls-files --cached --others --exclude-standard",
                                             nullptr, error)
                             : nullptr;
        if (!lines) {
            return std::nullopt;
        }
        for (std::string line; out.size() < maxIndexed && lines->readLine(line);) {
            out.push_back(line);
        }
        return out.empty() ? std::nullopt : std::optional(out); // not a repository: nothing printed
    }

    // every file under cwd: git's list when there is one, else a walk, nearest first
    // shortcut: the walk has no ignore rules and stops at 20000 files
    Files::Listing index(const std::string& host, const std::string& cwd, int epoch) {
        Files::Listing out;
        out.epoch = epoch;
        if (auto tracked = gitFiles(host, cwd)) {
            for (auto& path : *tracked) {
                out.entries.push_back({std::move(path), false});
            }
            return out;
        }
        std::deque<std::string> todo{""};
        // counted too: links can make a loop of folders with no file in it
        for (size_t seen = 0; !todo.empty() && out.entries.size() < maxIndexed && seen < maxIndexed; ++seen) {
            const std::string dir = std::move(todo.front());
            todo.pop_front();
            std::string error;
            for (auto& entry : list(host, under(cwd, dir), error)) {
                if (entry.name == ".git") {
                    continue;
                }
                std::string path = dir.empty() ? entry.name : dir + "/" + entry.name;
                if (entry.dir) {
                    todo.push_back(std::move(path));
                } else {
                    out.entries.push_back({std::move(path), false});
                }
            }
        }
        return out;
    }

    // one row of the tree; true when clicked
    bool row(const std::string& path, const char* icon, const std::string& name, int depth, bool selected,
             const char* dimTail = nullptr) {
        const Colors colors = themeColors();
        const bool dim = name.starts_with(".");
        const float x = 8 + static_cast<float>(depth) * 14;
        const bool clicked = ImGui::Selectable(("##" + path).c_str(), selected);
        ImGui::SameLine(x);
        ImGui::TextColored(colors.overlay1, "%s", icon);
        ImGui::SameLine(x + 22);
        ImGui::TextColored(dim ? colors.overlay1 : colors.text, "%s", name.c_str());
        if (dimTail && *dimTail) {
            ImGui::SameLine();
            ImGui::TextColored(colors.overlay0, "%s", dimTail);
        }
        return clicked;
    }

    // the loaded part of the tree under dir; opened: the file that was clicked
    void tree(Files& f, const std::string& dir, int depth, std::string& opened) {
        const Colors colors = themeColors();
        const auto it = f.dirs.find(dir);
        const float x = 8 + static_cast<float>(depth) * 14 + 22;
        if (it == f.dirs.end() || !it->second.error.empty()) {
            ImGui::SetCursorPosX(x);
            ImGui::TextColored(colors.overlay0, "%s", it == f.dirs.end() ? "Loading..." : it->second.error.c_str());
            return;
        }
        for (const auto& entry : it->second.entries) {
            if (entry.name == ".git") {
                continue;
            }
            const std::string path = dir.empty() ? entry.name : dir + "/" + entry.name;
            if (!entry.dir) {
                if (row(path, ICON_FA_FILE, entry.name, depth, path == f.doc.path)) {
                    opened = path;
                }
                if (ImGui::IsItemHovered()) {
                    f.hoverDir = dir;
                }
                if (ImGui::BeginPopupContextItem(("##menu" + path).c_str())) {
                    if (ImGui::MenuItem(ICON_FA_CLOCK_ROTATE_LEFT "  History of this file")) {
                        f.historyOf = path;
                    }
                    // a file on a host: a copy in ~/Downloads
                    if (!f.host.empty() && ImGui::MenuItem(ICON_FA_DOWNLOAD "  Download", nullptr, false, !f.downloadOp.isRunning())) {
                        f.downloading = "Downloading " + entry.name + "...";
                        f.downloadOp.start([host = f.host, full = under(f.cwd, path), name = entry.name] {
                            return download(host, full, name);
                        });
                    }
                    ImGui::EndPopup();
                }
                continue;
            }
            const bool expanded = f.expanded.contains(path);
            const bool folderRow = row(path, expanded ? ICON_FA_FOLDER_OPEN : ICON_FA_FOLDER, entry.name, depth, false);
            if (ImGui::IsItemHovered()) {
                f.hoverDir = path;
            }
            if (folderRow) {
                if (expanded) {
                    f.expanded.erase(path);
                } else {
                    f.expanded.insert(path);
                }
            } else if (expanded) {
                tree(f, path, depth + 1, opened);
            }
        }
    }

    // the files whose path has the filter in it, as a flat list
    void matches(Files& f, std::string& opened) {
        const Colors colors = themeColors();
        if (!f.indexed) {
            ImGui::SetCursorPosX(8);
            ImGui::TextColored(colors.overlay0, "Looking...");
            return;
        }
        const std::string_view query = f.filter;
        std::vector<const std::string*> found;
        for (const auto& entry : f.index.entries) {
            if (!std::ranges::search(entry.name, query, [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                           std::tolower(static_cast<unsigned char>(b));
                }).empty()) {
                found.push_back(&entry.name);
            }
        }
        if (found.empty()) {
            ImGui::SetCursorPosX(8);
            ImGui::TextColored(colors.overlay0, "Nothing found");
        }
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(found.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const std::string& path = *found[i];
                const size_t slash = path.rfind('/');
                const std::string folder = slash == std::string::npos ? "" : path.substr(0, slash);
                if (row(path, ICON_FA_FILE, path.substr(slash + 1), 0, path == f.doc.path, folder.c_str())) {
                    opened = path;
                }
            }
        }
    }

    // the file in imshell's editor, read-only: line numbers, selection, copy, find
    void code(Files& f) {
        if (f.shownPath != f.doc.path || f.shownSize != f.doc.text.size()) {
            const Syntax::Lang lang = Syntax::langOf(fs::path(f.doc.path).filename().string());
            f.editor.SetReadOnly(true);
            f.editor.SetShowLineNumbers(true);
            f.editor.SetHighlighter(codeColors(lang));
            f.editor.SetText(f.doc.text);
            f.shownPath = f.doc.path;
            f.shownSize = f.doc.text.size();
        }
        f.editor.Render("##code", ImVec2(0, 0));
    }
} // namespace

std::vector<std::string> projectFiles(const std::string& host, const std::string& cwd) {
    std::vector<std::string> out;
    for (auto& entry : index(host, cwd, 0).entries) {
        out.push_back(std::move(entry.name));
    }
    return out;
}

void App::showFile(const std::string& path) {
    files.want = path;
    files.open = true;
    files.treeOver = false;
    files.tab = 0;
}

void App::filesPoll() {
    Files& f = files;
    f.listOp.check([&](Files::Listing got) {
        if (got.epoch == f.epoch) {
            f.dirs[got.dir] = std::move(got);
        }
    });
    f.indexOp.check([&](Files::Listing got) {
        if (got.epoch == f.epoch) {
            f.index = std::move(got);
            f.indexed = true;
        }
    });
    f.downloadOp.check([&](std::string note) {
        f.downloading.clear();
        const bool good = note.starts_with("Saved to ") || note.starts_with("Uploaded");
        ui::Toast(note.c_str(), good ? ui::Tone::Success : ui::Tone::Danger, 8);
        if (f.uploadDir) {
            f.dirs.erase(*std::exchange(f.uploadDir, std::nullopt)); // listed again with the new file
            f.indexed = false;
        }
    });
    f.readOp.check([&](Files::Doc got) {
        if (got.epoch == f.epoch) {
            f.doc = std::move(got);
            f.picturePath.clear(); // a new read: its picture is uploaded again
            f.toTop = true;
            // shown in the tree too
            for (size_t slash = f.doc.path.find('/'); slash != std::string::npos;
                 slash = f.doc.path.find('/', slash + 1)) {
                f.expanded.insert(f.doc.path.substr(0, slash));
            }
        }
    });
    if (!f.open || current < 0) {
        return;
    }
    const Thread& t = *data.threads[current];
    if (f.host != t.host || f.cwd != t.cwd || f.epoch == 0) {
        ++f.epoch;
        f.host = t.host;
        f.cwd = t.cwd;
        f.dirs.clear();
        f.expanded.clear();
        f.index = {};
        f.indexed = false;
        f.doc = {};
        f.picturePath.clear(); // a new read: its picture is uploaded again
        f.filter[0] = 0;
        f.treeOver = f.want.empty(); // a file asked for as the panel opens stays in front
    }
    // one at a time of each kind: a running one cannot be replaced
    if (!f.listOp.isRunning()) {
        std::optional<std::string> next;
        if (!f.dirs.contains("")) {
            next = "";
        }
        for (const auto& dir : f.expanded) {
            if (!next && !f.dirs.contains(dir)) {
                next = dir;
            }
        }
        if (next) {
            f.listOp.start([host = f.host, cwd = f.cwd, dir = *next, epoch = f.epoch] {
                Files::Listing out;
                out.epoch = epoch;
                out.dir = dir;
                out.entries = list(host, under(cwd, dir), out.error);
                return out;
            });
        }
    }
    if (f.filter[0] && !f.indexed && !f.indexOp.isRunning()) {
        f.indexOp.start([host = f.host, cwd = f.cwd, epoch = f.epoch] { return index(host, cwd, epoch); });
    }
    if (!f.want.empty() && !f.readOp.isRunning()) {
        f.readOp.start([host = f.host, cwd = f.cwd, path = std::exchange(f.want, {}), epoch = f.epoch] {
            return read(host, cwd, path, epoch);
        });
    }
}

bool App::filesPanel(Thread& t, bool beside) {
    Files& f = files;
    bool released = false;
    const Colors colors = themeColors();
    const CoverFirstFrame cover(f.settle.first(), ImGui::GetContentRegionAvail()); // the panels are laid out as it opens
    ImGui::BeginChild("files");
    if (f.tab == 1) {
        // the open thread's folder, read again when it is another one
        if (dock.host != t.host || dock.cwd != t.cwd) {
            dock.list.clear();
            dock.lines.clear();
            dock.diffFor.clear();
            dock.selected = -1;
            dock.pushed = false;
            dock.said.clear();
            dock.host = t.host;
            dock.cwd = t.cwd;
            dock.reload = true;
        }
        changesDraw(dock, true);
        ImGui::EndChild();
        return released;
    }
    // no room for the file beside its tree: one at a time, as when the panel has the window
    const bool alone = !beside || ImGui::GetContentRegionAvail().x < 400;
    const bool showTree = !alone || f.treeOver || f.doc.path.empty();
    std::string opened;

    const auto fileView = [&] {
        if (f.doc.path.empty()) {
            ImGui::TextColored(colors.overlay1, "Pick a file in the tree to read it here.");
        } else {
            // project > path/to/file, and what the button at the right needs
            const float button = ImGui::GetFrameHeight();
            const size_t slash = f.doc.path.rfind('/');
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(colors.subtext0, "%s", fs::path(t.projectDir()).filename().string().c_str());
            ImGui::SameLine();
            ImGui::TextColored(colors.overlay0, ICON_FA_CHEVRON_RIGHT);
            ImGui::SameLine();
            ImGui::TextColored(colors.subtext0, "%s", f.doc.path.substr(0, slash + 1).c_str());
            ImGui::SameLine(0, 0);
            ImGui::TextUnformatted(f.doc.path.c_str() + slash + 1);
            if (alone) {
                ImGui::SameLine(ImGui::GetContentRegionMax().x - button);
                if (imshell::IconButton(ICON_FA_FOLDER_TREE, imshell::ButtonVariant::Secondary,
                                       ImVec2(button, button))) {
                    f.treeOver = true;
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Back to the files");
                }
            }
            if (!f.doc.note.empty()) {
                ImGui::TextColored(colors.yellow, "%s", f.doc.note.c_str());
            }
            ImGui::Spacing();
            if (!f.doc.image.empty()) {
                if (f.picturePath != f.doc.path) {
                    f.picturePath = f.doc.path;
                    f.picture = imshell::LoadTexture(reinterpret_cast<const uint8_t*>(f.doc.image.data()),
                                                     f.doc.image.size(), upload, &f.pictureWidth, &f.pictureHeight);
                }
                if (f.picture && f.pictureWidth > 0) {
                    // whole in the panel, never larger than it is
                    const ImVec2 room = ImGui::GetContentRegionAvail();
                    const float scale = std::min({1.0f, room.x / static_cast<float>(f.pictureWidth),
                                                  room.y / static_cast<float>(f.pictureHeight)});
                    ImGui::Image(f.picture, ImVec2(static_cast<float>(f.pictureWidth) * scale,
                                                   static_cast<float>(f.pictureHeight) * scale));
                    ImGui::TextColored(colors.overlay0, "%d x %d", f.pictureWidth, f.pictureHeight);
                } else {
                    ImGui::TextColored(colors.yellow, "This picture could not be read.");
                }
            } else if (f.doc.lines.size() > 1) {
                code(f);
            }
        }
    };
    const auto treeView = [&] {
        f.overTree = ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##filter", ICON_FA_MAGNIFYING_GLASS "  Search files", f.filter,
                                 sizeof(f.filter));
        ImGui::Spacing();
        ImGui::BeginChild("rows");
        if (f.filter[0]) {
            matches(f, opened);
        } else {
            tree(f, "", 0, opened);
        }
        ImGui::EndChild();
    };
    if (!alone) {
        if (imshell::BeginResizable("split", {.line = ImGui::GetColorU32(colors.surface1),
                                              .lit = ImGui::GetColorU32(colors.blue)})) {
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 6));
            const bool file = imshell::ResizablePanel({.min = 200, .childFlags = ImGuiChildFlags_AlwaysUseWindowPadding});
            ImGui::PopStyleVar();
            if (file) {
                fileView();
            }
            if (imshell::ResizablePanel({.size = &f.treeWidth, .min = 140, .fixed = true,
                                         .childFlags = ImGuiChildFlags_Borders})) {
                treeView();
            }
            released |= imshell::EndResizable();
        }
    } else if (showTree) {
        ImGui::BeginChild("tree", ImVec2(0, 0), ImGuiChildFlags_Borders);
        treeView();
        ImGui::EndChild();
    } else {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 6));
        ImGui::BeginChild("file", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        fileView();
        ImGui::EndChild();
    }
    ImGui::EndChild();
    if (!opened.empty()) {
        showFile(opened);
    }
    if (!files.historyOf.empty()) {
        if (current >= 0) {
            graphOpen(*data.threads[current], files.historyOf);
        }
        files.historyOf.clear();
    }
    return released;
}

// files dropped on the window, placed after the frame is drawn (it knows what is under the pointer):
// on the tree, copied into the folder there (uploaded on a host); anywhere else, attached to the
// next message, which uploads them to a host itself
void App::handleDrops() {
    if (dropped.empty()) {
        return;
    }
    Files& f = files;
    auto paths = std::exchange(dropped, {});
    if (f.open && f.overTree && current >= 0 && f.tab == 0 && !f.downloadOp.isRunning()) {
        f.uploadDir = f.hoverDir;
        f.downloading = "Uploading " + fs::path(paths.front()).filename().string() + (paths.size() > 1 ? " and more..." : "...");
        f.downloadOp.start([host = f.host, dir = under(f.cwd, f.hoverDir), rel = f.hoverDir, paths = std::move(paths)] {
            return copyIn(host, dir, rel, paths);
        });
        return;
    }
    attachments.insert(attachments.end(), paths.begin(), paths.end());
    focusInput = true;
}

// a card over the window's bottom right while a download or upload runs: no layout space, and the
// result then replaces it as a toast in the same corner
void App::transferCard() {
    if (files.downloading.empty()) {
        return;
    }
    const Colors colors = themeColors();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float margin = Theme::Spacing::L, pad = Theme::Spacing::M + Theme::Spacing::S;
    const float width = std::min(320.0f, vp->WorkSize.x - margin * 2);
    const ImVec2 text = ImGui::CalcTextSize(files.downloading.c_str(), nullptr, false, width - pad * 2);
    const ImVec2 size(width, text.y + pad * 2);
    const ImVec2 at(vp->WorkPos.x + vp->WorkSize.x - margin - width, vp->WorkPos.y + vp->WorkSize.y - margin - size.y);
    ImDrawList* draw = ImGui::GetForegroundDrawList(vp);
    draw->AddRectFilled(at, ImVec2(at.x + size.x, at.y + size.y), ImGui::GetColorU32(colors.mantle), Theme::Radius::Lg);
    draw->AddRect(at, ImVec2(at.x + size.x, at.y + size.y), ImGui::GetColorU32(colors.surface1), Theme::Radius::Lg);
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(at.x + pad, at.y + pad), ImGui::GetColorU32(colors.text),
                  files.downloading.c_str(), nullptr, width - pad * 2);
}
