#pragma once

#include <string>
#include <vector>

// conversations an agent's own cli left on this machine, to offer as projects and threads
namespace History {
    struct Found {
        std::string agentId, cwd, sessionId, title;
        long long updatedAt = 0;
    };
    // Claude Code: ~/.claude/projects/<folder>/<session>.jsonl; the newest few per folder,
    // only for folders that still exist
    std::vector<Found> claudeCode(int perFolder = 5);
} // namespace History
