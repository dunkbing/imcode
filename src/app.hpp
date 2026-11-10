#pragma once

#include "IconsFontAwesome6.h"
#include "acp_host/host.hpp"
#include "acp_host/registry.hpp"
#include "history.hpp"
#include "mydear/app.hpp"
#include "ssh.hpp"
#include "store.hpp"
#include <filesystem>
#include <map>
#include <string>
#include <vector>

// the window: onboarding, then a sidebar of threads and the open thread. one file per
// screen part (onboarding.cpp, palette.cpp, sidebar.cpp, thread_view.cpp)
struct App : mydear::Delegate {
    mydear::App* shell = nullptr; // the window: uploads textures
    Store::Data data;
    std::filesystem::path dataFile;
    int current = -1; // index into data.threads
    int project = -1; // the folder new threads go to
    char input[8192] = {};
    char search[128] = {};
    int shownRevision = -1; // of the current thread: scroll down when it moves on
    bool focusInput = false;
    bool sidebarOpen = false; // narrow windows (phones): the thread list covers the thread
    bool devOpenModels = false; // ui checks: open the model popup once
    std::string switchAgentTo; // picked in the model popup: applied after the frame's ui

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

    // mydear::Delegate
    void setup() override;
    void frame() override;
    bool wantsFrames() override;

    void save();
    void startScan();
    [[nodiscard]] bool agentOn(const std::string& id) const;
    [[nodiscard]] std::vector<const AcpAgentDef*> agentsOn() const;
    void select(int index);
    void dropDraft(); // a thread nothing was sent to is not kept
    int addProject(const std::string& host, const std::string& cwd);
    void newThread(int projectIndex, std::string agentId = "");

    void onboarding();
    void installAgent(const AcpAgentDef& def);
    void pollInstall();

    void paletteOpen(bool actions = false);
    void paletteGo(const std::string& host, std::string path);
    void paletteDraw();

    void sidebar();

    // the agent's own mark, tinted like text; a generic one when there is none
    void agentIcon(const std::string& agentId, float size = 16);

    void threadView(Thread& t);
    void transcriptItem(Thread& t, Item& item, int index, float width);
    void composer(Thread& t, float width);
};

// "now", "5m", "2h", "3d"
std::string ago(long long unixSeconds);
