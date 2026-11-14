#include "acp_host/client.hpp"
#include "acp_host/agents.hpp"
#include "acp_host/registry.hpp"
#include "acp_host/host.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include "acp_host/host.hpp"
#include <chrono>
#include <utility>

namespace {
    // passes lines through and reports each one: the log view of what crossed the wire
    class TracedTransport : public acp::Transport {
    public:
        TracedTransport(std::unique_ptr<acp::Transport> inner, std::function<void(char, const std::string&)> trace)
            : inner_(std::move(inner)), trace_(std::move(trace)) {}
        bool readLine(std::string& line) override {
            const bool ok = inner_->readLine(line);
            if (ok) {
                trace_('<', line);
            }
            return ok;
        }
        bool writeLine(const std::string& line) override {
            trace_('>', line);
            return inner_->writeLine(line);
        }
        void close() override {
            inner_->close();
        }
        [[nodiscard]] bool isOpen() const override {
            return inner_->isOpen();
        }

    private:
        std::unique_ptr<acp::Transport> inner_;
        std::function<void(char, const std::string&)> trace_;
    };
} // namespace

using json = nlohmann::json;

AcpClient::~AcpClient() {
    stop();
}

std::pair<bool, std::string>
AcpClient::start(const std::vector<std::string>& argv, const std::string& cwd,
                 const std::string& mcpUrl, const std::string& mcpName, const std::string& mcpToken,
                 const std::string& resumeSessionId,
                 const std::vector<std::pair<std::string, std::string>>& extraEnv,
                 const std::string& preferredAuthMethod) {
    if (conn_ && conn_->isRunning()) {
        return {true, ""};
    }
    cwd_ = cwd;
    mcpUrl_ = mcpUrl;
    mcpName_ = mcpName;
    mcpToken_ = mcpToken;
    resumeSessionId_ = resumeSessionId;

    acp::SpawnOptions opts;
    // an agent started over ssh works in a folder that is not on this machine
    if (std::error_code ec; std::filesystem::is_directory(cwd, ec)) {
        opts.cwd = cwd;
    }
    // GUI apps on macOS get a minimal PATH; use the login shell's so agents
    // installed via npm/homebrew are found
    if (const std::string& path = AcpAgents::loginShellPath(); !path.empty()) {
        opts.env.emplace_back("PATH", path);
    }
    // claude refuses to start when it thinks it is nested in another session;
    // inherited when the host app itself was launched from one
    opts.dropEnv = {"CLAUDECODE", "CLAUDE_CODE_ENTRYPOINT",
                    // the agent runs where the window is: started from an ssh shell, gemini
                    // would think it is remote and print a sign-in url into the protocol
                    "SSH_CONNECTION", "SSH_CLIENT", "SSH_TTY"};
    // the managed bun keeps its package cache next to itself, not in ~/.bun; the
    // user's own bun keeps theirs
    if (const auto bun = AcpRegistry::bunPath(); bun && !argv.empty() && argv.front() == *bun) {
        for (const auto& kv : AcpRegistry::bunEnv())
            opts.env.push_back(kv);
    }
    // api keys saved in AI Settings; a key already in the environment wins
    exportedApiKey_ = false;
    for (const auto& [name, value] : extraEnv) {
        if (!value.empty() && !std::getenv(name.c_str())) {
            opts.env.emplace_back(name, value);
            exportedApiKey_ = true;
        }
    }
    authTried_ = false;
    preferredAuth_ = preferredAuthMethod;
    sessionRequested_ = false;
    initialized_ = false;
    startParams_ = {argv, cwd, mcpUrl, mcpName, mcpToken, extraEnv};

    if (open_) {
        std::string err;
        auto transport = open_([this](const std::string& line) { agentStderr(line); }, err);
        if (!transport) {
            return {false, err};
        }
        conn_ = std::make_unique<acp::Connection>(*this, traced(std::move(transport)));
    } else {
        if (!opts.onStderr) {
            opts.onStderr = [this](const std::string& line) { agentStderr(line); };
        }
        auto [process, err] = acp::ProcessTransport::spawn(argv, opts);
        if (!process) {
            return {false, err};
        }
        conn_ = std::make_unique<acp::Connection>(*this, traced(std::move(process)));
    }
    // no fs and no terminal (acp v2 drops both). terminal sign-in
    // yes: claude offers no method without it (the panel opens the system terminal)
    acp::ClientCapabilities caps;
    caps.authTerminal = true;
    conn_->initialize(caps, acp::kProtocolVersion, [this](acp::Response r) { onInitialized(r); });
    return {true, ""};
}

std::pair<bool, std::string> AcpClient::startOver(OpenTransport open, const std::string& cwd,
                                                  const std::string& resumeSessionId) {
    open_ = std::move(open);
    return start({}, cwd, "", "", "", resumeSessionId);
}

void AcpClient::onInitialized(const acp::Response& r) {
    initialized_ = true;
    if (!r.ok()) {
        pushEvent({.type = AcpEvent::Type::Error, .text = r.error->describe()});
        return;
    }
    const auto init = acp::InitializeResult::fromJson(r.result);
    loadSession_ = init.agentCapabilities.loadSession;
    resumeSession_ = init.agentCapabilities.sessionResume;
    listSessions_ = init.agentCapabilities.sessionList;
    authMethods_ = init.authMethods;

    mcpServers_ = json::array();
    if (!mcpUrl_.empty() && init.agentCapabilities.mcpHttp) {
        acp::McpServerHttp server;
        server.name = mcpName_;
        server.url = mcpUrl_;
        if (!mcpToken_.empty()) {
            server.headers.emplace_back("Authorization", "Bearer " + mcpToken_);
        }
        mcpServers_.push_back(server.toJson());
    }
    if (authFirst_ && !preferredAuth_.empty()) {
        authFirst_ = false;
        authTried_ = true;
        authenticateWith(preferredAuth_); // opens the session once it succeeds
        return;
    }
    openSession();
}

void AcpClient::retrySession() {
    if (conn_ && conn_->isRunning() && !sessionReady_)
        openSession();
}

void AcpClient::openSession() {
    sessionRequested_ = true;
    if (!resumeSessionId_.empty() && historyShown_ && resumeSession_) {
        acp::ResumeSessionRequest req;
        req.sessionId = resumeSessionId_;
        req.cwd = cwd_;
        req.mcpServers = mcpServers_;
        conn_->resumeSession(req, [this](acp::Response res) { onSessionOpened("session/resume", res); });
        return;
    }
    if (!resumeSessionId_.empty() && loadSession_) {
        conn_->loadSession(resumeSessionId_, cwd_, mcpServers_,
                           [this](acp::Response res) { onSessionOpened("session/load", res); });
        return;
    }
    if (!resumeSessionId_.empty()) {
        pushEvent({.type = AcpEvent::Type::Info,
                   .text = "This agent cannot resume sessions. Starting a new one."});
        resumeSessionId_.clear();
    }
    conn_->newSession(cwd_, mcpServers_,
                      [this](acp::Response res) { onSessionOpened("session/new", res); });
}

namespace {
    // the useful part of an agent error is often nested in data (gemini wraps a
    // google 403 as -32603 "Internal error"), so look at all of it
    bool isAuthError(const acp::RpcError& err) {
        std::string lower = err.describe() + " " + err.data.dump();
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        for (const char* needle : {"auth", "api key", "unauthorized", "login", "permission_denied",
                                   "forbidden", "\"code\":401", "\"code\":403"}) {
            if (lower.find(needle) != std::string::npos) {
                return true;
            }
        }
        return err.code == -32000;
    }
} // namespace

// acp: session/new answers auth_required until authenticate() succeeds. a remembered
// method (or the api-key one when a key went into the env) is tried once on its own;
// otherwise the user picks from the agent.s methods
void AcpClient::handleAuthError(const acp::RpcError& err) {
    AcpHost::logf(acp::LogLevel::Info, "ACP: agent wants authentication: {}", err.describe());
    if (!authTried_) {
        std::string pick;
        for (const auto& m : authMethods_) {
            if (m.id == preferredAuth_) {
                pick = m.id;
            }
        }
        if (pick.empty() && exportedApiKey_) {
            for (const auto& m : authMethods_) {
                if (m.id.find("api") != std::string::npos) {
                    pick = m.id;
                    break;
                }
            }
        }
        if (!pick.empty()) {
            authTried_ = true;
            beginAuth(pick);
            return;
        }
    }
    pushEvent({.type = AcpEvent::Type::AuthRequired, .authMethods = authMethods_});
}

void AcpClient::authenticateWith(const std::string& methodId) {
    std::string name = methodId;
    for (const auto& m : authMethods_) {
        if (m.id == methodId && !m.name.empty()) {
            name = m.name;
        }
    }
    pushEvent({.type = AcpEvent::Type::Info,
               .text = "Signing in: " + name + ". If a browser window opened, finish there."});
    signingIn_ = true;
    conn_->authenticate(methodId, [this](acp::Response res) {
        signingIn_ = false;
        if (!res.ok()) {
            pushEvent({.type = AcpEvent::Type::Error,
                       .text = "Sign-in failed: " + res.error->describe()});
            pushEvent({.type = AcpEvent::Type::AuthRequired, .authMethods = authMethods_});
            return;
        }
        if (sessionReady_) {
            pushEvent({.type = AcpEvent::Type::Info, .text = "Signed in."});
        } else {
            openSession();
        }
    });
}

// before any session: authenticate in place. after one: restart the process and
// authenticate first, which is the only order gemini accepts
void AcpClient::beginAuth(const std::string& methodId) {
    preferredAuth_ = methodId;
    if (!sessionRequested_) {
        authenticateWith(methodId);
        return;
    }
    std::lock_guard lock(restartMutex_);
    pendingRestartAuth_ = methodId;
}

void AcpClient::authenticate(const std::string& methodId) {
    if (!conn_) {
        return;
    }
    authTried_ = true;
    beginAuth(methodId);
}

void AcpClient::tick() {
    if (restartMethod_.empty()) {
        {
            std::lock_guard lock(restartMutex_);
            restartMethod_.swap(pendingRestartAuth_);
        }
        if (restartMethod_.empty())
            return;
        pushEvent({.type = AcpEvent::Type::Info, .text = "Restarting the agent to sign in..."});
        // stopping waits up to a second for the process and joins its reader: not
        // here. ponytail: a late reply from the old process still reaches us
        stopping_.start([old = std::shared_ptr<acp::Connection>(std::move(conn_))] {
            if (old)
                old->stop();
            return true;
        });
        sessionReady_ = false;
        turnActive_ = false;
        return;
    }
    if (stopping_.isRunning() && !stopping_.check())
        return;
    const std::string method = std::exchange(restartMethod_, {});
    const StartParams p = startParams_;
    authFirst_ = true;
    auto [ok, err] = start(p.argv, p.cwd, p.mcpUrl, p.mcpName, p.mcpToken, "", p.extraEnv, method);
    if (!ok) {
        pushEvent({.type = AcpEvent::Type::Error, .text = err});
    }
}

void AcpClient::onSessionOpened(const std::string& method, const acp::Response& r) {
    if (!r.ok()) {
        if (isAuthError(*r.error) && !authMethods_.empty()) {
            handleAuthError(*r.error);
            return;
        }
        if (method != "session/new") {
            // pruned, or open in another client (a terminal): the app asks whether to try
            // again or start over, so a conversation is never dropped behind the user's back
            const json& data = r.error->data;
            pushEvent({.type = AcpEvent::Type::ResumeFailed,
                       .text = r.error->describe(),
                       .reason = data.is_object() ? data.value("reason", "") : ""});
            return;
        }
        pushEvent({.type = AcpEvent::Type::Error, .text = r.error->describe()});
        return;
    }
    {
        std::lock_guard lock(stateMutex_);
        sessionId_ = method == "session/new" ? r.result.value("sessionId", "") : resumeSessionId_;
    }
    pushEvent({.type = AcpEvent::Type::ConfigOptions,
               .configOptions = acp::configOptionsFromJson(r.result)});
    json queued;
    {
        std::lock_guard lock(stateMutex_);
        sessionReady_ = true;
        queued.swap(queued_);
    }
    AcpHost::logf(acp::LogLevel::Info, "ACP: {} ready", method);
    pushEvent({.type = AcpEvent::Type::SessionReady});
    if (!queued.is_null()) {
        prompt(queued);
    }
}

void AcpClient::stop() {
    if (conn_) {
        conn_->stop();
        conn_.reset();
    }
    sessionReady_ = false;
    turnActive_ = false;
    std::lock_guard lock(stateMutex_);
    queued_ = json();
}

bool AcpClient::isRunning() const {
    return conn_ && conn_->isRunning();
}

bool AcpClient::isSessionReady() const {
    return sessionReady_ && isRunning();
}

bool AcpClient::isTurnActive() const {
    return turnActive_ && isRunning();
}

std::string AcpClient::sessionId() {
    std::lock_guard lock(stateMutex_);
    return sessionId_;
}

void AcpClient::prompt(const json& contentBlocks) {
    if (turnActive_ || !isRunning()) {
        return;
    }
    {
        // sent from the reader thread when the session opens: an app whose ui loop is
        // asleep (a window in the background) still gets its first prompt out
        std::lock_guard lock(stateMutex_);
        if (!sessionReady_) {
            queued_ = contentBlocks;
            return;
        }
    }
    turnActive_ = true;
    conn_->prompt(sessionId(), contentBlocks, [this](acp::Response r) {
        turnActive_ = false;
        if (!r.ok() && isAuthError(*r.error) && !authMethods_.empty()) {
            authTried_ = false; // credentials expired mid-conversation: ask again
            handleAuthError(*r.error);
            return;
        }
        if (!r.ok()) {
            pushEvent({.type = AcpEvent::Type::Error, .text = r.error->describe()});
            return;
        }
        pushEvent({.type = AcpEvent::Type::TurnEnded, .text = r.result.value("stopReason", "")});
    });
}

void AcpClient::listSessions() {
    if (!isSessionReady() || !listSessions_) {
        return;
    }
    acp::ListSessionsRequest request;
    request.cwd = cwd_;
    conn_->listSessions(request, [this](acp::Response r) {
        if (r.ok()) {
            pushEvent({.type = AcpEvent::Type::Sessions,
                       .sessions = acp::ListSessionsResponse::fromJson(r.result).sessions});
        }
    });
}

void AcpClient::cancelTurn() {
    if (isSessionReady()) {
        conn_->cancel(sessionId());
    }
}

void AcpClient::setConfigOption(const std::string& configId, std::string value) {
    if (!isSessionReady()) {
        return;
    }
    conn_->setSessionConfigOption(sessionId(), configId, std::move(value), [this](acp::Response r) {
        if (!r.ok()) {
            pushEvent({.type = AcpEvent::Type::Error,
                       .text = "Could not change agent setting: " + r.error->describe()});
            return;
        }
        pushEvent({.type = AcpEvent::Type::ConfigOptions,
                   .configOptions = acp::configOptionsFromJson(r.result)});
    });
}

void AcpClient::respondPermission(const json& rpcId, const std::string& optionId) {
    if (conn_) {
        conn_->respondPermission(rpcId, optionId);
    }
}

std::vector<AcpEvent> AcpClient::drainEvents() {
    std::lock_guard lock(stateMutex_);
    std::vector<AcpEvent> out;
    out.swap(events_);
    return out;
}

void AcpClient::pushEvent(AcpEvent ev) {
    std::lock_guard lock(stateMutex_);
    events_.push_back(std::move(ev));
    AcpHost::wake(); // reader thread: wake the idle loop to drain it
}

// ---------------------------------------------------------------- acp::Client

void AcpClient::sessionUpdate(const acp::SessionNotification& n) {
    using K = acp::SessionUpdate::Kind;
    const auto& u = n.update;
    AcpEvent ev;
    switch (u.kind) {
    case K::UserMessageChunk:
    case K::AgentMessageChunk:
    case K::AgentThoughtChunk:
        if (u.text.empty()) {
            return;
        }
        ev.type = u.kind == K::UserMessageChunk    ? AcpEvent::Type::UserMessageDelta
                  : u.kind == K::AgentMessageChunk ? AcpEvent::Type::MessageDelta
                                                   : AcpEvent::Type::ThoughtDelta;
        ev.text = u.text;
        break;
    case K::ToolCall:
    case K::ToolCallUpdate:
        ev.type = u.kind == K::ToolCall ? AcpEvent::Type::ToolCall : AcpEvent::Type::ToolCallUpdate;
        ev.tool = {u.toolCall.id, u.toolCall.title, u.toolCall.kind, u.toolCall.status,
                   u.toolCall.contentText(), {}, {}};
        for (const auto& location : u.toolCall.locationList()) {
            ev.tool.paths.push_back(location.path);
        }
        for (const auto& item : u.toolCall.content.is_array() ? u.toolCall.content : json::array()) {
            if (item.is_object() && item.value("type", "") == "diff") {
                const auto text = [&](const char* key) {
                    const auto it = item.find(key);
                    return it != item.end() && it->is_string() ? it->get<std::string>() : std::string();
                };
                ev.tool.diffs.push_back({text("path"), text("oldText"), text("newText")});
            }
        }
        break;
    case K::Plan:
        ev.type = AcpEvent::Type::Plan;
        ev.plan = u.plan;
        break;
    case K::AvailableCommandsUpdate:
        ev.type = AcpEvent::Type::Commands;
        for (const auto& c : u.commands) {
            ev.commands.push_back({c.name, c.description, c.inputHint});
        }
        break;
    case K::ConfigOptionsUpdate:
        ev.type = AcpEvent::Type::ConfigOptions;
        ev.configOptions = u.configOptions;
        break;
    case K::UsageUpdate:
        ev.type = AcpEvent::Type::Usage;
        ev.used = u.used;
        ev.size = u.size;
        if (u.cost.is_object() && u.cost.contains("amount") && u.cost["amount"].is_number()) {
            ev.cost = u.cost["amount"].get<double>();
        }
        break;
    case K::SessionInfoUpdate:
        if (u.title.empty()) {
            return;
        }
        ev.type = AcpEvent::Type::Title;
        ev.text = u.title;
        break;
    default:
        return; // current_mode_update and unknown kinds
    }
    pushEvent(std::move(ev));
}

void AcpClient::requestPermission(const acp::PermissionRequest& req) {
    AcpEvent ev;
    ev.type = AcpEvent::Type::Permission;
    ev.permission.rpcId = req.rpcId;
    ev.permission.title = req.toolCall.title.empty() ? "Agent action" : req.toolCall.title;
    ev.permission.options = req.options;
    pushEvent(std::move(ev));
}

void AcpClient::agentStderr(const std::string& line) {
    AcpHost::logf(acp::LogLevel::Info, "ACP agent stderr: {}", line);
    trace('!', line);
    std::lock_guard lock(stateMutex_);
    stderrTail_.push_back(line);
    if (stderrTail_.size() > 12)
        stderrTail_.erase(stderrTail_.begin());
}

void AcpClient::agentExited() {
    sessionReady_ = false;
    signingIn_ = false;
    turnActive_ = false;
    std::string tail;
    {
        std::lock_guard lock(stateMutex_);
        for (const auto& line : stderrTail_)
            tail += (tail.empty() ? "" : "\n") + line;
        stderrTail_.clear();
    }
    pushEvent({.type = AcpEvent::Type::Exited, .text = std::move(tail)});
}

std::unique_ptr<acp::Transport> AcpClient::traced(std::unique_ptr<acp::Transport> inner) {
    return std::make_unique<TracedTransport>(std::move(inner), [this](char dir, const std::string& line) { trace(dir, line); });
}

void AcpClient::trace(char dir, const std::string& line) {
    constexpr size_t keep = 3000, longest = 8000; // lines kept; a line is cut (a pasted image is megabytes)
    const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::lock_guard lock(wireMutex_);
    wire_.push_back({dir, ms, line.size() > longest ? line.substr(0, longest) + "... (" + std::to_string(line.size()) + " bytes)" : line});
    if (wire_.size() > keep) {
        wire_.erase(wire_.begin(), wire_.begin() + static_cast<std::ptrdiff_t>(wire_.size() - keep));
    }
    ++wireCount_;
}

std::vector<AcpClient::WireLine> AcpClient::wire(size_t& count) {
    std::lock_guard lock(wireMutex_);
    count = wireCount_;
    return wire_;
}
