#include "git.hpp"

#include "acp_host/agents.hpp"
#include "acp_host/process_runner.hpp"
#include "ssh.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>

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
            const auto git = AcpAgents::locateExecutable("git");
            if (!git) {
                return {};
            }
            const ProcessResult res = ProcessRunner::run(
                {.args = {*git, "-C", cwd, "status", "--porcelain=v1", "-b"},
                 .timeout = std::chrono::seconds(10)});
            return res.success ? parse(res.output) : Status{};
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

    std::vector<Change> changes(const std::string& host, const std::string& cwd) {
        // both in one run, split by a marker line
        const std::string out = run(host, "cd " + quote(cwd) + " && git status --porcelain=v1 -uall && echo __imcode__ && "
                                          "git diff HEAD --numstat 2>/dev/null");
        const size_t mark = out.find("__imcode__\n");
        if (mark == std::string::npos) {
            return {};
        }
        return parseChanges(std::string_view(out).substr(0, mark), std::string_view(out).substr(mark + 10));
    }

    std::vector<Diff::Line> fileDiff(const std::string& host, const std::string& cwd, const Change& change) {
        // shortcut: one file's whole diff in memory; a huge one is cut at 4000 lines
        const std::string script =
            change.kind == '?' ? "cd " + quote(cwd) + " && git diff --no-index -- /dev/null " + quote(change.path) + " | head -4000"
                               : "cd " + quote(cwd) + " && git diff HEAD -- " + quote(change.path) + " | head -4000";
        return parseDiff(run(host, script));
    }

    std::string addWorktree(const std::string& host, const std::string& repo, const std::string& name,
                            std::string& error) {
        const std::string repoName = repo.substr(repo.find_last_of('/') + 1);
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
