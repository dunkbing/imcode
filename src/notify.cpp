#include "notify.hpp"

#include "acp_host/process_runner.hpp"
#include <chrono>
#include <thread>

// shortcut: the os's own command-line helpers (the banner is filed under Script Editor on
// macos, nothing on windows); a native notification with the app's icon belongs in imshell
void notify(const std::string& title, const std::string& body) {
    std::thread([title, body] {
#if defined(__APPLE__)
        auto quoted = [](const std::string& s) { // an applescript string
            std::string out = "\"";
            for (const char c : s) {
                if (c == '"' || c == '\\') {
                    out += '\\';
                }
                out += c == '\n' ? ' ' : c;
            }
            return out + "\"";
        };
        ProcessRunner::run({.args = {"/usr/bin/osascript", "-e",
                                     "display notification " + quoted(body) + " with title " +
                                         quoted(title)},
                            .timeout = std::chrono::seconds(5)});
#elif defined(__linux__)
        ProcessRunner::run(
            {.args = {"notify-send", title, body}, .timeout = std::chrono::seconds(5)});
#endif
    }).detach();
}
