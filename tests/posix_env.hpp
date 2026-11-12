// windows: the posix setenv the tests use (force-included there by CMakeLists.txt). the wide
// overload takes a path's c_str()
#pragma once
#ifdef _WIN32
#include <cstdlib>
#include <string>
inline int setenv(const char* name, const char* value, int) {
    return _putenv_s(name, value);
}
inline int setenv(const char* name, const wchar_t* value, int) {
    return _wputenv_s(std::wstring(name, name + std::char_traits<char>::length(name)).c_str(), value);
}
#endif
