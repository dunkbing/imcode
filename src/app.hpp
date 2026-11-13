#pragma once

#include "IconsFontAwesome6.h"
#include "acp_host/host.hpp"
#include "acp_host/registry.hpp"
#include "git.hpp"
#include "git_graph.hpp"
#include "history.hpp"
#include "imshell/app.hpp"
#include "imshell/editor.hpp"
#include "imshell/frames.hpp"
#include "ssh.hpp"
#include "store.hpp"
#include "terminal.hpp"
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <map>
#include <chrono>
#include <set>
#include <string>
#include <vector>

// a group of resizable panels shows a half-laid-out frame when it first appears. that frame is
// laid out as usual and covered with the window's own colour, and the next one asked for
struct Settle {
    int drawnAt = -10; // the frame it was last drawn on
    // true on the first frame of a run of frames; `enable` false: not drawn now (a later run starts anew)
    bool first(bool enable = true) {
        if (!enable) {
            return false;
        }
        const int frame = ImGui::GetFrameCount();
        const bool starts = drawnAt < frame - 1;
        drawnAt = frame;
        return starts;
    }
};
struct CoverFirstFrame {
    ImVec2 min, max;
    bool on;
    CoverFirstFrame(bool first, ImVec2 size) : min(ImGui::GetCursorScreenPos()), max(min.x + size.x, min.y + size.y), on(first) {
        if (on) {
            imshell::requestFrames();
        }
    }
    ~CoverFirstFrame() {
        if (on) {
            ImGui::GetForegroundDrawList()->AddRectFilled(min, max, ImGui::GetColorU32(ImGuiCol_WindowBg));
        }
    }
};

// the window: onboarding, then a sidebar of threads and the open thread. one file per
// screen part (onboarding.cpp, palette.cpp, sidebar.cpp, thread_view.cpp, terminal_view.cpp,
// files_view.cpp)
// corner radii, one scale for the whole app: dialogs and popups, big panels (composer, message
// bubble), and rows, chips and hover fills
namespace Round {
    constexpr float Dialog = 6, Panel = 8, Row = 4;
} // namespace Round

struct App : imshell::Delegate {
    imshell::App* shell = nullptr; // the window: uploads textures
    bool headless = false;        // no window (headless.cpp): frames are drawn into memory
    std::function<ImTextureID(const uint8_t* rgba, int width, int height)> upload;
    Store::Data data;
    std::filesystem::path dataFile;
    int current = -1; // index into data.threads
    int project = -1; // the folder new threads go to
    int filterProject = -1; // the sidebar lists this project's threads; -1 = all projects
    char projectSearch[64] = {};
    bool showArchived = false; // the sidebar lists the archived threads instead
    int renaming = -1;         // the thread whose name is being edited in the list
    std::string renameText;
    char input[8192] = {};
    std::vector<std::string> attachments; // files for the next message: picked, or dropped on the window
    char search[128] = {};
    // below 0 = bring the transcript to its end (for that many frames): a thread was opened or the user sent something.
    // what the agent writes does not move it (the user may be reading further up)
    int shownRevision = -3;
    bool focusInput = false;
    std::string pressedSession; // a notification clicked: its thread opens next frame
    std::string openFolder; // `imcode <folder>`: opened as a project with a new thread
    int recall = -1; // the sent prompt shown by up/down, counted back from the newest; -1 = none
    bool settingsWant = false;
    int switchProjectTo = -1; // picked in the projects popup: applied after the frame's ui
    bool sidebarOpen = false; // narrow windows (phones): the thread list covers the thread
    bool devOpenModels = false; // ui checks: open the model popup once
    bool devModeSet = false;    // ui checks: ;mode:<value>; applied
    std::string switchAgentTo; // picked in the model popup: applied after the frame's ui
    // a shell per thread in its folder, started when its panel is first shown
    std::map<const Thread*, std::unique_ptr<Terminal>> terminals;
    // per thread: the runs of tool calls opened (by their first item), and when the running turn began
    std::map<const Thread*, std::set<size_t>> openGroups;
    std::map<const Thread*, std::chrono::steady_clock::time_point> turnSince;
    // cmd+f over the open thread: the matching items, the one shown, and where each item was drawn
    struct Find {
        bool open = false, focus = false, jump = false;
        char query[256] = "";
        int at = 0;
        std::vector<size_t> hits;
        std::vector<float> itemY;
    } find;
    bool terminalOpen = false;
    bool focusTerminal = false;
    std::string terminalRun; // typed into the open thread's terminal once it is up
    bool terminalExited = false; // the open thread's shell ended this frame
    float sidebarWidth = 264;
    float terminalHeight = 240; // pixels

    // the open thread's folder beside the chat: a tree and one file, read-only. loaded off
    // the ui thread: the folder may be on another machine
    struct Files {
        struct Listing {
            int epoch = 0;
            std::string dir, error;
            std::vector<Ssh::Entry> entries;
        };
        struct Doc {
            int epoch = 0;
            std::string path, text, note; // note: why nothing, or not all of it, is shown
            std::string image;            // a picture's bytes, in place of text
            std::vector<int> lines;       // where each line starts in text, then text's size
            std::vector<bool> carry;      // per line: it starts inside a block comment
            int widest = 0;               // index of the longest line
        };
        imshell::TextEditor editor; // shows doc
        // the picture being shown, uploaded from doc.image
        // shortcut: its texture is not freed when another replaces it (imshell has no call for that yet)
        ImTextureID picture{};
        int pictureWidth = 0, pictureHeight = 0;
        std::string picturePath;
        std::string shownPath;      // what the editor holds
        size_t shownSize = 0;
        bool open = false;
        bool treeOver = true; // narrow windows: the tree covers the file
        bool toTop = false;   // a new file: scroll back
        float width = 0;      // pixels beside the chat; 0: not sized yet
        float treeWidth = 220;
        Settle settle;
        int epoch = 0;        // bumps with the folder: what was loading for the old one is dropped
        std::string host, cwd;
        std::map<std::string, Listing> dirs; // by path under cwd, "" = cwd itself
        std::set<std::string> expanded;
        char filter[128] = {};
        Listing index; // every file under cwd, for the filter
        bool indexed = false;
        Doc doc;
        std::string want; // the file to read next
        std::string historyOf; // a file the tree menu asked the history of
        int tab = 0;           // the right panel shows: 0 the files, 1 the changes
        AcpHost::AsyncOp<Listing> listOp, indexOp;
        AcpHost::AsyncOp<Doc> readOp;
    } files;

    // what is on this machine, looked up off the ui thread (the login shell is slow)
    struct Scan {
        std::vector<bool> ready;    // per catalog entry: it can be launched now
        std::vector<bool> detected; // its own cli is installed: on by default
        std::vector<History::Found> history;
    };
    AcpHost::AsyncOp<Scan> scanOp;
    AcpHost::AsyncOp<std::vector<History::Found>> historyOp; // the clis' sessions, looked at every minute
    long long historyAtMs = 0;
    Scan scan;
    bool scanned = false;
    bool importHistory = true;
    AcpRegistryClient registry;
    std::map<std::string, ImTextureID> icons;
    std::string installing; // agent id whose runtime or binary is being downloaded

    // "Add project": sources, then a host for a remote one, then a folder browser
    struct Palette {
        enum class Step { Actions, Sources, Host, Browse };
        struct Listing {
            bool ok = false;
            Ssh::Error error;
            std::string path;
            std::vector<std::string> dirs;
        };
        bool wantOpen = false;
        bool focus = false;
        Step step = Step::Sources;
        std::string query, secret, error;
        int selected = 0;
        bool askSecret = false;
        std::string host; // empty = this machine
        std::string home, dir;
        std::vector<std::string> dirs;
        AcpHost::AsyncOp<Listing> op;
    } palette;

    // the host: agents are polled here, between frames and while the window is in the
    // background (frames stop then). one lock for the threads; a frame holds it throughout.
    // shortcut: a thread inside the app; its own process with the sync protocol later
    std::mutex hostMutex;
    std::condition_variable_any hostWake;
    std::thread host;
    std::atomic<bool> hostStop{false};
    std::atomic<long long> lastFrameMs{0}; // no frame for a while = nobody is looking
    void hostLoop();
    void stopHost();

    // imshell::Delegate
    void setup() override;
    void frame() override;
    bool wantsFrames() override;
    void openFile(const std::string& path) override; // dropped on the window: attached

    void save();
    void applyTheme(const std::string& id); // an imshell theme, kept in the saved data
    void startScan();
    [[nodiscard]] bool agentOn(const std::string& id) const;
    [[nodiscard]] std::vector<const AcpAgentDef*> agentsOn() const;
    void select(int index);
    void dropDraft(); // a thread nothing was sent to is not kept
    int addProject(const std::string& host, const std::string& cwd);
    void newThread(int projectIndex, std::string agentId = "");

    void onboarding();
    void agentRows(bool narrow);
    void settingsDraw();
    // the changes of the open thread's folder since the last commit, in a dialog
    struct Changes {
        bool want = false, reload = false;
        std::string host, cwd;
        std::string from, to; // a turn's trees: what it changed; empty: since the last commit
        std::string title;    // what the dialog says it shows; empty: by from
        std::vector<Git::Change> list;
        int selected = 0;
        float listWidth = 280, listHeight = 200; // the files beside the diff (dialog), above it (docked)
        double readAt = 0;                         // docked: when the list was last read
        Settle settle;
        bool folded[3] = {};  // the list sections folded away: staged, tracked, not tracked
        std::string diffFor; // the file `lines` is (being) read for
        std::vector<Diff::Line> lines;
        AcpHost::AsyncOp<std::vector<Git::Change>> listOp;
        AcpHost::AsyncOp<std::pair<std::string, std::vector<Diff::Line>>> diffOp;
        std::string selKey;                        // the picked row (path, side), kept across reloads
        AcpHost::AsyncOp<std::string> opOp;        // stage, unstage or discard: the error, empty when done
        std::vector<Git::Change> asking;           // files to discard, until the user says yes
        Git::Change hunkFile;                      // a hunk to discard, until the user says yes
        int hunkIndex = -1;
        std::vector<Diff::Line> hunkLines;
        bool askWant = false;
        // the commit bar: a message, then commit (and push), then a pull request
        char message[2048] = "";
        bool pushed = false; // the last commit was pushed: a pull request can follow
        std::string said;    // what git or gh answered when it refused
        struct Done {
            bool ok = false, push = false, pull = false;
            std::string output, url;
        };
        AcpHost::AsyncOp<Done> commitOp;
    };
    Changes changes, dock; // the dialog (what a turn or a commit changed) and the right panel's tab (the folder's)
    void changesOpen(Thread& t, const std::string& from = "", const std::string& to = "");
    void changesOpenAt(const std::string& host, const std::string& cwd, const std::string& from, const std::string& to,
                       const std::string& title);
    void changesDraw(Changes& c, bool docked); // docked: in the right panel, not the dialog
    void dockOpen(Thread& t);                  // the folder's changes, in the right panel
    // the history of the open thread's repository, as a graph in a dialog
    struct Graph {
        bool want = false, reload = false, done = false;
        std::string host, cwd, path; // path: only the commits that changed this file
        char search[128] = {};       // matches stay lit, the rest dim; Enter jumps to the next
        bool focusSearch = false;
        std::vector<Git::LogEntry> commits;
        std::vector<Git::GraphRow> rows;
        int lanes = 1; // the widest row
        int selected = 0;
        struct Page {
            std::string key; // host and folder it was read for
            size_t skip = 0;
            std::vector<Git::LogEntry> got;
        };
        AcpHost::AsyncOp<Page> op;
        // the selected commit opened out under its row
        bool expanded = false, reveal = false;
        std::map<std::string, Git::CommitDetail> cache; // commits read in full, by hash
        AcpHost::AsyncOp<std::pair<std::string, Git::CommitDetail>> detailOp;
    } graph;
    void graphOpen(Thread& t, const std::string& path = {});
    void graphDraw();
    // the agent log of the open thread: every line in and out
    struct Log {
        bool want = false;
        Thread* thread = nullptr;
        bool follow = true;
    } log;
    void logOpen(Thread& t);
    void logDraw();
    void projectPopup(); // "projects": pick the folder a new thread goes to
    void installAgent(const AcpAgentDef& def);
    void pollInstall();

    void paletteOpen(bool actions = false);
    void paletteGo(const std::string& host, std::string path);
    void paletteDraw();

    void sidebar();

    // the composer's picker: the agent's commands after "/", the project's files after "@"
    struct Picker {
        std::optional<std::string> replace; // the input's new text, set through its callback
        int selected = 0;
        std::string dismissed; // the text escape was pressed on: no picker until it changes
        std::string filesFor;  // host and folder `files` lists
        std::vector<std::string> files;
        AcpHost::AsyncOp<std::pair<std::string, std::vector<std::string>>> op;
    } picker;

    // the open thread's repository: asked again every few seconds, off the ui thread
    Git::Status git;
    std::string gitFor; // host and folder `git` describes
    long long gitAt = 0;
    AcpHost::AsyncOp<std::pair<std::string, Git::Status>> gitOp;
    // under the composer: the project (click to pick another) and its branch
    void statusLine(Thread& t, float left);
    void attachedRows(float left);
    static bool imageFile(const std::string& path);
    float attachedStrip() const; // what the attachment tiles take above the composer: 0 for none
    struct Thumb {
        ImTextureID tex{};
        int w = 0, h = 0;
    };
    std::map<std::string, Thumb> thumbs; // attachments seen, by path

    // the agent's own mark, tinted like text; a generic one when there is none
    // rowHeight: the height of the row it sits in (a frame-height row of buttons), 0 = a text line
    void agentIcon(const std::string& agentId, float size = 16, float rowHeight = 0);
    void agentIconAt(const std::string& agentId, ImVec2 min, float size); // drawn only, no item

    void threadView(Thread& t);
    void threadBody(Thread& t);
    void transcriptItem(Thread& t, Item& item, int index, float width);
    // onHunk (changes dialog): a button per hunk, called with its number and 's'tage, 'u'nstage or 'd'iscard
    bool diffBlock(const std::string& path, int added, int removed, const std::vector<Diff::Line>& lines, float maxHeight,
                   const std::function<void(int, char)>& onHunk = nullptr, bool staged = false);
    bool toolGroupRow(Thread& t, size_t from, size_t to, float width, bool shown);
    void composer(Thread& t, float width);

    void toggleTerminal();
    void terminalPanel(Thread& t);

    void filesPoll(); // once per frame: results in, the next load out
    void showFile(const std::string& path); // under the thread's folder
    // splitWidth: the width shared with the chat on its left; 0 = the panel is alone
    // true when one of its handles was let go: its sizes want saving
    bool filesPanel(Thread& t, bool beside); // true when a divider was let go
};

// a full-width row to click: rounded, lit when chosen or under the mouse. its content is
// drawn after it, over it
// a handle between two panels side by side, as tall as `height`: lit while hovered or
// dragged, with a resting line when `line`. returns how far it was dragged this frame;
// *released is set when the drag ends (time to save)
// shortcut: imshell's Splitter only splits top from bottom; this belongs there
bool rowButton(const char* id, bool chosen, float height, ImGuiSelectableFlags flags = 0, float rounding = Round::Row);

// a project's mark: two letters of its name on a square in a colour of its own
void projectBadge(const std::string& name, float size = 16);
void drawProjectBadge(ImDrawList* draw, ImVec2 min, const std::string& name, float size);

// every file of a project, by git's list when it is a repository (files_view.cpp); blocks
std::vector<std::string> projectFiles(const std::string& host, const std::string& cwd);

// "now", "5m", "2h", "3d"
std::string ago(long long unixSeconds);
// the clock time of a unix time, in this machine's zone
std::tm localTm(time_t when);
