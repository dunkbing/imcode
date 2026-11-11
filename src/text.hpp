#pragma once

#include <string>
#include <string_view>

// the first `bytes` bytes of utf-8 text, shortened to end on a whole character: a cut through
// a character (an emoji is four bytes) would show as a replacement mark
inline std::string cut(std::string_view text, size_t bytes) {
    if (text.size() <= bytes) {
        return std::string(text);
    }
    while (bytes > 0 && (static_cast<unsigned char>(text[bytes]) & 0xC0) == 0x80) {
        --bytes; // a continuation byte: the character starts earlier
    }
    return std::string(text.substr(0, bytes));
}
