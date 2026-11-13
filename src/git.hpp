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
        std::string from;   // a rename: the old path
        // the index against the last commit, and the folder against the index: git status's two
        // columns. M A D R, '?' (work only: not tracked), or a space for no difference. both
        // blank for what a turn changed
        char index = ' ', work = ' ';
        [[nodiscard]] bool staged() const {
            return index != ' ';
        }
        [[nodiscard]] bool unstaged() const {
            return work != ' ';
        }
    };
    // from `git status --porcelain=v1 -z`-less output and `git diff HEAD --numstat`
    std::vector<Change> parseChanges(std::string_view porcelain, std::string_view numstat);
    // the changes of a folder's repository, here or on an ssh host; blocks. with from and to
    // (trees from snapshot()), what changed between them instead of since the last commit
    std::vector<Change> changes(const std::string& host, const std::string& cwd, const std::string& from = "",
                                const std::string& to = "");
    // every file of the folder as it is now (untracked ones too), as a git tree, without
    // touching the index or the history: what a turn changed is the diff of two of these.
    // empty outside a repository; blocks
    std::string snapshot(const std::string& host, const std::string& cwd);
    // from `git diff --name-status`: as status lines for parseChanges
    std::string nameStatusToPorcelain(std::string_view nameStatus);
    // a unified diff as lines to show: hunk headers become '~', file headers are dropped
    std::vector<Diff::Line> parseDiff(std::string_view unified);
    // one file's diff against the last commit (all added when it is not tracked); blocks
    std::vector<Diff::Line> fileDiff(const std::string& host, const std::string& cwd, const Change& change,
                                     const std::string& from = "", const std::string& to = "");
    // which side of the index a diff shows: all = the folder against the last commit
    enum class Side { All, Staged, Unstaged };
    std::vector<Diff::Line> fileDiff(const std::string& host, const std::string& cwd, const Change& change,
                                     Side side);
    // put the files into the index (a deleted one is removed from it), take them out again, or
    // throw their changes in the folder away (a file not tracked is deleted). false with why;
    // blocks. discard has no way back: the caller asks first
    bool stage(const std::string& host, const std::string& cwd, const std::vector<Change>& files, std::string& error);
    bool unstage(const std::string& host, const std::string& cwd, const std::vector<Change>& files, std::string& error);
    bool discard(const std::string& host, const std::string& cwd, const std::vector<Change>& files, std::string& error);
    // the same as shell commands, run in the repository (what an ssh host gets)
    std::string stageScript(const std::vector<Change>& files);
    std::string unstageScript(const std::vector<Change>& files);
    std::string discardScript(const std::vector<Change>& files);
    // one hunk of a file's diff, staged, unstaged or thrown away: the patch of just that hunk
    // goes to `git apply`. `seen` is the hunk's lines as the user was shown them: a file that
    // changed since is refused instead of patching the wrong place. blocks
    enum class HunkAction { Stage, Unstage, Discard };
    bool applyHunk(const std::string& host, const std::string& cwd, const Change& change, HunkAction action, int hunk,
                   const std::vector<Diff::Line>& seen, std::string& error);
    // the patch for hunk n of a unified diff, with the file's headers; empty when there is no such
    // hunk or its lines are not `seen`
    std::string hunkPatch(std::string_view unified, int n, const std::vector<Diff::Line>& seen);

    // the staged changes committed with `message` (staged_only), else every change; the git
    // command runs, so the repository's hooks and signing do. then, with push, the branch pushed (its upstream
    // set the first time). false with git's own words in `output`; blocks
    bool commit(const std::string& host, const std::string& cwd, const std::string& message, bool push,
                std::string& output, bool stagedOnly = false);
    // a pull request for the pushed branch, by the forge's command line (gh), its title and
    // body from the commits. the request's address, or empty with what gh said; blocks
    std::string pullRequest(const std::string& host, const std::string& cwd, std::string& output);

    // one commit of the history, as the graph lists it
    struct LogEntry {
        std::string hash, subject, author;
        long long time = 0; // unix seconds
        std::vector<std::string> parents;
        std::vector<std::string> refs; // "HEAD -> main", "origin/main", "tag: v1"
    };
    // from `git log --format=%H%x1f%P%x1f%an%x1f%at%x1f%D%x1f%s%x1e`
    std::vector<LogEntry> parseLog(std::string_view text);
    // the history of every branch and tag, newest first with parents after children; `limit`
    // entries from `skip`. here or on an ssh host; blocks
    // with a path: the commits that changed that file (renames followed), of the current branch, by the git command
    std::vector<LogEntry> log(const std::string& host, const std::string& cwd, size_t limit, size_t skip,
                              const std::string& path = {});

    // one commit in full: who, when, the whole message and the files it changed
    struct CommitDetail {
        std::string hash, author, authorEmail, committer, committerEmail, message;
        long long time = 0, committed = 0; // unix seconds: authored, committed
        std::vector<std::string> parents;
        std::vector<Change> files; // against the first parent; a first commit against nothing
    };
    // from `git show -s --format=%H%x1f%P%x1f%an%x1f%ae%x1f%cn%x1f%ce%x1f%at%x1f%ct%x1f%B`
    CommitDetail parseShow(std::string_view text);
    // here or on an ssh host; blocks. an empty hash when the commit is not found
    CommitDetail show(const std::string& host, const std::string& cwd, const std::string& hash);

    // a branch-safe name from a prompt: its first words, and a few characters to keep it unique
    std::string slug(const std::string& text);
} // namespace Git
