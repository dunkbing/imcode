// a Thread against acp-cpp's echo agent, without a window: one turn fills the transcript
#include "../src/store.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

int main(int argc, char** argv) {
    if (argc != 2) {
        return 2; // path of acp-echo-agent
    }
    setenv("IMCODE_AGENT_CMD", argv[1], 1);
    Thread t(".", "claude-code");
    t.send("one two");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
        t.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (t.busy() && std::chrono::steady_clock::now() < deadline);
    t.poll();

    const bool ok = !t.busy() && t.title == "one two" && t.items.size() == 2 &&
                    t.items[0].kind == Item::Kind::User && t.items[0].text == "one two" &&
                    t.items[1].kind == Item::Kind::Assistant && t.items[1].text == "one two ";
    // the list survives a save and a load; the echo agent gave the session an id
    const auto file = std::filesystem::temp_directory_path() / "imcode-threads-test.json";
    Store::Data saved;
    saved.projects.push_back({"", "/empty"});
    saved.threads.push_back(std::make_unique<Thread>("/x", "codex", "me@box"));
    saved.threads.back()->title = "t";
    saved.threads.back()->sessionId = t.sessionId;
    Store::save(file, saved);
    const Store::Data loaded = Store::load(file);
    std::filesystem::remove(file);
    const bool stored =
        !t.sessionId.empty() && loaded.threads.size() == 1 && loaded.threads[0]->cwd == "/x" &&
        loaded.threads[0]->agentId == "codex" && loaded.threads[0]->host == "me@box" &&
        loaded.threads[0]->title == "t" && loaded.threads[0]->sessionId == t.sessionId &&
        loaded.projects == std::vector<Store::Project>{{"", "/empty"}, {"me@box", "/x"}};
    if (!ok || !stored) {
        std::printf("FAILED: busy=%d items=%zu stored=%d\n", t.busy(), t.items.size(), stored);
        for (const auto& item : t.items) {
            std::printf("  %d: %s\n", static_cast<int>(item.kind), item.text.c_str());
        }
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
