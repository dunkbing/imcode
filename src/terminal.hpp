#pragma once

#include "pty.hpp"
#include <atomic>
#include <functional>
#include <ghostty/vt.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// a shell behind a pty and libghostty's emulator holding its screen. no ui in here (the
// panel is terminal_view.cpp). the pty is opened and read on its own thread; everything
// else is for one thread, the ui's
class Terminal {
public:
    using Open = std::function<std::unique_ptr<Pty>(int cols, int rows, std::string& error)>;
    // wake: called from the reader thread when there is output to show
    Terminal(Open open, int cols, int rows, std::function<void()> wake = nullptr);
    ~Terminal();
    Terminal(const Terminal&) = delete;
    Terminal& operator=(const Terminal&) = delete;

    // a pty on this machine running argv in cwd; no argv = $SHELL as a login shell
    static std::unique_ptr<Pty> local(const std::string& cwd, const std::vector<std::string>& argv,
                                      int cols, int rows, std::string& error);

    void poll(); // the output read so far goes into the screen
    void resize(int cols, int rows);
    void write(std::string_view bytes); // to the shell, as typed
    // codepoint: what the key types without modifiers (ctrl+c needs 'c')
    void key(GhosttyKey key, GhosttyMods mods, uint32_t codepoint = 0, bool repeat = false);
    void paste(std::string text);
    void scroll(int rows); // through the scrollback, negative = back; 0 = to the end
    [[nodiscard]] bool ended() const { // the shell is gone, or never started
        return ended_;
    }
    [[nodiscard]] bool failed() const { // it never started: the reason is on the screen
        return failed_;
    }
    std::string rowText(int y); // row y of what is shown, without trailing blanks

    // for drawing: update `render` from `vt`, then walk it with `rows` and `cells`
    GhosttyTerminal vt = nullptr;
    GhosttyRenderState render = nullptr;
    GhosttyRenderStateRowIterator rows = nullptr;
    GhosttyRenderStateRowCells cells = nullptr;

private:
    GhosttyKeyEncoder encoder_ = nullptr;
    GhosttyKeyEvent event_ = nullptr;
    int cols_, rows_;
    std::function<void()> wake_;
    std::mutex ptyMutex_; // pty_ is set by the reader thread
    std::unique_ptr<Pty> pty_;
    std::string early_; // written before the pty was open
    std::mutex pendingMutex_;
    std::string pending_; // read, not yet in the screen
    std::atomic<bool> ended_{false}, failed_{false}, closing_{false};
    std::thread reader_;
};
