#pragma once

#include <string>

// secrets kept by the system: the login keychain on macos. elsewhere nothing is kept yet
// (shortcut: libsecret on linux, the credential manager on windows, when those builds ship)
namespace Keychain {
    // empty when there is none
    std::string read(const std::string& service, const std::string& account);
    bool write(const std::string& service, const std::string& account, const std::string& secret);
    void remove(const std::string& service, const std::string& account);
} // namespace Keychain
