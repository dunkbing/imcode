// a small edit in a longer text: the changed lines, their context, a gap marker
#include "../src/diff.hpp"
#include <cstdio>
#include <string>

int main() {
    std::string before, after;
    for (int i = 1; i <= 20; ++i) {
        before += "line " + std::to_string(i) + "\n";
        after += (i == 3 ? "line three\n" : i == 15 ? "" : "line " + std::to_string(i) + "\n");
    }
    after += "tail\n";
    std::string got;
    for (const auto& line : Diff::lines(before, after, 1)) {
        got += line.kind;
        got += line.text + "|";
    }
    const std::string want = " line 2|-line 3|+line three| line 4|~| line 14|-line 15| line 16|~| line 20|+tail|";
    const bool same = Diff::lines("a\nb\n", "a\nb\n").empty();
    const bool created = Diff::lines("", "x\ny\n").size() == 2;
    if (got != want || !same || !created) {
        std::printf("FAILED:\n%s\n%s\nsame=%d created=%d\n", got.c_str(), want.c_str(), same, created);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
