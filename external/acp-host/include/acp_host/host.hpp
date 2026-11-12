#pragma once

#include <acp/log.hpp>
#include <chrono>
#include <format>
#include <functional>
#include <future>
#include <utility>

// what the library needs from the app around it: a way to wake an idle ui loop, and a
// background task the ui polls. logging goes through acp::setLogger.
namespace AcpHost {

    // called from any thread when an event is queued or a background task finishes;
    // an imgui app passes its "draw another frame" call. nothing by default
    void setWake(std::function<void()> fn);
    void wake();

    template <class... Args>
    void logf(acp::LogLevel level, std::format_string<Args...> fmt, Args&&... args) {
        acp::log(level, std::format(fmt, std::forward<Args>(args)...));
    }

    // one background task at a time. the destructor waits for it
    template <class T> class AsyncOp {
    public:
        [[nodiscard]] bool isRunning() const {
            return future_.valid();
        }
        void start(std::function<T()> task) {
            future_ = std::async(std::launch::async, [task = std::move(task)] {
                T result = task();
                wake();
                return result;
            });
        }
        // true once the task has finished; its result goes to done
        // wait: how long to hold the caller for it first. a task that is quick finishes in the
        // frame it started, so nothing flashes a loader
        bool check(const std::function<void(T)>& done = nullptr, std::chrono::milliseconds wait = std::chrono::milliseconds(0)) {
            if (!future_.valid() ||
                future_.wait_for(wait) != std::future_status::ready) {
                return false;
            }
            T result = future_.get();
            if (done) {
                done(std::move(result));
            }
            return true;
        }

    private:
        std::future<T> future_;
    };

} // namespace AcpHost
