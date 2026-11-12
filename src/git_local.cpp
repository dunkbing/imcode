// the git operations for a repository on this machine, through libgit2: no git command
#include "git_local.hpp"

#include <git2.h>
#include <git2/sys/repository.h> // git_repository_set_index

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
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
        using Walk = Own<git_revwalk, git_revwalk_free>;
        using RefIter = Own<git_reference_iterator, git_reference_iterator_free>;

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

        // the tree of a tree hash (a snapshot) or of a commit, or "<commit>^" (its first parent)
        Tree treeOf(git_repository* repo, const std::string& rev) {
            git_object* obj = nullptr;
            if (git_revparse_single(&obj, repo, (rev + "^{tree}").c_str()) != 0) {
                return nullptr;
            }
            return Tree(reinterpret_cast<git_tree*>(obj));
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
                if (c.kind == 'R' && d->old_file.path) {
                    c.from = d->old_file.path;
                }
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

    namespace {
        // git status's two columns for each file, by its path
        void fillStates(git_repository* repo, std::vector<Change>& changes) {
            git_status_options opts = GIT_STATUS_OPTIONS_INIT;
            opts.flags = GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS | GIT_STATUS_OPT_EXCLUDE_SUBMODULES;
            git_status_list* raw = nullptr;
            if (git_status_list_new(&raw, repo, &opts) != 0) {
                return;
            }
            const Statuses list(raw);
            std::map<std::string, std::pair<char, char>> states;
            for (size_t i = 0; i < git_status_list_entrycount(list.get()); ++i) {
                const git_status_entry* e = git_status_byindex(list.get(), i);
                const auto s = e->status;
                const char x = s & GIT_STATUS_INDEX_NEW ? 'A' : s & GIT_STATUS_INDEX_DELETED ? 'D' : s & GIT_STATUS_INDEX_RENAMED ? 'R'
                               : s & (GIT_STATUS_INDEX_MODIFIED | GIT_STATUS_INDEX_TYPECHANGE) ? 'M' : ' ';
                const char y = s & GIT_STATUS_WT_NEW ? '?' : s & GIT_STATUS_WT_DELETED ? 'D' : s & GIT_STATUS_WT_RENAMED ? 'R'
                               : s & (GIT_STATUS_WT_MODIFIED | GIT_STATUS_WT_TYPECHANGE) ? 'M' : ' ';
                const git_diff_delta* d = e->index_to_workdir ? e->index_to_workdir : e->head_to_index;
                if (d && d->new_file.path) {
                    states[d->new_file.path] = {x, y};
                }
            }
            for (Change& c : changes) {
                if (const auto at = states.find(c.path); at != states.end()) {
                    c.index = at->second.first;
                    c.work = at->second.second;
                }
            }
        }
    } // namespace

    std::optional<std::vector<Change>> changes(const std::string& cwd, const std::string& from, const std::string& to) {
        const Repo repo = open(cwd);
        if (!repo) {
            return std::nullopt;
        }
        const DiffList diff = from.empty() ? workdirDiff(repo.get()) : treesDiff(repo.get(), from, to);
        std::vector<Change> out = changesOf(diff.get());
        if (from.empty()) {
            fillStates(repo.get(), out);
        }
        return out;
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

    namespace {
        // the files' paths, for a pathspec: a renamed one is both its names
        std::vector<std::string> pathsOf(const std::vector<Change>& files) {
            std::vector<std::string> out;
            for (const Change& f : files) {
                out.push_back(f.path);
                if (!f.from.empty()) {
                    out.push_back(f.from);
                }
            }
            return out;
        }
    } // namespace

    std::optional<std::string> fileDiff(const std::string& cwd, const Change& change, Side side) {
        const Repo repo = open(cwd);
        if (!repo) {
            return std::nullopt;
        }
        if (side == Side::All) {
            return unified(workdirDiff(repo.get(), change.path.c_str()).get());
        }
        git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
        char* spec[] = {const_cast<char*>(change.path.c_str())};
        opts.pathspec = {spec, 1};
        opts.flags = GIT_DIFF_DISABLE_PATHSPEC_MATCH;
        git_index* raw = nullptr;
        if (git_repository_index(&raw, repo.get()) != 0) {
            return std::nullopt;
        }
        const Index index(raw);
        git_diff* diff = nullptr;
        int got = 0;
        if (side == Side::Staged) {
            const Tree head = headTree(repo.get());
            got = git_diff_tree_to_index(&diff, repo.get(), head.get(), index.get(), &opts);
        } else {
            opts.flags |= GIT_DIFF_INCLUDE_UNTRACKED | GIT_DIFF_RECURSE_UNTRACKED_DIRS | GIT_DIFF_SHOW_UNTRACKED_CONTENT;
            got = git_diff_index_to_workdir(&diff, repo.get(), index.get(), &opts);
        }
        if (got != 0) {
            return std::nullopt;
        }
        const DiffList own(diff);
        return unified(diff);
    }

    bool stage(const std::string& cwd, const std::vector<Change>& files, std::string& error) {
        const Repo repo = open(cwd);
        git_index* raw = nullptr;
        if (!repo || git_repository_index(&raw, repo.get()) != 0) {
            error = repo ? lastError() : "Not a git repository.";
            return false;
        }
        const Index index(raw);
        const std::filesystem::path root = git_repository_workdir(repo.get()) ? git_repository_workdir(repo.get()) : "";
        for (const std::string& path : pathsOf(files)) {
            std::error_code ec;
            // a file that is gone leaves the index; one that is there goes in
            const bool there = std::filesystem::symlink_status(root / path, ec).type() != std::filesystem::file_type::not_found;
            if (there && git_index_add_bypath(index.get(), path.c_str()) != 0) {
                error = lastError();
                return false;
            }
            if (!there) {
                git_index_remove_bypath(index.get(), path.c_str()); // not in the index is fine
            }
        }
        if (git_index_write(index.get()) != 0) {
            error = lastError();
            return false;
        }
        return true;
    }

    bool unstage(const std::string& cwd, const std::vector<Change>& files, std::string& error) {
        const Repo repo = open(cwd);
        if (!repo) {
            error = "Not a git repository.";
            return false;
        }
        std::vector<std::string> paths = pathsOf(files);
        std::vector<char*> raw;
        for (std::string& p : paths) {
            raw.push_back(p.data());
        }
        const git_strarray spec{raw.data(), raw.size()};
        // no commit yet: the index entries just go (a null target)
        git_object* head = nullptr;
        const bool born = git_revparse_single(&head, repo.get(), "HEAD^{commit}") == 0;
        const Object own(head);
        if (git_reset_default(repo.get(), born ? head : nullptr, &spec) != 0) {
            error = lastError();
            return false;
        }
        return true;
    }

    bool discard(const std::string& cwd, const std::vector<Change>& files, std::string& error) {
        const Repo repo = open(cwd);
        if (!repo) {
            error = "Not a git repository.";
            return false;
        }
        const std::filesystem::path root = git_repository_workdir(repo.get()) ? git_repository_workdir(repo.get()) : "";
        std::vector<std::string> tracked;
        for (const Change& f : files) {
            if (f.work == '?') {
                std::error_code ec; // not tracked: deleted
                std::filesystem::remove(root / f.path, ec);
            } else if (f.work != ' ') {
                tracked.push_back(f.path);
            }
        }
        if (tracked.empty()) {
            return true;
        }
        std::vector<char*> raw;
        for (std::string& p : tracked) {
            raw.push_back(p.data());
        }
        // the folder's copy comes back from the index: what was staged stays
        git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
        opts.checkout_strategy = GIT_CHECKOUT_FORCE | GIT_CHECKOUT_DISABLE_PATHSPEC_MATCH;
        opts.paths = {raw.data(), raw.size()};
        if (git_checkout_index(repo.get(), nullptr, &opts) != 0) {
            error = lastError();
            return false;
        }
        return true;
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
        if (git_index_open(&index, copy.string().c_str()) == 0) {
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

    bool commit(const std::string& cwd, const std::string& message, std::string& output, bool all) {
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
        if ((all && git_index_add_all(index, nullptr, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr) != 0) || git_index_write(index) != 0 ||
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

    std::optional<CommitDetail> show(const std::string& cwd, const std::string& hash) {
        const Repo repo = open(cwd);
        if (!repo) {
            return std::nullopt;
        }
        git_object* obj = nullptr;
        if (git_revparse_single(&obj, repo.get(), (hash + "^{commit}").c_str()) != 0) {
            return std::nullopt;
        }
        const Object own(obj);
        const auto* commit = reinterpret_cast<git_commit*>(obj);
        CommitDetail out;
        char buf[GIT_OID_MAX_HEXSIZE + 1];
        git_oid_tostr(buf, sizeof(buf), git_commit_id(commit));
        out.hash = buf;
        const git_signature* a = git_commit_author(commit);
        const git_signature* c = git_commit_committer(commit);
        out.author = a && a->name ? a->name : "";
        out.authorEmail = a && a->email ? a->email : "";
        out.committer = c && c->name ? c->name : "";
        out.committerEmail = c && c->email ? c->email : "";
        out.time = a ? static_cast<long long>(a->when.time) : 0;
        out.committed = c ? static_cast<long long>(c->when.time) : 0;
        out.message = git_commit_message(commit) ? git_commit_message(commit) : "";
        while (!out.message.empty() && std::isspace(static_cast<unsigned char>(out.message.back()))) {
            out.message.pop_back();
        }
        for (unsigned i = 0; i < git_commit_parentcount(commit); ++i) {
            git_oid_tostr(buf, sizeof(buf), git_commit_parent_id(commit, i));
            out.parents.push_back(buf);
        }
        return out;
    }

    std::optional<std::vector<LogEntry>> log(const std::string& cwd, size_t limit, size_t skip) {
        const Repo repo = open(cwd);
        if (!repo) {
            return std::nullopt;
        }
        // what points at each commit: branches, remote branches, tags, and where HEAD is
        std::map<std::string, std::vector<std::string>> refs;
        const auto hex = [](const git_oid& oid) {
            char buf[GIT_OID_MAX_HEXSIZE + 1];
            git_oid_tostr(buf, sizeof(buf), &oid);
            return std::string(buf);
        };
        std::string headName;
        git_reference* head = nullptr;
        if (git_repository_head(&head, repo.get()) == 0) {
            const Ref own(head);
            if (git_repository_head_detached(repo.get()) == 1) {
                refs[hex(*git_reference_target(head))].push_back("HEAD");
            } else {
                headName = git_reference_name(head);
            }
        }
        git_reference_iterator* raw = nullptr;
        if (git_reference_iterator_new(&raw, repo.get()) == 0) {
            const RefIter iter(raw);
            for (git_reference* r = nullptr; git_reference_next(&r, iter.get()) == 0;) {
                const Ref own(r);
                git_object* peeled = nullptr; // an annotated tag points at the commit through its tag object
                if (git_reference_type(r) != GIT_REFERENCE_DIRECT || git_reference_peel(&peeled, r, GIT_OBJECT_COMMIT) != 0) {
                    continue;
                }
                const Object commitObject(peeled);
                const std::string name = git_reference_name(r), shown = git_reference_shorthand(r);
                refs[hex(*git_object_id(peeled))].push_back(
                    name.starts_with("refs/tags/") ? "tag: " + shown : (name == headName ? "HEAD -> " : "") + shown);
            }
        }
        git_revwalk* walkRaw = nullptr;
        if (git_revwalk_new(&walkRaw, repo.get()) != 0) {
            return std::vector<LogEntry>{};
        }
        const Walk walk(walkRaw);
        git_revwalk_sorting(walk.get(), GIT_SORT_TOPOLOGICAL | GIT_SORT_TIME);
        git_revwalk_push_glob(walk.get(), "refs/*");
        git_revwalk_push_head(walk.get()); // a detached HEAD, or no refs yet: errors are fine
        std::vector<LogEntry> out;
        git_oid oid;
        for (size_t n = 0; git_revwalk_next(&oid, walk.get()) == 0; ++n) {
            if (n < skip) {
                continue;
            }
            if (out.size() >= limit) {
                break;
            }
            git_commit* raw = nullptr;
            if (git_commit_lookup(&raw, repo.get(), &oid) != 0) {
                continue;
            }
            const Commit c(raw);
            LogEntry e;
            e.hash = hex(oid);
            const char* summary = git_commit_summary(c.get());
            e.subject = summary ? summary : "";
            const git_signature* who = git_commit_author(c.get());
            e.author = who && who->name ? who->name : "";
            e.time = static_cast<long long>(git_commit_time(c.get()));
            for (unsigned i = 0; i < git_commit_parentcount(c.get()); ++i) {
                e.parents.push_back(hex(*git_commit_parent_id(c.get(), i)));
            }
            if (const auto at = refs.find(e.hash); at != refs.end()) {
                e.refs = at->second;
            }
            out.push_back(std::move(e));
        }
        return out;
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
