#pragma once

#include <functional>
#include <memory>
#include <set>
#include <string>

// what changed under a watched folder since the last take(). paths are relative to it
struct FolderChanges {
    std::set<std::string> dirs;  // folders whose entries came or went (or are gone)
    std::set<std::string> files; // files written, created or removed
    bool git = false;            // .git/HEAD, the index or a ref moved
    bool all = false;            // events were dropped: anything may have changed
    [[nodiscard]] bool any() const {
        return all || git || !dirs.empty() || !files.empty();
    }
    void merge(FolderChanges&& other);
};

// a watcher on one folder of this machine (macos: FSEvents; events come on its own thread,
// batched by the system). ok() is false where there is no backend
class Watch {
public:
    Watch(std::string root, std::function<void()> onChange);
    ~Watch();
    Watch(const Watch&) = delete;
    Watch& operator=(const Watch&) = delete;
    [[nodiscard]] bool ok() const;
    FolderChanges take(); // thread safe: what came in since the last call
    struct Impl;    // the backend's; public for its callback

private:
    std::unique_ptr<Impl> impl_;
};

// "Darwin arm64": the system the helper built here runs on; empty without a backend
std::string helperPlatform();

// the helper (imcode-helper watch <folder>) prints a hello line, then batches: one line per
// change ("d dir", "f file", "g", "a"), each batch ended by a "." line
std::string encode(const FolderChanges& changes);
// one line of that into changes; false for anything else (paths with .. or a leading / are refused)
bool decode(const std::string& line, FolderChanges& changes);

// the watch for the folder of the shown thread: here, or on a host through the helper
// (uploaded over ssh once, started in an exec channel). live() says events are coming; when
// it is false the caller keeps polling
class FolderWatch {
public:
    // wake: called from another thread when changes wait
    FolderWatch(std::string host, std::string cwd, std::function<void()> wake);
    ~FolderWatch(); // does not block: a reader still connecting is left to finish alone
    FolderWatch(const FolderWatch&) = delete;
    FolderWatch& operator=(const FolderWatch&) = delete;
    [[nodiscard]] bool live() const;
    FolderChanges take();
    struct State; // the reader's; public for it

private:
    std::shared_ptr<State> state_;
};
