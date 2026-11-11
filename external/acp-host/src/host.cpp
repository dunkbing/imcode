#include "acp_host/host.hpp"

#include <mutex>

namespace AcpHost {

    namespace {
        std::mutex wakeMutex;
        std::function<void()> wakeFn;
    } // namespace

    void setWake(std::function<void()> fn) {
        std::lock_guard lock(wakeMutex);
        wakeFn = std::move(fn);
    }

    void wake() {
        std::function<void()> fn;
        {
            std::lock_guard lock(wakeMutex);
            fn = wakeFn;
        }
        if (fn) {
            fn();
        }
    }

} // namespace AcpHost
