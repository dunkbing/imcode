// the branch and the count of changed files from git's porcelain status; a worktree of a
// scratch repository
#include "../src/git.hpp"
#include "../src/git_graph.hpp"
#include "../src/ssh.hpp"
#include <chrono>
#include <cstdio>
#include <thread>
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
    std::error_code held; // windows: a git process can still hold the folder for a moment
    fs::remove_all(home, held);
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
    // the counts are kept for a file that settled (older than two seconds), and follow it when it is written
    {
        std::this_thread::sleep_for(std::chrono::seconds(3));
        const auto first = Git::changes("", repo.string());
        const auto kept = Git::changes("", repo.string()); // from the kept counts
        const auto a1 = std::ranges::find(first, std::string("a.txt"), &Git::Change::path);
        const auto a2 = std::ranges::find(kept, std::string("a.txt"), &Git::Change::path);
        std::ofstream(repo / "a.txt") << "one\n2\nthree\nfour\nfive\nsix\n";
        const auto edited = Git::changes("", repo.string());
        const auto a3 = std::ranges::find(edited, std::string("a.txt"), &Git::Change::path);
        if (a1 == first.end() || a2 == kept.end() || a3 == edited.end() || a1->added != a2->added || a1->removed != a2->removed ||
            a3->added != a1->added + 2) {
            std::printf("FAILED: kept counts do not follow the file\n");
            ok = false;
        }
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
    // one file's history: only the commits that touched it
    const auto ofA = Git::log("", repo.string(), 50, 0, "a.txt");
    if (ofA.empty() || ofA[0].subject != "more" || ofA.size() >= history.size() + 1 ||
        !Git::log("", repo.string(), 50, 0, "no-such-file.txt").empty()) {
        std::printf("FAILED: file history %zu of %zu\n", ofA.size(), history.size());
        ok = false;
    }
    // hunks: stage one, unstage it, throw the other away
    {
        std::ofstream(repo / "h.txt") << "l1\nl2\nl3\nl4\nl5\nl6\nl7\nl8\nl9\nl10\nl11\nl12\nl13\nl14\nl15\nl16\nl17\nl18\nl19\nl20\nl21\nl22\nl23\nl24\nl25\nl26\nl27\nl28\nl29\nl30\n";
        shell("git -C " + q(repo) + " add h.txt && git -C " + q(repo) + " commit -q -m hfile");
        std::ofstream(repo / "h.txt") << "l1\nX2\nl3\nl4\nl5\nl6\nl7\nl8\nl9\nl10\nl11\nl12\nl13\nl14\nl15\nl16\nl17\nl18\nl19\nl20\nl21\nl22\nl23\nl24\nl25\nl26\nl27\nX28\nl29\nl30\n";
        const auto list = Git::changes("", repo.string());
        const auto file = std::ranges::find(list, std::string("h.txt"), &Git::Change::path);
        const auto hunkOf = [](const std::vector<Diff::Line>& lines, int n) {
            std::vector<Diff::Line> out;
            int at = 0;
            for (const auto& l : lines) {
                if (l.kind == '~') {
                    ++at;
                } else if (at == n) {
                    out.push_back(l);
                }
            }
            return out;
        };
        const auto grep = [&](const char* what, const char* text) {
            return shell("git -C " + q(repo) + " " + what + " | grep -q " + text) == 0;
        };
        bool hunksOk = file != list.end();
        std::string error;
        if (hunksOk) {
            const auto lines = Git::fileDiff("", repo.string(), *file, Git::Side::Unstaged);
            hunksOk = std::ranges::count(lines, '~', &Diff::Line::kind) == 1; // two hunks
            hunksOk = hunksOk && Git::applyHunk("", repo.string(), *file, Git::HunkAction::Stage, 0, hunkOf(lines, 0), error);
            hunksOk = hunksOk && grep("diff --cached", "X2") && !grep("diff --cached", "X28");
            const auto staged = Git::fileDiff("", repo.string(), *file, Git::Side::Staged);
            hunksOk = hunksOk && Git::applyHunk("", repo.string(), *file, Git::HunkAction::Unstage, 0, hunkOf(staged, 0), error);
            hunksOk = hunksOk && shell("git -C " + q(repo) + " diff --cached --quiet") == 0;
            // a stale view is refused
            auto stale = hunkOf(lines, 1);
            stale[0].text += "?";
            hunksOk = hunksOk && !Git::applyHunk("", repo.string(), *file, Git::HunkAction::Discard, 1, stale, error);
            hunksOk = hunksOk && Git::applyHunk("", repo.string(), *file, Git::HunkAction::Discard, 1, hunkOf(lines, 1), error);
            std::ifstream in(repo / "h.txt");
            const std::string text((std::istreambuf_iterator<char>(in)), {});
            hunksOk = hunksOk && text.find("X2\n") != std::string::npos && text.find("X28") == std::string::npos;
        }
        if (!hunksOk) {
            std::printf("FAILED: hunks: %s\n", error.c_str());
            ok = false;
        }
        shell("git -C " + q(repo) + " checkout -q -- h.txt");
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
    // one commit in full: libgit2 here, git's own show parsed the way an ssh host's is
    {
        const auto last = history.empty() ? Git::LogEntry{} : history[0]; // "more": a.txt changed
        const Git::CommitDetail via = Git::show("", repo.string(), last.hash);
        const std::string showFile = (home / "show.txt").string();
        std::system(("git -C " + q(repo) + " show -s --format='%H%x1f%P%x1f%an%x1f%ae%x1f%cn%x1f%ce%x1f%at%x1f%ct%x1f%B' " + last.hash + " > " + q(showFile)).c_str());
        std::ifstream showIn(showFile);
        const Git::CommitDetail cli = Git::parseShow(std::string(std::istreambuf_iterator<char>(showIn), {}));
        const bool same = !via.hash.empty() && via.hash == cli.hash && via.parents == cli.parents && via.author == cli.author &&
                          via.authorEmail == cli.authorEmail && via.committer == cli.committer && via.time == cli.time &&
                          via.committed == cli.committed && via.message == cli.message;
        const bool files = via.files.size() == 1 && via.files[0].path == "a.txt" && via.message == "more" &&
                           Git::show("", repo.string(), "0000000000").hash.empty();
        if (!same || !files) {
            std::printf("FAILED: show same %d files %d (%s | %s)\n", same, files, via.message.c_str(), cli.message.c_str());
            ok = false;
        }
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
    // staging: libgit2 here and the shell commands an ssh host gets must leave the same index
    {
        const fs::path one = home / "stage one", two = home / "stage two";
        const auto sh = [&](const std::string& cmd) { return std::system(cmd.c_str()) == 0; };
        const auto porcelain = [&](const fs::path& r) {
            const std::string out = (home / "porcelain.txt").string();
            std::system(("git -C " + q(r) + " status --porcelain -uall > " + q(out)).c_str());
            std::ifstream in(out);
            return std::string(std::istreambuf_iterator<char>(in), {});
        };
        fs::create_directories(one);
        bool staged = sh("cd " + q(one) + " && git init -q -b main . && printf '1\\n' > a.txt && printf 'x\\n' > gone.txt && printf 'r\\n' > ren.txt && "
                         "git add -A && git -c user.email=t@t -c user.name=t commit -q -m base && printf '1\\n2\\n' > a.txt && rm gone.txt && "
                         "mv ren.txt 'new name.txt' && printf 'n\\n' > n.txt");
        staged = staged && sh("cp -R " + q(one) + " " + q(two));
        const auto list = Git::changes("", one.string());
        const auto pick = [&](std::initializer_list<const char*> names) {
            std::vector<Git::Change> out;
            for (const char* n : names) {
                const auto at = std::ranges::find(list, std::string(n), &Git::Change::path);
                if (at != list.end()) {
                    out.push_back(*at);
                }
            }
            return out;
        };
        const auto all = pick({"a.txt", "gone.txt", "ren.txt", "new name.txt", "n.txt"});
        const auto some = pick({"a.txt", "n.txt"});
        std::string e1, e2;
        const std::pair<const char*, std::string (*)(const std::vector<Git::Change>&)> steps[] = {
            {"stage", Git::stageScript}, {"unstage", Git::unstageScript}, {"discard", Git::discardScript}};
        std::string same = all.size() == 5 ? "" : "list";
        // each step on both copies: the files of the first steps, then the two left
        bool ranAll = true;
        for (int i = 0; i < 3 && same.empty(); ++i) {
            const auto& files = i == 2 ? some : all;
            std::vector<Git::Change> given = files;
            if (i > 0) { // the states move on: read them again
                const auto now = Git::changes("", one.string());
                given.clear();
                for (const auto& f : files) {
                    const auto at = std::ranges::find(now, f.path, &Git::Change::path);
                    given.push_back(at == now.end() ? f : *at);
                }
            }
            ranAll = ranAll && (i == 0 ? Git::stage("", one.string(), given, e1) : i == 1 ? Git::unstage("", one.string(), given, e1)
                                                                                         : Git::discard("", one.string(), given, e1));
            ranAll = ranAll && sh("cd " + q(two) + " && " + steps[i].second(given) + " > /dev/null 2>&1");
            if (porcelain(one) != porcelain(two)) {
                same = std::string(steps[i].first) + ":\n" + porcelain(one) + "vs\n" + porcelain(two);
            }
        }
        if (!staged || !ranAll || !same.empty()) {
            std::printf("FAILED: staging (%d %d) %s %s\n", staged, ranAll, same.c_str(), e1.c_str());
            ok = false;
        }
        // staged and unstaged diffs of one file apart: "2" is only in the folder, then only in the index
        fs::remove_all(two);
        std::ofstream(one / "a.txt") << "1\n2\n3\n";
        sh("git -C " + q(one) + " add a.txt");
        std::ofstream(one / "a.txt", std::ios::app) << "4\n";
        const auto now = Git::changes("", one.string());
        const auto a = std::ranges::find(now, std::string("a.txt"), &Git::Change::path);
        bool sides = a != now.end() && a->index == 'M' && a->work == 'M';
        if (sides) {
            const auto inIndex = Git::fileDiff("", one.string(), *a, Git::Side::Staged);
            const auto inFolder = Git::fileDiff("", one.string(), *a, Git::Side::Unstaged);
            sides = std::ranges::count(inIndex, '+', &Diff::Line::kind) == 2 && std::ranges::count(inFolder, '+', &Diff::Line::kind) == 1;
        }
        // committing the index leaves the folder's own edit out
        std::string said2;
        sides = sides && Git::commit("", one.string(), "only the index", false, said2, true) &&
                sh("test \"$(git -C " + q(one) + " show HEAD:a.txt | wc -l)\" -eq 3") && porcelain(one).starts_with(" M a.txt");
        // a hook that refuses stops the commit (libgit2 would not have run it)
        std::ofstream(one / ".git" / "hooks" / "pre-commit") << "#!/bin/sh\necho no commits today\nexit 1\n";
        fs::permissions(one / ".git" / "hooks" / "pre-commit", fs::perms::owner_all);
        std::string refused;
        const bool hooked = !Git::commit("", one.string(), "blocked", false, refused) && refused.find("no commits today") != std::string::npos;
        fs::remove(one / ".git" / "hooks" / "pre-commit");
        // signing that cannot happen stops it too
        sh("git -C " + q(one) + " config commit.gpgsign true && git -C " + q(one) + " config gpg.program /nonexistent-gpg");
        std::string unsigned_;
        const bool signedRefused = !Git::commit("", one.string(), "unsigned", false, unsigned_) && !unsigned_.empty();
        if (!sides || !hooked || !signedRefused) {
            std::printf("FAILED: sides %d hook %d (%s) signing %d (%s)\n", sides, hooked, refused.c_str(), signedRefused, unsigned_.c_str());
            ok = false;
        }
    }
    std::string gone;
    ok = ok && refusedDirty && Git::removeWorktree("", repo.string(), made, gone) && !fs::exists(made);
    fs::remove_all(home, held);
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
