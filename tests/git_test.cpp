// the branch and the count of changed files from git's porcelain status; a worktree of a
// scratch repository
#include "../src/git.hpp"
#include "../src/ssh.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <algorithm>

int main() {
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
    fs::remove_all(home);
    const fs::path repo = home / "it's a repo";
    fs::create_directories(repo);
    setenv("HOME", home.c_str(), 1);
    const std::string init = "cd '" + home.string() + "/it'\\''s a repo' && git init -q -b main . && "
                             "git -c user.email=t@t -c user.name=t commit -q --allow-empty -m first";
    ok = ok && std::system(init.c_str()) == 0;
    const std::string name = Git::slug("Fix the Login test!\nmore");
    std::string error;
    const std::string made = Git::addWorktree("", repo.string(), name, error);
    const Git::Status in = made.empty() ? Git::Status{} : Git::status("", made);
    std::string again;
    const bool refused = Git::addWorktree("", repo.string(), name, again).empty() && !again.empty();
    ok = ok && name.starts_with("fix-the-login-test-") && fs::is_directory(made) && in.repo &&
         in.branch == "imcode/" + name && refused;
    // a worktree with uncommitted work is kept; a clean one goes, its branch stays
    std::ofstream(fs::path(made) / "new.txt") << "x";
    std::string kept;
    const bool refusedDirty = !Git::removeWorktree("", repo.string(), made, kept) && !kept.empty();
    fs::remove(fs::path(made) / "new.txt");

    // what changed, and one file's diff: a tracked file edited, a file not tracked yet
    std::ofstream(repo / "a.txt") << "one\ntwo\nthree\n";
    const std::string commit = "cd '" + home.string() + "/it'\\''s a repo' && git add a.txt && "
                               "git -c user.email=t@t -c user.name=t commit -q -m a";
    ok = ok && std::system(commit.c_str()) == 0;
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
    ok = ok && std::system(("git init -q --bare " + q(bare) + " && git -C " + q(repo) + " remote add origin " + q(bare)).c_str()) == 0;
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
    const bool pushed = std::system(("git -C " + q(bare) + " log --oneline main | grep -q more").c_str()) == 0;
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
    std::string gone;
    ok = ok && refusedDirty && Git::removeWorktree("", repo.string(), made, gone) && !fs::exists(made);
    fs::remove_all(home);
    std::printf(ok ? "ok\n" : "FAILED: %s | %s | %s | %s\n", name.c_str(), made.c_str(), error.c_str(),
                in.branch.c_str());
    return ok ? 0 : 1;
}
