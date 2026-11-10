#pragma once

#include <string_view>
#include <vector>

// colours for the file viewer: one line at a time, no parser
// shortcut: comments, strings, numbers and a keyword set for five languages; a grammar
// library when types, functions or more languages are wanted
namespace Syntax {

    enum class Lang { None, C, CMake, Json, Markdown, Shell };
    enum class Kind { Text, Comment, String, Number, Keyword };
    struct Span {
        int begin = 0, end = 0; // bytes of the line
        Kind kind{};
    };

    Lang langOf(std::string_view fileName);
    // the spans of one line in order, covering it. carry: the line starts inside a block
    // comment (c) or a fenced block (markdown); set for the line after it
    void line(Lang lang, std::string_view text, bool& carry, std::vector<Span>& out);

} // namespace Syntax
