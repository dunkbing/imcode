#pragma once

#include "acp_host/agents.hpp"
#include "acp_host/client.hpp"
#include "acp_host/host.hpp"
#include "diff.hpp"
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <string>
#include <vector>

// one conversation with one agent in one folder, here or on another machine. no ui in
// here: this is the part a remote client will later drive through commands.
struct Item {
    enum class Kind { User, Assistant, Thought, Tool, Plan, Permission, Auth, Info, Error, TurnChanges };
    Kind kind{};
    std::string text;
    std::vector<std::string> files; // User: what was attached, by name
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
    bool resumeChoice = false;   // Error: the session could not be resumed; offer to try again or start over
    bool loadEarlier = false;    // Info: a button reads more of the transcript
    // TurnChanges: the folder before and after a turn (git trees), and what differs
    std::string treeFrom, treeTo;
    int filesChanged = 0, linesAdded = 0, linesRemoved = 0;
};

// the remembered choices to apply now that these options arrived. they leave `wanted`, as do
// those the agent no longer offers; one whose option is not there yet (effort, before its
// model is set) stays
std::vector<std::pair<std::string, std::string>> dueChoices(const std::vector<acp::ConfigOption>& config,
                                                            std::map<std::string, std::string>& wanted);

// a permission mode that lets the agent do anything unasked (bypass, full access, ...): shown
// as a warning, and never carried over to new threads
bool riskyMode(std::string nameOrValue);

class Thread {
public:
    // host: empty = this machine, else an ssh host (see Ssh::session) and cwd is a folder there
    Thread(std::string cwd, std::string agentId, std::string host = "");

    std::string title;     // first prompt, shortened; then the agent's own, unless renamed
    bool renamed = false;  // the user named it: the agent's title no longer replaces it
    bool archived = false; // out of the list, kept
    std::string cwd;       // absolute, on the host
    std::string root;      // the project's folder when cwd is a worktree of it; else empty
    bool wantWorktree = false; // the first message makes a worktree and works there
    std::string sessionId; // the agent's id for this conversation: what a restart resumes
    long long updatedAt = 0; // unix seconds of the last prompt or reply
    long long ownWriteAt = 0; // the same, for this app only: the agent's transcript was last written by it, not by a terminal
    const std::string agentId;
    const std::string host;
    std::vector<Item> items;
    struct Queued {
        std::string text;
        std::vector<std::string> files;
    };
    std::vector<Queued> queue; // sent one by one as turns end
    std::string terminalCommand;    // a sign-in to run in this thread's terminal: taken by the app
    std::vector<acp::ConfigOption> config; // model, mode, ... as the agent offers them
    // a new thread starts with what was last picked for its agent: option id -> value, applied
    // as the options arrive. `picked` is what the user chose here, taken by the app to remember
    std::map<std::string, std::string> wanted, picked;
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
    // its agent is needed now (the model picker, the slash commands): started if it is not
    void wake();
    // after a failed resume: the same session again, or a new one in its place
    void retryResume();
    void startOver();
    // an idle agent is stopped and its history dropped, so the
    // conversation is free for the agent's cli and comes back fresh when opened again
    void release();
    long long leftAt = 0; // when it was last left, to keep the latest idle agents running
    bool errored = false; // the last turn or start ended in an error: shown in the list until the next message
    bool unread = false; // a turn ended and the thread has not been looked at since
    [[nodiscard]] bool hasAgent() const {
        return acp_ != nullptr;
    }
    // another client (the agent's cli) is writing this conversation on this machine: shown
    // live from its transcript, read only, until it has been quiet for a while
    // read only while the other client wrote recently; followed (not read only) while it is
    // quiet, so it is noticed when the user carries on there
    [[nodiscard]] bool watching() const {
        return watching_ && live_;
    }
    // files: local paths. images go along as pictures, other files as links (not over ssh)
    void send(const std::string& text, const std::vector<std::string>& files = {});
    void cancel();
    // a queued message goes ahead of the rest right away: the running turn is stopped for it
    void sendNow(size_t queued);
    void setConfig(const std::string& id, const std::string& value);
    void answerPermission(Item& item, const AcpPermissionOption& option);
    void signIn(Item& item, const acp::AuthMethod& method);
    void continueAfterTerminalSignIn(Item& item);

    [[nodiscard]] bool starting() const;    // the agent is coming up: no session yet
    [[nodiscard]] std::string startStep() const; // which part of coming up it is on
    [[nodiscard]] bool busy() const;        // a turn is running, or waits for the session
    [[nodiscard]] bool needsAnswer() const; // an unanswered permission or sign-in
    [[nodiscard]] bool replaying() const { // a saved conversation is coming back in
        return replaying_;
    }
    [[nodiscard]] int revision() const {    // bumps when items change: scroll to the end
        return revision_;
    }
    // true once after title, cwd or sessionId changed: time to save
    // what crossed the wire to the agent; empty before it starts. count rises with every line
    std::vector<AcpClient::WireLine> wire(size_t& count) const {
        count = 0;
        return acp_ ? acp_->wire(count) : std::vector<AcpClient::WireLine>{};
    }
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
    long long ownSavedAt_ = 0;
    bool queued_ = false; // a prompt waits for the session
    // a remote host is reached off the ui thread: the result is the error, empty when
    // connected. what was sent meanwhile waits in heldPrompt_
    AcpHost::AsyncOp<std::string> connectOp_;
    std::string heldPrompt_;
    // files for a remote thread go to the host off the ui thread; the message waits for them
    struct Uploaded {
        std::map<std::string, std::string> remote; // local path -> path on the host
        std::map<std::string, std::string> failed; // local path -> why not
        std::string before;                        // the folder before the turn (a git tree), read on the host
        bool snapped = false;
    };
    AcpHost::AsyncOp<Uploaded> uploadOp_;
    // what a remote turn changed, measured off the ui thread
    AcpHost::AsyncOp<std::optional<Item>> changesOp_;
    AcpHost::AsyncOp<std::string> snapOp_; // the folder before a local turn, read off the ui thread
    AcpHost::AsyncOp<std::string> tailOp_; // a remote thread's transcript end, read over ssh while the agent is not started yet
    bool tailTried_ = false;
    std::string uploadText_;
    std::vector<std::string> uploadFiles_;
    bool uploadDropped_ = false; // stopped meanwhile: the message is not sent
    Uploaded uploaded_;          // answers for the re-entered send()
    bool sendNow_ = false; // the turn was stopped for a queued message: the queue moves on
    bool bunTried_ = false; // the managed bun was asked for once
    std::string turnBefore_; // the folder as the running turn found it (a git tree); empty: not a repository
    bool watching_ = false; // following the transcript
    bool live_ = false;     // and it was written recently
    size_t tailBytes_ = 512 * 1024; // how much of the end of a transcript is read at first; "load earlier" grows it
    bool earlierWanted_ = false;
    bool keepHistory_ = false; // the transcript is on screen: the agent's replay of it is dropped
    double traceStartedAt_ = 0, traceSentAt_ = 0; // trace.hpp: when the agent was launched, when the turn began
    bool emptyNoted_ = false; // the "nothing to show" line was added
    [[nodiscard]] bool writtenElsewhere() const; // the transcript was written lately, and not by this app
    void leaveWatch();                           // carry on here: the agent starts, the transcript stays
public:
    [[nodiscard]] bool following() const { // the transcript is on screen, no agent yet
        return watching_;
    }
    void loadEarlier() { // from the view: done in poll(), not while the items are drawn
        earlierWanted_ = true;
    }
private:
    std::string watchPath_; // the transcript followed
    size_t watchOffset_ = 0;
    bool catchingUp_ = false; // the last read was a full one: there is more
    long long watchCheckMs_ = 0;
    bool watch(); // start following the transcript; false when there is none
    void follow(); // what the followed transcript gained since the last look
    // while this app's agent sits idle: the transcript growing means another client writes it
    size_t idleSize_ = 0;
    long long idleSinceMs_ = 0, idleCheckMs_ = 0;
    void noticeElsewhere();
    std::vector<std::string> heldFiles_;
    bool changed_ = false;
    int revision_ = 0;
};
