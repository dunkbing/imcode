#pragma once

#include "pty.hpp"
#include <acp/transport.hpp>
#include <functional>
#include <memory>
#include <string>
#include <vector>

// built-in ssh (libssh2): one connection per host, shared by the folder browser (sftp) and
// every agent started there (an exec channel each). safe to call from any thread.
namespace Ssh {

    struct Error {
        std::string message;
        bool needsSecret = false; // a password or key passphrase would let it through
    };

    struct Entry {
        std::string name;
        bool dir = false;
    };

    class Session : public std::enable_shared_from_this<Session> {
    public:
        ~Session();
        [[nodiscard]] bool alive() const;
        // absolute form of a path on the host ("." is the home folder)
        std::string realpath(const std::string& path, std::string& error);
        // names of the folders in path, sorted
        std::vector<std::string> listDirs(const std::string& path, std::string& error);
        // everything in path: folders first, then by name
        std::vector<Entry> list(const std::string& path, std::string& error);
        // the first `cap` bytes of a file
        std::string read(const std::string& path, size_t cap, std::string& error);
        // runs command; its stdin and stdout are the transport, stderr goes to onStderr
        std::unique_ptr<acp::Transport> exec(const std::string& command,
                                             std::function<void(const std::string&)> onStderr,
                                             std::string& error);
        // the login shell in a pty of that size, started in cwd (an absolute folder on the host)
        std::unique_ptr<Pty> shell(int cols, int rows, const std::string& cwd, std::string& error);
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

    // the connection to `host` ("user@host", "user@host:port" or a Host of ~/.ssh/config),
    // opened when there is none. tries the ssh agent, the config's IdentityFile and the
    // default keys, then `secret` as password or passphrase. blocks. a secret that worked
    // is kept in memory for reconnects
    std::shared_ptr<Session> session(const std::string& host, const std::string& secret,
                                     Error& error);
    // there is a live connection to host: session() will not block on the network
    bool connected(const std::string& host);

    // Host names of ~/.ssh/config
    std::vector<std::string> configHosts();

} // namespace Ssh
