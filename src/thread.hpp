#pragma once

#include "acp_host/agents.hpp"
#include "acp_host/client.hpp"
#include "diff.hpp"
#include <memory>
#include <optional>
#include <utility>
#include <string>
#include <vector>

// one conversation with one agent in one folder, here or on another machine. no ui in
// here: this is the part a remote client will later drive through commands.
struct Item {
    enum class Kind { User, Assistant, Thought, Tool, Plan, Permission, Auth, Info, Error };
    Kind kind{};
    std::string text;
    // Tool
    std::string toolId, toolTitle, toolStatus, toolOutput;
    struct Change { // a file the tool edited
        std::string path;
        std::vector<Diff::Line> lines;
        int added = 0, removed = 0;
    };
    std::vector<Change> changes;
    std::string toolPath; // the file the tool is about, under the project; empty = none there
    // Plan
    std::vector<AcpPlanEntry> plan;
    // Permission, Auth: what was asked, and the answer once given
    AcpPermissionRequest permission;
    std::vector<acp::AuthMethod> authMethods;
    bool answered = false;
    std::string choice;
    bool terminalSignIn = false; // a terminal sign-in is open: offer Continue
};

class Thread {
public:
    // host: empty = this machine, else an ssh host (see Ssh::session) and cwd is a folder there
    Thread(std::string cwd, std::string agentId, std::string host = "");

    std::string title;     // first prompt, shortened
    std::string cwd;       // absolute, on the host
    std::string root;      // the project's folder when cwd is a worktree of it; else empty
    bool wantWorktree = false; // the first message makes a worktree and works there
    std::string sessionId; // the agent's id for this conversation: what a restart resumes
    long long updatedAt = 0; // unix seconds of the last prompt or reply
    const std::string agentId;
    const std::string host;
    std::vector<Item> items;
    std::vector<std::string> queue; // sent one by one as turns end
    std::vector<acp::ConfigOption> config; // model, mode, ... as the agent offers them
    std::vector<AcpCommand> commands;      // its slash commands
    long long contextUsed = 0, contextSize = 0; // tokens, as the agent last reported
    double cost = -1;                           // of the session so far; -1 = not reported
    // other conversations the agent holds for this folder (started elsewhere, e.g. in a
    // terminal); asked once when the session opens. taken by the app, which lists them
    std::vector<acp::SessionInfo> found;

    // the project this belongs to: its folder, also when the work happens in a worktree
    [[nodiscard]] const std::string& projectDir() const {
        return root.empty() ? cwd : root;
    }
    void poll(); // once per frame
    // shown for the first time: start the agent, so its settings (model, mode) are there
    // before the first message and a saved thread replays its history
    void open();
    void send(const std::string& text);
    void cancel();
    void setConfig(const std::string& id, const std::string& value);
    void answerPermission(Item& item, const AcpPermissionOption& option);
    void signIn(Item& item, const acp::AuthMethod& method);
    void continueAfterTerminalSignIn(Item& item);

    [[nodiscard]] bool starting() const;    // the agent is coming up: no session yet
    [[nodiscard]] bool busy() const;        // a turn is running, or waits for the session
    [[nodiscard]] bool needsAnswer() const; // an unanswered permission or sign-in
    [[nodiscard]] int revision() const {    // bumps when items change: scroll to the end
        return revision_;
    }
    // true once after title, cwd or sessionId changed: time to save
    bool takeChanged() {
        return std::exchange(changed_, false);
    }

private:
    bool ensureStarted();
    Item* findTool(const std::string& id);
    // a path the agent named, as the project sees it; unchanged when outside it
    [[nodiscard]] std::string underProject(std::string path) const;
    void add(Item item);

    std::unique_ptr<AcpClient> acp_;
    std::string heldUser_; // sent while history replays: shown after it, where it belongs
    bool replaying_ = false;
    bool queued_ = false; // a prompt waits for the session
    bool changed_ = false;
    int revision_ = 0;
};
