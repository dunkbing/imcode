// a Terminal without a window: a real pty's output through the emulator onto the screen,
// and typed text and keys back to the program
#include "../src/terminal.hpp"
#include <chrono>
#include <cstdio>
#include <thread>

namespace {
    // true once any row of the screen reads `want`
    bool shows(Terminal& t, const std::string& want, int rows) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        do {
            t.poll();
            for (int y = 0; y < rows; ++y) {
                if (t.rowText(y) == want) {
                    return true;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    Terminal::Open run(std::vector<std::string> argv) {
        return [argv = std::move(argv)](int cols, int rows, std::string& error) {
            return Terminal::local(".", argv, cols, rows, error);
        };
    }
} // namespace

int main() {
#ifdef _WIN32
    Terminal out(run({"cmd", "/c", "echo hello"}), 80, 24);
#else
    Terminal out(run({"printf", "hello\\n"}), 80, 24);
#endif
    const bool printed = shows(out, "hello", 1); // the first row

    // the line is typed, enter comes from the key encoder; a resize reaches the pty
#ifdef _WIN32
    Terminal in(run({"powershell", "-NoProfile", "-Command",
                     "$x = Read-Host; 'got ' + $x; '' + $Host.UI.RawUI.WindowSize.Height + ' ' + $Host.UI.RawUI.WindowSize.Width"}),
                80, 24);
#else
    Terminal in(run({"sh", "-c", "read x; echo \"got $x\"; stty size"}), 80, 24);
#endif
    in.resize(100, 30);
    in.write("hi");
    in.key(GHOSTTY_KEY_ENTER, 0);
    const bool typed = shows(in, "got hi", 30) && shows(in, "30 100", 30);

    if (!printed || !typed) {
        std::printf("FAILED: printed=%d typed=%d first row=\"%s\"\n", printed, typed,
                    out.rowText(0).c_str());
        for (int y = 0; y < 6; ++y) {
            std::printf("  in %d: \"%s\"\n", y, in.rowText(y).c_str());
        }
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
