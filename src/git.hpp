#pragma once

#include <string>
#include <string_view>

// what a folder's repository says about itself, for the line under the composer
namespace Git {
    struct Status {
        bool repo = false;
        std::string branch; // "main", or "detached" with no branch
        int changed = 0;    // files modified, added, removed or untracked
    };
    // from the output of `git status --porcelain=v1 -b`
    Status parse(std::string_view porcelain);
    // of a folder here (host empty) or on an ssh host; blocks
    Status status(const std::string& host, const std::string& cwd);
} // namespace Git
