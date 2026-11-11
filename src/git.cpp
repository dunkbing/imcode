#include "git.hpp"

#include "acp_host/agents.hpp"
#include "acp_host/process_runner.hpp"
#include "ssh.hpp"
#include <algorithm>
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

} // namespace Git
