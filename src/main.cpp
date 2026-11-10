#include "app.hpp"

#include "markdown.hpp"
#include "imgui_internal.h"
#include "mydear/alert.hpp"
#include "mydear/button.hpp"
#include "mydear/components.hpp"
#include "mydear/embedded.hpp"
#include "mydear/fonts.hpp"
#include "mydear/image.hpp"
#include "mydear/splitter.hpp"
#include "mydear/frames.hpp"
#include "mydear/themes.hpp"
#include "mydear/title_bar.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <format>

namespace Theme = mydear::Theme;
namespace ui = mydear::ui;

std::string ago(long long unixSeconds) {
    const long long s = std::max(0LL, static_cast<long long>(std::time(nullptr)) - unixSeconds);
    return unixSeconds == 0 ? ""
           : s < 60         ? "now"
           : s < 3600       ? std::format("{}m", s / 60)
           : s < 86400      ? std::format("{}h", s / 3600)
                            : std::format("{}d", s / 86400);
}

void App::setup() {
    Theme::Current() = &Theme::themeById("tokyo-night");
    Theme::ApplyTheme(Theme::Current()->colors);
    static const ImWchar iconRanges[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};
    const EmbeddedFont* text = findEmbeddedFont("JetBrainsMono-Medium");
    const EmbeddedFont* icons = findEmbeddedFont("fa-solid-900");
    if (text && icons) {
        mydear::AddFont({{text->data, text->size}, {icons->data, icons->size, iconRanges, true}},
                        15.0f);
    }
    AcpHost::setWake([] { mydear::requestFrames(); });
    // IMCODE_DATA_DIR: another folder for the saved data (a second profile, ui checks)
    const char* dataDir = std::getenv("IMCODE_DATA_DIR");
    const std::filesystem::path dir = dataDir ? dataDir : mydear::DataDir("imcode");
    AcpRegistry::setInstallRoot(dir / "agents");
    dataFile = dir / "threads.json";
    data = Store::load(dataFile);
    project = data.projects.empty() ? -1 : 0;
    startScan();
}

void App::agentIcon(const std::string& agentId, float size) {
    auto it = icons.find(agentId);
    if (it == icons.end()) {
        const EmbeddedImage* image = findEmbeddedImage(("agent-" + agentId).c_str());
        const ImTextureID texture =
            image && shell ? mydear::LoadTexture(image->data, image->size,
                                                 [this](const uint8_t* rgba, int w, int h) {
                                                     return shell->createTexture(rgba, w, h);
                                                 })
                           : ImTextureID{};
        it = icons.emplace(agentId, texture).first;
    }
    if (it->second == ImTextureID{}) {
        ImGui::TextUnformatted(ICON_FA_ROBOT);
        return;
    }
    // centred on the text of the line it sits in (which a frame-height row pushes down)
    const float line = ImGui::GetTextLineHeight();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                         ImGui::GetCurrentWindow()->DC.CurrLineTextBaseOffset + (line - size) * 0.5f);
    ImGui::ImageWithBg(it->second, ImVec2(size, size), ImVec2(0, 0), ImVec2(1, 1), ImVec4(0, 0, 0, 0),
                       ImGui::GetStyleColorVec4(ImGuiCol_Text));
}

void App::startScan() {
    scanOp.start([] {
        Scan out;
        // the cli each agent wraps: installed means the user works with it
        static const std::pair<const char*, const char*> clis[] = {{"claude-code", "claude"},
                                                                   {"codex", "codex"},
                                                                   {"gemini", "gemini"},
                                                                   {"grok", "grok"},
                                                                   {"cursor", "cursor-agent"}};
        for (const auto& def : AcpAgents::catalog()) {
            out.ready.push_back(AcpAgents::resolveInvocation(def).has_value());
            const auto cli = std::ranges::find(clis, def.id, [](const auto& c) {
                return std::string(c.first);
            });
            out.detected.push_back(cli != std::end(clis) &&
                                   AcpAgents::locateExecutable(cli->second).has_value());
        }
        out.history = History::claudeCode();
        return out;
    });
}

bool App::wantsFrames() {
    return scanOp.isRunning() || palette.op.isRunning() || registry.isBusy() ||
           std::ranges::any_of(data.threads,
                               [](const auto& t) { return t->busy() || t->starting(); });
}

void App::save() {
    Store::save(dataFile, data);
}

bool App::agentOn(const std::string& id) const {
    return std::ranges::find(data.agents, id) != data.agents.end();
}

std::vector<const AcpAgentDef*> App::agentsOn() const {
    std::vector<const AcpAgentDef*> out;
    for (const auto& def : AcpAgents::catalog()) {
        if (agentOn(def.id) || data.agents.empty()) {
            out.push_back(&def);
        }
    }
    return out;
}

void App::dropDraft() {
    if (current >= 0 && data.threads[current]->title.empty()) {
        terminals.erase(data.threads[current].get());
        data.threads.erase(data.threads.begin() + current); // stops its agent
        current = -1;
        save();
    }
}

void App::select(int index) {
    if (index != current) {
        const Thread* keep = index >= 0 ? data.threads[index].get() : nullptr;
        dropDraft();
        index = -1;
        for (int i = 0; keep && i < static_cast<int>(data.threads.size()); ++i) {
            if (data.threads[i].get() == keep) {
                index = i; // the draft before it is gone
            }
        }
    }
    current = index;
    sidebarOpen = false;
    shownRevision = -1;
    focusInput = true;
    if (index >= 0) {
        Thread& t = *data.threads[index];
        project = addProject(t.host, t.cwd);
        t.open();
    }
}

int App::addProject(const std::string& host, const std::string& cwd) {
    const Store::Project p{host, cwd};
    auto it = std::ranges::find(data.projects, p);
    if (it == data.projects.end()) {
        data.projects.push_back(p);
        save();
        return static_cast<int>(data.projects.size()) - 1;
    }
    return static_cast<int>(it - data.projects.begin());
}

void App::newThread(int projectIndex, std::string agentId) {
    if (projectIndex < 0 || projectIndex >= static_cast<int>(data.projects.size())) {
        paletteOpen(); // a thread needs a folder first
        return;
    }
    const auto on = agentsOn();
    if (agentId.empty()) {
        // the agent of the thread being left, else the first one turned on
        agentId = current >= 0 ? data.threads[current]->agentId : on.front()->id;
    }
    const Store::Project p = data.projects[projectIndex];
    dropDraft();
    data.threads.push_back(std::make_unique<Thread>(p.cwd, agentId, p.host));
    project = projectIndex;
    current = -1;
    select(static_cast<int>(data.threads.size()) - 1);
}

void App::frame() {
    bool changed = false;
    for (auto& t : data.threads) {
        t->poll(); // every thread, shown or not
        changed |= t->takeChanged();
    }
    if (changed) {
        save();
    }
    scanOp.check([this](Scan got) {
        scan = std::move(got);
        // first launch: the agents whose cli is here start turned on
        if (!scanned && !data.onboarded && data.agents.empty()) {
            const auto& catalog = AcpAgents::catalog();
            for (size_t i = 0; i < catalog.size(); ++i) {
                if (scan.ready[i] && scan.detected[i]) {
                    data.agents.push_back(catalog[i].id);
                }
            }
        }
        scanned = true;
    });
    pollInstall();

    const auto& colors = Theme::Current()->colors;
    const bool narrow = mydear::ContentRect().size.x < 720; // one pane at a time
    const mydear::TitleBar bar = mydear::BeginTitleBar();
    if (data.onboarded && narrow) {
        if (mydear::SmallButton(ICON_FA_BARS)) {
            sidebarOpen = !sidebarOpen;
        }
        ImGui::SameLine();
    }
    ImGui::AlignTextToFramePadding();
    if (data.onboarded && current >= 0) {
        const Thread& t = *data.threads[current];
        ImGui::TextColored(colors.subtext0, "%s  /",
                           std::filesystem::path(t.cwd).filename().string().c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted(t.title.empty() ? "New thread" : t.title.substr(0, narrow ? 22 : 70).c_str());
        // at the right end, before the os's own buttons
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(bar.pos.x + bar.size.x - bar.rightInset - 40,
                                         ImGui::GetCursorScreenPos().y));
        if (mydear::SmallButton(ICON_FA_TERMINAL, terminalOpen ? mydear::ButtonVariant::Primary
                                                               : mydear::ButtonVariant::Secondary)) {
            toggleTerminal();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Terminal (Ctrl+`)");
        }
    } else {
        ImGui::TextColored(colors.subtext0, "imcode"); // an empty bar asserts (mydear#45)
    }
    mydear::EndTitleBar();

    const mydear::Rect content = mydear::ContentRect();
    ImGui::SetNextWindowPos(content.pos);
    ImGui::SetNextWindowSize(content.size);
    ImGui::Begin("main", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    if (!data.onboarded) {
        onboarding();
    } else {
        const bool list = !narrow || sidebarOpen || current < 0;
        if (list) {
            ImGui::BeginChild("sidebar", ImVec2(narrow ? 0 : 264, 0), ImGuiChildFlags_Borders);
            sidebar();
            ImGui::EndChild();
            ImGui::SameLine();
        }
        ImGui::BeginChild("thread", ImVec2(0, 0), 0,
                          narrow && list ? ImGuiWindowFlags_NoInputs : 0);
        if (current >= 0) {
            Thread& t = *data.threads[current];
            const float height = ImGui::GetContentRegionAvail().y;
            ImGui::BeginChild("chat", ImVec2(0, terminalOpen ? height * terminalSplit : 0));
            threadView(t);
            ImGui::EndChild();
            if (terminalOpen) {
                mydear::Splitter("##terminal", &terminalSplit, height, 160, 80);
                terminalPanel(t);
            }
        } else if (ui::EmptyState(data.projects.empty() ? "No project yet" : "No thread open",
                                  data.projects.empty()
                                      ? "Add a folder on this machine, or on another one over ssh."
                                      : "Start a thread in one of your projects.",
                                  data.projects.empty() ? "Add project" : "New thread")) {
            newThread(project);
        }
        ImGui::EndChild();
    }
    // cmd/ctrl+shift+p: quick actions
    const ImGuiIO& io = ImGui::GetIO();
    if (data.onboarded && (io.KeySuper || io.KeyCtrl) && io.KeyShift &&
        ImGui::IsKeyPressed(ImGuiKey_P, false)) {
        paletteOpen(true);
    }
    // ctrl+`: the terminal of the open thread
    if (data.onboarded && current >= 0 && (io.KeySuper || io.KeyCtrl) &&
        ImGui::IsKeyPressed(ImGuiKey_GraveAccent, false)) {
        toggleTerminal();
    }
    // ui checks: IMCODE_DEV="new;palette;actions;models;send:<text>" (any of them), each once
    if (const char* dev = std::getenv("IMCODE_DEV"); dev && data.onboarded) {
        static int frames = 0;
        ++frames;
        const std::string all = std::string(";") + dev + ";";
        if (frames == 5 && (all.find(";new;") != std::string::npos)) {
            newThread(project);
        }
        if (frames == 40 && ((all.find(";palette;") != std::string::npos) || (all.find(";actions;") != std::string::npos))) {
            paletteOpen((all.find(";actions;") != std::string::npos));
        }
        if (const auto at = all.find(";send:"); frames == 40 && at != std::string::npos && current >= 0) {
            data.threads[current]->send(all.substr(at + 6, all.find(';', at + 1) - at - 6));
        }
        devOpenModels = frames == 40 && (all.find(";models;") != std::string::npos);
        if (frames == 40 && all.find(";browse;") != std::string::npos) {
            paletteOpen();
            paletteGo("", data.projects.empty() ? "" : data.projects.front().cwd);
        }
        if (frames == 40 && all.find(";term;") != std::string::npos) {
            toggleTerminal();
        }
        mydear::requestFrames();
    }
    paletteDraw();
    if (!switchAgentTo.empty()) {
        newThread(project, std::exchange(switchAgentTo, {}));
    }
    ImGui::End();
    ui::RenderToasts();
    mydear::Alert::render(); // last: over everything
}

int main() {
    mydear::Config config;
    config.title = "imcode";
    config.appName = "imcode";
    config.titleBar = true;
    // IMCODE_WINDOW=<width>x<height>: the first window size (ui checks of narrow layouts)
    if (const char* size = std::getenv("IMCODE_WINDOW")) {
        std::sscanf(size, "%dx%d", &config.width, &config.height);
    }
    static App app;
    // not mydear::Run: the app needs the shell for its texture upload (mydear#47)
    static mydear::App shell;
    app.shell = &shell;
    if (!shell.create(config, app)) {
        return 1;
    }
    shell.run();
    app.icons.clear();
    Markdown::shutdown();
    // stop the shells and the agents while the loop they wake still exists, not during static teardown
    app.terminals.clear();
    app.data.threads.clear();
    AcpHost::setWake(nullptr);
    shell.destroy();
    return 0;
}
