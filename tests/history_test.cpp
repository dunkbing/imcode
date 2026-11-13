// the two session formats, read from a fake home
#include "../src/history.hpp"
#include <cctype>
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
    std::string cwdJson; // as json writes it: a windows path's backslashes escaped
    for (const char c : cwd) {
        cwdJson += c == '\\' ? std::string("\\\\") : std::string(1, c);
    }
    std::ofstream(home / ".claude/projects/-x/abc.jsonl")
        << R"({"type":"mode","sessionId":"abc"})" << "\n"
        << R"({"type":"user","cwd":")" << cwdJson << R"(","message":{"role":"user","content":"<command>x</command>"}})" << "\n"
        << R"({"type":"user","cwd":")" << cwdJson << R"(","message":{"role":"user","content":[{"type":"text","text":"fix the bug\nplease"}]}})" << "\n";
    std::ofstream(home / ".claude/projects/-x/abc/subagents/sub.jsonl")
        << R"({"type":"user","cwd":")" << cwdJson << R"(","message":{"role":"user","content":"a subagent"}})" << "\n";
    std::ofstream(home / ".codex/sessions/2026/11/11/rollout-1.jsonl")
        << R"({"type":"session_meta","payload":{"id":"cdx","cwd":")" << cwdJson << R"("}})" << "\n"
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
    // the same two formats as an ssh host prints them: newest first by time, a folder's sessions capped
    const std::string remote = std::string("@@imcode\t100\t/home/u/.claude/projects/-srv-app/old.jsonl\n") +
                               R"({"type":"user","cwd":"/srv/app","message":{"role":"user","content":"older ask"}})" "\n"
                               "@@imcode\t300\t/home/u/.codex/sessions/2026/11/11/rollout-1-cdx.jsonl\n"
                               R"({"type":"session_meta","payload":{"id":"cdx","cwd":"/srv/api"}})" "\n"
                               R"({"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"hello?"}]}})" "\n"
                               "@@imcode\t200\t/home/u/.claude/projects/-srv-app/new.jsonl\n"
                               "not json\n"
                               R"({"type":"user","cwd":"/srv/app","message":{"role":"user","content":"newer ask"}})" "\n";
    const auto fromHost = History::parseRemote(remote);
    if (fromHost.size() != 3 || fromHost[0].sessionId != "cdx" || fromHost[0].cwd != "/srv/api" ||
        fromHost[1].sessionId != "new" || fromHost[1].title != "newer ask" || fromHost[1].updatedAt != 200 ||
        fromHost[2].agentId != "claude-code" || History::parseRemote(remote, 1).size() != 2) {
        std::printf("FAILED: remote parse, %zu found\n", fromHost.size());
        return 1;
    }
    // a conversation read as its cli writes it: the cwd's folder named the way claude does
    std::string folder = cwd;
    for (char& c : folder) {
        c = std::isalnum(static_cast<unsigned char>(c)) ? c : '-';
    }
    fs::create_directories(home / ".claude/projects" / folder);
    const fs::path live = home / ".claude/projects" / folder / "s1.jsonl";
    std::ofstream(live) << R"({"type":"user","message":{"role":"user","content":"<system-reminder>x</system-reminder>"}})" << "\n"
                        << R"({"type":"user","message":{"role":"user","content":[{"type":"text","text":"rename it"}]}})" << "\n"
                        << R"({"type":"assistant","message":{"role":"assistant","content":[{"type":"text","text":"On it."},)"
                        << R"({"type":"tool_use","name":"Edit","input":{"file_path":"a.cpp"}}]}})" << "\n"
                        << R"({"type":"user","message":{"role":"user","content":[{"type":"tool_result","content":"ok"}]}})" << "\n"
                        << R"({"type":"assistant","message":{"role":"assistant","content":[{"type":"te)"; // half written
    const std::string path = History::transcript("claude-code", cwd, "s1");
    size_t offset = 0;
    auto entries = History::readFrom("claude-code", path, offset);
    bool ok = fs::path(path) == live && entries.size() == 3 && entries[0].text == "rename it" &&
              entries[1].kind == History::Entry::Kind::Assistant && entries[2].text == "Edit a.cpp";
    std::ofstream(live, std::ios::app) << R"(xt","text":"Done."}]}})" << "\n"; // the line completes
    entries = History::readFrom("claude-code", path, offset);
    ok = ok && entries.size() == 1 && entries[0].text == "Done." && History::quietFor(path) < 5 &&
         History::quietFor("") > 1000;
    // codex: found by its session id in the file name; injected instructions left out
    fs::create_directories(home / ".codex/sessions/2026/11/11");
    const fs::path rollout = home / ".codex/sessions/2026/11/11/rollout-2026-11-11T10-00-00-cdx2.jsonl";
    std::ofstream(rollout) << R"({"type":"session_meta","payload":{"id":"cdx2","cwd":"x"}})" << "\n"
                           << R"({"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"# AGENTS.md instructions"}]}})" << "\n"
                           << R"({"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"hi"}]}})" << "\n"
                           << R"({"type":"response_item","payload":{"type":"custom_tool_call","name":"exec","input":"ls -la\nmore"}})" << "\n"
                           << R"({"type":"response_item","payload":{"type":"message","role":"assistant","content":[{"type":"output_text","text":"Hello"}]}})" << "\n";
    size_t codexOffset = 0;
    const std::string codexPath = History::transcript("codex", "x", "cdx2");
    entries = History::readFrom("codex", codexPath, codexOffset);
    ok = ok && fs::path(codexPath) == rollout && entries.size() == 3 && entries[0].text == "hi" &&
         entries[1].text == "exec ls -la" && entries[2].text == "Hello";
    if (!ok) {
        std::printf("FAILED: transcripts (%zu entries) claude path '%s' codex path '%s'\n", entries.size(), path.c_str(),
                    codexPath.c_str());
        for (const auto& e : entries) {
            std::printf("  %d %s\n", static_cast<int>(e.kind), e.text.c_str());
        }
        return 1;
    }
    fs::remove_all(home);
    std::printf("ok\n");
    return 0;
}
