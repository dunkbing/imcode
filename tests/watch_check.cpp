// manual check against a real ssh host:
//   imcode-watch-check <host> <folder on the host> <trace dir>
// installs the helper there, makes changes in the folder and prints the batches that come back
#include "../src/ssh.hpp"
#include "../src/trace.hpp"
#include "../src/watch.hpp"
#include <chrono>
#include <cstdio>
#include <thread>

namespace {
    void run(Ssh::Session& session, const std::string& command) {
        std::string failed;
        if (const auto channel = session.exec(command, nullptr, failed)) {
            std::string line;
            while (channel->readLine(line)) {
            }
        }
    }

    void show(const char* label, const FolderChanges& changes) {
        std::printf("%s: all=%d git=%d dirs=%zu files=%zu\n", label, changes.all, changes.git, changes.dirs.size(), changes.files.size());
        for (const auto& dir : changes.dirs) {
            std::printf("  d '%s'\n", dir.c_str());
        }
        for (const auto& file : changes.files) {
            std::printf("  f '%s'\n", file.c_str());
        }
    }

    // what arrives within `seconds`
    FolderChanges gather(FolderWatch& watch, const int seconds) {
        FolderChanges seen;
        for (int i = 0; i < seconds * 10; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            seen.merge(watch.take());
        }
        return seen;
    }
} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        return 2;
    }
    trace::open(argv[3]);
    Ssh::Error error;
    const auto session = Ssh::session(argv[1], "", error);
    if (!session) {
        std::printf("refused: %s\n", error.message.c_str());
        return 1;
    }
    const std::string dir = argv[2];
    run(*session, "mkdir -p " + dir + "/sub " + dir + "/.git/refs/heads " + dir + "/.git/objects; echo x > " + dir + "/a.txt");
    FolderWatch watch(argv[1], dir, [] {});
    for (int i = 0; i < 300 && !watch.live(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::printf("live: %d\n", watch.live());
    if (!watch.live()) {
        return 1;
    }
    show("start", gather(watch, 1));
    run(*session, "echo n > " + dir + "/sub/new.txt; mkdir -p " + dir + "/deep/er; echo y > " + dir + "/deep/er/f.txt; touch " + dir + "/.git/objects/junk");
    show("files", gather(watch, 2));
    run(*session, "touch " + dir + "/.git/HEAD");
    show("git", gather(watch, 2));
    return 0;
}
