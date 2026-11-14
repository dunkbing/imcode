#include "watch.hpp"

#include "ssh.hpp"
#include "trace.hpp"
#include <acp/transport.hpp>
#include <acp_host/host.hpp>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <mutex>
#include <random>
#include <thread>

namespace fs = std::filesystem;

struct FolderWatch::State {
    std::mutex mutex;
    FolderChanges pending;
    std::atomic<bool> stop{false}, live{false};
    std::function<void()> wake;
    std::unique_ptr<Watch> local;      // this machine, in process
    acp::Transport* channel = nullptr; // the helper's output, while the reader has it: closed to end the reader
};

extern const unsigned char kHelperBytes[];
extern const std::size_t kHelperBytesSize;
extern const unsigned char kHelperSource[];
extern const std::size_t kHelperSourceSize;

namespace {
    // the helper program, built for this machine and embedded in the app (cmake/embed_helper.cmake):
    // empty where there is no backend
    std::string helperBytes() {
        return std::string(reinterpret_cast<const char*>(kHelperBytes), kHelperBytesSize);
    }

    // the linux helper's c source: a linux host builds it with its own cc
    std::string helperSource() {
        return std::string(reinterpret_cast<const char*>(kHelperSource), kHelperSourceSize);
    }

    std::string quoted(const std::string& text) {
        std::string out = "'";
        for (const char c : text) {
            out += c == '\'' ? std::string("'\\''") : std::string(1, c);
        }
        return out + "'";
    }

    std::uint64_t fnv(const std::string& bytes) {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const unsigned char c : bytes) {
            hash = (hash ^ c) * 1099511628211ULL;
        }
        return hash;
    }

    // the first line a command prints, after reading it to its end
    std::string firstLine(Ssh::Session& session, const std::string& command) {
        std::string failed;
        const auto channel = session.exec(command, nullptr, failed);
        std::string line, first;
        while (channel && channel->readLine(line)) {
            if (first.empty()) {
                first = line;
            }
        }
        return first;
    }

    // the helper running on a host, over ssh: put there once (named by its contents), then started
    std::unique_ptr<acp::Transport> openRemote(const std::string& host, const std::string& cwd) {
        Ssh::Error error;
        const auto session = Ssh::session(host, "", error);
        if (!session) {
            trace::event("watch.skipped", host + ": " + error.message);
            return nullptr;
        }
        const std::string system = firstLine(*session, "uname -sm");
        const bool linux = system.starts_with("Linux ");
        // a linux host builds the embedded source; another system gets this build's helper if it is the same
        const std::string bytes = linux ? helperSource() : helperBytes();
        if (!linux && (bytes.empty() || system != helperPlatform())) {
            trace::event("watch.skipped", std::format("{} is {}, no helper for it", host, system));
            return nullptr;
        }
        std::string failed;
        const std::string home = session->realpath(".", failed);
        if (home.empty()) {
            return nullptr;
        }
        const std::string dir = home + "/.imcode/helper";
        // named by its contents and the machine: a home folder can be shared between machines
        std::string arch = system.substr(system.find(' ') + 1);
        std::erase_if(arch, [](const char c) { return !std::isalnum(static_cast<unsigned char>(c)) && c != '_'; });
        const std::string path = std::format("{}/imcode-helper-{:016x}-{}", dir, fnv(bytes), arch);
        if (firstLine(*session, "test -x " + quoted(path) + " && echo yes") != "yes") {
            trace::Span put("watch.install", host, 0);
            firstLine(*session, "mkdir -p " + quoted(dir));
            // a temporary name, then a rename: two ImCodes starting together cannot run a half-written file
            const std::string temp = std::format("{}.{:x}", path, std::random_device{}());
            if (!session->write(linux ? temp + ".c" : temp, bytes, failed)) {
                trace::event("watch.skipped", "upload: " + failed);
                return nullptr;
            }
            const std::string build = linux ? "cc -w -O2 -o " + quoted(temp) + " " + quoted(temp + ".c") + " 2>&1 && " : "";
            const std::string done = firstLine(*session, build + "chmod 755 " + quoted(temp) + " && mv " + quoted(temp) + " " + quoted(path) +
                                                             " && echo yes");
            firstLine(*session, "rm -f " + quoted(temp + ".c") + " " + quoted(temp));
            if (done != "yes") {
                trace::event("watch.skipped", host + ": install: " + done);
                return nullptr;
            }
        }
        auto channel = session->exec(quoted(path) + " watch " + quoted(cwd),
                                     [host](const std::string& line) { trace::event("watch.stderr", host + ": " + line); }, failed);
        if (!channel) {
            trace::event("watch.skipped", "start: " + failed);
        }
        return channel;
    }

    // for tests: the helper as a child of this process (IMCODE_WATCH_VIA_HELPER), the rest as over ssh
    std::unique_ptr<acp::Transport> openChild(const std::string& cwd) {
        const std::string bytes = helperBytes();
        if (bytes.empty()) {
            return nullptr;
        }
        const std::string file = (fs::temp_directory_path() / std::format("imcode-helper-{:016x}", fnv(bytes))).string();
        if (!fs::exists(file)) {
            std::ofstream(file, std::ios::binary) << bytes;
            fs::permissions(file, fs::perms::owner_all);
        }
        auto [transport, error] = acp::ProcessTransport::spawn({file, "watch", cwd});
        if (!transport) {
            trace::event("watch.skipped", "spawn: " + error);
        }
        return std::move(transport);
    }

    // the reader: what the helper prints into the batches the ui takes
    void read(const std::shared_ptr<FolderWatch::State>& state, const std::string& host, const std::string& cwd) {
        const auto channel = host.empty() ? openChild(cwd) : openRemote(host, cwd);
        if (!channel) {
            return;
        }
        {
            const std::lock_guard lock(state->mutex);
            if (state->stop) {
                return;
            }
            state->channel = channel.get();
        }
        std::string line;
        FolderChanges batch;
        bool hello = false;
        while (!state->stop && channel->readLine(line)) {
            if (!hello) {
                if (!line.starts_with("imcode-helper 1 ")) {
                    break;
                }
                hello = true;
                // what changed while it was starting is not known: everything is read once
                {
                    const std::lock_guard lock(state->mutex);
                    state->pending.all = true;
                }
                state->live = true;
                trace::event("watch.live", host + " " + cwd);
                state->wake();
            } else if (line == ".") {
                {
                    const std::lock_guard lock(state->mutex);
                    state->pending.merge(std::move(batch));
                }
                batch = {};
                state->wake();
            } else if (!decode(line, batch)) {
                trace::event("watch.bad-line", line.substr(0, 80));
            }
        }
        state->live = false;
        {
            const std::lock_guard lock(state->mutex);
            state->channel = nullptr;
        }
        trace::event("watch.ended", host + " " + cwd);
        if (!state->stop) {
            state->wake();
        }
    }
} // namespace

FolderWatch::FolderWatch(std::string host, std::string cwd, std::function<void()> wake) : state_(std::make_shared<State>()) {
    state_->wake = std::move(wake);
#if defined(__APPLE__) || defined(_WIN32)
    constexpr bool inProcess = true; // a backend in the app; linux runs the helper as a child
#else
    constexpr bool inProcess = false;
#endif
    if (host.empty() && inProcess && !std::getenv("IMCODE_WATCH_VIA_HELPER")) {
        state_->local = std::make_unique<Watch>(cwd, [wake = state_->wake] { wake(); });
        trace::event("watch.local", cwd + (state_->local->ok() ? "" : " (no backend)"));
        return;
    }
    // a host can take seconds to connect: not waited for; the thread ends alone when the watch is dropped
    std::thread([state = state_, host = std::move(host), cwd = std::move(cwd)] { read(state, host, cwd); }).detach();
}

FolderWatch::~FolderWatch() {
    state_->stop = true;
    const std::lock_guard lock(state_->mutex);
    if (state_->channel) {
        state_->channel->close(); // unblocks the reader
    }
}

bool FolderWatch::live() const {
    return state_->local ? state_->local->ok() : state_->live.load();
}

FolderChanges FolderWatch::take() {
    if (state_->local) {
        return state_->local->take();
    }
    const std::lock_guard lock(state_->mutex);
    return std::exchange(state_->pending, FolderChanges{});
}
