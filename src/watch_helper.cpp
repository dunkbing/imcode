// imcode-helper watch <folder>: prints what changes under a folder, for ImCode on another
// machine (uploaded over ssh, started in an exec channel). exits when its input closes
#include "watch.hpp"

#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 3 || std::string(argv[1]) != "watch") {
        std::fprintf(stderr, "usage: imcode-helper watch <folder>\n");
        return 64;
    }
    std::mutex mutex;
    std::condition_variable woken;
    bool dirty = false;
    Watch watch(argv[2], [&] {
        {
            const std::lock_guard lock(mutex);
            dirty = true;
        }
        woken.notify_one();
    });
    if (!watch.ok()) {
        std::fprintf(stderr, "cannot watch %s\n", argv[2]);
        return 2;
    }
    std::printf("imcode-helper 1 %s\n", helperPlatform().c_str());
    std::fflush(stdout);
    // the client closing the channel closes this input: nothing is read from it otherwise
    std::thread([] {
        char buffer[256];
        while (read(STDIN_FILENO, buffer, sizeof buffer) > 0) {
        }
        _exit(0);
    }).detach();
    for (;;) {
        {
            std::unique_lock lock(mutex);
            woken.wait(lock, [&] { return dirty; });
            dirty = false;
        }
        const std::string batch = encode(watch.take());
        if (batch.empty()) {
            continue;
        }
        const std::string out = batch + ".\n";
        if (std::fwrite(out.data(), 1, out.size(), stdout) != out.size() || std::fflush(stdout) != 0) {
            return 0;
        }
    }
}
