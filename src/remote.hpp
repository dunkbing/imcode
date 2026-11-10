#pragma once

#include "acp_host/agents.hpp"
#include <string>

namespace Remote {

    // what the remote shell is asked to run: the agent in `dir`, with the PATH of the
    // remote login shell. empty when the agent has neither an npm package nor a command
    // (registry-only binaries)
    std::string agentCommand(const AcpAgentDef& def, const std::string& dir);

} // namespace Remote
