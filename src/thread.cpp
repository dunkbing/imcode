#include "thread.hpp"

#include "acp_host/registry.hpp"
#include "remote.hpp"
#include "ssh.hpp"
#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <utility>

using json = nlohmann::json;

Thread::Thread(std::string cwd_, std::string agentId_, std::string host_)
    : cwd(std::move(cwd_)), agentId(std::move(agentId_)), host(std::move(host_)) {}

void Thread::add(Item item) {
    items.push_back(std::move(item));
    ++revision_;
}

Item* Thread::findTool(const std::string& id) {
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        if (it->kind == Item::Kind::Tool && it->toolId == id) {
            return &*it;
        }
    }
    return nullptr;
}

bool Thread::starting() const {
    return acp_ && acp_->isRunning() && !acp_->isSessionReady();
}

bool Thread::busy() const {
    return acp_ && acp_->isRunning() && (acp_->isTurnActive() || (queued_ && starting()));
}

bool Thread::needsAnswer() const {
    return std::ranges::any_of(items, [](const Item& item) {
        return (item.kind == Item::Kind::Permission || item.kind == Item::Kind::Auth) &&
               (!item.answered || item.terminalSignIn);
    });
}

bool Thread::ensureStarted() {
    if (acp_ && acp_->isRunning()) {
        return true;
    }
    const AcpAgentDef* def = AcpAgents::find(agentId);
    // tests: IMCODE_AGENT_CMD=<binary> stands in for the picked agent (acp-echo-agent)
    const char* testCmd = std::getenv("IMCODE_AGENT_CMD");
    std::string error;
    std::string command;                          // remote
    std::optional<std::vector<std::string>> argv; // local
    if (!def && !testCmd) {
        error = "Unknown agent: " + agentId;
    } else if (!host.empty()) {
        AcpAgentDef test;
        test.runCmd = {testCmd ? testCmd : ""};
        command = Remote::agentCommand(testCmd ? test : *def, cwd);
        if (command.empty()) {
            error = def->name + " cannot be started on a remote machine yet.";
        }
    } else {
        argv = testCmd ? std::vector<std::string>{testCmd} : AcpAgents::resolveInvocation(*def);
        if (!argv) {
            // shortcut: no download of bun or registry binaries yet (the install card in
            // DearSQL's panel)
            error = def->name + " is not installed (it needs bun or its own binary).";
        }
    }
    if (!error.empty()) {
        add({.kind = Item::Kind::Error, .text = error});
        return false;
    }
    // the agent replays the conversation it kept: ours would only double it
    replaying_ = !sessionId.empty();
    if (replaying_) {
        items.clear();
        ++revision_;
    }
    acp_ = std::make_unique<AcpClient>();
    // shortcut: a reconnect to the host blocks the ui here; the open folder dialog has
    // usually connected already, then this is one round trip
    const auto [ok, err] =
        host.empty() ? acp_->start(*argv, cwd, "", "", "", sessionId)
                     : acp_->startOver(
                           [host = host, command](auto onStderr, std::string& failed)
                               -> std::unique_ptr<acp::Transport> {
                               Ssh::Error sshError;
                               const auto session = Ssh::session(host, "", sshError);
                               if (!session) {
                                   failed = sshError.message;
                                   return nullptr;
                               }
                               return session->exec(command, std::move(onStderr), failed);
                           },
                           cwd, sessionId);
    if (!ok) {
        add({.kind = Item::Kind::Error, .text = err});
        acp_.reset();
    }
    return ok;
}

void Thread::open() {
    if (!acp_ && items.empty()) {
        ensureStarted();
    }
}

void Thread::send(const std::string& text) {
    if (text.empty() || busy()) {
        return;
    }
    queued_ = starting() || !acp_;
    if (title.empty()) {
        title = text.substr(0, text.find('\n'));
    }
    updatedAt = std::time(nullptr);
    changed_ = true;
    if (!ensureStarted()) {
        return;
    }
    if (replaying_) {
        heldUser_ = text;
    } else {
        add({.kind = Item::Kind::User, .text = text});
    }
    acp_->prompt(json::array({acp::textBlock(text)})); // held until the session is ready
}

void Thread::setConfig(const std::string& id, const std::string& value) {
    if (acp_) {
        acp_->setConfigOption(id, value);
    }
}

void Thread::cancel() {
    if (acp_) {
        acp_->cancelTurn();
    }
}

void Thread::answerPermission(Item& item, const AcpPermissionOption& option) {
    if (acp_) {
        acp_->respondPermission(item.permission.rpcId, option.optionId);
    }
    item.answered = true;
    item.choice = option.name;
}

void Thread::signIn(Item& item, const acp::AuthMethod& method) {
    item.answered = true;
    item.choice = method.name;
    if (!acp_) {
        return;
    }
    if (method.type != "terminal") {
        acp_->authenticate(method.id);
        return;
    }
    // the agent's own command plus the method's args, in a terminal
    // shortcut: the terminal runs the local agent; a remote one needs a terminal over ssh
    const AcpAgentDef* def = AcpAgents::find(agentId);
    auto argv = def && host.empty() ? AcpAgents::resolveInvocation(*def) : std::nullopt;
    if (!argv) {
        add({.kind = Item::Kind::Error,
             .text = "Sign in on " + (host.empty() ? "this machine" : host) + ": " +
                     (def ? def->authHint : std::string())});
        return;
    }
    argv->insert(argv->end(), method.args.begin(), method.args.end());
    auto env = method.env;
    if (const auto bun = AcpRegistry::bunPath(); bun && argv->front() == *bun) {
        const auto extra = AcpRegistry::bunEnv();
        env.insert(env.end(), extra.begin(), extra.end());
    }
    const std::string err = AcpAgents::openInTerminal(*argv, env);
    item.terminalSignIn = err.empty();
    if (!err.empty()) {
        add({.kind = Item::Kind::Error, .text = err}); // invalidates `item`
    }
}

void Thread::continueAfterTerminalSignIn(Item& item) {
    item.terminalSignIn = false;
    if (acp_) {
        acp_->retrySession();
    }
}

void Thread::poll() {
    if (!acp_) {
        return;
    }
    acp_->tick(); // sign-in restarts happen here, on the ui thread
    bool gone = false;
    auto append = [this](Item::Kind kind, const std::string& text) {
        if (items.empty() || items.back().kind != kind) {
            items.push_back({.kind = kind});
        }
        items.back().text += text;
        ++revision_;
    };
    for (auto& ev : acp_->drainEvents()) {
        switch (ev.type) {
        case AcpEvent::Type::SessionReady:
            replaying_ = false;
            queued_ = false;
            if (const std::string id = acp_->sessionId(); id != sessionId) {
                sessionId = id;
                changed_ = true;
            }
            if (!heldUser_.empty()) {
                add({.kind = Item::Kind::User, .text = std::exchange(heldUser_, {})});
            }
            break;
        case AcpEvent::Type::MessageDelta:
            append(Item::Kind::Assistant, ev.text);
            break;
        case AcpEvent::Type::UserMessageDelta:
            append(Item::Kind::User, ev.text);
            break;
        case AcpEvent::Type::ThoughtDelta:
            append(Item::Kind::Thought, ev.text);
            break;
        case AcpEvent::Type::ToolCall:
        case AcpEvent::Type::ToolCallUpdate: {
            Item* item = findTool(ev.tool.id);
            if (!item) {
                add({.kind = Item::Kind::Tool, .toolId = ev.tool.id});
                item = &items.back();
            }
            if (!ev.tool.title.empty()) {
                item->toolTitle = ev.tool.title;
            }
            if (!ev.tool.status.empty()) {
                item->toolStatus = ev.tool.status;
            }
            item->toolOutput += ev.tool.output;
            ++revision_;
            break;
        }
        case AcpEvent::Type::Plan: {
            auto it = std::ranges::find_if(items.rbegin(), items.rend(), [](const Item& item) {
                return item.kind == Item::Kind::Plan;
            });
            if (it == items.rend()) {
                add({.kind = Item::Kind::Plan});
                it = items.rbegin();
            }
            it->plan = std::move(ev.plan);
            ++revision_;
            break;
        }
        case AcpEvent::Type::Permission: {
            Item item{.kind = Item::Kind::Permission};
            item.permission = std::move(ev.permission);
            add(std::move(item));
            break;
        }
        case AcpEvent::Type::AuthRequired: {
            Item item{.kind = Item::Kind::Auth};
            item.authMethods = std::move(ev.authMethods);
            add(std::move(item));
            break;
        }
        case AcpEvent::Type::TurnEnded:
            updatedAt = std::time(nullptr);
            changed_ = true;
            if (ev.text != "end_turn" && ev.text != "cancelled" && !ev.text.empty()) {
                add({.kind = Item::Kind::Info, .text = "Turn stopped: " + ev.text});
            }
            break;
        case AcpEvent::Type::Info:
            add({.kind = Item::Kind::Info, .text = ev.text});
            break;
        case AcpEvent::Type::Error: {
            std::string text = ev.text;
            if (const AcpAgentDef* def = AcpAgents::find(agentId);
                def && text.find("uth") != std::string::npos) {
                text += "\n" + def->authHint;
            }
            add({.kind = Item::Kind::Error, .text = std::move(text)});
            break;
        }
        case AcpEvent::Type::Exited:
            if (!ev.text.empty()) { // why it died, as the agent put it
                add({.kind = Item::Kind::Error, .text = ev.text});
            }
            add({.kind = Item::Kind::Info, .text = "Agent exited. The next message restarts it."});
            gone = true;
            break;
        case AcpEvent::Type::ConfigOptions:
            config = std::move(ev.configOptions);
            break;
        case AcpEvent::Type::Commands: // shortcut: slash commands are not offered yet
            break;
        }
    }
    if (gone) {
        acp_.reset();
        replaying_ = false;
        queued_ = false;
        heldUser_.clear();
    }
}
