// the branch and the count of changed files from git's porcelain status
#include "../src/git.hpp"
#include <cstdio>

int main() {
    const Git::Status a = Git::parse("## main...origin/main [ahead 1]\n M src/a.cpp\n?? new.txt\n");
    const Git::Status b = Git::parse("## No commits yet on trunk\n");
    const Git::Status c = Git::parse("## HEAD (no branch)\nA  x\n");
    const Git::Status d = Git::parse("## feature/x\n");
    const Git::Status none = Git::parse("");
    const bool ok = a.repo && a.branch == "main" && a.changed == 2 && b.branch == "trunk" && b.changed == 0 &&
                    c.branch == "detached" && c.changed == 1 && d.branch == "feature/x" && !none.repo;
    std::printf(ok ? "ok\n" : "FAILED: %s %d | %s | %s | %s\n", a.branch.c_str(), a.changed, b.branch.c_str(),
                c.branch.c_str(), d.branch.c_str());
    return ok ? 0 : 1;
}
