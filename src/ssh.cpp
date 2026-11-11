#include "ssh.hpp"

#include "acp_host/process_runner.hpp"
#include "ssh_config.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <libssh2.h>
#include <libssh2_sftp.h>
#include <map>
#include <mutex>

// shortcut: posix sockets only; windows needs the winsock variants (DearSQL's ssh_tunnel.cpp)
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace Ssh {

    struct Session::Impl {
        int sock = -1;
        LIBSSH2_SESSION* session = nullptr;
        LIBSSH2_SFTP* sftp = nullptr;
        std::mutex mutex; // libssh2 allows one thread in a session at a time
        std::atomic<bool> dead{false};
        std::string secret; // keyboard-interactive reads it through the session's abstract
    };

    namespace {
        using Clock = std::chrono::steady_clock;

        std::string home() {
            const char* h = std::getenv("HOME");
            return h ? h : "";
        }

        std::string lastError(LIBSSH2_SESSION* session) {
            char* msg = nullptr;
            libssh2_session_last_error(session, &msg, nullptr, 0);
            return msg ? msg : "unknown error";
        }

        bool again(LIBSSH2_SESSION*, long rc) {
            return rc == LIBSSH2_ERROR_EAGAIN;
        }
        template <class T> bool again(LIBSSH2_SESSION* session, T* made) {
            return !made && libssh2_session_last_errno(session) == LIBSSH2_ERROR_EAGAIN;
        }

        void waitSocket(int sock) {
            // short: another thread may have read the packet this one waits for
            pollfd p{.fd = sock, .events = POLLIN, .revents = 0};
            poll(&p, 1, 20);
        }

        // f under the session lock, again while libssh2 wants the socket. the session is
        // non-blocking so a reader waiting for output never holds the lock against a writer
        template <class F> auto retry(Session::Impl& i, F f, int timeoutMs = 15000) {
            const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
            for (;;) {
                {
                    std::lock_guard lock(i.mutex);
                    auto result = f();
                    if (!again(i.session, result) || Clock::now() > deadline) {
                        return result;
                    }
                }
                waitSocket(i.sock);
            }
        }

        // tcp connect with a timeout, trying every address of the host
        int connectTo(const std::string& host, int port, std::string& error) {
            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo* res = nullptr;
            if (const int rc = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res);
                rc != 0) {
                error = "Could not resolve " + host + ": " + gai_strerror(rc);
                return -1;
            }
            int result = -1;
            for (addrinfo* a = res; a && result < 0; a = a->ai_next) {
                const int s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
                if (s < 0) {
                    continue;
                }
                const int flags = fcntl(s, F_GETFL, 0);
                fcntl(s, F_SETFL, flags | O_NONBLOCK);
                pollfd p{.fd = s, .events = POLLOUT, .revents = 0};
                int err = 0;
                socklen_t len = sizeof(err);
                if ((connect(s, a->ai_addr, a->ai_addrlen) == 0 || errno == EINPROGRESS) &&
                    poll(&p, 1, 10000) == 1 &&
                    getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0) {
                    fcntl(s, F_SETFL, flags);
#ifdef SO_NOSIGPIPE
                    int on = 1;
                    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
                    result = s;
                } else {
                    close(s);
                }
            }
            freeaddrinfo(res);
            if (result < 0) {
                error = "Could not reach " + host + ":" + std::to_string(port);
            }
            return result;
        }

        int knownHostKeyMask(int type) {
            switch (type) {
            case LIBSSH2_HOSTKEY_TYPE_RSA:
                return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
            case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
                return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
            case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
                return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
            case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
                return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
            case LIBSSH2_HOSTKEY_TYPE_ED25519:
                return LIBSSH2_KNOWNHOST_KEY_ED25519;
            default:
                return 0;
            }
        }

        // ssh's StrictHostKeyChecking=accept-new: a known key must match, a new host is
        // remembered. read line by line (readfile stops at a line it cannot parse)
        std::string checkHostKey(LIBSSH2_SESSION* session, const std::string& host, int port) {
            std::string path = home() + "/.ssh/known_hosts";
            if (const char* f = std::getenv("IMCODE_SSH_KNOWN_HOSTS_FILE"); f && *f) {
                path = f; // tests leave the real file alone
            }
            size_t keyLen = 0;
            int keyType = 0;
            const char* key = libssh2_session_hostkey(session, &keyLen, &keyType);
            const int mask = knownHostKeyMask(keyType);
            LIBSSH2_KNOWNHOSTS* known = libssh2_knownhost_init(session);
            if (!key || !mask || !known) {
                return "Could not check the host key of " + host;
            }
            std::ifstream in(path);
            for (std::string line; std::getline(in, line);) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                if (!line.empty() && line[0] != '#') {
                    libssh2_knownhost_readline(known, line.c_str(), line.size(),
                                               LIBSSH2_KNOWNHOST_FILE_OPENSSH);
                }
            }
            in.close();

            const int typeMask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | mask;
            const int check =
                libssh2_knownhost_checkp(known, host.c_str(), port, key, keyLen, typeMask, nullptr);
            std::string error;
            if (check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH) {
                error = "The host key of " + host + " has changed. If the machine was reinstalled, " +
                        "remove its old line from " + path +
                        "; otherwise someone may be intercepting the connection.";
            } else if (check != LIBSSH2_KNOWNHOST_CHECK_MATCH) {
                const std::string name =
                    port == 22 ? host : "[" + host + "]:" + std::to_string(port);
                libssh2_knownhost* entry = nullptr;
                char line[4096];
                size_t lineLen = 0;
                if (libssh2_knownhost_addc(known, name.c_str(), nullptr, key, keyLen, nullptr, 0,
                                           typeMask, &entry) == 0 &&
                    libssh2_knownhost_writeline(known, entry, line, sizeof(line), &lineLen,
                                                LIBSSH2_KNOWNHOST_FILE_OPENSSH) == 0) {
                    std::error_code ec;
                    std::filesystem::create_directories(
                        std::filesystem::path(path).parent_path(), ec);
                    std::ofstream out(path, std::ios::app | std::ios::binary);
                    out.write(line, static_cast<std::streamsize>(lineLen));
                    if (lineLen == 0 || line[lineLen - 1] != '\n') {
                        out << '\n';
                    }
                }
            }
            libssh2_knownhost_free(known);
            return error;
        }

        // keyboard-interactive servers ask for the password as a prompt
        void answerWithSecret(const char*, int, const char*, int, int numPrompts,
                              const LIBSSH2_USERAUTH_KBDINT_PROMPT*,
                              LIBSSH2_USERAUTH_KBDINT_RESPONSE* responses, void** abstract) {
            const auto* secret = static_cast<const std::string*>(*abstract);
            for (int i = 0; i < numPrompts; ++i) {
                responses[i].text = strdup(secret->c_str());
                responses[i].length = static_cast<unsigned int>(secret->size());
            }
        }

        struct Target {
            std::string user, host, identityFile;
            int port = 22;
        };

        Target resolve(const std::string& name) {
            Target t;
            std::string rest = name;
            if (const auto at = rest.find('@'); at != std::string::npos) {
                t.user = rest.substr(0, at);
                rest = rest.substr(at + 1);
            }
            if (const auto colon = rest.rfind(':'); colon != std::string::npos) {
                t.port = std::atoi(rest.c_str() + colon + 1);
                rest = rest.substr(0, colon);
            }
            t.host = rest;
            const char* config = std::getenv("IMCODE_SSH_CONFIG"); // tests
            for (const auto& entry : parseSSHConfig(config ? config : "")) {
                if (entry.alias != rest) {
                    continue;
                }
                if (!entry.hostname.empty()) {
                    t.host = entry.hostname;
                }
                if (t.user.empty()) {
                    t.user = entry.user;
                }
                if (t.port == 22) {
                    t.port = entry.port;
                }
                t.identityFile = entry.identityFile;
                break;
            }
            if (t.user.empty()) {
                const char* user = std::getenv("USER");
                t.user = user ? user : "";
            }
            return t;
        }

        bool authenticate(LIBSSH2_SESSION* session, const Target& t, const std::string& secret,
                          Error& error) {
            const auto userLen = static_cast<unsigned int>(t.user.size());
            const char* list = libssh2_userauth_list(session, t.user.c_str(), userLen);
            const std::string offered = list ? list : "";
            if (libssh2_userauth_authenticated(session)) {
                return true;
            }
#if defined(__APPLE__)
            // the system ssh reads a key's passphrase from the keychain by itself; we cannot,
            // but the agent can be told to load those keys, once
            static std::once_flag keychain;
            std::call_once(keychain, [] {
                ProcessRunner::run({.args = {"/usr/bin/ssh-add", "--apple-load-keychain"},
                                    .timeout = std::chrono::seconds(5)});
            });
#endif
            if (LIBSSH2_AGENT* agent = libssh2_agent_init(session)) {
                if (libssh2_agent_connect(agent) == 0 && libssh2_agent_list_identities(agent) == 0) {
                    libssh2_agent_publickey* id = nullptr;
                    libssh2_agent_publickey* prev = nullptr;
                    while (libssh2_agent_get_identity(agent, &id, prev) == 0 &&
                           libssh2_agent_userauth(agent, t.user.c_str(), id) != 0) {
                        prev = id;
                    }
                    libssh2_agent_disconnect(agent);
                }
                libssh2_agent_free(agent);
            }
            std::vector<std::string> keys;
            if (!t.identityFile.empty()) {
                keys.push_back(t.identityFile);
            }
            for (const char* name : {"id_ed25519", "id_ecdsa", "id_rsa"}) {
                keys.push_back(home() + "/.ssh/" + name);
            }
            for (const auto& key : keys) {
                if (libssh2_userauth_authenticated(session)) {
                    break;
                }
                if (std::error_code ec; std::filesystem::exists(key, ec)) {
                    libssh2_userauth_publickey_fromfile_ex(session, t.user.c_str(), userLen, nullptr,
                                                           key.c_str(),
                                                           secret.empty() ? nullptr : secret.c_str());
                }
            }
            if (!secret.empty() && !libssh2_userauth_authenticated(session)) {
                if (offered.find("password") != std::string::npos) {
                    libssh2_userauth_password_ex(session, t.user.c_str(), userLen, secret.c_str(),
                                                 static_cast<unsigned int>(secret.size()), nullptr);
                }
                if (!libssh2_userauth_authenticated(session) &&
                    offered.find("keyboard-interactive") != std::string::npos) {
                    libssh2_userauth_keyboard_interactive_ex(session, t.user.c_str(), userLen,
                                                             answerWithSecret);
                }
            }
            if (libssh2_userauth_authenticated(session)) {
                return true;
            }
            error.needsSecret = true;
            error.message = secret.empty()
                                ? "No key was accepted for " + t.user + "@" + t.host +
                                      ". Enter the password, or the passphrase of your key."
                                : "Sign-in to " + t.user + "@" + t.host + " failed.";
            return false;
        }

        std::shared_ptr<Session> connect(const std::string& name, const std::string& secret,
                                         Error& error) {
            static std::once_flag once;
            std::call_once(once, [] { libssh2_init(0); }); // never exited: channels close late
            const Target t = resolve(name);
            auto out = std::make_shared<Session>();
            out->impl = std::make_unique<Session::Impl>();
            Session::Impl& i = *out->impl;
            i.secret = secret;
            i.sock = connectTo(t.host, t.port, error.message);
            if (i.sock < 0) {
                return nullptr;
            }
            i.session = libssh2_session_init_ex(nullptr, nullptr, nullptr, &i.secret);
            if (!i.session) {
                error.message = "Could not start an ssh session";
                return nullptr;
            }
            libssh2_session_set_blocking(i.session, 1);
            libssh2_session_set_timeout(i.session, 15000);
            if (libssh2_session_handshake(i.session, i.sock) != 0) {
                error.message = "ssh handshake with " + t.host + " failed: " + lastError(i.session);
                return nullptr;
            }
            if (error.message = checkHostKey(i.session, t.host, t.port); !error.message.empty()) {
                return nullptr;
            }
            if (!authenticate(i.session, t, secret, error)) {
                return nullptr;
            }
            libssh2_session_set_blocking(i.session, 0);
            libssh2_keepalive_config(i.session, 1, 30);
            return out;
        }

        // an exec channel as newline-delimited lines
        class ChannelTransport : public acp::Transport {
        public:
            ChannelTransport(std::shared_ptr<Session> session, LIBSSH2_CHANNEL* channel,
                             std::function<void(const std::string&)> onStderr)
                : session_(std::move(session)), channel_(channel), onStderr_(std::move(onStderr)) {}

            ~ChannelTransport() override {
                close();
                retry(*session_->impl, [&] { return libssh2_channel_free(channel_); }, 2000);
            }

            bool readLine(std::string& line) override {
                Session::Impl& i = *session_->impl;
                char buf[16384];
                for (;;) {
                    if (takeLine(out_, line)) {
                        return true;
                    }
                    if (closed_) {
                        return false;
                    }
                    long n = 0, e = 0;
                    bool eof = false;
                    {
                        std::lock_guard lock(i.mutex);
                        n = libssh2_channel_read(channel_, buf, sizeof(buf));
                        if (n > 0) {
                            out_.append(buf, static_cast<size_t>(n));
                        }
                        char errBuf[4096];
                        e = libssh2_channel_read_stderr(channel_, errBuf, sizeof(errBuf));
                        if (e > 0) {
                            err_.append(errBuf, static_cast<size_t>(e));
                        }
                        eof = libssh2_channel_eof(channel_) != 0;
                        int next = 0;
                        libssh2_keepalive_send(i.session, &next);
                    }
                    for (std::string l; onStderr_ && takeLine(err_, l);) {
                        onStderr_(l);
                    }
                    const bool failed = (n < 0 && n != LIBSSH2_ERROR_EAGAIN);
                    if (failed && n != LIBSSH2_ERROR_CHANNEL_CLOSED) {
                        i.dead = true; // the connection itself went away
                    }
                    if (n > 0 || e > 0) {
                        continue;
                    }
                    if (failed || eof) {
                        closed_ = true;
                        continue; // one more pass: a complete line may still be buffered
                    }
                    waitSocket(i.sock);
                }
            }

            bool writeLine(const std::string& line) override {
                const std::string data = line + "\n";
                size_t sent = 0;
                while (sent < data.size() && !closed_) {
                    const long n = retry(*session_->impl, [&] {
                        return static_cast<long>(
                            libssh2_channel_write(channel_, data.data() + sent, data.size() - sent));
                    });
                    if (n < 0) {
                        return false;
                    }
                    sent += static_cast<size_t>(n);
                }
                return sent == data.size();
            }

            // the remote command sees its stdin end and the channel close: agents exit on that
            void close() override {
                if (closed_.exchange(true)) {
                    return;
                }
                retry(*session_->impl, [&] { return libssh2_channel_close(channel_); }, 2000);
            }

            [[nodiscard]] bool isOpen() const override {
                return !closed_;
            }

        private:
            static bool takeLine(std::string& from, std::string& line) {
                const auto nl = from.find('\n');
                if (nl == std::string::npos) {
                    return false;
                }
                line = from.substr(0, nl);
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                from.erase(0, nl + 1);
                return true;
            }

            std::shared_ptr<Session> session_;
            LIBSSH2_CHANNEL* channel_;
            std::function<void(const std::string&)> onStderr_;
            std::string out_, err_;
            std::atomic<bool> closed_{false};
        };

        // a channel with a pty: the bytes of a remote shell
        class ChannelPty : public Pty {
        public:
            ChannelPty(std::shared_ptr<Session> session, LIBSSH2_CHANNEL* channel)
                : session_(std::move(session)), channel_(channel) {}

            ~ChannelPty() override {
                close();
                retry(*session_->impl, [&] { return libssh2_channel_close(channel_); }, 2000);
                retry(*session_->impl, [&] { return libssh2_channel_free(channel_); }, 2000);
            }

            long read(char* buf, size_t size) override {
                Session::Impl& i = *session_->impl;
                while (!closed_) {
                    long n = 0;
                    bool eof = false;
                    {
                        std::lock_guard lock(i.mutex);
                        n = libssh2_channel_read(channel_, buf, size);
                        eof = libssh2_channel_eof(channel_) != 0;
                        int next = 0;
                        libssh2_keepalive_send(i.session, &next);
                    }
                    if (n > 0) {
                        return n;
                    }
                    if (n < 0 && n != LIBSSH2_ERROR_EAGAIN) {
                        if (n != LIBSSH2_ERROR_CHANNEL_CLOSED) {
                            i.dead = true; // the connection itself went away
                        }
                        return n;
                    }
                    if (eof) {
                        return 0;
                    }
                    waitSocket(i.sock);
                }
                return 0;
            }

            bool write(const char* data, size_t size) override {
                size_t sent = 0;
                while (sent < size && !closed_) {
                    const long n = retry(*session_->impl, [&] {
                        return static_cast<long>(
                            libssh2_channel_write(channel_, data + sent, size - sent));
                    });
                    if (n < 0) {
                        return false;
                    }
                    sent += static_cast<size_t>(n);
                }
                return sent == size;
            }

            void resize(int cols, int rows) override {
                retry(*session_->impl, [&] {
                    return static_cast<long>(libssh2_channel_request_pty_size(channel_, cols, rows));
                });
            }

            void close() override {
                closed_ = true;
            }

        private:
            std::shared_ptr<Session> session_;
            LIBSSH2_CHANNEL* channel_;
            std::atomic<bool> closed_{false};
        };

        LIBSSH2_SFTP* sftp(Session::Impl& i, std::string& error) {
            if (!i.sftp) {
                i.sftp = retry(i, [&] { return libssh2_sftp_init(i.session); });
                if (!i.sftp) {
                    std::lock_guard lock(i.mutex);
                    error = "Could not open sftp: " + lastError(i.session);
                }
            }
            return i.sftp;
        }

        std::mutex cacheMutex;
        std::map<std::string, std::shared_ptr<Session>> cache;
        std::map<std::string, std::string> secrets;
    } // namespace

    Session::~Session() {
        if (impl && impl->session) {
            libssh2_session_set_blocking(impl->session, 1);
            libssh2_session_set_timeout(impl->session, 2000);
            if (impl->sftp) {
                libssh2_sftp_shutdown(impl->sftp);
            }
            libssh2_session_disconnect(impl->session, "bye");
            libssh2_session_free(impl->session);
        }
        if (impl && impl->sock >= 0) {
            close(impl->sock);
        }
    }

    bool Session::alive() const {
        return !impl->dead;
    }

    std::string Session::realpath(const std::string& path, std::string& error) {
        LIBSSH2_SFTP* s = sftp(*impl, error);
        if (!s) {
            return "";
        }
        char buf[4096];
        const long n = retry(*impl, [&] {
            return static_cast<long>(libssh2_sftp_realpath(s, path.c_str(), buf, sizeof(buf)));
        });
        if (n <= 0) {
            error = "No such folder: " + path;
            return "";
        }
        return std::string(buf, static_cast<size_t>(n));
    }

    std::vector<Entry> Session::list(const std::string& path, std::string& error) {
        std::vector<Entry> out;
        LIBSSH2_SFTP* s = sftp(*impl, error);
        if (!s) {
            return out;
        }
        LIBSSH2_SFTP_HANDLE* dir = retry(*impl, [&] { return libssh2_sftp_opendir(s, path.c_str()); });
        if (!dir) {
            error = "Could not open " + path;
            return out;
        }
        for (;;) {
            char name[1024];
            LIBSSH2_SFTP_ATTRIBUTES attrs{};
            const long n = retry(*impl, [&] {
                return static_cast<long>(
                    libssh2_sftp_readdir_ex(dir, name, sizeof(name), nullptr, 0, &attrs));
            });
            if (n <= 0) {
                break;
            }
            const std::string entry(name, static_cast<size_t>(n));
            if (entry == "." || entry == "..") {
                continue;
            }
            bool isDir = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
                         LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
            if ((attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) &&
                LIBSSH2_SFTP_S_ISLNK(attrs.permissions)) {
                // a link to a folder is a folder to the user
                const std::string full = path + "/" + entry;
                LIBSSH2_SFTP_ATTRIBUTES target{};
                isDir = retry(*impl, [&] {
                            return static_cast<long>(libssh2_sftp_stat(s, full.c_str(), &target));
                        }) == 0 &&
                        LIBSSH2_SFTP_S_ISDIR(target.permissions);
            }
            out.push_back({entry, isDir});
        }
        retry(*impl, [&] { return static_cast<long>(libssh2_sftp_closedir(dir)); });
        std::ranges::sort(out, [](const Entry& a, const Entry& b) {
            return a.dir != b.dir ? a.dir : a.name < b.name;
        });
        return out;
    }

    std::vector<std::string> Session::listDirs(const std::string& path, std::string& error) {
        std::vector<std::string> out;
        for (auto& entry : list(path, error)) {
            if (entry.dir) {
                out.push_back(std::move(entry.name));
            }
        }
        return out;
    }

    std::string Session::read(const std::string& path, size_t cap, std::string& error) {
        std::string out;
        LIBSSH2_SFTP* s = sftp(*impl, error);
        if (!s) {
            return out;
        }
        LIBSSH2_SFTP_HANDLE* file =
            retry(*impl, [&] { return libssh2_sftp_open(s, path.c_str(), LIBSSH2_FXF_READ, 0); });
        if (!file) {
            error = "Could not open " + path;
            return out;
        }
        while (out.size() < cap) {
            char buf[32768];
            const long n = retry(*impl, [&] {
                return static_cast<long>(
                    libssh2_sftp_read(file, buf, std::min(sizeof(buf), cap - out.size())));
            });
            if (n < 0) {
                error = "Could not read " + path;
            }
            if (n <= 0) {
                break;
            }
            out.append(buf, static_cast<size_t>(n));
        }
        retry(*impl, [&] { return static_cast<long>(libssh2_sftp_close(file)); });
        return out;
    }

    bool Session::write(const std::string& path, const std::string& bytes, std::string& error) {
        LIBSSH2_SFTP* s = sftp(*impl, error);
        if (!s) {
            return false;
        }
        const std::string dir = path.substr(0, path.find_last_of('/'));
        retry(*impl, [&] { return static_cast<long>(libssh2_sftp_mkdir(s, dir.c_str(), 0700)); }); // there already: fine
        LIBSSH2_SFTP_HANDLE* file = retry(*impl, [&] {
            return libssh2_sftp_open(s, path.c_str(), LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC, 0600);
        });
        if (!file) {
            error = "Could not write " + path;
            return false;
        }
        size_t sent = 0;
        while (sent < bytes.size()) {
            const long n = retry(*impl, [&] {
                return static_cast<long>(libssh2_sftp_write(file, bytes.data() + sent, bytes.size() - sent));
            });
            if (n <= 0) {
                error = "Could not write " + path;
                break;
            }
            sent += static_cast<size_t>(n);
        }
        retry(*impl, [&] { return static_cast<long>(libssh2_sftp_close(file)); });
        return sent == bytes.size();
    }

    std::unique_ptr<acp::Transport> Session::exec(const std::string& command,
                                                  std::function<void(const std::string&)> onStderr,
                                                  std::string& error) {
        LIBSSH2_CHANNEL* channel =
            retry(*impl, [&] { return libssh2_channel_open_session(impl->session); });
        if (!channel) {
            std::lock_guard lock(impl->mutex);
            error = "Could not open an ssh channel: " + lastError(impl->session);
            impl->dead = true;
            return nullptr;
        }
        const long rc = retry(*impl, [&] {
            return static_cast<long>(libssh2_channel_exec(channel, command.c_str()));
        });
        if (rc != 0) {
            error = "Could not start the command on the host";
            retry(*impl, [&] { return libssh2_channel_free(channel); }, 2000);
            return nullptr;
        }
        return std::make_unique<ChannelTransport>(shared_from_this(), channel, std::move(onStderr));
    }

    std::unique_ptr<Pty> Session::shell(int cols, int rows, const std::string& cwd,
                                        std::string& error) {
        LIBSSH2_CHANNEL* channel =
            retry(*impl, [&] { return libssh2_channel_open_session(impl->session); });
        if (!channel) {
            std::lock_guard lock(impl->mutex);
            error = "Could not open an ssh channel: " + lastError(impl->session);
            impl->dead = true;
            return nullptr;
        }
        std::string dir = "'";
        for (const char c : cwd) {
            dir += c == '\'' ? std::string("'\\''") : std::string(1, c);
        }
        // ';' not '&&': a folder that is gone still gets a shell
        const std::string command = "cd " + dir + "'; exec $SHELL -l";
        const char term[] = "xterm-256color";
        const long pty = retry(*impl, [&] {
            return static_cast<long>(libssh2_channel_request_pty_ex(
                channel, term, sizeof(term) - 1, nullptr, 0, cols, rows, 0, 0));
        });
        const long rc = pty != 0 ? pty : retry(*impl, [&] {
            return static_cast<long>(libssh2_channel_exec(channel, command.c_str()));
        });
        if (rc != 0) {
            error = "Could not start a shell on the host";
            retry(*impl, [&] { return libssh2_channel_free(channel); }, 2000);
            return nullptr;
        }
        return std::make_unique<ChannelPty>(shared_from_this(), channel);
    }

    std::shared_ptr<Session> session(const std::string& host, const std::string& secret,
                                     Error& error) {
        std::string useSecret = secret;
        {
            std::lock_guard lock(cacheMutex);
            if (const auto it = cache.find(host); it != cache.end() && it->second->alive()) {
                return it->second;
            }
            if (useSecret.empty()) {
                useSecret = secrets[host];
            }
        }
        auto made = connect(host, useSecret, error);
        if (made) {
            std::lock_guard lock(cacheMutex);
            cache[host] = made;
            secrets[host] = useSecret;
        }
        return made;
    }

    std::string destination(const std::string& typed) {
        std::string user, host, port;
        std::string word, flag;
        auto take = [&] {
            if (word.empty()) {
                return;
            }
            if (!flag.empty()) { // the value of the option before it
                (flag == "-p" ? port : flag == "-l" ? user : word) = word;
                flag.clear();
            } else if (word == "-p" || word == "-l" || word == "-i" || word == "-o" || word == "-J") {
                flag = word;
            } else if (word != "ssh" && word.front() != '-') {
                host = word;
            }
            word.clear();
        };
        for (const char c : typed) {
            if (c == ' ' || c == '\t') {
                take();
            } else {
                word += c;
            }
        }
        take();
        if (!user.empty() && host.find('@') == std::string::npos) {
            host = user + "@" + host;
        }
        return port.empty() || host.empty() ? host : host + ":" + port;
    }

    bool connected(const std::string& host) {
        std::lock_guard lock(cacheMutex);
        const auto it = cache.find(host);
        return it != cache.end() && it->second->alive();
    }

    std::vector<std::string> configHosts() {
        std::vector<std::string> out;
        const char* config = std::getenv("IMCODE_SSH_CONFIG");
        for (const auto& entry : parseSSHConfig(config ? config : "")) {
            out.push_back(entry.alias);
        }
        return out;
    }

} // namespace Ssh
