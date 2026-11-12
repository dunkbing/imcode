#pragma once

#include "git.hpp"

#include <optional>
#include <string>
#include <vector>

// Git's operations for a repository on this machine, through libgit2 (the git command is
// not used). nullopt: the folder is not in a repository. remote projects run git on their
// host instead (git.cpp)
namespace Git::Local {
    std::optional<Status> status(const std::string& cwd);
    std::optional<std::vector<Change>> changes(const std::string& cwd, const std::string& from, const std::string& to);
    std::optional<std::string> fileDiff(const std::string& cwd, const Change& change, const std::string& from,
                                        const std::string& to); // unified text
    // one side of the index: staged = last commit to index, unstaged = index to folder
    std::optional<std::string> fileDiff(const std::string& cwd, const Change& change, Side side);
    bool stage(const std::string& cwd, const std::vector<Change>& files, std::string& error);
    bool unstage(const std::string& cwd, const std::vector<Change>& files, std::string& error);
    bool discard(const std::string& cwd, const std::vector<Change>& files, std::string& error);
    std::optional<std::string> snapshot(const std::string& cwd);
    // commit the index, after adding everything unless `all` is false; false with why. hooks and
    // commit signing are not run (the fallback where there is no git command)
    bool commit(const std::string& cwd, const std::string& message, std::string& output, bool all = true);
    // the history as Git::log gives it
    std::optional<std::vector<LogEntry>> log(const std::string& cwd, size_t limit, size_t skip);
    // tracked files and new ones not ignored, under cwd and relative to it, at most cap
    std::optional<std::vector<std::string>> files(const std::string& cwd, size_t cap);
    // a new branch at HEAD checked out in `folder` (made, with its parents)
    std::string addWorktree(const std::string& repo, const std::string& folder, const std::string& branch,
                            std::string& error);
    bool removeWorktree(const std::string& repo, const std::string& path, std::string& error);
} // namespace Git::Local
