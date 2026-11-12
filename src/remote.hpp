#pragma once

#include "acp_host/agents.hpp"
#include <string>

namespace Remote {

    // what the remote shell is asked to run: the agent in `dir`, with the PATH of the
    // remote login shell. empty when the agent has neither an npm package nor a command
    // (registry-only binaries)
    std::string agentCommand(const AcpAgentDef& def, const std::string& dir);

    // sh: when the machine has no bun, download the app's pinned one into ~/.imcode/bun
    // (checked against the release's checksums). quiet on stdout
    std::string bunSetup();

    // where an attached file goes on the host: a stamp keeps same-named files apart
    std::string uploadPath(const std::string& name, long long stamp);

} // namespace Remote
