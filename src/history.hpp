#pragma once

#include <filesystem>
#include <functional>
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

    // the same for a host reached over ssh (one shell command, run there). empty when it
    // cannot be reached or has none
    std::vector<Found> scanRemote(const std::string& host, int perFolder = 5);
    // what the shell command of scanRemote printed: "@@imcode<TAB>mtime<TAB>path" before
    // each file's first lines. the folders are not checked
    std::vector<Found> parseRemote(const std::string& output, int perFolder = 5);

    // a conversation as its agent's cli writes it on this machine, read as it grows: another
    // client (a terminal) can be followed live, read only
    struct Entry {
        enum class Kind { User, Assistant, Tool };
        Kind kind;
        std::string text; // a message's text, or a tool call's title
    };
    // the session's transcript file on this machine; empty when there is none
    std::string transcript(const std::string& agentId, const std::string& cwd, const std::string& sessionId);
    // the entries in the complete lines from `offset`, which moves past them; at most maxRead
    // bytes a call (a line longer than that is skipped)
    // the entries in transcript text (whole lines)
    std::vector<Entry> parse(const std::string& agentId, const std::string& lines);
    // a remote session's transcript end, kept on this machine (the same text as the file there) so a thread
    // opens at once and is brought up to date over ssh afterwards. shortcut: Claude Code only, Codex
    // rollouts sit under dated folders
    struct Tail {
        std::string text;      // whole lines: the end of the file
        long long size = -1;   // the offset in the remote file where `text` ends
        std::string delta;     // refresh: what was appended to the cached text (empty when `replaced`)
        bool replaced = false; // refresh: `text` is a fresh read, not the cache plus `delta`
        bool ok = false;       // there is a cache entry / the host answered
        long long ageSeconds = -1;
    };
    void setCacheDir(const std::filesystem::path& dir); // unset: nothing is cached
    void clearCache();                                   // deletes what was kept
    void setRunner(std::function<std::string(const std::string& host, const std::string& script)> run); // tests: no ssh
    Tail cachedTail(const std::string& host, const std::string& agentId, const std::string& cwd, const std::string& sessionId);
    // asks the host for the file's size, then only the bytes after `have.size` (or the last `bytes` when it shrank
    // or grew past that), and writes the cache (50 MB in all, least recently used out). blocks
    Tail refreshTail(const std::string& host, const std::string& agentId, const std::string& cwd, const std::string& sessionId,
                     const Tail& have, size_t bytes, bool background = false);
    // the same later, one at a time on a thread of its own, after any fetch the user waits for
    void prefetch(const std::string& host, const std::string& agentId, const std::string& cwd, const std::string& sessionId);
    inline constexpr size_t maxRead = 2 * 1024 * 1024;
    std::vector<Entry> readFrom(const std::string& agentId, const std::string& path, size_t& offset);
    // seconds since the file was last written; a large number when it is not there
    long long quietFor(const std::string& path);
} // namespace History
