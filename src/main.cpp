#include "acp_host/host.hpp"
#include "acp_host/registry.hpp"
#include "mydear/alert.hpp"
#include "mydear/app.hpp"
#include "mydear/button.hpp"
#include "mydear/components.hpp"
#include "mydear/embedded.hpp"
#include "mydear/file_dialog.hpp"
#include "mydear/fonts.hpp"
#include "mydear/frames.hpp"
#include "mydear/menu.hpp"
#include "mydear/themes.hpp"
#include "mydear/title_bar.hpp"
#include "ssh.hpp"
#include "store.hpp"
#include <algorithm>
#include <filesystem>
#include <format>
#include <thread>

namespace Theme = mydear::Theme;
namespace ui = mydear::ui;

// "Open folder": this machine through the system dialog, or a host over ssh and then a
// folder there, browsed over sftp
struct OpenFolder {
    struct Listing {
        bool ok = false;
        Ssh::Error error;
        std::string path; // absolute
        std::vector<std::string> dirs;
    };
    bool wantOpen = false;
    std::string host, secret; // as typed
    bool askSecret = false;
    std::string connected; // the host being browsed; empty = picking one
    std::string home, path, pathInput, error;
    std::vector<std::string> dirs;
    bool showHidden = false;
    AcpHost::AsyncOp<Listing> op;

    // connect when needed, then list path ("" = the home folder)
    void browse(const std::string& onHost, std::string folder) {
        if (op.isRunning() || onHost.empty()) {
            return;
        }
        if (folder.starts_with("~")) {
            folder.replace(0, 1, home);
        }
        error.clear();
        op.start([onHost, folder, secret = secret] {
            Listing out;
            const auto session = Ssh::session(onHost, secret, out.error);
            if (!session) {
                return out;
            }
            out.path = session->realpath(folder.empty() ? "." : folder, out.error.message);
            if (!out.path.empty()) {
                out.dirs = session->listDirs(out.path, out.error.message);
                out.ok = out.error.message.empty();
            }
            return out;
        });
    }

    void poll() {
        op.check([this](Listing got) {
            error = got.error.message;
            askSecret = got.error.needsSecret;
            if (!got.ok) {
                return;
            }
            if (connected.empty()) {
                home = got.path; // the first listing of a host is its home folder
            }
            connected = host;
            path = pathInput = got.path;
            dirs = std::move(got.dirs);
            secret.clear();
        });
    }
};

struct T3 : mydear::Delegate {
    Store::Data data;
    std::filesystem::path dataFile;
    int current = -1;
    char input[8192] = {};
    int shownRevision = -1; // of the current thread: scroll down when it moves on
    bool focusInput = false;
    OpenFolder openFolder;

    void setup() override {
        Theme::Current() = &Theme::themeById("tokyo-night");
        Theme::ApplyTheme(Theme::Current()->colors);
        if (const EmbeddedFont* font = findEmbeddedFont("JetBrainsMono-Medium")) {
            mydear::AddFont({{font->data, font->size}}, 15.0f);
        }
        AcpHost::setWake([] { mydear::requestFrames(); });
        const std::filesystem::path dir = mydear::DataDir("imcode");
        AcpRegistry::setInstallRoot(dir / "agents");
        dataFile = dir / "threads.json";
        data = Store::load(dataFile);
        // the login shell can take seconds: ask it now, not on the first message
        std::thread([] { AcpAgents::loginShellPath(); }).detach();
    }

    bool wantsFrames() override {
        return openFolder.op.isRunning() || std::ranges::any_of(data.threads, [](const auto& t) {
            return t->busy() || t->starting();
        });
    }

    void save() {
        Store::save(dataFile, data);
    }

    void select(int index) {
        current = index;
        shownRevision = -1;
        focusInput = true;
        if (index >= 0) {
            data.threads[index]->open();
        }
    }

    void addProject(const std::string& host, const std::string& cwd) {
        const Store::Project p{host, cwd};
        if (std::ranges::find(data.projects, p) == data.projects.end()) {
            data.projects.push_back(p);
            save();
        }
    }

    void newThread(const Store::Project& p, const std::string& agentId) {
        data.threads.push_back(std::make_unique<Thread>(p.cwd, agentId, p.host));
        select(static_cast<int>(data.threads.size()) - 1);
        save();
    }

    void pickLocalFolder() {
        mydear::PickFolderDialog([this](const std::string& folder) { addProject("", folder); });
    }

    void openFolderDialog() {
        OpenFolder& d = openFolder;
        if (std::exchange(d.wantOpen, false)) {
            d.connected.clear();
            d.error.clear();
            d.askSecret = false;
            ImGui::OpenPopup("Open folder");
        }
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(520, 0));
        if (!ImGui::BeginPopupModal("Open folder", nullptr,
                                    ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings)) {
            return;
        }
        d.poll();
        const auto& colors = Theme::Current()->colors;
        bool localPicked = false;
        if (d.connected.empty()) {
            if (mydear::Button("This machine...", mydear::ButtonVariant::Primary)) {
                localPicked = true;
            }
            ImGui::Spacing();
            ImGui::TextColored(colors.subtext0, "Another machine, over ssh");
            bool go = ui::TextField("Host", d.host,
                                    {.placeholder = "user@host, or a name from ~/.ssh/config",
                                     .flags = ImGuiInputTextFlags_EnterReturnsTrue});
            if (d.askSecret) {
                go |= ui::TextField("Password or key passphrase", d.secret,
                                    {.password = true, .flags = ImGuiInputTextFlags_EnterReturnsTrue});
            }
            // hosts already in use, then the ones ssh knows
            std::vector<std::string> known;
            for (const auto& p : data.projects) {
                if (!p.host.empty() && std::ranges::find(known, p.host) == known.end()) {
                    known.push_back(p.host);
                }
            }
            for (const auto& name : Ssh::configHosts()) {
                if (std::ranges::find(known, name) == known.end()) {
                    known.push_back(name);
                }
            }
            if (!known.empty()) {
                ImGui::BeginChild("hosts", ImVec2(0, 140), ImGuiChildFlags_Borders);
                for (const auto& name : known) {
                    if (ImGui::Selectable(name.c_str(), name == d.host)) {
                        d.host = name;
                        go = true;
                    }
                }
                ImGui::EndChild();
            }
            if (d.op.isRunning()) {
                ui::Progress(-1, "Connecting");
            } else {
                go |= mydear::Button("Connect");
            }
            if (go) {
                d.browse(d.host, "");
            }
        } else {
            ImGui::TextColored(colors.subtext0, "%s", d.connected.c_str());
            ImGui::SameLine();
            if (mydear::SmallButton("Change host")) {
                d.connected.clear();
            }
            if (ui::TextField("Folder", d.pathInput,
                              {.flags = ImGuiInputTextFlags_EnterReturnsTrue})) {
                d.browse(d.connected, d.pathInput);
            }
            ImGui::BeginChild("dirs", ImVec2(0, 260), ImGuiChildFlags_Borders);
            if (d.path != "/" && ImGui::Selectable("..")) {
                d.browse(d.connected, std::filesystem::path(d.path).parent_path().string());
            }
            for (const auto& name : d.dirs) {
                if (name.starts_with(".") && !d.showHidden) {
                    continue;
                }
                if (ImGui::Selectable(name.c_str())) {
                    d.browse(d.connected, (d.path == "/" ? "" : d.path) + "/" + name);
                }
            }
            ImGui::EndChild();
            ImGui::Checkbox("Hidden folders", &d.showHidden);
            if (d.op.isRunning()) {
                ImGui::SameLine();
                ui::Progress(-1, "Loading", 120);
            }
            if (mydear::Button("Open", mydear::ButtonVariant::Primary)) {
                addProject(d.connected, d.path);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::SameLine();
        if (mydear::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        if (!d.error.empty()) {
            ui::Callout(d.askSecret ? "Sign in" : "Could not open", d.error.c_str(),
                        d.askSecret ? ui::Tone::Warning : ui::Tone::Danger);
        }
        if (localPicked) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        if (localPicked) {
            pickLocalFolder(); // after the popup: the system dialog is modal itself
        }
    }

    void sidebar() {
        const auto& colors = Theme::Current()->colors;
        if (mydear::Button("Open folder", mydear::ButtonVariant::Primary, ImVec2(-FLT_MIN, 0))) {
            openFolder.wantOpen = true;
        }
        ImGui::Spacing();
        int removeProject = -1, removeThread = -1;
        for (int pi = 0; pi < static_cast<int>(data.projects.size()); ++pi) {
            const Store::Project& p = data.projects[pi];
            ImGui::PushID(pi);
            ImGui::Separator();
            // the folder, with "+" at the right: a new thread with the agent picked there
            const float plus = ImGui::GetFrameHeight();
            ImGui::BeginGroup();
            ImGui::TextUnformatted(std::filesystem::path(p.cwd).filename().string().c_str());
            ImGui::TextColored(colors.overlay1, "%s", p.host.empty() ? p.cwd.c_str()
                                                                     : (p.host + ":" + p.cwd).c_str());
            ImGui::EndGroup();
            if (ImGui::BeginPopupContextItem("folder")) {
                if (ImGui::MenuItem("Close folder")) {
                    removeProject = pi;
                }
                ImGui::EndPopup();
            }
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - plus + ImGui::GetCursorPosX());
            if (mydear::Button("+", mydear::ButtonVariant::Secondary, ImVec2(plus, plus))) {
                ImGui::OpenPopup("agents");
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("New thread");
            }
            if (ImGui::BeginPopup("agents")) {
                for (const auto& agent : AcpAgents::catalog()) {
                    if (ImGui::MenuItem(agent.name.c_str())) {
                        newThread(p, agent.id);
                    }
                }
                ImGui::EndPopup();
            }
            for (int i = 0; i < static_cast<int>(data.threads.size()); ++i) {
                const Thread& t = *data.threads[i];
                if (t.host != p.host || t.cwd != p.cwd) {
                    continue;
                }
                ImGui::PushID(i);
                const AcpAgentDef* def = AcpAgents::find(t.agentId);
                const std::string label = t.title.empty()
                                              ? "New " + (def ? def->name : t.agentId) + " thread"
                                              : t.title;
                if (ImGui::Selectable(label.c_str(), i == current)) {
                    select(i);
                }
                if (ImGui::BeginPopupContextItem("thread")) {
                    if (ImGui::MenuItem("Remove from list")) {
                        removeThread = i;
                    }
                    ImGui::EndPopup();
                }
                if (t.needsAnswer()) {
                    ui::Badge("needs you", ui::Tone::Warning);
                } else if (t.busy()) {
                    ui::Badge("working", ui::Tone::Info);
                }
                ImGui::PopID();
            }
            ImGui::PopID();
        }
        if (removeProject >= 0) {
            const Store::Project p = data.projects[removeProject];
            std::erase_if(data.threads, // stops their agents
                          [&](const auto& t) { return t->host == p.host && t->cwd == p.cwd; });
            data.projects.erase(data.projects.begin() + removeProject);
        } else if (removeThread >= 0) {
            data.threads.erase(data.threads.begin() + removeThread);
        }
        if (removeProject >= 0 || removeThread >= 0) {
            select(-1);
            save();
        }
    }

    void item(Thread& t, Item& it, int index) {
        const auto& colors = Theme::Current()->colors;
        ImGui::PushID(index);
        switch (it.kind) {
        case Item::Kind::User:
            ui::BeginCard("user", "You");
            ImGui::TextWrapped("%s", it.text.c_str());
            ui::EndCard();
            break;
        case Item::Kind::Assistant:
            // shortcut: plain text; markdown (imgui_rich_md, as in DearSQL) comes next
            ImGui::TextWrapped("%s", it.text.c_str());
            break;
        case Item::Kind::Thought:
            ImGui::PushStyleColor(ImGuiCol_Text, colors.overlay1);
            ImGui::TextWrapped("%s", it.text.c_str());
            ImGui::PopStyleColor();
            break;
        case Item::Kind::Tool: {
            const bool open =
                ImGui::TreeNodeEx("tool", it.toolOutput.empty() ? ImGuiTreeNodeFlags_Leaf : 0,
                                  "%s", it.toolTitle.empty() ? "Tool" : it.toolTitle.c_str());
            ImGui::SameLine();
            ui::Badge(it.toolStatus.empty() ? "pending" : it.toolStatus.c_str(),
                      it.toolStatus == "failed"      ? ui::Tone::Danger
                      : it.toolStatus == "completed" ? ui::Tone::Success
                                                     : ui::Tone::Info);
            if (open) {
                if (!it.toolOutput.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
                    ImGui::TextWrapped("%s", it.toolOutput.c_str());
                    ImGui::PopStyleColor();
                }
                ImGui::TreePop();
            }
            break;
        }
        case Item::Kind::Plan:
            ui::BeginCard("plan", "Plan");
            for (const auto& entry : it.plan) {
                const char* box = entry.status == "completed"     ? "[x]"
                                  : entry.status == "in_progress" ? "[~]"
                                                                  : "[ ]";
                ImGui::TextWrapped("%s %s", box, entry.content.c_str());
            }
            ui::EndCard();
            break;
        case Item::Kind::Permission:
            ui::BeginCard("permission", "Permission");
            ImGui::TextWrapped("%s", it.permission.title.c_str());
            if (it.answered) {
                ImGui::TextColored(colors.subtext0, "-> %s", it.choice.c_str());
            }
            for (size_t i = 0; !it.answered && i < it.permission.options.size(); ++i) {
                const auto& option = it.permission.options[i];
                if (i > 0) {
                    ImGui::SameLine(0, Theme::Spacing::S);
                }
                if (mydear::SmallButton(std::format("{}##p{}", option.name, i).c_str(),
                                        option.kind.starts_with("allow")
                                            ? mydear::ButtonVariant::Primary
                                            : mydear::ButtonVariant::Secondary)) {
                    t.answerPermission(it, option);
                }
            }
            ui::EndCard();
            break;
        case Item::Kind::Auth: {
            ui::BeginCard("auth", "Sign in required");
            // a copy: signing in may add an item, which moves the one `it` refers to
            std::optional<acp::AuthMethod> picked;
            if (it.answered) {
                ImGui::TextColored(colors.subtext0, "-> %s", it.choice.c_str());
                if (it.terminalSignIn) {
                    ImGui::TextWrapped("Finish signing in in the Terminal window, then continue.");
                    if (mydear::SmallButton("Continue", mydear::ButtonVariant::Primary)) {
                        t.continueAfterTerminalSignIn(it);
                    }
                }
            }
            for (size_t i = 0; !it.answered && i < it.authMethods.size(); ++i) {
                if (i > 0) {
                    ImGui::SameLine(0, Theme::Spacing::S);
                }
                if (mydear::SmallButton(std::format("{}##a{}", it.authMethods[i].name, i).c_str(),
                                        mydear::ButtonVariant::Primary)) {
                    picked = it.authMethods[i];
                }
            }
            ui::EndCard();
            if (picked) {
                t.signIn(it, *picked);
            }
            break;
        }
        case Item::Kind::Info:
            ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
            ImGui::TextWrapped("%s", it.text.c_str());
            ImGui::PopStyleColor();
            break;
        case Item::Kind::Error:
            ui::Callout("Error", it.text.c_str(), ui::Tone::Danger);
            break;
        }
        ImGui::PopID();
        ImGui::Spacing();
    }

    // the agent's own settings (model, mode, effort), one small picker each
    void configPickers(Thread& t) {
        for (const auto& option : t.config) {
            if (option.type != "select" || !option.currentValue.is_string()) {
                continue;
            }
            const std::string value = option.currentValue.get<std::string>();
            std::string shown = value;
            for (const auto& choice : option.options) {
                if (choice.value == value) {
                    shown = choice.name;
                }
            }
            ImGui::SameLine();
            ImGui::SetNextItemWidth(170);
            if (ImGui::BeginCombo(("##" + option.id).c_str(), shown.c_str())) {
                for (const auto& choice : option.options) {
                    if (ImGui::Selectable(choice.name.c_str(), choice.value == value)) {
                        t.setConfig(option.id, choice.value);
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", option.name.c_str());
            }
        }
    }

    void threadView(Thread& t) {
        const auto& colors = Theme::Current()->colors;
        const AcpAgentDef* def = AcpAgents::find(t.agentId);
        const std::string agent = def ? def->name : t.agentId;
        const float inputHeight = ImGui::GetTextLineHeight() * 4;
        ImGui::BeginChild("transcript",
                          ImVec2(0, -(inputHeight + ImGui::GetFrameHeightWithSpacing() * 2)));
        // by index: answering a sign-in can add an item and move the rest
        for (size_t i = 0; i < t.items.size(); ++i) {
            item(t, t.items[i], static_cast<int>(i));
        }
        if (t.starting()) {
            ui::Progress(-1, ("Starting " + agent).c_str());
        } else if (t.busy()) {
            ui::Progress(-1, "Working");
        }
        if (shownRevision != t.revision()) {
            shownRevision = t.revision();
            ImGui::SetScrollHereY(1.0f);
        }
        ImGui::EndChild();

        if (std::exchange(focusInput, false)) {
            ImGui::SetKeyboardFocusHere();
        }
        // enter sends, ctrl+enter is a new line
        bool send = ImGui::InputTextMultiline(
            "##input", input, sizeof(input), ImVec2(-FLT_MIN, inputHeight),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine);
        if (t.busy()) {
            if (mydear::Button("Stop", mydear::ButtonVariant::Danger)) {
                t.cancel();
            }
        } else {
            send |= mydear::Button("Send", mydear::ButtonVariant::Primary);
        }
        ImGui::SameLine();
        ImGui::TextColored(colors.subtext0, "%s", agent.c_str());
        configPickers(t); // there once the agent has started
        if (send && !t.busy() && input[0]) {
            t.send(input);
            input[0] = 0;
            focusInput = true;
        }
    }

    void frame() override {
        bool changed = false;
        for (auto& t : data.threads) {
            t->poll(); // every thread, shown or not
            changed |= t->takeChanged();
        }
        if (changed) {
            save();
        }

        mydear::BeginTitleBar();
        ImGui::TextUnformatted("imcode");
        mydear::EndTitleBar();

        const mydear::Rect content = mydear::ContentRect();
        ImGui::SetNextWindowPos(content.pos);
        ImGui::SetNextWindowSize(content.size);
        ImGui::Begin("main", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);

        ImGui::BeginChild("sidebar", ImVec2(280, 0), ImGuiChildFlags_Borders);
        sidebar();
        ImGui::EndChild();

        ImGui::SameLine();
        ImGui::BeginChild("thread");
        if (current >= 0) {
            threadView(*data.threads[current]);
        } else if (data.projects.empty()) {
            if (ui::EmptyState("No folder open",
                               "Open a folder on this machine, or on another one over ssh.",
                               "Open folder")) {
                openFolder.wantOpen = true;
            }
        } else {
            ui::EmptyState("No thread open", "Press + beside a folder and pick an agent.");
        }
        ImGui::EndChild();

        openFolderDialog();
        ImGui::End();
        ui::RenderToasts();
        mydear::Alert::render(); // last: over everything
    }
};

int main() {
    mydear::Config config;
    config.title = "imcode";
    config.appName = "imcode";
    config.titleBar = true;
    static T3 app;
    const int code = mydear::Run(config, app);
    // stop the agents while the loop they wake still exists, not during static teardown
    app.data.threads.clear();
    AcpHost::setWake(nullptr);
    return code;
}
