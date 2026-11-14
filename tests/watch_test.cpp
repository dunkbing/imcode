// the helper's line protocol, and (macos) the watcher seeing a folder change
#include "watch.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;

int main() {
    FolderChanges sent;
    sent.dirs = {"", "src", "a b"};
    sent.files = {"src/x.cpp"};
    sent.git = true;
    FolderChanges got;
    size_t at = 0;
    const std::string wire = encode(sent);
    for (size_t end; (end = wire.find('\n', at)) != std::string::npos; at = end + 1) {
        if (!decode(wire.substr(at, end - at), got)) {
            std::printf("FAILED: a line of our own output is refused: '%s'\n", wire.substr(at, end - at).c_str());
            return 1;
        }
    }
    bool ok = got.dirs == sent.dirs && got.files == sent.files && got.git && !got.all;
    // from a host: nothing outside the folder
    FolderChanges bad;
    ok = ok && !decode("d ../etc", bad) && !decode("f /etc/passwd", bad) && !decode("f a/../../b", bad) && !decode("x y", bad) && !bad.any();
    if (!ok) {
        std::printf("FAILED: protocol\n");
        return 1;
    }
#if !defined(__APPLE__) && !defined(_WIN32)
    std::printf("ok (linux watches through the helper, not in process)\n");
    return 0;
#endif
    const fs::path root = fs::temp_directory_path() / "imcode-watch-test";
    fs::remove_all(root);
    fs::create_directories(root / "sub");
    Watch watch(root.string(), [] {});
    if (!watch.ok()) {
        std::printf("FAILED: no watch\n");
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    std::ofstream(root / "sub" / "new.txt") << "x";
    fs::create_directories(root / ".git" / "objects");
    std::ofstream(root / ".git" / "objects" / "junk") << "x";
    std::ofstream(root / ".git" / "HEAD") << "x";
    FolderChanges seen;
    for (int i = 0; i < 40 && !(seen.files.contains("sub/new.txt") && seen.git); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        seen.merge(watch.take());
    }
    // a created file: its folder's entries changed; git's objects are not news, its HEAD is
    ok = seen.dirs.contains("sub") && seen.files.contains("sub/new.txt") && seen.git && !seen.files.contains(".git/objects/junk");
    fs::remove_all(root);
    if (!ok) {
        std::printf("FAILED: watcher (dirs %zu files %zu git %d)\n", seen.dirs.size(), seen.files.size(), seen.git);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
