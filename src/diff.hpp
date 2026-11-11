#pragma once

#include <string>
#include <string_view>
#include <vector>

// what changed between two texts, by line, for showing an edit
namespace Diff {
    struct Line {
        char kind = ' '; // ' ' same, '+' added, '-' removed, '~' lines skipped between hunks
        std::string text;
    };
    // changed lines with `context` unchanged lines around them
    std::vector<Line> lines(std::string_view before, std::string_view after, int context = 3);
} // namespace Diff
