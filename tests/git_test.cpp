// the branch and the count of changed files from git's porcelain status; a worktree of a
// scratch repository
#include "../src/git.hpp"
#include "../src/git_graph.hpp"
#include "../src/ssh.hpp"
#include <cstdio>
#include <exception>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <algorithm>
#ifdef _WIN32
#include "acp_host/process_runner.hpp"
#include <windows.h>
#endif

namespace {
    // a posix shell line's exit code: windows runs it in git for windows' sh, not cmd
    int shell(const std::string& line) {
#ifdef _WIN32
        char git[MAX_PATH] = "";
        SearchPathA(nullptr, "git.exe", nullptr, MAX_PATH, git, nullptr);
        const std::string sh = (std::filesystem::path(git).parent_path().parent_path() / "bin" / "sh.exe").string();
        const ProcessResult r = ProcessRunner::run({.args = {sh, "-c", line}});
        if (!r.success) {
            std::printf("sh failed (%d): %s %s\n", r.exitCode, r.output.c_str(), r.errorMessage.c_str());
        }
        return r.success ? 0 : 1;
#else
        return std::system(line.c_str());
#endif
    }
} // namespace

static int run() {
    const Git::Status a = Git::parse("## main...origin/main [ahead 1]\n M src/a.cpp\n?? new.txt\n");
    const Git::Status b = Git::parse("## No commits yet on trunk\n");
    const Git::Status c = Git::parse("## HEAD (no branch)\nA  x\n");
    const Git::Status d = Git::parse("## feature/x\n");
    const Git::Status none = Git::parse("");
    bool ok = a.repo && a.branch == "main" && a.changed == 2 && b.branch == "trunk" && b.changed == 0 &&
              c.branch == "detached" && c.changed == 1 && d.branch == "feature/x" && !none.repo;

    // a host as typed, or as a command copied from a terminal
    ok = ok && Ssh::destination("me@box") == "me@box" && Ssh::destination("ssh me@box") == "me@box" &&
         Ssh::destination("  ssh -p 2222 -i ~/.ssh/k me@box ") == "me@box:2222" &&
         Ssh::destination("ssh -l me box") == "me@box" && Ssh::destination("devbox") == "devbox";

    namespace fs = std::filesystem;
    const fs::path home = fs::temp_directory_path() / "imcode-git-test";
    std::error_code gone; // windows: a git process can still hold the folder for a moment
    fs::remove_all(home, gone);
    const fs::path repo = home / "it's a repo";
    fs::create_directories(repo);
    setenv("HOME", home.c_str(), 1);
    const std::string init = "cd '" + home.string() + "/it'\\''s a repo' && git init -q -b main . && "
                             "git -c user.email=t@t -c user.name=t commit -q --allow-empty -m first";
    ok = shell(init) == 0 && ok;
    const std::string name = Git::slug("Fix the Login test!\nmore");
    std::string error;
    const std::string made = Git::addWorktree("", repo.string(), name, error);
    const Git::Status in = made.empty() ? Git::Status{} : Git::status("", made);
    std::string again;
    const bool refused = Git::addWorktree("", repo.string(), name, again).empty() && !again.empty();
    ok = ok && name.starts_with("fix-the-login-test-") && fs::is_directory(made) && in.repo &&
         in.branch == "imcode/" + name && refused;
    if (!ok) {
        std::printf("FAILED: worktree '%s' (%s) branch %s refused %d\n", made.c_str(), error.c_str(), in.branch.c_str(), refused);
    }
    // a worktree with uncommitted work is kept; a clean one goes, its branch stays
    std::ofstream(fs::path(made) / "new.txt") << "x";
    std::string kept;
    const bool refusedDirty = !Git::removeWorktree("", repo.string(), made, kept) && !kept.empty();
    fs::remove(fs::path(made) / "new.txt");

    // what changed, and one file's diff: a tracked file edited, a file not tracked yet
    std::ofstream(repo / "a.txt") << "one\ntwo\nthree\n";
    const std::string commit = "cd '" + home.string() + "/it'\\''s a repo' && git add a.txt && "
                               "git -c user.email=t@t -c user.name=t commit -q -m a";
    ok = shell(commit) == 0 && ok;
    std::ofstream(repo / "a.txt") << "one\n2\nthree\nfour\n";
    std::ofstream(repo / "b c.txt") << "new\n";
    const auto found = Git::changes("", repo.string());
    const auto edit = std::ranges::find(found, std::string("a.txt"), &Git::Change::path);
    const auto fresh = std::ranges::find(found, std::string("b c.txt"), &Git::Change::path);
    const bool listed = found.size() == 2 && edit != found.end() && edit->kind == 'M' && edit->added == 2 &&
                        edit->removed == 1 && fresh != found.end() && fresh->kind == '?';
    const auto lines = listed ? Git::fileDiff("", repo.string(), *edit) : std::vector<Diff::Line>{};
    const auto added = listed ? Git::fileDiff("", repo.string(), *fresh) : std::vector<Diff::Line>{};
    const bool diffed = lines.size() == 5 && lines[1].kind == '-' && lines[1].text == "two" && lines[2].kind == '+' &&
                        lines[2].text == "2" && lines[4].text == "four" && added.size() == 1 && added[0].kind == '+' &&
                        added[0].text == "new";
    if (!listed || !diffed) {
        std::printf("FAILED: changes %zu, diff %zu, new %zu\n", found.size(), lines.size(), added.size());
        for (const auto& f : found) {
            std::printf("  %c [%s] +%d -%d\n", f.kind, f.path.c_str(), f.added, f.removed);
        }
        for (const auto& l : lines) {
            std::printf("  %c%s\n", l.kind, l.text.c_str());
        }
        ok = false;
    }
    // what a turn changed: two snapshots, the files between them (new ones too), the index untouched
    const std::string before = Git::snapshot("", repo.string());
    std::ofstream(repo / "a.txt") << "one\n2\nthree\nfour\nfive\n";
    std::ofstream(repo / "turn.txt") << "made by the turn\n";
    const std::string after = Git::snapshot("", repo.string());
    const auto turn = Git::changes("", repo.string(), before, after);
    const auto turnNew = std::ranges::find(turn, std::string("turn.txt"), &Git::Change::path);
    const auto turnDiff = turnNew == turn.end() ? std::vector<Diff::Line>{} : Git::fileDiff("", repo.string(), *turnNew, before, after);
    const auto now = Git::changes("", repo.string());
    const auto still = std::ranges::find(now, std::string("turn.txt"), &Git::Change::path);
    // a.txt, b c.txt, turn.txt; and turn.txt still untracked: the snapshot did not stage it
    const bool indexUntouched = now.size() == 3 && still != now.end() && still->kind == '?';
    if (before.size() < 40 || after == before || turn.size() != 2 || turnNew == turn.end() || turnNew->kind != 'A' ||
        turnDiff.size() != 1 || !indexUntouched) {
        std::printf("FAILED: turn %zu files, before %s after %s\n", turn.size(), before.c_str(), after.c_str());
        ok = false;
    }
    fs::remove(repo / "turn.txt");
    const bool renamed = Git::nameStatusToPorcelain("R100\told.txt\tnew.txt\nM\tx\n") == "R  old.txt -> new.txt\nM  x\n";
    ok = ok && renamed && Git::snapshot("", home.string()).empty();

    // committed and pushed to a remote of its own: the first push sets the upstream
    const fs::path bare = home / "remote.git";
    const auto q = [](const fs::path& p) {
        std::string out = "'";
        for (const char c : p.string()) {
            out += c == '\'' ? std::string("'\\''") : std::string(1, c);
        }
        return out + "'";
    };
    ok = shell(("git init -q --bare " + q(bare) + " && git -C " + q(repo) + " remote add origin " + q(bare))) == 0 && ok;
    setenv("GIT_AUTHOR_NAME", "t", 1);
    setenv("GIT_AUTHOR_EMAIL", "t@t", 1);
    setenv("GIT_COMMITTER_NAME", "t", 1);
    setenv("GIT_COMMITTER_EMAIL", "t@t", 1);
    std::string said;
    const bool committed = Git::commit("", repo.string(), "it's done", true, said);
    const bool clean = Git::changes("", repo.string()).empty();
    std::string again2;
    std::ofstream(repo / "a.txt") << "five\n";
    const bool second = Git::commit("", repo.string(), "more", true, again2); // to the upstream now set
    std::string nothing;
    const bool empty = !Git::commit("", repo.string(), "nothing", false, nothing) && !nothing.empty();
    const bool pushed = shell(("git -C " + q(bare) + " log --oneline main | grep -q more")) == 0;
    if (!committed || !clean || !second || !empty || !pushed) {
        std::printf("FAILED: commit %d clean %d second %d refused-empty %d pushed %d: %s | %s\n", committed, clean, second,
                    empty, pushed, said.c_str(), again2.c_str());
        ok = false;
    }
    // parsers: renames, quoted paths, binary files, hunk breaks
    const auto parsed = Git::parseChanges("R  old.txt -> new.txt\n?? \"has space.txt\"\n D gone\n",
                                          "3\t1\tnew.txt\n-\t-\tpic.png\n");
    ok = ok && parsed.size() == 3 && parsed[0].kind == 'R' && parsed[0].path == "new.txt" && parsed[0].added == 3 &&
         parsed[1].kind == '?' && parsed[1].path == "has space.txt" && parsed[2].kind == 'D' && parsed[2].added == -1;
    const auto hunks = Git::parseDiff("diff --git a/x b/x\n--- a/x\n+++ b/x\n@@ -1 +1 @@\n-a\n+b\n@@ -9 +9 @@\n c\n");
    ok = ok && hunks.size() == 4 && hunks[0].kind == '-' && hunks[2].kind == '~' && hunks[3].kind == ' ';
    // the history: libgit2 here, and git's own log parsed the way an ssh host's is
    const auto history = Git::log("", repo.string(), 50, 0);
    const std::string logFile = (home / "log.txt").string();
    shell("git -C " + q(repo) + " log --all --topo-order --format='%H%x1f%P%x1f%an%x1f%at%x1f%D%x1f%s%x1e' > " +
          q(logFile));
    std::ifstream logIn(logFile);
    const auto viaCli = Git::parseLog(std::string(std::istreambuf_iterator<char>(logIn), {}));
    bool sameLog = history.size() >= 3 && history.size() == viaCli.size();
    for (const auto& e : history) {
        const auto twin = std::ranges::find(viaCli, e.hash, &Git::LogEntry::hash);
        sameLog = sameLog && twin != viaCli.end() && twin->subject == e.subject && twin->parents == e.parents &&
                  twin->time == e.time && twin->author == e.author;
    }
    const bool refsShown = !history.empty() && history[0].subject == "more" &&
                           std::ranges::find(history[0].refs, std::string("HEAD -> main")) != history[0].refs.end() &&
                           std::ranges::find(history[0].refs, std::string("origin/main")) != history[0].refs.end() &&
                           Git::log("", repo.string(), 1, 1).size() == 1 && Git::log("", home.string(), 5, 0).empty();
    if (!sameLog || !refsShown) {
        std::printf("FAILED: log %zu vs %zu, refs %d\n", history.size(), viaCli.size(), refsShown);
        ok = false;
    }
    // a commit's files: against its first parent, and the first commit against the empty tree
    const auto commitA = std::ranges::find(history, std::string("a"), &Git::LogEntry::subject);
    const auto first = std::ranges::find(history, std::string("first"), &Git::LogEntry::subject);
    bool commitDiff = false;
    if (commitA != history.end() && first != history.end()) {
        const auto files = Git::changes("", repo.string(), commitA->hash + "^", commitA->hash);
        const auto root = Git::changes("", repo.string(), "4b825dc642cb6eb9a060e54bf8d69288fbee4904", first->hash);
        commitDiff = files.size() == 1 && files[0].path == "a.txt" && files[0].kind == 'A' && root.empty();
    }
    if (!commitDiff) {
        std::printf("FAILED: a commit's changes\n");
        ok = false;
    }
    // the graph of a merge: a(b, c), b(d), c(d), d
    const auto graph = Git::layoutGraph({{.hash = "a", .parents = {"b", "c"}},
                                         {.hash = "b", .parents = {"d"}},
                                         {.hash = "c", .parents = {"d"}},
                                         {.hash = "d"}});
    using Lanes = std::vector<int>;
    ok = ok && graph.size() == 4 && graph[0].col == 0 && graph[0].parents == Lanes{0, 1} && graph[0].width == 2 &&
         graph[1].col == 0 && graph[1].pass == Lanes{1} && graph[1].parents == Lanes{0} && graph[2].col == 1 &&
         graph[2].pass == Lanes{0} && graph[2].parents == Lanes{0} && graph[3].col == 0 && graph[3].into == Lanes{0} &&
         graph[3].parents.empty();
    std::string gone;
    ok = ok && refusedDirty && Git::removeWorktree("", repo.string(), made, gone) && !fs::exists(made);
    fs::remove_all(home, gone);
    std::printf(ok ? "ok\n" : "FAILED: %s | %s | %s | %s\n", name.c_str(), made.c_str(), error.c_str(),
                in.branch.c_str());
    return ok ? 0 : 1;
}

int main() {
    try {
        return run();
    } catch (const std::exception& e) { // a filesystem or process error says what went wrong
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
