#pragma once

#include "IconsFontAwesome6.h"
#include "acp_host/host.hpp"
#include "acp_host/registry.hpp"
#include "git.hpp"
#include "history.hpp"
#include "imshell/app.hpp"
#include "imshell/editor.hpp"
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
#include <set>
#include <string>
#include <vector>

// the window: onboarding, then a sidebar of threads and the open thread. one file per
// screen part (onboarding.cpp, palette.cpp, sidebar.cpp, thread_view.cpp, terminal_view.cpp,
// files_view.cpp)
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
    std::string openFolder; // `imcode <folder>`: opened as a project with a new thread
    int recall = -1; // the sent prompt shown by up/down, counted back from the newest; -1 = none
    bool settingsWant = false;
    int switchProjectTo = -1; // picked in the projects popup: applied after the frame's ui
    bool sidebarOpen = false; // narrow windows (phones): the thread list covers the thread
    bool devOpenModels = false; // ui checks: open the model popup once
    std::string switchAgentTo; // picked in the model popup: applied after the frame's ui
    // a shell per thread in its folder, started when its panel is first shown
    std::map<const Thread*, std::unique_ptr<Terminal>> terminals;
    bool terminalOpen = false;
    bool focusTerminal = false;
    std::string terminalRun; // typed into the open thread's terminal once it is up
    bool terminalExited = false; // the open thread's shell ended this frame
    float sidebarWidth = 264;
    float terminalSplit = 0.62f; // share of the height the chat keeps above the terminal

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
        float split = 0.42f;  // share of the width the chat keeps
        float treeWidth = 220;
        int epoch = 0;        // bumps with the folder: what was loading for the old one is dropped
        std::string host, cwd;
        std::map<std::string, Listing> dirs; // by path under cwd, "" = cwd itself
        std::set<std::string> expanded;
        char filter[128] = {};
        Listing index; // every file under cwd, for the filter
        bool indexed = false;
        Doc doc;
        std::string want; // the file to read next
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

    // the agent's own mark, tinted like text; a generic one when there is none
    // rowHeight: the height of the row it sits in (a frame-height row of buttons), 0 = a text line
    void agentIcon(const std::string& agentId, float size = 16, float rowHeight = 0);

    void threadView(Thread& t);
    void transcriptItem(Thread& t, Item& item, int index, float width);
    void composer(Thread& t, float width);

    void toggleTerminal();
    void terminalPanel(Thread& t);

    void filesPoll(); // once per frame: results in, the next load out
    void showFile(const std::string& path); // under the thread's folder
    // splitWidth: the width shared with the chat on its left; 0 = the panel is alone
    // true when one of its handles was let go: its sizes want saving
    bool filesPanel(Thread& t, float splitWidth);
};

// a full-width row to click: rounded, lit when chosen or under the mouse. its content is
// drawn after it, over it
// a handle between two panels side by side, as tall as `height`: lit while hovered or
// dragged, with a resting line when `line`. returns how far it was dragged this frame;
// *released is set when the drag ends (time to save)
// shortcut: imshell's Splitter only splits top from bottom; this belongs there
float sideSplitter(const char* id, float height, bool line, bool* released);
bool rowButton(const char* id, bool chosen, float height, ImGuiSelectableFlags flags = 0);

// a project's mark: two letters of its name on a square in a colour of its own
void projectBadge(const std::string& name, float size = 16);

// every file of a project, by git's list when it is a repository (files_view.cpp); blocks
std::vector<std::string> projectFiles(const std::string& host, const std::string& cwd);

// "now", "5m", "2h", "3d"
std::string ago(long long unixSeconds);
