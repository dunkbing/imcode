#include "app.hpp"

#include "markdown.hpp"
#include "notify.hpp"
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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <format>

namespace Theme = mydear::Theme;
namespace ui = mydear::ui;

int runHeadless(App& app, const char* out, int width, int height, int frames); // headless.cpp

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
    AcpHost::setWake([this] {
        mydear::requestFrames();
        hostWake.notify_one();
    });
    // IMCODE_DATA_DIR: another folder for the saved data (a second profile, ui checks)
    const char* dataDir = std::getenv("IMCODE_DATA_DIR");
    const std::filesystem::path dir = dataDir ? dataDir : mydear::DataDir("imcode");
    AcpRegistry::setInstallRoot(dir / "agents");
    dataFile = dir / "threads.json";
    data = Store::load(dataFile);
    project = data.projects.empty() ? -1 : 0;
    startScan();
    // ui checks: IMCODE_DEV_SEND=<text> starts a thread in the first project before any frame
    if (const char* text = std::getenv("IMCODE_DEV_SEND"); text && !data.projects.empty()) {
        const Store::Project& p = data.projects.front();
        data.threads.push_back(std::make_unique<Thread>(p.cwd, agentsOn().front()->id, p.host));
        data.threads.back()->send(text);
    }
    host = std::thread([this] { hostLoop(); });
}

void App::agentIcon(const std::string& agentId, float size) {
    auto it = icons.find(agentId);
    if (it == icons.end()) {
        const EmbeddedImage* image = findEmbeddedImage(("agent-" + agentId).c_str());
        const ImTextureID texture =
            image && upload ? mydear::LoadTexture(image->data, image->size, upload) : ImTextureID{};
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
        out.history = History::scan();
        return out;
    });
}

bool App::wantsFrames() {
    // called by the loop, outside a frame: the host may be changing the threads
    std::unique_lock lock(hostMutex, std::try_to_lock);
    if (!lock) {
        return true;
    }
    return scanOp.isRunning() || palette.op.isRunning() || registry.isBusy() ||
           files.listOp.isRunning() || files.indexOp.isRunning() || files.readOp.isRunning() ||
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

namespace {
    long long nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
} // namespace

void App::hostLoop() {
    struct Seen {
        bool busy = false, needsAnswer = false;
    };
    std::map<const Thread*, Seen> seen;
    std::unique_lock lock(hostMutex);
    while (!hostStop) {
        bool changed = false;
        const bool watched = nowMs() - lastFrameMs < 1500;
        std::map<const Thread*, Seen> now;
        for (auto& t : data.threads) {
            t->poll(); // every thread, shown or not
            changed |= t->takeChanged();
            const Seen state{t->busy(), t->needsAnswer()};
            const auto before = seen.find(t.get());
            // tell the user what they did not see happen
            if (!watched && before != seen.end() && !t->title.empty()) {
                if (state.needsAnswer && !before->second.needsAnswer) {
                    notify("Needs your answer", t->title);
                } else if (before->second.busy && !state.busy && !state.needsAnswer) {
                    notify("Finished", t->title);
                }
            }
            now[t.get()] = state;
        }
        seen = std::move(now);
        if (changed) {
            save();
            mydear::requestFrames();
        }
        hostWake.wait_for(lock, std::chrono::milliseconds(250));
    }
}

void App::stopHost() {
    hostStop = true;
    hostWake.notify_one();
    if (host.joinable()) {
        host.join();
    }
}

void App::frame() {
    std::lock_guard lock(hostMutex);
    lastFrameMs = nowMs();
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
    filesPoll();

    const auto& colors = Theme::Current()->colors;
    // without a window the bar is a plain strip, with room where the os buttons would be
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const mydear::Rect content = headless ? mydear::Rect{ImVec2(0, 28), ImVec2(display.x, display.y - 28)}
                                          : mydear::ContentRect();
    const bool narrow = content.size.x < 720; // one pane at a time
    mydear::TitleBar bar{ImVec2(0, 0), ImVec2(display.x, 28), 78, 0};
    if (headless) {
        ImGui::SetNextWindowPos(bar.pos);
        ImGui::SetNextWindowSize(bar.size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 2));
        ImGui::Begin("##bar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleVar();
        ImGui::SetCursorPosX(bar.leftInset);
    } else {
        bar = mydear::BeginTitleBar();
    }
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
        ImGui::SetCursorScreenPos(ImVec2(bar.pos.x + bar.size.x - bar.rightInset - 84,
                                         ImGui::GetCursorScreenPos().y));
        if (mydear::SmallButton(ICON_FA_TERMINAL, terminalOpen ? mydear::ButtonVariant::Primary
                                                               : mydear::ButtonVariant::Secondary)) {
            toggleTerminal();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Terminal (Ctrl+`)");
        }
        ImGui::SameLine(0, 6);
        if (mydear::SmallButton(ICON_FA_FOLDER_TREE, files.open ? mydear::ButtonVariant::Primary
                                                                : mydear::ButtonVariant::Secondary)) {
            files.open = !files.open;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Files (Ctrl+B)");
        }
    } else {
        ImGui::TextColored(colors.subtext0, "imcode"); // an empty bar asserts (mydear#45)
    }
    if (headless) {
        ImGui::End();
    } else {
        mydear::EndTitleBar();
    }

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
            const ImVec2 area = ImGui::GetContentRegionAvail();
            // the files beside the chat; alone where there is no room for both
            const bool beside = files.open && !narrow;
            if (!files.open || beside) {
                ImGui::BeginChild("work", ImVec2(beside ? area.x * files.split : 0, 0));
                ImGui::BeginChild("chat", ImVec2(0, terminalOpen ? area.y * terminalSplit : 0));
                threadView(t);
                ImGui::EndChild();
                if (terminalOpen) {
                    mydear::Splitter("##terminal", &terminalSplit, area.y, 160, 80);
                    terminalPanel(t);
                }
                ImGui::EndChild();
            }
            if (files.open) {
                filesPanel(t, beside ? area.x : 0);
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
    // ctrl+b: the files of the open thread
    if (data.onboarded && current >= 0 && (io.KeySuper || io.KeyCtrl) &&
        ImGui::IsKeyPressed(ImGuiKey_B, false)) {
        files.open = !files.open;
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
        if (frames == 40 && all.find(";files;") != std::string::npos) {
            files.open = !files.open;
        }
        if (const auto at = all.find(";file:"); frames == 40 && at != std::string::npos) {
            showFile(all.substr(at + 6, all.find(';', at + 1) - at - 6));
        }
        // later: the panel's first frame starts with an empty filter
        if (const auto at = all.find(";filter:"); frames == 45 && at != std::string::npos) {
            std::snprintf(files.filter, sizeof(files.filter), "%s",
                          all.substr(at + 8, all.find(';', at + 1) - at - 8).c_str());
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
    // IMCODE_SHOT=<file.ppm>: no window; the ui after IMCODE_SHOT_FRAMES frames (240) as a picture
    if (const char* shot = std::getenv("IMCODE_SHOT")) {
        const char* frames = std::getenv("IMCODE_SHOT_FRAMES");
        const int code = runHeadless(app, shot, config.width, config.height, frames ? std::atoi(frames) : 240);
        app.stopHost();
        app.terminals.clear();
        app.data.threads.clear();
        AcpHost::setWake(nullptr);
        return code;
    }
    // not mydear::Run: the app needs the shell for its texture upload (mydear#47)
    static mydear::App shell;
    app.shell = &shell;
    app.upload = [](const uint8_t* rgba, int w, int h) { return shell.createTexture(rgba, w, h); };
    if (!shell.create(config, app)) {
        return 1;
    }
    shell.run();
    app.icons.clear();
    Markdown::shutdown();
    // stop the shells and the agents while the loop they wake still exists, not during static teardown
    app.stopHost();
    app.terminals.clear();
    app.data.threads.clear();
    AcpHost::setWake(nullptr);
    shell.destroy();
    return 0;
}
