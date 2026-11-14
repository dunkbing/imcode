#pragma once

#include "acp_host/host.hpp"
#include <acp/connection.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// One agent process and its session, built on acp-cpp. The library calls
// back on its reader thread; this adapter turns those calls into AcpEvents that
// the UI drains once per frame, and runs the initialize → session/new|load
// handshake so the panel only has to wait for SessionReady.

struct AcpToolCall {
    std::string id;
    std::string title;
    std::string kind;   // read/edit/execute/search/fetch/...
    std::string status; // pending/in_progress/completed/failed
    std::string output; // extracted text content, best effort
    // file edits the call made or proposes: the whole old and new text of each file
    struct Diff {
        std::string path, oldText, newText;
    };
    std::vector<Diff> diffs;
    std::vector<std::string> paths; // the files the call is about, as the agent names them
};
using AcpPlanEntry = acp::PlanEntry;
using AcpPermissionOption = acp::PermissionOption;

struct AcpPermissionRequest {
    nlohmann::json rpcId; // JSON-RPC id to answer with
    std::string title;
    std::vector<AcpPermissionOption> options;
};

struct AcpCommand {
    std::string name;
    std::string description;
    std::string hint; // shown when the command takes arguments
};

struct AcpEvent {
    enum class Type {
        SessionReady,
        ResumeFailed, // text = why session/load failed; nothing else is started: the app decides
        Info,             // text = progress note (signing in, ...)
        UserMessageDelta, // replayed by session/load
        MessageDelta,
        ThoughtDelta,
        ToolCall,
        ToolCallUpdate,
        Plan,
        Commands, // agent published its slash commands
        ConfigOptions,
        Sessions, // the answer to listSessions(): what the agent has kept for the folder
        Usage,    // used / size = how full the context is; cost when the agent reports one
        Title,    // text = the agent's own title for the session
        Permission,
        AuthRequired, // authMethods = what the agent accepts; answer with authenticate()
        TurnEnded,    // text = stopReason
        Error,        // text = message
        Exited,       // agent process died; text = its last stderr lines
    };
    Type type{};
    std::string text;
    std::string reason; // ResumeFailed: the agent's own code for why (data.reason), as "thread_active_writer"
    AcpToolCall tool;
    std::vector<AcpPlanEntry> plan;
    std::vector<AcpCommand> commands;
    std::vector<acp::ConfigOption> configOptions;
    AcpPermissionRequest permission;
    std::vector<acp::AuthMethod> authMethods;
    std::vector<acp::SessionInfo> sessions;
    long long used = 0, size = 0; // Usage: tokens
    double cost = -1;             // Usage: in the agent's currency, -1 = not reported
};

class AcpClient final : public acp::Client {
public:
    ~AcpClient() override;

    // spawn agent + initialize + session/new. mcpUrl (optional) is offered as an
    // HTTP MCP server when the agent advertises support for it. resumeSessionId
    // asks for session/load instead when the agent supports it (history is replayed
    // as events); otherwise a new session is started.
    // Returns false with error message if the process could not be spawned.
    std::pair<bool, std::string>
    start(const std::vector<std::string>& argv, const std::string& cwd, const std::string& mcpUrl,
          const std::string& mcpName, const std::string& mcpToken,
          const std::string& resumeSessionId = "",
          const std::vector<std::pair<std::string, std::string>>& extraEnv = {},
          const std::string& preferredAuthMethod = "");
    // the same over a transport the app opens itself (an ssh channel to an agent on
    // another machine) instead of a local process. open gets the sink for the agent's
    // stderr lines and returns nullptr with an error when it cannot connect
    using OpenTransport = std::function<std::unique_ptr<acp::Transport>(
        std::function<void(const std::string&)> onStderr, std::string& error)>;
    std::pair<bool, std::string> startOver(OpenTransport open, const std::string& cwd,
                                           const std::string& resumeSessionId = "");
    void stop();

    [[nodiscard]] bool isRunning() const;
    [[nodiscard]] bool isSessionReady() const;
    // an authenticate request is out (a browser sign-in can take minutes)
    [[nodiscard]] bool isSigningIn() const {
        return signingIn_;
    }
    // the agent answered initialize. before that bun x may still be installing it
    [[nodiscard]] bool isInitialized() const {
        return initialized_;
    }
    // session/new again, after a sign-in done outside the agent (a terminal login)
    void retrySession();
    [[nodiscard]] bool isTurnActive() const;
    // set before start: the conversation is on screen already, so session/resume (no replay) is
    // used when the agent offers it
    void setHistoryShown(bool shown) {
        historyShown_ = shown;
    }
    [[nodiscard]] bool supportsLoadSession() const {
        return loadSession_;
    }
    std::string sessionId();

    // send a user prompt (array of ACP content blocks). before the session is ready the
    // prompt is held and sent as soon as it opens (one at a time: a second replaces it)
    void prompt(const nlohmann::json& contentBlocks);
    void cancelTurn();
    // the agent's sessions for this folder (the first page), as a Sessions event. nothing
    // happens when the agent cannot list them
    void listSessions();
    [[nodiscard]] bool supportsListSessions() const {
        return listSessions_;
    }
    void setConfigOption(const std::string& configId, std::string value);
    // answer an AuthRequired event. agents (gemini) only accept authenticate before
    // session/new, so this restarts the process and signs in first when a session exists
    void authenticate(const std::string& methodId);
    // ui thread, once per frame: performs restarts that callbacks could not (they run
    // on the reader thread, which stop() joins)
    void tick();
    void respondPermission(const nlohmann::json& rpcId, const std::string& optionId);

    std::vector<AcpEvent> drainEvents();

    // everything that crossed the wire, newest last: dir is > sent, < received, ! agent stderr.
    // count rises with every line, so a reader sees a change without comparing the lines
    struct WireLine {
        char dir;
        long long ms;
        std::string text;
    };
    std::vector<WireLine> wire(size_t& count);

private:
    // acp::Client
    void sessionUpdate(const acp::SessionNotification& n) override;
    void requestPermission(const acp::PermissionRequest& req) override;
    void agentStderr(const std::string& line) override;
    void agentExited() override;

    void onInitialized(const acp::Response& r);
    void openSession();
    void handleAuthError(const acp::RpcError& err);
    void authenticateWith(const std::string& methodId);
    void beginAuth(const std::string& methodId);

    struct StartParams {
        std::vector<std::string> argv;
        std::string cwd, mcpUrl, mcpName, mcpToken;
        std::vector<std::pair<std::string, std::string>> extraEnv;
    };
    OpenTransport open_; // set by startOver; kept for sign-in restarts
    StartParams startParams_;
    bool sessionRequested_ = false; // session/new or load has been sent on this process
    bool authFirst_ = false;        // authenticate(preferredAuth_) before opening a session
    std::mutex restartMutex_;
    std::string pendingRestartAuth_; // method to restart with, consumed by tick()
    void onSessionOpened(const std::string& method, const acp::Response& r);
    void pushEvent(AcpEvent ev);
    std::unique_ptr<acp::Transport> traced(std::unique_ptr<acp::Transport> inner);
    void trace(char dir, const std::string& line);
    std::mutex wireMutex_;
    std::vector<WireLine> wire_;
    size_t wireCount_ = 0;

    std::unique_ptr<acp::Connection> conn_;
    std::mutex stateMutex_; // guards events_, sessionId_, queued_
    nlohmann::json queued_; // prompt waiting for the session
    std::vector<AcpEvent> events_;
    std::vector<std::string> stderrTail_; // last lines, shown when the agent dies
    std::string sessionId_;
    std::string resumeSessionId_;
    std::string cwd_;
    std::string mcpUrl_;
    std::string mcpName_;
    std::string mcpToken_;
    nlohmann::json mcpServers_ = nlohmann::json::array();
    std::vector<acp::AuthMethod> authMethods_;
    bool exportedApiKey_ = false; // an api key went into the agent env, prefer that auth method
    bool authTried_ = false;
    std::string preferredAuth_; // method id remembered from the last successful sign-in
    std::atomic<bool> loadSession_{false};
    std::atomic<bool> resumeSession_{false}; // the agent can reopen a session without replaying it
    std::atomic<bool> historyShown_{false};  // the host already shows the conversation: no replay wanted
    std::atomic<bool> listSessions_{false};
    std::atomic<bool> sessionReady_{false};
    std::atomic<bool> turnActive_{false};
    std::atomic<bool> signingIn_{false};
    std::atomic<bool> initialized_{false};
    // a sign-in restart: the old process stops here, off the ui thread, and tick()
    // starts the new one once it is gone. last member: joined before the rest go
    std::string restartMethod_;
    AcpHost::AsyncOp<bool> stopping_;
};
