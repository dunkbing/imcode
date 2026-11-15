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
    // a remote transcript cached here: the first read fetches, the next asks only for what grew (a local shell
    // stands for the host, with a pause like a slow link)
    {
        const fs::path far = fs::temp_directory_path() / "imcode-history-far";
        const fs::path cache = fs::temp_directory_path() / "imcode-history-cache";
        fs::remove_all(far);
        fs::remove_all(cache);
        fs::create_directories(far / ".claude/projects/-srv-app");
        const fs::path file = far / ".claude/projects/-srv-app/s9.jsonl";
        const auto line = [](const char* who, const char* text) {
            return std::string(R"({"type":"user","message":{"role":"user","content":")") + text + R"("}})" + (who ? "\n" : "\n");
        };
        std::ofstream(file, std::ios::binary) << line("u", "first") << line("u", "second");
        int asked = 0;
        History::setCacheDir(cache);
        History::setRunner([&](const std::string&, const std::string& script) {
            ++asked;
            std::string quote = "'";
            for (const char c : script) {
                quote += c == '\'' ? std::string("'\\''") : std::string(1, c);
            }
            FILE* p = popen(("sleep 0.2; HOME='" + far.string() + "' sh -c " + quote + "' 2>/dev/null").c_str(), "r");
            std::string out, row;
            char buf[4096];
            while (p && fgets(buf, sizeof buf, p)) {
                row += buf;
                if (row.ends_with('\n')) { // as ssh's readLine gives it: a newline after every line
                    out += row;
                    row.clear();
                }
            }
            out += row.empty() ? "" : row + "\n";
            if (p) {
                pclose(p);
            }
            return out;
        });
        if (History::cachedTail("h", "claude-code", "/srv/app", "s9").ok) {
            std::printf("FAILED: a cache before any read\n");
            return 1;
        }
        const auto first = History::refreshTail("h", "claude-code", "/srv/app", "s9", {}, 4096);
        const auto hit = History::cachedTail("h", "claude-code", "/srv/app", "s9");
        if (!first.ok || !first.replaced || History::parse("claude-code", first.text).size() != 2 || !hit.ok || hit.text != first.text ||
            hit.size != first.size) {
            std::printf("FAILED: first read ok=%d replaced=%d cache=%d\n", first.ok, first.replaced, hit.ok);
            return 1;
        }
        std::ofstream(file, std::ios::binary | std::ios::app) << line("u", "third") << R"({"type":"user","mess)"; // and a line half written
        const auto more = History::refreshTail("h", "claude-code", "/srv/app", "s9", hit, 4096);
        const auto entries = History::parse("claude-code", more.delta);
        if (!more.ok || more.replaced || entries.size() != 1 || entries[0].text != "third" || more.text != hit.text + more.delta ||
            more.size != static_cast<long long>(more.text.size()) + (hit.size - static_cast<long long>(hit.text.size()))) {
            std::printf("FAILED: append replaced=%d delta=%zu entries=%zu size=%lld\n", more.replaced, more.delta.size(), entries.size(),
                        more.size);
            return 1;
        }
        // the unfinished line is finished: only that line comes
        std::ofstream(file, std::ios::binary | std::ios::trunc) << line("u", "first") << line("u", "second") << line("u", "third") << line("u", "fourth");
        const auto done = History::refreshTail("h", "claude-code", "/srv/app", "s9", more, 4096);
        if (done.replaced || History::parse("claude-code", done.delta).size() != 1) {
            std::printf("FAILED: finishing a line gave %zu bytes, replaced=%d\n", done.delta.size(), done.replaced);
            return 1;
        }
        // smaller than what was cached: read again from the end
        std::ofstream(file, std::ios::binary | std::ios::trunc) << line("u", "other");
        const auto fresh = History::refreshTail("h", "claude-code", "/srv/app", "s9", done, 4096);
        if (!fresh.replaced || History::parse("claude-code", fresh.text).size() != 1) {
            std::printf("FAILED: a shorter file was not replaced\n");
            return 1;
        }
        // the limit: old entries go first
        for (int i = 0; i < 3; ++i) {
            std::ofstream(cache / ("old" + std::to_string(i) + ".tail")) << std::string(20 * 1024 * 1024, 'x');
        }
        History::refreshTail("h", "claude-code", "/srv/app", "s9", {}, 4096);
        std::uintmax_t total = 0;
        for (const auto& e : fs::directory_iterator(cache)) {
            total += e.file_size();
        }
        if (total > 50ull * 1024 * 1024 || !History::cachedTail("h", "claude-code", "/srv/app", "s9").ok) {
            std::printf("FAILED: the cache holds %ju bytes\n", total);
            return 1;
        }
        History::setRunner(nullptr);
        History::setCacheDir({});
        fs::remove_all(far);
        fs::remove_all(cache);
    }
    // a conversation read as its cli writes it: the cwd's folder named the way claude does
    std::string folder = cwd;
    for (char& c : folder) {
        c = std::isalnum(static_cast<unsigned char>(c)) ? c : '-';
    }
    fs::create_directories(home / ".claude/projects" / folder);
    // a codex rollout is found under its dated folders, old or new, and what was found is kept
    fs::create_directories(home / ".codex/sessions/2025/01/02");
    fs::create_directories(home / ".codex/sessions/2026/11/11");
    std::ofstream(home / ".codex/sessions/2025/01/02/rollout-a-old1.jsonl") << "{}\n";
    std::ofstream(home / ".codex/sessions/2026/11/11/rollout-b-new1.jsonl") << "{}\n";
    const std::string oldPath = History::transcript("codex", cwd, "old1");
    const std::string newPath = History::transcript("codex", cwd, "new1");
    if (!oldPath.ends_with("2025/01/02/rollout-a-old1.jsonl") || !newPath.ends_with("2026/11/11/rollout-b-new1.jsonl") ||
        History::transcript("codex", cwd, "nope") != "" || History::transcript("codex", cwd, "old1") != oldPath) {
        std::printf("FAILED: codex lookup old=%s new=%s\n", oldPath.c_str(), newPath.c_str());
        return 1;
    }
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
    // a slash command is shown as typed; its output tag is left out
    std::ofstream(live, std::ios::app)
        << R"({"type":"user","message":{"role":"user","content":"<command-name>/exit</command-name>\n<command-message>exit</command-message>\n<command-args></command-args>"}})" << "\n"
        << R"({"type":"user","message":{"role":"user","content":"<local-command-stdout>Goodbye!</local-command-stdout>"}})" << "\n";
    entries = History::readFrom("claude-code", path, offset);
    ok = ok && entries.size() == 1 && entries[0].kind == History::Entry::Kind::User && entries[0].text == "/exit";
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
    // the end of a transcript read over ssh: the cut first line is skipped, a last line without its newline is read
    const auto tail = History::parse("claude-code", std::string(R"(sistant","content":"x"}})") + "\n" +
                                                        R"({"type":"user","message":{"role":"user","content":"after the cut"}})");
    ok = ok && tail.size() == 1 && tail[0].text == "after the cut";
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
