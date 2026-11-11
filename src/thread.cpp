#include "thread.hpp"

#include "acp_host/registry.hpp"
#include "git.hpp"
#include "remote.hpp"
#include "ssh.hpp"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ctime>
#include <utility>

using json = nlohmann::json;

Thread::Thread(std::string cwd_, std::string agentId_, std::string host_)
    : cwd(std::move(cwd_)), agentId(std::move(agentId_)), host(std::move(host_)) {}

void Thread::add(Item item) {
    items.push_back(std::move(item));
    ++revision_;
}

std::string Thread::underProject(std::string path) const {
    // the agent may name the folder through a symlink (/tmp is /private/tmp)
    for (const std::string& root : {cwd, "/private" + cwd}) {
        if (path.starts_with(root + "/")) {
            path.erase(0, root.size() + 1);
            break;
        }
    }
    return path;
}

namespace {
    std::string base64(const std::string& bytes) {
        static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out;
        out.reserve((bytes.size() + 2) / 3 * 4);
        for (size_t i = 0; i < bytes.size(); i += 3) {
            const unsigned a = static_cast<unsigned char>(bytes[i]);
            const unsigned b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
            const unsigned c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
            out += digits[a >> 2];
            out += digits[((a & 3) << 4) | (b >> 4)];
            out += i + 1 < bytes.size() ? digits[((b & 15) << 2) | (c >> 6)] : '=';
            out += i + 2 < bytes.size() ? digits[c & 63] : '=';
        }
        return out;
    }
} // namespace

Item* Thread::findTool(const std::string& id) {
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        if (it->kind == Item::Kind::Tool && it->toolId == id) {
            return &*it;
        }
    }
    return nullptr;
}

bool Thread::starting() const {
    return connectOp_.isRunning() || (acp_ && acp_->isRunning() && !acp_->isSessionReady());
}

bool Thread::busy() const {
    return (connectOp_.isRunning() && !heldPrompt_.empty()) ||
           (acp_ && acp_->isRunning() && (acp_->isTurnActive() || (queued_ && starting())));
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
    if (testCmd && !*testCmd) {
        testCmd = nullptr;
    }
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
    // the ssh handshake takes seconds: not on the ui thread. poll() comes back here
    if (!host.empty() && !Ssh::connected(host)) {
        if (!connectOp_.isRunning()) {
            connectOp_.start([host = host] {
                Ssh::Error sshError;
                return Ssh::session(host, "", sshError) ? std::string() : sshError.message;
            });
        }
        return false;
    }
    // the agent replays the conversation it kept: ours would only double it
    replaying_ = !sessionId.empty();
    if (replaying_) {
        items.clear();
        ++revision_;
    }
    acp_ = std::make_unique<AcpClient>();
    // the host is connected by now: opening the channel is one round trip
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

void Thread::send(const std::string& text, const std::vector<std::string>& files) {
    if (text.empty() && files.empty()) {
        return;
    }
    if (busy()) {
        queue.push_back({text, files}); // a turn is running: this one goes after it
        ++revision_;
        return;
    }
    queued_ = starting() || !acp_;
    if (wantWorktree && root.empty() && items.empty()) {
        // shortcut: blocks the ui for git (and ssh); with the other blocking starts, move to
        // the host thread
        std::string error;
        const std::string made = Git::addWorktree(host, cwd, Git::slug(text), error);
        if (made.empty()) {
            add({.kind = Item::Kind::Error, .text = error});
            return;
        }
        acp_.reset(); // the agent that came up for the pickers is in the project's folder
        sessionId.clear(); // and so is its session: the worktree gets a new one
        root = cwd;
        cwd = made;
        wantWorktree = false;
        add({.kind = Item::Kind::Info, .text = "Working in a new worktree: " + made});
    }
    if (title.empty()) {
        title = text.empty() ? "Attachment" : text.substr(0, text.find('\n'));
    }
    updatedAt = std::time(nullptr);
    changed_ = true;
    if (!ensureStarted()) {
        if (connectOp_.isRunning()) {
            heldPrompt_ = text; // sent from poll() once the host answers
            heldFiles_ = files;
        }
        return;
    }
    // the message's parts: its text, pictures as data, other files as links
    json blocks = json::array({acp::textBlock(text)});
    std::vector<std::string> names;
    for (const auto& file : files) {
        const std::filesystem::path path(file);
        std::string ext = path.extension().string();
        std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return std::tolower(c); });
        static const std::pair<const char*, const char*> images[] = {
            {".png", "image/png"}, {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"},
            {".gif", "image/gif"}, {".webp", "image/webp"}};
        const auto image = std::ranges::find(images, ext, [](const auto& i) { return std::string(i.first); });
        std::error_code ec;
        if (image != std::end(images)) {
            // shortcut: sent whether or not the agent said it reads pictures; one that
            // does not answers with its own error
            if (std::filesystem::file_size(path, ec) > 5'000'000 || ec) {
                add({.kind = Item::Kind::Info, .text = path.filename().string() + " was not attached (over 5 MB)."});
                continue;
            }
            std::ifstream in(path, std::ios::binary);
            const std::string bytes((std::istreambuf_iterator<char>(in)), {});
            blocks.push_back({{"type", "image"}, {"mimeType", image->second}, {"data", base64(bytes)}});
        } else if (host.empty()) {
            blocks.push_back(acp::resourceLinkBlock("file://" + file, path.filename().string()));
        } else {
            add({.kind = Item::Kind::Info,
                 .text = path.filename().string() + " was not attached: it is on this machine, the agent is on " + host + "."});
            continue;
        }
        names.push_back(path.filename().string());
    }
    if (replaying_) {
        heldUser_ = text;
    } else {
        add({.kind = Item::Kind::User, .text = text, .files = names});
    }
    acp_->prompt(blocks); // held until the session is ready
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
    // the agent's own command plus the method's args, typed into this thread's terminal:
    // the same for a folder here and one over ssh
    const AcpAgentDef* def = AcpAgents::find(agentId);
    const char* testCmd = std::getenv("IMCODE_AGENT_CMD"); // as in ensureStarted
    auto quote = [](const std::string& s) {
        std::string out = "'";
        for (const char c : s) {
            out += c == '\'' ? std::string("'\\''") : std::string(1, c);
        }
        return out + "'";
    };
    std::string command;
    for (const auto& [name, value] : method.env) {
        command += name + "=" + quote(value) + " ";
    }
    if (testCmd && *testCmd) {
        command += quote(testCmd);
    } else if (!def) {
        return;
    } else if (!host.empty() && !def->npmPackage.empty()) {
        // on the host: whichever runner it has, as the agent itself is started there
        command += "$(command -v bun >/dev/null 2>&1 && echo 'bun x --bun' || echo 'npx -y') " +
                   quote(def->npmPackage);
    } else if (!host.empty()) {
        for (const auto& part : def->runCmd) {
            command += quote(part) + " ";
        }
    } else if (const auto argv = AcpAgents::resolveInvocation(*def)) {
        if (const auto bun = AcpRegistry::bunPath(); bun && argv->front() == *bun) {
            for (const auto& [name, value] : AcpRegistry::bunEnv()) {
                command += name + "=" + quote(value) + " ";
            }
        }
        for (const auto& part : *argv) {
            command += quote(part) + " ";
        }
    } else {
        add({.kind = Item::Kind::Error, .text = def->authHint}); // invalidates `item`
        return;
    }
    for (const auto& arg : def && !(testCmd && *testCmd) ? def->runArgs : std::vector<std::string>{}) {
        command += " " + quote(arg);
    }
    for (const auto& arg : method.args) {
        command += " " + quote(arg);
    }
    terminalCommand = command;
    item.terminalSignIn = true;
}

void Thread::continueAfterTerminalSignIn(Item& item) {
    item.terminalSignIn = false;
    if (acp_) {
        acp_->retrySession();
    }
}

void Thread::poll() {
    connectOp_.check([this](std::string error) {
        const std::string text = std::exchange(heldPrompt_, {});
        const auto files = std::exchange(heldFiles_, {});
        if (!error.empty()) {
            add({.kind = Item::Kind::Error, .text = error});
        } else if (ensureStarted() && (!text.empty() || !files.empty())) {
            send(text, files);
        }
    });
    if (!acp_) {
        return;
    }
    acp_->tick(); // sign-in restarts happen here, on the ui thread
    bool gone = false;
    bool next = false; // a turn ended: the queue moves on
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
            acp_->listSessions();
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
            if (!ev.tool.paths.empty()) {
                const std::string path = underProject(ev.tool.paths.front());
                item->toolPath = path.starts_with("/") ? "" : path; // the viewer shows the project
            }
            if (!ev.tool.diffs.empty()) { // each update carries the edits as they stand
                item->changes.clear();
                for (const auto& diff : ev.tool.diffs) {
                    Item::Change change{diff.path, Diff::lines(diff.oldText, diff.newText), 0, 0};
                    change.path = underProject(change.path);
                    for (const auto& line : change.lines) {
                        change.added += line.kind == '+';
                        change.removed += line.kind == '-';
                    }
                    item->changes.push_back(std::move(change));
                }
            }
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
            next = ev.text != "cancelled"; // a stop keeps the queue for the user to decide
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
        case AcpEvent::Type::Usage:
            contextUsed = ev.used;
            contextSize = ev.size;
            cost = ev.cost >= 0 ? ev.cost : cost;
            break;
        case AcpEvent::Type::Title: // the agent's summary reads better than the first prompt
            if (!renamed) {
                title = ev.text.substr(0, 80);
                changed_ = true;
            }
            break;
        case AcpEvent::Type::Sessions:
            found = std::move(ev.sessions);
            break;
        case AcpEvent::Type::ConfigOptions:
            config = std::move(ev.configOptions);
            break;
        case AcpEvent::Type::Commands:
            commands = std::move(ev.commands);
            break;
        }
    }
    if (next && !gone && !queue.empty() && !busy()) {
        const Queued waiting = queue.front();
        queue.erase(queue.begin());
        send(waiting.text, waiting.files);
    }
    if (gone) {
        acp_.reset();
        replaying_ = false;
        queued_ = false;
        heldUser_.clear();
    }
}
