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
    t.send("three"); // while the first turn starts: queued, sent when it ends
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
        t.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while ((t.busy() || !t.queue.empty()) && std::chrono::steady_clock::now() < deadline);
    t.poll();

    const bool ok = !t.busy() && t.title == "one two" && t.items.size() == 4 && t.items[2].text == "three" && t.items[3].text == "three " &&
                    t.items[0].kind == Item::Kind::User && t.items[0].text == "one two" &&
                    t.items[1].kind == Item::Kind::Assistant && t.items[1].text == "one two ";
    // send now: the running turn is stopped and the picked message goes before the other
    Thread u(".", "claude-code");
    u.send("a b c d e f g h i j k l m n o p q r s t u v w x y z a b c d e f g h i j k l m n o p q r s t u v w x y z");
    const auto wait = [&](auto done) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done() && std::chrono::steady_clock::now() < until) {
            u.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };
    wait([&] { return u.items.size() >= 2; }); // the reply has begun
    u.send("first");
    u.send("second");
    u.sendNow(1);
    wait([&] { return !u.busy() && u.queue.empty(); });
    u.poll();
    const bool sentNow = u.items.size() == 6 && u.items[1].text.size() < 100 && // the long reply was cut short
                         u.items[2].text == "second" && u.items[4].text == "first";
    if (!sentNow) {
        std::printf("FAILED: send now, items=%zu\n", u.items.size());
        return 1;
    }
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
    // remembered choices: applied when offered and different, dropped when not offered, kept
    // while their option has not arrived
    std::map<std::string, std::string> wanted{{"model", "big"}, {"mode", "gone"}, {"effort", "high"}, {"same", "a"}};
    wanted["mode"] = "bypassPermissions"; // remembered by an older build
    std::vector<acp::ConfigOption> offered(3);
    offered[0].id = "model";
    offered[0].currentValue = "small";
    offered[0].options = {{.value = "small"}, {.value = "big"}};
    offered[1].id = "mode";
    offered[1].currentValue = "ask";
    offered[1].category = "mode";
    offered[1].options = {{.value = "ask"}, {.value = "bypassPermissions"}};
    offered[2].id = "same";
    offered[2].currentValue = "a";
    offered[2].options = {{.value = "a"}};
    const auto due = dueChoices(offered, wanted);
    const bool remembered = due == std::vector<std::pair<std::string, std::string>>{{"model", "big"}} &&
                            wanted == std::map<std::string, std::string>{{"effort", "high"}};
    if (!remembered) {
        std::printf("FAILED: remembered choices: %zu due, %zu left\n", due.size(), wanted.size());
        return 1;
    }
    // a mode that allows everything stays with its thread
    Thread v(".", "claude-code");
    v.config.resize(1);
    v.config[0].id = "mode";
    v.config[0].category = "mode";
    v.setConfig("mode", "bypassPermissions");
    v.setConfig("model", "big");
    if (v.picked != std::map<std::string, std::string>{{"model", "big"}} || !riskyMode("Full access") || riskyMode("Auto")) {
        std::printf("FAILED: risky mode remembered\n");
        return 1;
    }
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
