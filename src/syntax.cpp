#include "syntax.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace Syntax {

    namespace {
        struct Rules {
            std::string_view lineComment;
            std::string_view quotes;
            bool blockComments = false;
            bool anyCase = false; // keywords
            std::vector<std::string_view> keywords;
        };

        const Rules& rulesOf(Lang lang) {
            static const Rules c{
                "//", "\"'", true, false,
                {"alignas", "alignof", "auto", "bool", "break", "case", "catch", "char", "class",
                 "concept", "const", "constexpr", "continue", "decltype", "default", "delete", "do",
                 "double", "else", "enum", "explicit", "extern", "false", "float", "for", "friend",
                 "goto", "if", "inline", "int", "long", "mutable", "namespace", "new", "noexcept",
                 "nullptr", "operator", "override", "private", "protected", "public", "requires",
                 "return", "short", "signed", "sizeof", "static", "struct", "switch", "template",
                 "this", "throw", "true", "try", "typedef", "typename", "union", "unsigned", "using",
                 "virtual", "void", "volatile", "while"}};
            static const Rules cmake{
                "#", "\"", false, true,
                {"add_executable", "add_library", "add_subdirectory", "add_test", "else", "elseif",
                 "endforeach", "endfunction", "endif", "endmacro", "endwhile", "file", "find_package",
                 "foreach", "function", "if", "include", "install", "macro", "message", "option",
                 "project", "return", "set", "target_compile_definitions", "target_include_directories",
                 "target_link_libraries", "while"}};
            static const Rules json{"", "\"", false, false, {"true", "false", "null"}};
            static const Rules shell{
                "#", "\"'", false, false,
                {"case", "do", "done", "elif", "else", "esac", "exit", "export", "fi", "for",
                 "function", "if", "in", "local", "return", "then", "while"}};
            static const Rules none{};
            return lang == Lang::C ? c : lang == Lang::CMake ? cmake : lang == Lang::Json ? json
                   : lang == Lang::Shell ? shell : none;
        }

        bool word(char ch) {
            return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
        }

        void add(std::vector<Span>& out, int begin, int end, Kind kind) {
            if (end <= begin) {
                return;
            }
            if (!out.empty() && out.back().kind == kind) {
                out.back().end = end;
            } else {
                out.push_back({begin, end, kind});
            }
        }

        // headings, fenced blocks and `code`
        void markdown(std::string_view text, bool& carry, std::vector<Span>& out) {
            const int n = static_cast<int>(text.size());
            if (text.starts_with("```")) {
                carry = !carry;
                add(out, 0, n, Kind::Comment);
            } else if (carry) {
                add(out, 0, n, Kind::String);
            } else if (text.starts_with("#")) {
                add(out, 0, n, Kind::Keyword);
            } else {
                for (int i = 0; i < n;) {
                    const size_t close = text[i] == '`' ? text.find('`', i + 1) : std::string_view::npos;
                    const int end = close == std::string_view::npos ? i + 1 : static_cast<int>(close) + 1;
                    add(out, i, end, end > i + 1 ? Kind::String : Kind::Text);
                    i = end;
                }
            }
        }
    } // namespace

    Lang langOf(std::string_view fileName) {
        const size_t dot = fileName.rfind('.');
        const std::string_view ext = dot == std::string_view::npos ? "" : fileName.substr(dot + 1);
        const auto in = [&](std::initializer_list<std::string_view> list) {
            return std::ranges::find(list, ext) != list.end();
        };
        return fileName == "CMakeLists.txt" || ext == "cmake"                         ? Lang::CMake
               : in({"c", "h", "cc", "cpp", "cxx", "hh", "hpp", "hxx", "m", "mm"})     ? Lang::C
               : ext == "json"                                                         ? Lang::Json
               : ext == "md"                                                           ? Lang::Markdown
               : in({"sh", "bash", "zsh"})                                             ? Lang::Shell
                                                                                       : Lang::None;
    }

    void line(Lang lang, std::string_view text, bool& carry, std::vector<Span>& out) {
        out.clear();
        const int n = static_cast<int>(text.size());
        if (lang == Lang::Markdown) {
            markdown(text, carry, out);
            return;
        }
        const Rules& r = rulesOf(lang);
        for (int i = 0; i < n;) {
            const std::string_view rest = text.substr(i);
            if (carry || (r.blockComments && rest.starts_with("/*"))) {
                const size_t close = text.find("*/", carry ? i : i + 2);
                carry = close == std::string_view::npos;
                const int end = carry ? n : static_cast<int>(close) + 2;
                add(out, i, end, Kind::Comment);
                i = end;
            } else if (!r.lineComment.empty() && rest.starts_with(r.lineComment) &&
                       // a # inside a shell word ($#, a#b) is not a comment
                       (r.lineComment != "#" || i == 0 || text[i - 1] == ' ' || text[i - 1] == '\t')) {
                add(out, i, n, Kind::Comment);
                i = n;
            } else if (r.quotes.find(text[i]) != std::string_view::npos) {
                int end = i + 1;
                while (end < n && text[end] != text[i]) {
                    end += text[end] == '\\' ? 2 : 1;
                }
                end = std::min(n, end + 1);
                add(out, i, end, Kind::String);
                i = end;
            } else if (word(text[i])) {
                int end = i;
                while (end < n && (word(text[end]) || (text[end] == '.' && std::isdigit(static_cast<unsigned char>(text[i]))))) {
                    ++end;
                }
                std::string token(text.substr(i, end - i));
                if (r.anyCase) {
                    std::ranges::transform(token, token.begin(), [](unsigned char ch) { return std::tolower(ch); });
                }
                add(out, i, end,
                    std::isdigit(static_cast<unsigned char>(text[i]))  ? Kind::Number
                    : std::ranges::find(r.keywords, token) != r.keywords.end() ? Kind::Keyword
                                                                       : Kind::Text);
                i = end;
            } else {
                add(out, i, i + 1, Kind::Text);
                ++i;
            }
        }
    }

} // namespace Syntax
