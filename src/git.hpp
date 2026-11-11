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
    // a new worktree of the repository in `repo` on its own branch imcode/<name>, kept under
    // ~/.imcode/worktrees/<repository>/<name> on that machine. returns its folder; blocks.
    // empty with an error when git refuses
    std::string addWorktree(const std::string& host, const std::string& repo, const std::string& name,
                            std::string& error);
    // remove a worktree of `repo` (its branch stays). git refuses when it holds changes that
    // are not committed: false with git's own words; blocks
    bool removeWorktree(const std::string& host, const std::string& repo, const std::string& path,
                        std::string& error);
    // a branch-safe name from a prompt: its first words, and a few characters to keep it unique
    std::string slug(const std::string& text);
} // namespace Git
