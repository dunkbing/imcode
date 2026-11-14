#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>

// a timing log for finding slow steps: one line per event or timed span, in <data dir>/trace.log
// (the run before it is kept as trace.prev.log). header only, safe from any thread. it holds
// names, sizes, paths and durations, never message text. IMCODE_TRACE=0 turns it off.
namespace trace {
    namespace detail {
        inline std::mutex mu;
        inline std::FILE* file = nullptr;
        inline const auto start = std::chrono::steady_clock::now();
        inline std::atomic<int> nextThread{0};

        inline int threadTag() {
            thread_local const int tag = nextThread++; // 0 is the first thread to log: the ui
            return tag;
        }
    } // namespace detail

    [[nodiscard]] inline double nowMs() {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - detail::start).count();
    }

    inline void open(const std::filesystem::path& dir) {
        if (const char* off = std::getenv("IMCODE_TRACE"); off && std::string_view(off) == "0") {
            return;
        }
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        std::filesystem::rename(dir / "trace.log", dir / "trace.prev.log", ec);
        std::lock_guard lock(detail::mu);
        detail::file = std::fopen((dir / "trace.log").string().c_str(), "w");
    }

    [[nodiscard]] inline bool enabled() {
        return detail::file != nullptr;
    }

    inline void line(std::string_view text) {
        if (!detail::file) {
            return;
        }
        const double at = nowMs();
        const int tag = detail::threadTag();
        std::lock_guard lock(detail::mu);
        std::fprintf(detail::file, "%9.1f t%d %.*s\n", at, tag, static_cast<int>(text.size()), text.data());
        std::fflush(detail::file);
    }

    // a point in time: what the user did, or something that happened
    inline void event(std::string_view name, std::string_view detail = {}) {
        if (!detail::file) {
            return;
        }
        line(detail.empty() ? std::string(name) : std::format("{}  {}", name, detail));
    }

    // a timed step: logs its duration when it ends (only if it took at least minMs)
    class Span {
    public:
        explicit Span(std::string name, std::string info = {}, double minMs = 0)
            : name_(std::move(name)), info_(std::move(info)), minMs_(minMs), began_(nowMs()) {}
        Span(const Span&) = delete;
        Span& operator=(const Span&) = delete;
        ~Span() {
            end();
        }
        void note(std::string_view more) { // add to what is logged at the end
            if (!info_.empty()) {
                info_ += ' ';
            }
            info_ += more;
        }
        void force() { // log it whatever it took
            minMs_ = 0;
        }
        void end() {
            if (done_) {
                return;
            }
            done_ = true;
            const double took = nowMs() - began_;
            if (detail::file && took >= minMs_) {
                line(std::format("{}  {:.1f} ms  {}", name_, took, info_));
            }
        }

    private:
        std::string name_, info_;
        double minMs_;
        double began_;
        bool done_ = false;
    };
} // namespace trace
