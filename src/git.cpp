#include "git.hpp"
#include "trace.hpp"
#include "text.hpp"
#include "home.hpp"

#include "acp_host/agents.hpp"
#include "acp_host/process_runner.hpp"
#include "git_local.hpp"
#include "ssh.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#ifdef _WIN32
#include <windows.h>
#endif

namespace Git {

    Status parse(std::string_view out) {
        Status status;
        for (size_t at = 0; at < out.size();) {
            const size_t end = std::min(out.find('\n', at), out.size());
            const std::string_view line = out.substr(at, end - at);
            at = end + 1;
            if (line.starts_with("## ")) {
                status.repo = true;
                std::string_view name = line.substr(3);
                // "main...origin/main [ahead 1]", "No commits yet on main", "HEAD (no branch)"
                if (name.starts_with("No commits yet on ")) {
                    name.remove_prefix(18);
                } else if (name.starts_with("HEAD (no branch)")) {
                    name = "detached";
                }
                name = name.substr(0, std::min(name.find("..."), name.find(' ')));
                status.branch = std::string(name);
            } else if (!line.empty()) {
                ++status.changed;
            }
        }
        return status;
    }

    Status status(const std::string& host, const std::string& cwd) {
        trace::Span traced_("git.status", host + " " + cwd, 5);
        if (host.empty()) {
            return Local::status(cwd).value_or(Status{});
        }
        Ssh::Error sshError;
        const auto session = Ssh::session(host, "", sshError);
        std::string quoted = "'";
        for (const char c : cwd) {
            quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
        }
        std::string error;
        auto lines = session ? session->exec("cd " + quoted + "' && git --no-optional-locks status --porcelain=v1 -b", nullptr, error)
                             : nullptr;
        std::string out;
        for (std::string line; lines && lines->readLine(line);) {
            out += line + "\n";
        }
        return parse(out);
    }

    std::string slug(const std::string& text) {
        std::string out;
        for (const char c : text) {
            if (out.size() >= 28 || c == '\n') {
                break;
            }
            if (std::isalnum(static_cast<unsigned char>(c))) {
                out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            } else if (!out.empty() && out.back() != '-') {
                out += '-';
            }
        }
        while (!out.empty() && out.back() == '-') {
            out.pop_back();
        }
        // the clock's low bits: two threads asking the same thing still get two branches
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
        std::string tail;
        for (int i = 0; i < 4; ++i) {
            tail += digits[(ticks >> (i * 5)) % 36];
        }
        return (out.empty() ? "thread" : out) + "-" + tail;
    }

    namespace {
        std::string quote(const std::string& s) {
            std::string out = "'";
            for (const char c : s) {
                out += c == '\'' ? std::string("'\\''") : std::string(1, c);
            }
            return out + "'";
        }

        // a path for git to take literally from the top of the repository (status paths are relative to it)
        std::string pathspec(const std::string& path) {
            return quote(":(top,literal)" + path);
        }

        // a path as git status prints it: in double quotes when it has spaces and such
        std::string unquote(std::string_view path) {
            if (path.size() >= 2 && path.front() == '"' && path.back() == '"') {
                path = path.substr(1, path.size() - 2);
            }
            return std::string(path);
        }

        // the local sh. windows: git for windows' own (<Git>\cmd\git.exe is on PATH, sh is
        // <Git>\bin\sh.exe); without it local git commands fail with its message
        const std::string& localShell() {
#ifdef _WIN32
            static const std::string sh = [] {
                char git[MAX_PATH];
                if (SearchPathA(nullptr, "git.exe", nullptr, MAX_PATH, git, nullptr) == 0) {
                    return std::string("sh.exe");
                }
                return (std::filesystem::path(git).parent_path().parent_path() / "bin" / "sh.exe").string();
            }();
#else
            static const std::string sh = "/bin/sh";
#endif
            return sh;
        }

        // a shell script on this machine or on the host, its output (both streams)
        std::string run(const std::string& host, const std::string& script, std::chrono::seconds timeout = std::chrono::seconds(60)) {
        trace::Span traced_("git.run", host + " " + cut(script, 70), 0);
            if (host.empty()) {
                return ProcessRunner::run({.args = {localShell(), "-c", script},
                                           .environment = {{"PATH", AcpAgents::loginShellPath()}},
                                           .timeout = timeout})
                    .output;
            }
            Ssh::Error sshError;
            const auto session = Ssh::session(host, "", sshError);
            std::string failed, out;
            auto lines = session ? session->exec(script, nullptr, failed) : nullptr;
            for (std::string line; lines && lines->readLine(line);) {
                out += line + "\n";
            }
            return lines ? out : sshError.message + failed;
        }
    } // namespace

    std::vector<Change> parseChanges(std::string_view porcelain, std::string_view numstat) {
        std::vector<Change> out;
        const auto eachLine = [](std::string_view text, auto&& fn) {
            for (size_t at = 0; at < text.size();) {
                const size_t end = std::min(text.find('\n', at), text.size());
                fn(text.substr(at, end - at));
                at = end + 1;
            }
        };
        eachLine(porcelain, [&](std::string_view line) {
            if (line.size() < 4 || line.starts_with("## ")) {
                return;
            }
            Change c;
            const char x = line[0], y = line[1];
            c.kind = x == '?' ? '?' : (x == 'R' || y == 'R') ? 'R' : (x == 'A' || y == 'A') ? 'A'
                                    : (x == 'D' || y == 'D') ? 'D' : 'M';
            c.index = x == '?' ? ' ' : x;
            c.work = y;
            std::string_view path = line.substr(3);
            if (const size_t arrow = path.find(" -> "); arrow != std::string_view::npos) {
                c.from = unquote(path.substr(0, arrow)); // a rename: its new name
                path = path.substr(arrow + 4);
            }
            if (path.size() >= 2 && path.front() == '"' && path.back() == '"') {
                path = path.substr(1, path.size() - 2); // quoted by git (spaces and such)
            }
            c.path = std::string(path);
            out.push_back(std::move(c));
        });
        std::unordered_map<std::string_view, size_t> byPath; // a path's place, not a scan for each line
        for (size_t i = 0; i < out.size(); ++i) {
            byPath[out[i].path] = i;
        }
        // "<added>\t<removed>\t<path>", "-" for binary files
        eachLine(numstat, [&](std::string_view line) {
            const size_t t1 = line.find('\t'), t2 = line.find('\t', t1 + 1);
            if (t1 == std::string_view::npos || t2 == std::string_view::npos) {
                return;
            }
            std::string path(line.substr(t2 + 1));
            if (const size_t arrow = path.find(" => "); arrow != std::string::npos) {
                path = path.substr(arrow + 4);
            }
            if (line[0] == '-') {
                return;
            }
            if (const auto at = byPath.find(path); at != byPath.end()) {
                Change& c = out[at->second];
                c.added = std::atoi(std::string(line.substr(0, t1)).c_str());
                c.removed = std::atoi(std::string(line.substr(t1 + 1, t2 - t1 - 1)).c_str());
            }
        });
        return out;
    }

    std::vector<Diff::Line> parseDiff(std::string_view unified) {
        std::vector<Diff::Line> out;
        bool body = false; // past the file's headers
        for (size_t at = 0; at < unified.size();) {
            const size_t end = std::min(unified.find('\n', at), unified.size());
            const std::string_view line = unified.substr(at, end - at);
            at = end + 1;
            if (line.starts_with("@@")) {
                if (!out.empty()) {
                    out.push_back({'~', ""});
                }
                body = true;
            } else if (line.starts_with("diff --git")) {
                body = false;
            } else if (body && !line.empty() && (line[0] == '+' || line[0] == '-' || line[0] == ' ')) {
                out.push_back({line[0], std::string(line.substr(1))});
            } else if (body && line.empty()) {
                out.push_back({' ', ""});
            }
        }
        return out;
    }

    std::string nameStatusToPorcelain(std::string_view nameStatus) {
        // "M\tpath", "R100\told\tnew" -> "M  path", "R  old -> new"
        std::string out;
        for (size_t at = 0; at < nameStatus.size();) {
            const size_t end = std::min(nameStatus.find('\n', at), nameStatus.size());
            const std::string_view line = nameStatus.substr(at, end - at);
            at = end + 1;
            const size_t t1 = line.find('\t');
            if (line.empty() || t1 == std::string_view::npos) {
                continue;
            }
            std::string path(line.substr(t1 + 1));
            if (const size_t t2 = path.find('\t'); t2 != std::string::npos) {
                path = path.substr(0, t2) + " -> " + path.substr(t2 + 1);
            }
            out += std::string(1, line[0]) + "  " + path + "\n";
        }
        return out;
    }

    std::vector<Change> changes(const std::string& host, const std::string& cwd, const std::string& from,
                                const std::string& to) {
        trace::Span traced_("git.changes", host + " " + cwd, 0);
        if (host.empty()) {
            return Local::changes(cwd, from, to).value_or(std::vector<Change>{});
        }
        // both in one run, split by a marker line
        const std::string range = quote(from) + " " + quote(to);
        const std::string out = run(host, from.empty() ? "cd " + quote(cwd) + " && git --no-optional-locks status --porcelain=v1 -uall && echo __imcode__ && "
                                                                       "git --no-optional-locks diff HEAD --numstat 2>/dev/null"
                                                     : "cd " + quote(cwd) + " && git diff --name-status " + range +
                                                           " && echo __imcode__ && git diff --numstat " + range);
        constexpr std::string_view marker = "__imcode__\n";
        const size_t mark = out.find(marker);
        if (mark == std::string::npos) {
            return {};
        }
        const std::string_view status = std::string_view(out).substr(0, mark);
        return parseChanges(from.empty() ? std::string(status) : nameStatusToPorcelain(status),
                            std::string_view(out).substr(mark + marker.size()));
    }

    std::vector<LogEntry> parseLog(std::string_view text) {
        std::vector<LogEntry> out;
        const auto split = [](std::string_view s, char sep) {
            std::vector<std::string> parts;
            for (size_t at = 0;;) {
                const size_t end = s.find(sep, at);
                parts.emplace_back(s.substr(at, end == std::string_view::npos ? end : end - at));
                if (end == std::string_view::npos) {
                    return parts;
                }
                at = end + 1;
            }
        };
        for (const auto& record : split(text, '\x1e')) {
            const auto f = split(record, '\x1f');
            if (f.size() < 6) {
                continue;
            }
            LogEntry e;
            e.hash = f[0];
            e.hash.erase(0, e.hash.find_first_not_of('\n')); // git ends each record with a newline
            e.author = f[2];
            e.time = std::atoll(f[3].c_str());
            e.subject = f[5];
            for (auto& p : split(f[1], ' ')) {
                if (!p.empty()) {
                    e.parents.push_back(std::move(p));
                }
            }
            for (auto& r : split(f[4], ',')) {
                r.erase(0, r.find_first_not_of(' '));
                if (!r.empty()) {
                    e.refs.push_back(std::move(r));
                }
            }
            if (!e.hash.empty()) {
                out.push_back(std::move(e));
            }
        }
        return out;
    }

    std::vector<LogEntry> log(const std::string& host, const std::string& cwd, size_t limit, size_t skip,
                              const std::string& path) {
        trace::Span traced_("git.log", host + " " + cwd, 0);
        if (!path.empty()) {
            return parseLog(run(host, "cd " + quote(cwd) + " && git log --follow --skip=" + std::to_string(skip) + " -n " +
                                          std::to_string(limit) + " --format='%H%x1f%P%x1f%an%x1f%at%x1f%D%x1f%s%x1e' -- " +
                                          quote(path) + " 2>/dev/null"));
        }
        if (host.empty()) {
            return Local::log(cwd, limit, skip).value_or(std::vector<LogEntry>{});
        }
        return parseLog(run(host, "cd " + quote(cwd) + " && git log --all --topo-order --skip=" + std::to_string(skip) +
                                      " -n " + std::to_string(limit) +
                                      " --format='%H%x1f%P%x1f%an%x1f%at%x1f%D%x1f%s%x1e' 2>/dev/null"));
    }

    CommitDetail parseShow(std::string_view text) {
        std::vector<std::string> f;
        for (size_t at = 0; f.size() < 8; ) {
            const size_t end = text.find('\x1f', at);
            if (end == std::string_view::npos) {
                return {};
            }
            f.emplace_back(text.substr(at, end - at));
            at = end + 1;
            if (f.size() == 8) {
                text = text.substr(at);
            }
        }
        CommitDetail out;
        out.hash = f[0];
        out.hash.erase(0, out.hash.find_first_not_of('\n'));
        std::string parents = f[1];
        for (size_t at = 0; at < parents.size();) {
            const size_t end = std::min(parents.find(' ', at), parents.size());
            if (end > at) {
                out.parents.push_back(parents.substr(at, end - at));
            }
            at = end + 1;
        }
        out.author = f[2];
        out.authorEmail = f[3];
        out.committer = f[4];
        out.committerEmail = f[5];
        out.time = std::atoll(f[6].c_str());
        out.committed = std::atoll(f[7].c_str());
        out.message = std::string(text);
        while (!out.message.empty() && std::isspace(static_cast<unsigned char>(out.message.back()))) {
            out.message.pop_back(); // the format ends it with a line break of its own
        }
        return out;
    }

    CommitDetail show(const std::string& host, const std::string& cwd, const std::string& hash) {
        trace::Span traced_("git.show", host + " " + cwd, 0);
        CommitDetail out;
        if (host.empty()) {
            out = Local::show(cwd, hash).value_or(CommitDetail{});
        } else {
            out = parseShow(run(host, "cd " + quote(cwd) + " && git show -s --format='%H%x1f%P%x1f%an%x1f%ae%x1f%cn%x1f%ce%x1f%at%x1f%ct%x1f%B' " +
                                          quote(hash) + " -- 2>/dev/null"));
        }
        if (out.hash.empty()) {
            return out;
        }
        // against the first parent; a first commit against the tree of no files
        out.files = changes(host, cwd, out.parents.empty() ? "4b825dc642cb6eb9a060e54bf8d69288fbee4904" : out.parents[0], out.hash);
        return out;
    }

    std::string snapshot(const std::string& host, const std::string& cwd) {
        trace::Span traced_("git.snapshot", host + " " + cwd, 0);
        if (host.empty()) {
            return Local::snapshot(cwd).value_or("");
        }
        // a copy of the index takes every file, then becomes a tree; the real index is untouched
        std::string out = run(host, "cd " + quote(cwd) + " && idx=$(git rev-parse --git-path index) && t=$(mktemp) && "
                                    "{ cp \"$idx\" \"$t\" 2>/dev/null; GIT_INDEX_FILE=\"$t\" git add -A >/dev/null 2>&1 && "
                                    "GIT_INDEX_FILE=\"$t\" git write-tree; rm -f \"$t\"; }");
        while (!out.empty() && out.back() == '\n') {
            out.pop_back();
        }
        out = out.substr(out.find_last_of('\n') + 1);
        const bool hash = out.size() >= 40 && std::ranges::all_of(out, [](char c) { return std::isxdigit(static_cast<unsigned char>(c)); });
        return hash ? out : "";
    }

    std::vector<Diff::Line> fileDiff(const std::string& host, const std::string& cwd, const Change& change,
                                     const std::string& from, const std::string& to) {
        if (host.empty()) {
            return parseDiff(Local::fileDiff(cwd, change, from, to).value_or(""));
        }
        // shortcut: one file's whole diff in memory; a huge one is cut at 4000 lines
        const std::string script =
            !from.empty()       ? "cd " + quote(cwd) + " && git diff " + quote(from) + " " + quote(to) + " -- " + quote(change.path) + " | head -4000"
            : change.kind == '?' ? "cd " + quote(cwd) + " && git diff --no-index -- /dev/null " + quote(change.path) + " | head -4000"
                                 : "cd " + quote(cwd) + " && git diff HEAD -- " + quote(change.path) + " | head -4000";
        return parseDiff(run(host, script));
    }

    std::vector<Diff::Line> fileDiff(const std::string& host, const std::string& cwd, const Change& change, Side side) {
        trace::Span traced_("git.file-diff", host + " " + cwd, 0);
        if (host.empty()) {
            return parseDiff(Local::fileDiff(cwd, change, side).value_or(""));
        }
        const std::string at = "cd " + quote(cwd) + " && git diff ";
        const std::string path = " -- " + pathspec(change.path) + " | head -4000";
        if (side == Side::Staged) {
            return parseDiff(run(host, at + "--cached" + path));
        }
        if (side == Side::Unstaged && change.work == '?') {
            return parseDiff(run(host, "cd " + quote(cwd) + " && git diff --no-index -- /dev/null " + quote(change.path) + " | head -4000"));
        }
        return parseDiff(run(host, at + (side == Side::All ? "HEAD" : "") + path));
    }

    namespace {
        std::string pathList(const std::vector<Change>& files) {
            std::string out;
            for (const Change& f : files) {
                out += " " + pathspec(f.path);
                if (!f.from.empty()) {
                    out += " " + pathspec(f.from);
                }
            }
            return out;
        }

        bool operate(const std::string& host, const std::string& cwd, const std::vector<Change>& files, std::string& error,
                     const std::string& script, bool (*local)(const std::string&, const std::vector<Change>&, std::string&)) {
            if (files.empty()) {
                return true;
            }
            if (host.empty()) {
                return local(cwd, files, error);
            }
            constexpr std::string_view ok = "__imcode_ok__";
            std::string out = run(host, "cd " + quote(cwd) + " && " + script + " 2>&1 && echo " + std::string(ok));
            while (!out.empty() && out.back() == '\n') {
                out.pop_back();
            }
            if (out.ends_with(ok)) {
                return true;
            }
            error = out.empty() ? "git did not answer" : out;
            return false;
        }
    } // namespace

    std::string stageScript(const std::vector<Change>& files) {
        return "git add -A --" + pathList(files); // a file that is gone is removed from the index
    }
    std::string unstageScript(const std::vector<Change>& files) {
        // before the first commit there is no HEAD to reset to: the entries just go
        return "{ git rev-parse -q --verify HEAD >/dev/null && git reset -q --" + pathList(files) + " || git rm -q -r --cached --ignore-unmatch --" +
               pathList(files) + "; }";
    }
    std::string discardScript(const std::vector<Change>& files) {
        std::vector<Change> tracked, fresh;
        for (const Change& f : files) {
            (f.work == '?' ? fresh : tracked).push_back(f);
        }
        std::string out = "true";
        if (!tracked.empty()) {
            out += " && git checkout --" + pathList(tracked);
        }
        if (!fresh.empty()) {
            out += " && git clean -f -q --" + pathList(fresh);
        }
        return out;
    }

    bool stage(const std::string& host, const std::string& cwd, const std::vector<Change>& files, std::string& error) {
        return operate(host, cwd, files, error, stageScript(files), Local::stage);
    }
    bool unstage(const std::string& host, const std::string& cwd, const std::vector<Change>& files, std::string& error) {
        return operate(host, cwd, files, error, unstageScript(files), Local::unstage);
    }
    bool discard(const std::string& host, const std::string& cwd, const std::vector<Change>& files, std::string& error) {
        return operate(host, cwd, files, error, discardScript(files), Local::discard);
    }

    std::string hunkPatch(std::string_view unified, int n, const std::vector<Diff::Line>& seen) {
        std::string header;
        std::vector<std::string> hunks;
        for (size_t at = 0; at < unified.size();) {
            const size_t end = std::min(unified.find('\n', at), unified.size());
            const std::string line(unified.substr(at, end - at));
            at = end + 1;
            if (line.starts_with("@@")) {
                hunks.push_back(line + "\n");
            } else if (hunks.empty()) {
                header += line + "\n";
            } else {
                hunks.back() += line + "\n";
            }
        }
        if (n < 0 || n >= static_cast<int>(hunks.size())) {
            return "";
        }
        // the hunk's lines against what was shown ("\ No newline" notes are not lines)
        std::vector<Diff::Line> now = parseDiff(header + hunks[static_cast<size_t>(n)]);
        if (now.size() != seen.size()) {
            return "";
        }
        for (size_t i = 0; i < now.size(); ++i) {
            if (now[i].kind != seen[i].kind || now[i].text != seen[i].text) {
                return "";
            }
        }
        return header + hunks[static_cast<size_t>(n)];
    }

    bool applyHunk(const std::string& host, const std::string& cwd, const Change& change, HunkAction action, int hunk,
                   const std::vector<Diff::Line>& seen, std::string& error) {
        const bool fromIndex = action == HunkAction::Unstage; // the staged diff; else the folder's against the index
        std::string unified;
        if (host.empty()) {
            unified = Local::fileDiff(cwd, change, fromIndex ? Side::Staged : Side::Unstaged).value_or("");
        } else {
            unified = run(host, "cd " + quote(cwd) + " && git diff " + (fromIndex ? "--cached " : "") + "-- " + pathspec(change.path));
        }
        const std::string patch = hunkPatch(unified, hunk, seen);
        if (patch.empty()) {
            error = "The file changed since it was shown: read the diff again.";
            return false;
        }
        // staging and unstaging touch the index; discarding takes the hunk out of the folder
        const std::string flags = action == HunkAction::Stage ? "--cached" : action == HunkAction::Unstage ? "--cached -R" : "-R";
        constexpr std::string_view ok = "__imcode_ok__";
        std::string out = run(host, "cd " + quote(cwd) + " && git apply " + flags + " - 2>&1 <<'IMCODE_PATCH' && echo " + std::string(ok) +
                                        "\n" + patch + "IMCODE_PATCH\n");
        while (!out.empty() && out.back() == '\n') {
            out.pop_back();
        }
        if (out.ends_with(ok)) {
            return true;
        }
        error = out.empty() ? "git did not answer" : out;
        return false;
    }

    bool commit(const std::string& host, const std::string& cwd, const std::string& message, bool push,
                std::string& output, bool stagedOnly) {
        // the git command commits, so the repository's hooks and signing run (libgit2 runs neither)
        // and the push has the user's credentials (helpers, ssh agent). a machine with no git
        // command falls back to libgit2
        const std::string commitScript = "cd " + quote(cwd) + (stagedOnly ? "" : " && git add -A") + " && git commit -q -m " +
                                         quote(message) + " 2>&1";
        const std::string pushScript = " && { git rev-parse --abbrev-ref --symbolic-full-name '@{u}' >/dev/null 2>&1 && git push -q 2>&1 || "
                                       "git push -q -u origin HEAD 2>&1; }"; // the first push of a branch sets where it goes
        constexpr std::string_view ok = "__imcode_ok__";
        // a hook or a signing prompt can take a while
        output = run(host, commitScript + (push ? pushScript : "") + " && echo " + std::string(ok), std::chrono::minutes(5));
        while (!output.empty() && output.back() == '\n') {
            output.pop_back();
        }
        if (output.ends_with(ok)) {
            output.resize(output.size() - ok.size());
            return true;
        }
        const bool noGit = output.find("git: command not found") != std::string::npos || output.find("git: not found") != std::string::npos ||
                           output.find("xcode-select") != std::string::npos;
        if (host.empty() && noGit) {
            if (!Local::commit(cwd, message, output, !stagedOnly)) {
                return false;
            }
            if (!push) {
                return true;
            }
            output = run(host, "cd " + quote(cwd) + pushScript + " && echo " + std::string(ok));
            while (!output.empty() && output.back() == '\n') {
                output.pop_back();
            }
            if (output.ends_with(ok)) {
                output.resize(output.size() - ok.size());
                return true;
            }
        }
        if (output.empty()) {
            output = "Git did not finish (a hook or a signing prompt may be waiting).";
        }
        return false;
    }

    std::string pullRequest(const std::string& host, const std::string& cwd, std::string& output) {
        output = run(host, "cd " + quote(cwd) + " && gh pr create --fill 2>&1");
        // gh prints the request's address last
        std::string last = output;
        while (!last.empty() && last.back() == '\n') {
            last.pop_back();
        }
        last = last.substr(last.find_last_of('\n') + 1);
        return last.starts_with("https://") ? last : "";
    }

    std::string addWorktree(const std::string& host, const std::string& repo, const std::string& name,
                            std::string& error) {
        trace::Span traced_("git.add-worktree", host + " " + repo, 0);
        const std::string repoName = repo.substr(repo.find_last_of('/') + 1);
        if (host.empty()) {
            const char* home = homeEnv();
            // the local name by path rules: a windows path splits at backslashes
            const auto folder = std::filesystem::path(home ? home : "") / ".imcode" / "worktrees" /
                                std::filesystem::path(repo).filename() / name;
            return Local::addWorktree(repo, folder.string(), "imcode/" + name, error);
        }
        // one script for both: it prints the new folder on its last line
        const std::string script = "dir=\"$HOME/.imcode/worktrees/\"" + quote(repoName) + " && mkdir -p \"$dir\" && git -C " +
                                   quote(repo) + " worktree add -q -b " + quote("imcode/" + name) + " \"$dir\"/" +
                                   quote(name) + " 2>&1 && printf '%s\\n' \"$dir\"/" + quote(name);
        std::string out = run(host, script);
        while (!out.empty() && out.back() == '\n') {
            out.pop_back();
        }
        const std::string last = out.substr(out.find_last_of('\n') + 1);
        if (!last.starts_with("/") || !last.ends_with("/" + name)) {
            error = "Could not make a worktree: " + out;
            return "";
        }
        return last;
    }

    bool removeWorktree(const std::string& host, const std::string& repo, const std::string& path,
                        std::string& error) {
        if (host.empty()) {
            return Local::removeWorktree(repo, path, error);
        }
        std::string out =
            run(host, "git -C " + quote(repo) + " worktree remove " + quote(path) + " 2>&1 && echo imcode-removed");
        while (!out.empty() && out.back() == '\n') {
            out.pop_back();
        }
        if (out.ends_with("imcode-removed")) {
            return true;
        }
        error = out;
        return false;
    }

} // namespace Git
