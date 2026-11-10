// the two session formats, read from a fake home
#include "../src/history.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

int main() {
    namespace fs = std::filesystem;
    const fs::path home = fs::temp_directory_path() / "imcode-history-test";
    fs::remove_all(home);
    const fs::path work = home / "work";
    fs::create_directories(work);
    fs::create_directories(home / ".claude/projects/-x/abc/subagents");
    fs::create_directories(home / ".codex/sessions/2026/11/11");
    setenv("HOME", home.c_str(), 1);
    const std::string cwd = work.string();
    std::ofstream(home / ".claude/projects/-x/abc.jsonl")
        << R"({"type":"mode","sessionId":"abc"})" << "\n"
        << R"({"type":"user","cwd":")" << cwd << R"(","message":{"role":"user","content":"<command>x</command>"}})" << "\n"
        << R"({"type":"user","cwd":")" << cwd << R"(","message":{"role":"user","content":[{"type":"text","text":"fix the bug\nplease"}]}})" << "\n";
    std::ofstream(home / ".claude/projects/-x/abc/subagents/sub.jsonl")
        << R"({"type":"user","cwd":")" << cwd << R"(","message":{"role":"user","content":"a subagent"}})" << "\n";
    std::ofstream(home / ".codex/sessions/2026/11/11/rollout-1.jsonl")
        << R"({"type":"session_meta","payload":{"id":"cdx","cwd":")" << cwd << R"("}})" << "\n"
        << R"({"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"# AGENTS.md"}]}})" << "\n"
        << R"({"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"hello?"}]}})" << "\n";
    const auto found = History::scan();
    fs::remove_all(home);
    bool claude = false, codex = false;
    for (const auto& f : found) {
        claude |= f.agentId == "claude-code" && f.sessionId == "abc" && f.title == "fix the bug" &&
                  f.cwd == cwd && f.updatedAt > 0;
        codex |= f.agentId == "codex" && f.sessionId == "cdx" && f.title == "hello?" && f.cwd == cwd;
    }
    if (found.size() != 2 || !claude || !codex) {
        std::printf("FAILED: %zu found, claude=%d codex=%d\n", found.size(), claude, codex);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
