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

    std::string addWorktree(const std::string& host, const std::string& repo, const std::string& name,
                            std::string& error) {
        auto quote = [](const std::string& s) {
            std::string out = "'";
            for (const char c : s) {
                out += c == '\'' ? std::string("'\\''") : std::string(1, c);
            }
            return out + "'";
        };
        const std::string repoName = repo.substr(repo.find_last_of('/') + 1);
        // one script for both: it prints the new folder on its last line
        const std::string script = "dir=\"$HOME/.imcode/worktrees/\"" + quote(repoName) + " && mkdir -p \"$dir\" && git -C " +
                                   quote(repo) + " worktree add -q -b " + quote("imcode/" + name) + " \"$dir\"/" +
                                   quote(name) + " 2>&1 && printf '%s\\n' \"$dir\"/" + quote(name);
        std::string out;
        if (host.empty()) {
            const ProcessResult res =
                ProcessRunner::run({.args = {"/bin/sh", "-c", script},
                                    .environment = {{"PATH", AcpAgents::loginShellPath()}},
                                    .timeout = std::chrono::seconds(60)});
            out = res.output;
        } else {
            Ssh::Error sshError;
            const auto session = Ssh::session(host, "", sshError);
            std::string failed;
            auto lines = session ? session->exec(script, nullptr, failed) : nullptr;
            for (std::string line; lines && lines->readLine(line);) {
                out += line + "\n";
            }
            if (!lines) {
                out = sshError.message + failed;
            }
        }
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

} // namespace Git
