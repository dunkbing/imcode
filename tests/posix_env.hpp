// windows: the posix setenv the tests use (force-included there by CMakeLists.txt)
#pragma once
#ifdef _WIN32
#include <cstdlib>
inline int setenv(const char* name, const char* value, int) {
    return _putenv_s(name, value);
}
#endif
