#include "watch.hpp"

#include <mutex>
#include <string_view>
#include <utility>

#ifdef __APPLE__
#include <CoreServices/CoreServices.h>
#include <climits>
#include <cstdlib>
#include <dispatch/dispatch.h>
#endif

void FolderChanges::merge(FolderChanges&& other) {
    dirs.merge(other.dirs);
    files.merge(other.files);
    git |= other.git;
    all |= other.all;
}

namespace {
    // an agent's build writes thousands of files: past this the caller reads everything again
    constexpr size_t kMaxPaths = 2000;

    std::string parentOf(const std::string& path) {
        const size_t slash = path.rfind('/');
        return slash == std::string::npos ? std::string() : path.substr(0, slash);
    }

    // a path from the system or the helper, relative and inside the folder
    bool safeRelative(std::string_view path) {
        if (path.empty() || path.front() == '/' || path.find('\n') != std::string_view::npos || path.find('\0') != std::string_view::npos) {
            return false;
        }
        size_t at = 0;
        while (at <= path.size()) {
            const size_t slash = path.find('/', at);
            const std::string_view part = path.substr(at, slash == std::string_view::npos ? std::string_view::npos : slash - at);
            if (part == "..") {
                return false;
            }
            if (slash == std::string_view::npos) {
                break;
            }
            at = slash + 1;
        }
        return true;
    }

    // one event into the batch: git's own files only where they say something happened
    void note(FolderChanges& into, const std::string& rel, bool entries, bool isDir) {
        if (rel.starts_with(".git/") || rel == ".git") {
            if (rel == ".git/HEAD" || rel == ".git/index" || rel == ".git/packed-refs" || rel.starts_with(".git/refs/")) {
                into.git = true;
            }
            return;
        }
        if (rel.starts_with("node_modules/") || rel.find("/node_modules/") != std::string::npos) {
            return;
        }
        into.files.insert(rel);
        if (entries) {
            into.dirs.insert(parentOf(rel));
            if (isDir) {
                into.dirs.insert(rel);
            }
        }
        if (into.files.size() > kMaxPaths || into.dirs.size() > kMaxPaths) {
            into = FolderChanges{.git = into.git, .all = true};
        }
    }
} // namespace

std::string encode(const FolderChanges& changes) {
    std::string out;
    if (changes.all) {
        out += "a\n";
    }
    if (changes.git) {
        out += "g\n";
    }
    for (const auto& dir : changes.dirs) {
        out += dir.empty() ? "d\n" : "d " + dir + "\n"; // the folder itself has no path
    }
    for (const auto& file : changes.files) {
        out += "f " + file + "\n";
    }
    return out;
}

bool decode(const std::string& line, FolderChanges& changes) {
    if (line == "a") {
        changes.all = true;
        return true;
    }
    if (line == "g") {
        changes.git = true;
        return true;
    }
    if (line.size() > 2 && line[1] == ' ' && (line[0] == 'd' || line[0] == 'f')) {
        const std::string path = line.substr(2);
        if (!safeRelative(path)) {
            return false;
        }
        (line[0] == 'd' ? changes.dirs : changes.files).insert(path);
        return true;
    }
    // the folder itself: its entries changed
    if (line == "d") {
        changes.dirs.insert("");
        return true;
    }
    return false;
}

#ifdef __APPLE__

std::string helperPlatform() {
#if defined(__aarch64__)
    return "Darwin arm64";
#else
    return "Darwin x86_64";
#endif
}

struct Watch::Impl {
    std::string root; // as the system reports paths: symlinks resolved
    FSEventStreamRef stream = nullptr;
    dispatch_queue_t queue = nullptr;
    std::mutex mutex;
    FolderChanges pending;
    std::function<void()> onChange;
};

namespace {
    void onEvents(ConstFSEventStreamRef, void* info, size_t count, void* paths, const FSEventStreamEventFlags flags[],
                  const FSEventStreamEventId[]) {
        auto* impl = static_cast<Watch::Impl*>(info);
        const auto* names = static_cast<const char* const*>(paths);
        FolderChanges batch;
        for (size_t i = 0; i < count; ++i) {
            const FSEventStreamEventFlags f = flags[i];
            if (f & (kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped | kFSEventStreamEventFlagKernelDropped |
                     kFSEventStreamEventFlagRootChanged | kFSEventStreamEventFlagMount | kFSEventStreamEventFlagUnmount)) {
                batch.all = true;
                continue;
            }
            const std::string path = names[i];
            if (!path.starts_with(impl->root)) {
                continue;
            }
            const std::string rel = path.size() > impl->root.size() + 1 ? path.substr(impl->root.size() + 1) : std::string();
            if (rel.empty()) {
                batch.dirs.insert("");
                continue;
            }
            const bool entries = f & (kFSEventStreamEventFlagItemCreated | kFSEventStreamEventFlagItemRemoved | kFSEventStreamEventFlagItemRenamed);
            note(batch, rel, entries, f & kFSEventStreamEventFlagItemIsDir);
        }
        if (!batch.any()) {
            return;
        }
        {
            const std::lock_guard lock(impl->mutex);
            impl->pending.merge(std::move(batch));
        }
        if (impl->onChange) {
            impl->onChange();
        }
    }
} // namespace

Watch::Watch(std::string root, std::function<void()> onChange) : impl_(std::make_unique<Impl>()) {
    char real[PATH_MAX];
    impl_->root = realpath(root.c_str(), real) ? real : root;
    impl_->onChange = std::move(onChange);
    FSEventStreamContext context{0, impl_.get(), nullptr, nullptr, nullptr};
    const void* values[] = {CFStringCreateWithCString(nullptr, impl_->root.c_str(), kCFStringEncodingUTF8)};
    CFArrayRef folders = CFArrayCreate(nullptr, values, 1, &kCFTypeArrayCallBacks);
    // 0.1 s: the system batches what happens within it
    impl_->stream = FSEventStreamCreate(nullptr, &onEvents, &context, folders, kFSEventStreamEventIdSinceNow, 0.1,
                                        kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagWatchRoot | kFSEventStreamCreateFlagNoDefer);
    CFRelease(folders);
    CFRelease(static_cast<CFStringRef>(values[0]));
    if (!impl_->stream) {
        return;
    }
    impl_->queue = dispatch_queue_create("imcode.watch", DISPATCH_QUEUE_SERIAL);
    FSEventStreamSetDispatchQueue(impl_->stream, impl_->queue);
    if (!FSEventStreamStart(impl_->stream)) {
        FSEventStreamInvalidate(impl_->stream);
        FSEventStreamRelease(impl_->stream);
        impl_->stream = nullptr;
    }
}

Watch::~Watch() {
    if (impl_->stream) {
        FSEventStreamStop(impl_->stream);
        FSEventStreamInvalidate(impl_->stream);
        FSEventStreamRelease(impl_->stream);
    }
    if (impl_->queue) {
        // events already queued must not reach the freed impl: drain the queue first
        dispatch_sync(impl_->queue, ^{});
        dispatch_release(impl_->queue);
    }
}

bool Watch::ok() const {
    return impl_->stream != nullptr;
}

FolderChanges Watch::take() {
    const std::lock_guard lock(impl_->mutex);
    return std::exchange(impl_->pending, FolderChanges{});
}

#else

std::string helperPlatform() {
    return {};
}

struct Watch::Impl {};

Watch::Watch(std::string, std::function<void()>) : impl_(std::make_unique<Impl>()) {}
Watch::~Watch() = default;

bool Watch::ok() const {
    return false;
}

FolderChanges Watch::take() {
    return {};
}

#endif
