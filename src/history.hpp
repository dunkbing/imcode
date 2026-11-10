#pragma once

#include <string>
#include <vector>

// conversations the agents' own clis left on this machine, to offer as projects and threads
namespace History {
    struct Found {
        std::string agentId, cwd, sessionId, title;
        long long updatedAt = 0;
    };
    // the newest few sessions per folder and agent, only for folders that still exist:
    // Claude Code (~/.claude/projects/<folder>/<session>.jsonl) and
    // Codex (~/.codex/sessions/<y>/<m>/<d>/rollout-*.jsonl)
    std::vector<Found> scan(int perFolder = 5);
} // namespace History
