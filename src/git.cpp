#include "git.hpp"

#include "acp_host/agents.hpp"
#include "acp_host/process_runner.hpp"
#include "git_local.hpp"
#include "ssh.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>

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
        auto lines = session ? session->exec("cd " + quoted + "' && git status --porcelain=v1 -b", nullptr, error)
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

        // a shell script on this machine or on the host, its output (both streams)
        std::string run(const std::string& host, const std::string& script) {
            if (host.empty()) {
                return ProcessRunner::run({.args = {"/bin/sh", "-c", script},
                                           .environment = {{"PATH", AcpAgents::loginShellPath()}},
                                           .timeout = std::chrono::seconds(60)})
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
            std::string_view path = line.substr(3);
            if (const size_t arrow = path.find(" -> "); arrow != std::string_view::npos) {
                path = path.substr(arrow + 4); // a rename: its new name
            }
            if (path.size() >= 2 && path.front() == '"' && path.back() == '"') {
                path = path.substr(1, path.size() - 2); // quoted by git (spaces and such)
            }
            c.path = std::string(path);
            out.push_back(std::move(c));
        });
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
            for (Change& c : out) {
                if (c.path == path && line[0] != '-') {
                    c.added = std::atoi(std::string(line.substr(0, t1)).c_str());
                    c.removed = std::atoi(std::string(line.substr(t1 + 1, t2 - t1 - 1)).c_str());
                }
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
        if (host.empty()) {
            return Local::changes(cwd, from, to).value_or(std::vector<Change>{});
        }
        // both in one run, split by a marker line
        const std::string range = quote(from) + " " + quote(to);
        const std::string out = run(host, from.empty() ? "cd " + quote(cwd) + " && git status --porcelain=v1 -uall && echo __imcode__ && "
                                                                       "git diff HEAD --numstat 2>/dev/null"
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

    std::string snapshot(const std::string& host, const std::string& cwd) {
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

    bool commit(const std::string& host, const std::string& cwd, const std::string& message, bool push,
                std::string& output) {
        // here: committed through libgit2; the push stays on the git command, which has the
        // user's credentials (helpers, ssh agent)
        std::string script = "cd " + quote(cwd);
        if (host.empty()) {
            if (!Local::commit(cwd, message, output)) {
                return false;
            }
            if (!push) {
                return true;
            }
        } else {
            script += " && git add -A && git commit -q -m " + quote(message) + " 2>&1";
        }
        if (push) {
            // the first push of a branch sets where it goes
            script += " && { git rev-parse --abbrev-ref --symbolic-full-name '@{u}' >/dev/null 2>&1 && git push -q 2>&1 || "
                      "git push -q -u origin HEAD 2>&1; }";
        }
        constexpr std::string_view ok = "__imcode_ok__";
        output = run(host, script + " && echo " + std::string(ok));
        while (!output.empty() && output.back() == '\n') {
            output.pop_back();
        }
        if (output.ends_with(ok)) {
            output.resize(output.size() - ok.size());
            return true;
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
        const std::string repoName = repo.substr(repo.find_last_of('/') + 1);
        if (host.empty()) {
            const char* home = std::getenv("HOME");
            const std::string folder = std::string(home ? home : "") + "/.imcode/worktrees/" + repoName + "/" + name;
            return Local::addWorktree(repo, folder, "imcode/" + name, error);
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
