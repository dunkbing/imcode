#pragma once

#include "imshell/editor.hpp"
#include "syntax.hpp"
#include <algorithm>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// a code fence's language name as one of ours
inline Syntax::Lang fenceLang(const std::string& name) {
    static const std::pair<const char*, const char*> files[] = {
        {"c", "a.c"},     {"cpp", "a.cpp"},         {"c++", "a.cpp"},     {"h", "a.h"},   {"hpp", "a.hpp"},
        {"cmake", "CMakeLists.txt"},               {"json", "a.json"},   {"md", "a.md"}, {"markdown", "a.md"},
        {"sh", "a.sh"},   {"bash", "a.sh"},         {"zsh", "a.sh"},      {"shell", "a.sh"}};
    for (const auto& [lang, file] : files) {
        if (name == lang) {
            return Syntax::langOf(file);
        }
    }
    return Syntax::Lang::None;
}

// our line tokenizer as an editor highlighter: a line at a time over the whole text
inline imshell::TextEditor::Highlighter codeColors(Syntax::Lang lang) {
    return [lang](std::string_view text, std::span<imshell::TextEditor::Token> tokens,
                  const imshell::TextEditor::Edit&) {
        using Token = imshell::TextEditor::Token;
        static const Token kinds[] = {Token::Text, Token::Comment, Token::String, Token::Number,
                                      Token::Keyword};
        std::vector<Syntax::Span> spans;
        bool carry = false;
        for (size_t at = 0; at < text.size();) {
            const size_t end = std::min(text.find('\n', at), text.size());
            Syntax::line(lang, text.substr(at, end - at), carry, spans);
            for (const auto& span : spans) {
                std::fill(tokens.begin() + static_cast<long>(at) + span.begin,
                          tokens.begin() + static_cast<long>(at) + span.end,
                          kinds[static_cast<int>(span.kind)]);
            }
            at = end + 1;
        }
    };
}
