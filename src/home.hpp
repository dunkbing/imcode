#pragma once

#include <cstdlib>

// the user's home folder, null when unknown: $HOME, else %USERPROFILE% (windows rarely sets HOME)
inline const char* homeEnv() {
    const char* home = std::getenv("HOME");
#ifdef _WIN32
    if (!home || !*home) {
        home = std::getenv("USERPROFILE");
    }
#endif
    return home;
}
