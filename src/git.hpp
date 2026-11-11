#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "diff.hpp"

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
    // a file that differs from the last commit
    struct Change {
        std::string path;   // under the repository
        char kind = 'M';    // 'M' modified, 'A' added, 'D' deleted, 'R' renamed, '?' not tracked
        int added = -1, removed = -1; // lines; -1: not counted (binary, not tracked)
    };
    // from `git status --porcelain=v1 -z`-less output and `git diff HEAD --numstat`
    std::vector<Change> parseChanges(std::string_view porcelain, std::string_view numstat);
    // the changes of a folder's repository, here or on an ssh host; blocks
    std::vector<Change> changes(const std::string& host, const std::string& cwd);
    // a unified diff as lines to show: hunk headers become '~', file headers are dropped
    std::vector<Diff::Line> parseDiff(std::string_view unified);
    // one file's diff against the last commit (all added when it is not tracked); blocks
    std::vector<Diff::Line> fileDiff(const std::string& host, const std::string& cwd, const Change& change);

    // every change committed with `message`; then, with push, the branch pushed (its upstream
    // set the first time). false with git's own words in `output`; blocks
    bool commit(const std::string& host, const std::string& cwd, const std::string& message, bool push,
                std::string& output);
    // a pull request for the pushed branch, by the forge's command line (gh), its title and
    // body from the commits. the request's address, or empty with what gh said; blocks
    std::string pullRequest(const std::string& host, const std::string& cwd, std::string& output);

    // a branch-safe name from a prompt: its first words, and a few characters to keep it unique
    std::string slug(const std::string& text);
} // namespace Git
