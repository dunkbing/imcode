// the branch and the count of changed files from git's porcelain status; a worktree of a
// scratch repository
#include "../src/git.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

int main() {
    const Git::Status a = Git::parse("## main...origin/main [ahead 1]\n M src/a.cpp\n?? new.txt\n");
    const Git::Status b = Git::parse("## No commits yet on trunk\n");
    const Git::Status c = Git::parse("## HEAD (no branch)\nA  x\n");
    const Git::Status d = Git::parse("## feature/x\n");
    const Git::Status none = Git::parse("");
    bool ok = a.repo && a.branch == "main" && a.changed == 2 && b.branch == "trunk" && b.changed == 0 &&
              c.branch == "detached" && c.changed == 1 && d.branch == "feature/x" && !none.repo;

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
    std::string gone;
    ok = ok && refusedDirty && Git::removeWorktree("", repo.string(), made, gone) && !fs::exists(made);
    fs::remove_all(home);
    std::printf(ok ? "ok\n" : "FAILED: %s | %s | %s | %s\n", name.c_str(), made.c_str(), error.c_str(),
                in.branch.c_str());
    return ok ? 0 : 1;
}
