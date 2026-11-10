// the file viewer's colours: what kind each piece of a line is
#include "../src/syntax.hpp"
#include <cstdio>
#include <string>

namespace {
    // "k:if t: (x) c:// hi": each span's first letter and its text
    std::string show(Syntax::Lang lang, std::string_view text, bool& carry) {
        std::vector<Syntax::Span> spans;
        Syntax::line(lang, text, carry, spans);
        std::string out;
        int at = 0;
        for (const auto& s : spans) {
            if (s.begin != at) {
                return "gap";
            }
            at = s.end;
            out += std::string(1, "tcsnk"[static_cast<int>(s.kind)]) + ":" +
                   std::string(text.substr(s.begin, s.end - s.begin)) + "|";
        }
        return at == static_cast<int>(text.size()) ? out : "short";
    }

    int failed = 0;
    void expect(Syntax::Lang lang, std::string_view text, const std::string& want, bool carryIn = false,
                bool carryOut = false) {
        bool carry = carryIn;
        const std::string got = show(lang, text, carry);
        if (got != want || carry != carryOut) {
            std::printf("FAIL %.*s\n  got  %s (carry %d)\n  want %s (carry %d)\n", static_cast<int>(text.size()),
                        text.data(), got.c_str(), carry, want.c_str(), carryOut);
            ++failed;
        }
    }
} // namespace

int main() {
    using enum Syntax::Lang;
    expect(C, "if (x == 42) return \"a\\\"b\"; // why", "k:if|t: (x == |n:42|t:) |k:return|t: |s:\"a\\\"b\"|t:; |c:// why|");
    expect(C, "int a; /* open", "k:int|t: a; |c:/* open|", false, true);
    expect(C, "still */ x = 1.5f;", "c:still */|t: x = |n:1.5f|t:;|", true, false);
    expect(C, "all of it", "c:all of it|", true, true);
    expect(C, "char c = '\\''; int_ x1;", "k:char|t: c = |s:'\\''|t:; int_ x1;|");
    expect(C, "\"open", "s:\"open|");
    expect(CMake, "SET(A \"b\") # c", "k:SET|t:(A |s:\"b\"|t:) |c:# c|");
    expect(Json, "{\"a\": [true, -1.5e3]}", "t:{|s:\"a\"|t:: [|k:true|t:, -|n:1.5e3|t:]}|");
    expect(Shell, "echo $# 'x' #done", "t:echo $# |s:'x'|t: |c:#done|");
    expect(Markdown, "# Title", "k:# Title|");
    expect(Markdown, "a `b` c", "t:a |s:`b`|t: c|");
    expect(Markdown, "```sh", "c:```sh|", false, true);
    expect(Markdown, "ls # x", "s:ls # x|", true, true);
    expect(None, "if 1 // x", "t:if |n:1|t: // x|");
    expect(C, "", "");
    if (Syntax::langOf("CMakeLists.txt") != CMake || Syntax::langOf("a/b.hpp") != C ||
        Syntax::langOf("x.json") != Json || Syntax::langOf("README.md") != Markdown ||
        Syntax::langOf("run.sh") != Shell || Syntax::langOf("Makefile") != None) {
        std::printf("FAIL langOf\n");
        ++failed;
    }
    std::printf(failed ? "%d failed\n" : "ok\n", failed);
    return failed ? 1 : 0;
}
