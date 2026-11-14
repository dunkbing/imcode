/* imcode-helper watch <folder> on linux: prints what changes under a folder (inotify), in the
 * line protocol of src/watch.hpp. plain c so a host without a c++ compiler can build it:
 * the app uploads this file and runs `cc -O2 -o helper helper_linux.c`. exits when stdin closes */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define MAX_LINES 2000 /* past this the app reads everything again */

static int ino = -1;
static char** paths; /* by watch descriptor (they count up from 1): the folder, relative to the root */
static int npaths;
static char** lines; /* this batch: "d dir", "f file" */
static int nlines;
static int git, all, rebuild;
static const char* root;

static void die(const char* what, const char* path) {
    fprintf(stderr, "%s %s: %s\n", what, path, strerror(errno));
    exit(2);
}

static void emit(char kind, const char* path) {
    if (nlines >= MAX_LINES) {
        all = 1;
        return;
    }
    char* line = malloc(strlen(path) + 4);
    if (!line) {
        exit(2);
    }
    sprintf(line, "%c%s%s", kind, *path ? " " : "", path);
    lines = realloc(lines, (size_t)(nlines + 1) * sizeof *lines);
    lines[nlines++] = line;
}

static int in_git(const char* rel) { /* .git itself, or what is under it */
    return strcmp(rel, ".git") == 0 || strncmp(rel, ".git/", 5) == 0;
}

/* a folder worth a watch: git's refs and .git itself, not its objects; not node_modules */
static int watched(const char* rel) {
    if (in_git(rel)) {
        return strcmp(rel, ".git") == 0 || strcmp(rel, ".git/refs") == 0 || strncmp(rel, ".git/refs/", 10) == 0;
    }
    const char* base = strrchr(rel, '/');
    return strcmp(base ? base + 1 : rel, "node_modules") != 0;
}

static void add(const char* rel) { /* a watch on one folder and, below it, the rest (not symlinks) */
    char full[PATH_MAX];
    snprintf(full, sizeof full, "%s%s%s", root, *rel ? "/" : "", rel);
    const int wd = inotify_add_watch(ino, full, IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_MODIFY | IN_ONLYDIR | IN_DONT_FOLLOW);
    if (wd < 0) {
        if (errno == ENOENT || errno == ENOTDIR) { /* gone meanwhile */
            return;
        }
        die(errno == ENOSPC ? "no watches left (fs.inotify.max_user_watches)" : "cannot watch", full);
    }
    if (wd >= npaths) {
        const int grown = wd + 256;
        paths = realloc(paths, (size_t)grown * sizeof *paths);
        memset(paths + npaths, 0, (size_t)(grown - npaths) * sizeof *paths);
        npaths = grown;
    }
    free(paths[wd]);
    paths[wd] = strdup(rel);
    DIR* dir = opendir(full);
    if (!dir) {
        return;
    }
    struct dirent* entry;
    while ((entry = readdir(dir))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[PATH_MAX];
        snprintf(child, sizeof child, "%s%s%s", rel, *rel ? "/" : "", entry->d_name);
        int is_dir = entry->d_type == DT_DIR;
        if (entry->d_type == DT_UNKNOWN) {
            char fullchild[PATH_MAX];
            struct stat st;
            snprintf(fullchild, sizeof fullchild, "%s/%s", root, child);
            is_dir = lstat(fullchild, &st) == 0 && S_ISDIR(st.st_mode);
        }
        if (is_dir && watched(child)) {
            add(child);
        }
    }
    closedir(dir);
}

static void start(void) {
    ino = inotify_init1(IN_NONBLOCK);
    if (ino < 0) {
        die("inotify", root);
    }
    add("");
}

static void event(const struct inotify_event* ev) {
    if (ev->mask & IN_Q_OVERFLOW) {
        all = 1;
        return;
    }
    if (ev->wd < 0 || ev->wd >= npaths || !paths[ev->wd] || !ev->len) {
        if (ev->mask & IN_IGNORED && ev->wd >= 0 && ev->wd < npaths) {
            free(paths[ev->wd]);
            paths[ev->wd] = NULL;
        }
        return;
    }
    const char* dir = paths[ev->wd];
    char rel[PATH_MAX];
    snprintf(rel, sizeof rel, "%s%s%s", dir, *dir ? "/" : "", ev->name);
    if (in_git(dir)) {
        if (strcmp(dir, ".git") != 0 || strcmp(ev->name, "HEAD") == 0 || strcmp(ev->name, "index") == 0 || strcmp(ev->name, "packed-refs") == 0) {
            git = 1;
        }
        if ((ev->mask & (IN_CREATE | IN_MOVED_TO)) && (ev->mask & IN_ISDIR) && watched(rel)) {
            add(rel);
        }
        return;
    }
    const int entries = ev->mask & (IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
    if (ev->mask & IN_ISDIR) {
        if (!watched(rel)) {
            return;
        }
        if (ev->mask & (IN_MOVED_FROM | IN_MOVED_TO)) {
            rebuild = 1; /* the paths of what is below it are now wrong */
        }
        if (ev->mask & (IN_CREATE | IN_MOVED_TO)) {
            add(rel);
            emit('d', rel); /* files may have landed before the watch */
        }
    } else if (strcmp(ev->name, ".git") == 0 || strncmp(rel, "node_modules/", 13) == 0 || strstr(rel, "/node_modules/")) {
        return;
    }
    emit('f', rel);
    if (entries) {
        emit('d', dir);
    }
}

static int by_text(const void* a, const void* b) {
    return strcmp(*(char* const*)a, *(char* const*)b);
}

static int flush(void) {
    if (rebuild) {
        close(ino);
        for (int i = 0; i < npaths; ++i) {
            free(paths[i]);
            paths[i] = NULL;
        }
        start();
        all = 1;
        rebuild = 0;
    }
    if (all) { /* the rest would be redundant */
        for (int i = 0; i < nlines; ++i) {
            free(lines[i]);
        }
        nlines = 0;
        fputs("a\n", stdout);
    }
    if (git) {
        fputs("g\n", stdout);
    }
    qsort(lines, (size_t)nlines, sizeof *lines, by_text);
    for (int i = 0; i < nlines; ++i) {
        if (i == 0 || strcmp(lines[i], lines[i - 1]) != 0) {
            fprintf(stdout, "%s\n", lines[i]);
        }
    }
    for (int i = 0; i < nlines; ++i) {
        free(lines[i]);
    }
    const int any = all || git || nlines;
    nlines = git = all = 0;
    if (!any) {
        return 1;
    }
    fputs(".\n", stdout);
    return fflush(stdout) == 0;
}

int main(int argc, char** argv) {
    if (argc != 3 || strcmp(argv[1], "watch") != 0) {
        fprintf(stderr, "usage: imcode-helper watch <folder>\n");
        return 64;
    }
    root = argv[2];
    start(); /* every folder is watched before this says it is live: a partial watch would stop the app's polling */
    struct utsname name;
    uname(&name);
    printf("imcode-helper 1 %s %s\n", name.sysname, name.machine);
    fflush(stdout);
    char buffer[64 * 1024] __attribute__((aligned(__alignof__(struct inotify_event))));
    struct pollfd fds[2] = {{.fd = ino, .events = POLLIN}, {.fd = STDIN_FILENO, .events = POLLIN}};
    int pending = 0;
    struct timespec first = {0, 0};
    for (;;) {
        fds[0].fd = ino;
        /* 100 ms of quiet before a batch goes out (a build writes thousands of files), 500 ms at most */
        int wait = -1;
        if (pending) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            const long waited = (now.tv_sec - first.tv_sec) * 1000 + (now.tv_nsec - first.tv_nsec) / 1000000;
            wait = waited >= 500 ? 0 : 100;
        }
        const int ready = poll(fds, 2, wait);
        if (ready < 0 && errno != EINTR) {
            return 2;
        }
        if (fds[1].revents & (POLLIN | POLLHUP)) { /* the app closing the channel closes this */
            char drop[256];
            if (read(STDIN_FILENO, drop, sizeof drop) <= 0) {
                return 0;
            }
        }
        if (ready > 0 && (fds[0].revents & POLLIN)) {
            for (ssize_t got; (got = read(ino, buffer, sizeof buffer)) > 0;) {
                for (char* at = buffer; at < buffer + got;) {
                    const struct inotify_event* ev = (const struct inotify_event*)at;
                    event(ev);
                    at += sizeof *ev + ev->len;
                }
            }
            if (!pending) {
                clock_gettime(CLOCK_MONOTONIC, &first);
            }
            pending = 1;
        }
        if (pending && (ready == 0 || wait == 0)) {
            pending = 0;
            if (!flush()) {
                return 0;
            }
        }
    }
}
