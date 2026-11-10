// manual check against a real ssh server:
//   imcode-ssh-check <host> <folder> <path of acp-echo-agent on the host>
// connects, lists the folder over sftp, then runs one turn of a remote thread
#include "../src/ssh.hpp"
#include "../src/thread.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

int main(int argc, char** argv) {
    if (argc != 4) {
        return 2;
    }
    Ssh::Error error;
    const auto session = Ssh::session(argv[1], "", error);
    if (!session) {
        std::printf("FAILED: connect: %s\n", error.message.c_str());
        return 1;
    }
    std::string err;
    const std::string dir = session->realpath(argv[2], err);
    const auto dirs = session->listDirs(dir, err);
    std::printf("folder %s: %zu folders %s\n", dir.c_str(), dirs.size(), err.c_str());
    for (const auto& d : dirs) {
        std::printf("  %s\n", d.c_str());
    }
    if (dir.empty() || !err.empty()) {
        return 1;
    }

    setenv("IMCODE_AGENT_CMD", argv[3], 1);
    Thread t(dir, "claude-code", argv[1]);
    t.send("over ssh");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    do {
        t.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (t.busy() && std::chrono::steady_clock::now() < deadline);
    t.poll();
    for (const auto& item : t.items) {
        std::printf("  item %d: %s\n", static_cast<int>(item.kind), item.text.c_str());
    }
    const bool ok = !t.busy() && t.items.size() == 2 && t.items[1].text == "over ssh ";
    std::printf(ok ? "ok\n" : "FAILED\n");
    return ok ? 0 : 1;
}
