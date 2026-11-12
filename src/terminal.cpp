#include "terminal.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#ifdef _WIN32
#include <filesystem>
#include <windows.h>
#else
#include <csignal>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif

extern char** environ;
#endif

namespace {
#ifdef _WIN32
    std::wstring wide(const std::string& s) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring out(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
        return out;
    }

    // one argument for CreateProcess's command line (the msvc runtime's parsing rules)
    std::wstring quoteArg(const std::wstring& arg) {
        if (!arg.empty() && arg.find_first_of(L" \t\"") == std::wstring::npos) {
            return arg;
        }
        std::wstring out = L"\"";
        size_t slashes = 0;
        for (const wchar_t c : arg) {
            if (c == L'\\') {
                ++slashes;
                continue;
            }
            out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
            slashes = 0;
            out += c;
        }
        return out.append(slashes * 2, L'\\') + L"\"";
    }

    // a conpty: the shell writes to `out_`, reads what is typed from `in_`
    class LocalPty : public Pty {
    public:
        LocalPty(HPCON console, HANDLE in, HANDLE out, HANDLE process)
            : console_(console), in_(in), out_(out), process_(process) {}

        ~LocalPty() override {
            TerminateProcess(process_, 1);
            CloseHandle(process_);
            CloseHandle(in_);
            // the output first: ClosePseudoConsole can wait on output nobody reads
            CloseHandle(out_);
            ClosePseudoConsole(console_);
        }

        long read(char* buf, size_t size) override {
            while (!closed_) {
                // peek in short waits: close() comes from another thread
                DWORD ready = 0;
                if (!PeekNamedPipe(out_, nullptr, 0, nullptr, &ready, nullptr)) {
                    return 0;
                }
                if (ready > 0) {
                    DWORD n = 0;
                    const DWORD want = static_cast<DWORD>(std::min<size_t>(size, ready));
                    return ReadFile(out_, buf, want, &n, nullptr) ? static_cast<long>(n) : 0;
                }
                // conpty keeps the pipe open after the shell exits
                if (WaitForSingleObject(process_, 10) == WAIT_OBJECT_0) {
                    return 0;
                }
            }
            return 0;
        }

        bool write(const char* data, size_t size) override {
            while (size > 0) {
                DWORD n = 0;
                if (!WriteFile(in_, data, static_cast<DWORD>(size), &n, nullptr)) {
                    return false;
                }
                data += n;
                size -= n;
            }
            return true;
        }

        void resize(int cols, int rows) override {
            ResizePseudoConsole(console_, COORD{static_cast<SHORT>(cols), static_cast<SHORT>(rows)});
        }

        void close() override {
            closed_ = true;
        }

    private:
        HPCON console_;
        HANDLE in_, out_, process_;
        std::atomic<bool> closed_{false};
    };
#else
    class LocalPty : public Pty {
    public:
        LocalPty(int fd, pid_t pid) : fd_(fd), pid_(pid) {}

        ~LocalPty() override {
            ::close(fd_); // the shell gets a hangup
            kill(pid_, SIGHUP);
            for (int tries = 0; waitpid(pid_, nullptr, WNOHANG) == 0; usleep(10000)) {
                if (++tries == 50) {
                    kill(pid_, SIGKILL); // it ignored the hangup
                }
            }
        }

        long read(char* buf, size_t size) override {
            while (!closed_) {
                // short waits: close() comes from another thread
                pollfd p{.fd = fd_, .events = POLLIN, .revents = 0};
                if (poll(&p, 1, 100) > 0) {
                    const long n = ::read(fd_, buf, size);
                    if (n >= 0 || errno != EINTR) {
                        return n;
                    }
                }
            }
            return 0;
        }

        bool write(const char* data, size_t size) override {
            while (size > 0) {
                const long n = ::write(fd_, data, size);
                if (n < 0 && errno != EINTR) {
                    return false;
                }
                if (n > 0) {
                    data += n;
                    size -= static_cast<size_t>(n);
                }
            }
            return true;
        }

        void resize(int cols, int rows) override {
            const winsize size{.ws_row = static_cast<unsigned short>(rows),
                               .ws_col = static_cast<unsigned short>(cols),
                               .ws_xpixel = 0,
                               .ws_ypixel = 0};
            ioctl(fd_, TIOCSWINSZ, &size);
        }

        void close() override {
            closed_ = true;
        }

    private:
        int fd_;
        pid_t pid_;
        std::atomic<bool> closed_{false};
    };
#endif

    void toShell(GhosttyTerminal, void* self, const uint8_t* data, size_t size) {
        static_cast<Terminal*>(self)->write({reinterpret_cast<const char*>(data), size});
    }
} // namespace

#ifdef _WIN32
std::unique_ptr<Pty> Terminal::local(const std::string& cwd, const std::vector<std::string>& argv,
                                     int cols, int rows, std::string& error) {
    // no argv: $SHELL when set (git bash), else powershell
    const char* shellEnv = std::getenv("SHELL");
    std::wstring command;
    if (argv.empty()) {
        command = quoteArg(wide(shellEnv && *shellEnv ? shellEnv : "powershell.exe"));
    }
    for (const auto& arg : argv) {
        command += (command.empty() ? L"" : L" ") + quoteArg(wide(arg));
    }
    auto fail = [&](const char* what) {
        error = std::string("Could not open a terminal: ") + what + " failed (" + std::to_string(GetLastError()) + ")";
        return nullptr;
    };
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite, nullptr, 0)) {
        return fail("CreatePipe");
    }
    if (!CreatePipe(&outRead, &outWrite, nullptr, 0)) {
        CloseHandle(inRead);
        CloseHandle(inWrite);
        return fail("CreatePipe");
    }
    HPCON console = nullptr;
    const HRESULT made = CreatePseudoConsole(COORD{static_cast<SHORT>(cols), static_cast<SHORT>(rows)},
                                             inRead, outWrite, 0, &console);
    CloseHandle(inRead); // the console holds its own ends
    CloseHandle(outWrite);
    if (FAILED(made)) {
        CloseHandle(inWrite);
        CloseHandle(outRead);
        return fail("CreatePseudoConsole");
    }
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<char> attrs(attrSize);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    // null std handles: otherwise a parent with redirected stdio (a test runner, a launcher)
    // hands its own to the shell, which then bypasses the console
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.data());
    InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize);
    UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console,
                              sizeof(console), nullptr, nullptr);
    std::error_code ec;
    const std::wstring dir = wide(cwd);
    const bool dirOk = !cwd.empty() && std::filesystem::is_directory(dir, ec); // gone: where the app is
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT,
                                        nullptr, dirOk ? dir.c_str() : nullptr, &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    if (!started) {
        auto result = fail("CreateProcess");
        CloseHandle(inWrite);
        CloseHandle(outRead);
        ClosePseudoConsole(console);
        return result;
    }
    CloseHandle(pi.hThread);
    return std::make_unique<LocalPty>(console, inWrite, outRead, pi.hProcess);
}
#else
std::unique_ptr<Pty> Terminal::local(const std::string& cwd, const std::vector<std::string>& argv,
                                     int cols, int rows, std::string& error) {
    // all built before the fork: the child of a threaded process must not allocate
    const char* shellEnv = std::getenv("SHELL");
    const std::string shell = shellEnv && *shellEnv ? shellEnv : "/bin/sh";
    const auto slash = shell.rfind('/');
    const std::string login = "-" + shell.substr(slash == std::string::npos ? 0 : slash + 1);
    std::vector<char*> args;
    for (const auto& arg : argv) {
        args.push_back(const_cast<char*>(arg.c_str()));
    }
    if (args.empty()) {
        args.push_back(const_cast<char*>(login.c_str())); // a leading '-' asks for a login shell
    }
    args.push_back(nullptr);
    const char* file = argv.empty() ? shell.c_str() : argv[0].c_str();
    char term[] = "TERM=xterm-256color";
    std::vector<char*> env;
    for (char** e = environ; *e; ++e) {
        if (std::strncmp(*e, "TERM=", 5) != 0) {
            env.push_back(*e);
        }
    }
    env.push_back(term);
    env.push_back(nullptr);

    winsize size{.ws_row = static_cast<unsigned short>(rows),
                 .ws_col = static_cast<unsigned short>(cols),
                 .ws_xpixel = 0,
                 .ws_ypixel = 0};
    int fd = -1;
    const pid_t pid = forkpty(&fd, nullptr, nullptr, &size);
    if (pid < 0) {
        error = std::string("Could not open a terminal: ") + std::strerror(errno);
        return nullptr;
    }
    if (pid == 0) {
        for (int other = 3; other < 256; ++other) {
            ::close(other); // the app's sockets and files are not the shell's
        }
        if (chdir(cwd.c_str()) != 0) {
            // a folder that is gone: the shell starts where the app is
        }
        environ = env.data();
        execvp(file, args.data());
        _exit(127);
    }
    return std::make_unique<LocalPty>(fd, pid);
}
#endif

Terminal::Terminal(Open open, int cols, int rows, std::function<void()> wake)
    : cols_(cols), rows_(rows), wake_(std::move(wake)) {
    ghostty_terminal_new(nullptr, &vt, static_cast<uint16_t>(cols), static_cast<uint16_t>(rows));
    ghostty_terminal_set(vt, GHOSTTY_TERMINAL_OPT_USERDATA, this);
    // answers to the shell's queries (cursor position, device attributes)
    ghostty_terminal_set(vt, GHOSTTY_TERMINAL_OPT_WRITE_PTY, reinterpret_cast<const void*>(&toShell));
    ghostty_render_state_new(nullptr, &render);
    ghostty_render_state_row_iterator_new(nullptr, &this->rows);
    ghostty_render_state_row_cells_new(nullptr, &cells);
    ghostty_key_encoder_new(nullptr, &encoder_);
    ghostty_key_event_new(nullptr, &event_);

    reader_ = std::thread([this, open = std::move(open), cols, rows] {
        std::string error;
        std::unique_ptr<Pty> made = open(cols, rows, error); // slow for a remote host
        Pty* pty = made.get();
        {
            std::lock_guard lock(ptyMutex_);
            if (pty && !closing_) {
                pty_ = std::move(made);
                pty_->resize(cols_, rows_); // the panel may have changed size meanwhile
                pty_->write(early_.data(), early_.size());
            } else {
                pty = nullptr;
            }
        }
        char buf[16384];
        for (long n; pty && (n = pty->read(buf, sizeof(buf))) > 0;) {
            size_t waiting = 0;
            {
                std::lock_guard lock(pendingMutex_);
                pending_.append(buf, static_cast<size_t>(n));
                waiting = pending_.size();
            }
            if (wake_) {
                wake_();
            }
            // nobody draws (hidden panel, window in the background): the shell waits
            // instead of the buffer growing
            while (waiting > (1u << 20) && !closing_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                std::lock_guard lock(pendingMutex_);
                waiting = pending_.size();
            }
        }
        if (!error.empty()) {
            failed_ = true;
            std::lock_guard lock(pendingMutex_);
            pending_ += error + "\r\n";
        }
        ended_ = true;
        if (wake_) {
            wake_();
        }
    });
}

Terminal::~Terminal() {
    {
        std::lock_guard lock(ptyMutex_);
        closing_ = true;
        if (pty_) {
            pty_->close();
        }
    }
    // shortcut: waits for an ssh connect still in flight, give Ssh::session a cancel if that hurts
    reader_.join();
    pty_.reset(); // before the screen: nothing writes to it afterwards
    ghostty_key_event_free(event_);
    ghostty_key_encoder_free(encoder_);
    ghostty_render_state_row_cells_free(cells);
    ghostty_render_state_row_iterator_free(rows);
    ghostty_render_state_free(render);
    ghostty_terminal_free(vt);
}

void Terminal::poll() {
    std::string got;
    {
        std::lock_guard lock(pendingMutex_);
        got.swap(pending_);
    }
    if (!got.empty()) {
        ghostty_terminal_vt_write(vt, reinterpret_cast<const uint8_t*>(got.data()), got.size());
    }
}

void Terminal::resize(int cols, int rows) {
    if (cols == cols_ && rows == rows_) {
        return;
    }
    // before the lock: a size report goes out through write()
    ghostty_terminal_resize(vt, static_cast<uint16_t>(cols), static_cast<uint16_t>(rows), 0, 0);
    std::lock_guard lock(ptyMutex_);
    cols_ = cols;
    rows_ = rows;
    if (pty_) {
        pty_->resize(cols, rows);
    }
}

void Terminal::write(std::string_view bytes) {
    std::lock_guard lock(ptyMutex_);
    if (!pty_) {
        early_ += bytes; // typed while the shell was still coming up
    } else if (!bytes.empty()) {
        // shortcut: blocks the ui while the shell does not read (a huge paste), queue it then
        pty_->write(bytes.data(), bytes.size());
    }
}

void Terminal::key(GhosttyKey key, GhosttyMods mods, uint32_t codepoint, bool repeat) {
    // cursor-key and keypad modes, the kitty keyboard flags the program asked for
    ghostty_key_encoder_set_from_terminal(encoder_, vt);
    ghostty_key_event_set_action(event_, repeat ? GHOSTTY_KEY_ACTION_REPEAT : GHOSTTY_KEY_ACTION_PRESS);
    ghostty_key_event_set_key(event_, key);
    ghostty_key_event_set_mods(event_, mods);
    ghostty_key_event_set_unshifted_codepoint(event_, codepoint);
    uint8_t buf[128];
    size_t size = 0;
    if (ghostty_key_encoder_encode(encoder_, event_, buf, sizeof(buf), &size) == GHOSTTY_SUCCESS) {
        scroll(0);
        write({reinterpret_cast<const char*>(buf), size});
    }
}

void Terminal::paste(std::string text) {
    GhosttyTerminalModeConfig bracketed{.mode = GHOSTTY_MODE_BRACKETED_PASTE, .value = false};
    ghostty_terminal_get(vt, GHOSTTY_TERMINAL_DATA_MODE, &bracketed);
    size_t size = 0; // first call: how much room the encoded text needs
    ghostty_paste_encode(text.data(), text.size(), bracketed.value, nullptr, 0, &size);
    std::string out(size, '\0');
    if (ghostty_paste_encode(text.data(), text.size(), bracketed.value,
                             reinterpret_cast<uint8_t*>(out.data()), out.size(),
                             &size) == GHOSTTY_SUCCESS) {
        scroll(0);
        write(std::string_view(out).substr(0, size));
    }
}

void Terminal::scroll(int rows) {
    GhosttyTerminalScrollViewport to{};
    to.tag = rows == 0 ? GHOSTTY_TERMINAL_SCROLL_VIEWPORT_BOTTOM : GHOSTTY_TERMINAL_SCROLL_VIEWPORT_DELTA;
    to.value.delta = rows;
    ghostty_terminal_scroll_viewport(vt, to);
}

std::string Terminal::rowText(int y) {
    std::string out;
    ghostty_render_state_update(render, vt);
    ghostty_render_state_get(render, GHOSTTY_RENDER_STATE_DATA_ROW_ITERATOR, &rows);
    for (int i = 0; i <= y; ++i) {
        if (!ghostty_render_state_row_iterator_next(rows)) {
            return out;
        }
    }
    ghostty_render_state_row_get(rows, GHOSTTY_RENDER_STATE_ROW_DATA_CELLS, &cells);
    while (ghostty_render_state_row_cells_next(cells)) {
        uint8_t text[64];
        GhosttyBuffer buf{.ptr = text, .cap = sizeof(text), .len = 0};
        const bool has = ghostty_render_state_row_cells_get(
                             cells, GHOSTTY_RENDER_STATE_ROW_CELLS_DATA_GRAPHEMES_UTF8, &buf) ==
                             GHOSTTY_SUCCESS &&
                         buf.len > 0;
        out += has ? std::string(reinterpret_cast<const char*>(text), buf.len) : " ";
    }
    out.erase(out.find_last_not_of(' ') + 1);
    return out;
}
