#include "app.hpp"

#include "mydear/alert.hpp"
#include "mydear/components.hpp"
#include "mydear/embedded.hpp"
#include "mydear/fonts.hpp"
#include "mydear/frames.hpp"
#include "mydear/themes.hpp"
#include "mydear/title_bar.hpp"
#include <algorithm>
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
    const std::filesystem::path dir = mydear::DataDir("imcode");
    AcpRegistry::setInstallRoot(dir / "agents");
    dataFile = dir / "threads.json";
    data = Store::load(dataFile);
    project = data.projects.empty() ? -1 : 0;
    startScan();
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
    mydear::BeginTitleBar();
    if (data.onboarded && current >= 0) {
        const Thread& t = *data.threads[current];
        ImGui::TextColored(colors.subtext0, "%s  /",
                           std::filesystem::path(t.cwd).filename().string().c_str());
        ImGui::SameLine();
        ImGui::TextUnformatted(t.title.empty() ? "New thread" : t.title.c_str());
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
        ImGui::BeginChild("sidebar", ImVec2(264, 0), ImGuiChildFlags_Borders);
        sidebar();
        ImGui::EndChild();
        ImGui::SameLine();
        ImGui::BeginChild("thread");
        if (current >= 0) {
            threadView(*data.threads[current]);
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
    static App app;
    const int code = mydear::Run(config, app);
    // stop the agents while the loop they wake still exists, not during static teardown
    app.data.threads.clear();
    AcpHost::setWake(nullptr);
    return code;
}
