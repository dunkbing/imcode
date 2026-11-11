// the git operations for a repository on this machine, through libgit2: no git command
#include "git_local.hpp"

#include <git2.h>
#include <git2/sys/repository.h> // git_repository_set_index

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>

namespace Git::Local {
    namespace {
        void init() {
            static std::once_flag once;
            std::call_once(once, [] { git_libgit2_init(); }); // never shut down: threads use it late
        }

        // owners that free what libgit2 hands out
        template <typename T, void (*Free)(T*)> struct Deleter {
            void operator()(T* p) const {
                Free(p);
            }
        };
        template <typename T, void (*Free)(T*)> using Own = std::unique_ptr<T, Deleter<T, Free>>;
        using Repo = Own<git_repository, git_repository_free>;
        using Index = Own<git_index, git_index_free>;
        using Tree = Own<git_tree, git_tree_free>;
        using DiffList = Own<git_diff, git_diff_free>;
        using Patch = Own<git_patch, git_patch_free>;
        using Ref = Own<git_reference, git_reference_free>;
        using Commit = Own<git_commit, git_commit_free>;
        using Statuses = Own<git_status_list, git_status_list_free>;
        using Signature = Own<git_signature, git_signature_free>;
        using Object = Own<git_object, git_object_free>;
        using Worktree = Own<git_worktree, git_worktree_free>;

        std::string lastError() {
            const git_error* e = git_error_last();
            return e && e->message ? e->message : "git failed";
        }

        // the repository holding `cwd` (a subfolder works); null outside one
        Repo open(const std::string& cwd) {
            init();
            git_repository* repo = nullptr;
            if (git_repository_open_ext(&repo, cwd.c_str(), 0, nullptr) != 0) {
                return nullptr;
            }
            return Repo(repo);
        }

        // HEAD's tree; null in a repository with no commit yet
        Tree headTree(git_repository* repo) {
            git_object* obj = nullptr;
            if (git_revparse_single(&obj, repo, "HEAD^{tree}") != 0) {
                return nullptr;
            }
            return Tree(reinterpret_cast<git_tree*>(obj));
        }

        Tree treeOf(git_repository* repo, const std::string& hex) {
            git_oid oid;
            git_tree* tree = nullptr;
            if (git_oid_fromstr(&oid, hex.c_str()) != 0 || git_tree_lookup(&tree, repo, &oid) != 0) {
                return nullptr;
            }
            return Tree(tree);
        }

        // the working folder against HEAD, untracked files included (their contents too)
        DiffList workdirDiff(git_repository* repo, const char* path = nullptr) {
            git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
            opts.flags = GIT_DIFF_INCLUDE_UNTRACKED | GIT_DIFF_RECURSE_UNTRACKED_DIRS | GIT_DIFF_SHOW_UNTRACKED_CONTENT;
            char* spec[] = {const_cast<char*>(path)};
            if (path) {
                opts.pathspec = {spec, 1};
                opts.flags |= GIT_DIFF_DISABLE_PATHSPEC_MATCH;
            }
            const Tree head = headTree(repo);
            git_diff* diff = nullptr;
            if (git_diff_tree_to_workdir_with_index(&diff, repo, head.get(), &opts) != 0) {
                return nullptr;
            }
            return DiffList(diff);
        }

        DiffList treesDiff(git_repository* repo, const std::string& from, const std::string& to, const char* path = nullptr) {
            const Tree a = treeOf(repo, from), b = treeOf(repo, to);
            if (!a || !b) {
                return nullptr;
            }
            git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
            char* spec[] = {const_cast<char*>(path)};
            if (path) {
                opts.pathspec = {spec, 1};
                opts.flags |= GIT_DIFF_DISABLE_PATHSPEC_MATCH;
            }
            git_diff* diff = nullptr;
            if (git_diff_tree_to_tree(&diff, repo, a.get(), b.get(), &opts) != 0) {
                return nullptr;
            }
            return DiffList(diff);
        }

        std::vector<Change> changesOf(git_diff* diff) {
            std::vector<Change> out;
            if (!diff) {
                return out;
            }
            git_diff_find_options find = GIT_DIFF_FIND_OPTIONS_INIT; // renames, as git status shows them
            git_diff_find_similar(diff, &find);
            for (size_t i = 0; i < git_diff_num_deltas(diff); ++i) {
                const git_diff_delta* d = git_diff_get_delta(diff, i);
                Change c;
                c.kind = d->status == GIT_DELTA_UNTRACKED ? '?'
                         : d->status == GIT_DELTA_ADDED   ? 'A'
                         : d->status == GIT_DELTA_DELETED ? 'D'
                         : d->status == GIT_DELTA_RENAMED ? 'R'
                                                          : 'M';
                c.path = d->new_file.path ? d->new_file.path : d->old_file.path;
                git_patch* patch = nullptr;
                if (c.kind != '?' && git_patch_from_diff(&patch, diff, i) == 0 && patch &&
                    !(git_patch_get_delta(patch)->flags & GIT_DIFF_FLAG_BINARY)) {
                    size_t context = 0, added = 0, removed = 0;
                    git_patch_line_stats(&context, &added, &removed, patch);
                    c.added = static_cast<int>(added);
                    c.removed = static_cast<int>(removed);
                }
                git_patch_free(patch);
                out.push_back(std::move(c));
            }
            return out;
        }

        // a diff as the unified text `git diff` prints
        std::string unified(git_diff* diff) {
            std::string out;
            if (diff) {
                git_diff_print(
                    diff, GIT_DIFF_FORMAT_PATCH,
                    [](const git_diff_delta*, const git_diff_hunk*, const git_diff_line* line, void* payload) {
                        auto& text = *static_cast<std::string*>(payload);
                        if (line->origin == GIT_DIFF_LINE_CONTEXT || line->origin == GIT_DIFF_LINE_ADDITION ||
                            line->origin == GIT_DIFF_LINE_DELETION) {
                            text += line->origin;
                        }
                        text.append(line->content, line->content_len);
                        return text.size() < 4'000'000 ? 0 : 1; // a huge diff is cut
                    },
                    &out);
            }
            return out;
        }
    } // namespace

    std::optional<Status> status(const std::string& cwd) {
        const Repo repo = open(cwd);
        if (!repo) {
            return std::nullopt;
        }
        Status s{.repo = true};
        git_reference* head = nullptr;
        const int got = git_repository_head(&head, repo.get());
        const Ref own(head);
        if (got == 0) {
            s.branch = git_reference_is_branch(head) ? git_reference_shorthand(head) : "detached";
        } else if (got == GIT_EUNBORNBRANCH) { // no commit yet: the branch HEAD names
            git_reference* sym = nullptr;
            if (git_reference_lookup(&sym, repo.get(), "HEAD") == 0) {
                const char* target = git_reference_symbolic_target(sym);
                s.branch = target && std::string_view(target).starts_with("refs/heads/") ? target + 11 : "";
                git_reference_free(sym);
            }
        }
        git_status_options opts = GIT_STATUS_OPTIONS_INIT;
        opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_EXCLUDE_SUBMODULES;
        git_status_list* list = nullptr;
        if (git_status_list_new(&list, repo.get(), &opts) == 0) {
            const Statuses own2(list);
            s.changed = static_cast<int>(git_status_list_entrycount(list));
        }
        return s;
    }

    std::optional<std::vector<Change>> changes(const std::string& cwd, const std::string& from, const std::string& to) {
        const Repo repo = open(cwd);
        if (!repo) {
            return std::nullopt;
        }
        const DiffList diff = from.empty() ? workdirDiff(repo.get()) : treesDiff(repo.get(), from, to);
        return changesOf(diff.get());
    }

    std::optional<std::string> fileDiff(const std::string& cwd, const Change& change, const std::string& from,
                                        const std::string& to) {
        const Repo repo = open(cwd);
        if (!repo) {
            return std::nullopt;
        }
        const DiffList diff = from.empty() ? workdirDiff(repo.get(), change.path.c_str())
                                           : treesDiff(repo.get(), from, to, change.path.c_str());
        return unified(diff.get());
    }

    std::optional<std::string> snapshot(const std::string& cwd) {
        Repo repo = open(cwd);
        if (!repo || git_repository_is_bare(repo.get())) {
            return std::nullopt;
        }
        // a copy of the index, given to this handle of the repository only: everything is added
        // to the copy, which becomes a tree. the real index file is never written
        git_index* real = nullptr;
        if (git_repository_index(&real, repo.get()) != 0) {
            return std::nullopt;
        }
        const std::string realPath = git_index_path(real) ? git_index_path(real) : "";
        git_index_free(real);
        std::error_code ec;
        const auto copy = std::filesystem::temp_directory_path(ec) /
                          ("imcode-index-" + std::to_string(reinterpret_cast<uintptr_t>(repo.get())));
        std::filesystem::copy_file(realPath, copy, std::filesystem::copy_options::overwrite_existing, ec);
        git_index* index = nullptr;
        std::optional<std::string> out;
        if (git_index_open(&index, copy.c_str()) == 0) {
            const Index own(index);
            git_repository_set_index(repo.get(), index);
            git_oid oid;
            if (git_index_add_all(index, nullptr, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr) == 0 &&
                git_index_write_tree(&oid, index) == 0) {
                char hex[GIT_OID_MAX_HEXSIZE + 1] = {};
                git_oid_tostr(hex, sizeof(hex), &oid);
                out = hex;
            }
        }
        std::filesystem::remove(copy, ec);
        return out ? out : std::optional<std::string>(std::string());
    }

    bool commit(const std::string& cwd, const std::string& message, std::string& output) {
        const Repo repo = open(cwd);
        if (!repo) {
            output = "Not a git repository.";
            return false;
        }
        git_index* index = nullptr;
        if (git_repository_index(&index, repo.get()) != 0) {
            output = lastError();
            return false;
        }
        const Index own(index);
        git_oid treeId, commitId;
        if (git_index_add_all(index, nullptr, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr) != 0 || git_index_write(index) != 0 ||
            git_index_write_tree(&treeId, index) != 0) {
            output = lastError();
            return false;
        }
        git_tree* tree = nullptr;
        git_tree_lookup(&tree, repo.get(), &treeId);
        const Tree ownTree(tree);
        // the parent, unless this is the first commit
        git_object* parentObj = nullptr;
        const bool hasParent = git_revparse_single(&parentObj, repo.get(), "HEAD^{commit}") == 0;
        const Object ownParent(parentObj);
        const git_commit* parents[] = {reinterpret_cast<git_commit*>(parentObj)};
        if (hasParent && git_oid_equal(git_tree_id(tree), git_commit_tree_id(reinterpret_cast<git_commit*>(parentObj)))) {
            output = "Nothing to commit.";
            return false;
        }
        // who: as git decides it, the GIT_AUTHOR_* and GIT_COMMITTER_* variables over the config
        git_signature *author = nullptr, *committer = nullptr;
        if (git_signature_default_from_env(&author, &committer, repo.get()) != 0) {
            output = "Set your name and email for git first (git config user.name / user.email).";
            return false;
        }
        const Signature ownAuthor(author), ownCommitter(committer);
        if (git_commit_create(&commitId, repo.get(), "HEAD", author, committer, nullptr, message.c_str(), tree,
                              hasParent ? 1 : 0, parents) != 0) {
            output = lastError();
            return false;
        }
        return true;
    }

    std::optional<std::vector<std::string>> files(const std::string& cwd, size_t cap) {
        const Repo repo = open(cwd);
        if (!repo || git_repository_is_bare(repo.get())) {
            return std::nullopt;
        }
        // what is tracked, then what is new and not ignored: under cwd, relative to it
        const std::string root = git_repository_workdir(repo.get());
        std::error_code ec;
        std::string prefix = std::filesystem::relative(std::filesystem::weakly_canonical(cwd, ec),
                                                       std::filesystem::weakly_canonical(root, ec), ec)
                                 .string();
        prefix = prefix == "." || prefix.empty() ? "" : prefix + "/";
        std::vector<std::string> out;
        const auto keep = [&](const char* path) {
            const std::string_view p(path);
            if (p.starts_with(prefix) && out.size() < cap) {
                out.emplace_back(p.substr(prefix.size()));
            }
        };
        git_index* index = nullptr;
        if (git_repository_index(&index, repo.get()) == 0) {
            const Index own(index);
            for (size_t i = 0; i < git_index_entrycount(index); ++i) {
                keep(git_index_get_byindex(index, i)->path);
            }
        }
        git_status_options opts = GIT_STATUS_OPTIONS_INIT;
        opts.show = GIT_STATUS_SHOW_WORKDIR_ONLY;
        opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS | GIT_STATUS_OPT_EXCLUDE_SUBMODULES;
        git_status_list* list = nullptr;
        if (git_status_list_new(&list, repo.get(), &opts) == 0) {
            const Statuses own(list);
            for (size_t i = 0; i < git_status_list_entrycount(list); ++i) {
                const git_status_entry* e = git_status_byindex(list, i);
                if ((e->status & GIT_STATUS_WT_NEW) && e->index_to_workdir) {
                    keep(e->index_to_workdir->new_file.path);
                }
            }
        }
        return out;
    }

    std::string addWorktree(const std::string& repoDir, const std::string& folder, const std::string& branch,
                            std::string& error) {
        const Repo repo = open(repoDir);
        if (!repo) {
            error = "Not a git repository.";
            return "";
        }
        // a new branch at HEAD, checked out in the new folder
        git_object* headObj = nullptr;
        if (git_revparse_single(&headObj, repo.get(), "HEAD^{commit}") != 0) {
            error = "Could not make a worktree: the repository has no commit yet.";
            return "";
        }
        const Object ownHead(headObj);
        git_reference* ref = nullptr;
        if (git_branch_create(&ref, repo.get(), branch.c_str(), reinterpret_cast<git_commit*>(headObj), 0) != 0) {
            error = "Could not make a worktree: " + lastError();
            return "";
        }
        const Ref ownRef(ref);
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(folder).parent_path(), ec);
        git_worktree_add_options opts = GIT_WORKTREE_ADD_OPTIONS_INIT;
        opts.ref = ref;
        git_worktree* wt = nullptr;
        const std::string name = std::filesystem::path(folder).filename().string();
        if (git_worktree_add(&wt, repo.get(), name.c_str(), folder.c_str(), &opts) != 0) {
            error = "Could not make a worktree: " + lastError();
            git_branch_delete(ref);
            return "";
        }
        git_worktree_free(wt);
        return folder;
    }

    bool removeWorktree(const std::string& repoDir, const std::string& path, std::string& error) {
        // like git: refused while it holds work that is not committed
        if (const auto s = status(path); s && s->changed > 0) {
            error = path + " has changes that are not committed.";
            return false;
        }
        const Repo repo = open(repoDir);
        if (!repo) {
            error = "Not a git repository.";
            return false;
        }
        git_strarray names{};
        git_worktree_list(&names, repo.get());
        bool done = false;
        std::error_code ec;
        const auto want = std::filesystem::weakly_canonical(path, ec);
        for (size_t i = 0; i < names.count && !done; ++i) {
            git_worktree* wt = nullptr;
            if (git_worktree_lookup(&wt, repo.get(), names.strings[i]) != 0) {
                continue;
            }
            const Worktree own(wt);
            if (std::filesystem::weakly_canonical(git_worktree_path(wt), ec) != want) {
                continue;
            }
            git_worktree_prune_options opts = GIT_WORKTREE_PRUNE_OPTIONS_INIT;
            opts.flags = GIT_WORKTREE_PRUNE_VALID | GIT_WORKTREE_PRUNE_WORKING_TREE; // its branch stays
            done = git_worktree_prune(wt, &opts) == 0;
            if (!done) {
                error = lastError();
            }
        }
        git_strarray_dispose(&names);
        if (!done && error.empty()) {
            error = path + " is not a worktree of " + repoDir + ".";
        }
        return done;
    }
} // namespace Git::Local
