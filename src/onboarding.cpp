#include "app.hpp"

#include "mydear/button.hpp"
#include "mydear/components.hpp"
#include "mydear/themes.hpp"
#include <algorithm>
#include <cstring>
#include <format>
#include <set>
#include <unistd.h>

namespace Theme = mydear::Theme;
namespace ui = mydear::ui;

namespace {
    // a section: its name in the left column, the rows to the right of it
    // (a narrow window stacks them: the name above its rows)
    void beginSection(const char* name, bool narrow) {
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, Theme::Spacing::M));
        ImGui::TextColored(Theme::Current()->colors.subtext0, "%s", name);
        if (!narrow) {
            ImGui::SameLine(140);
        }
        ImGui::BeginGroup();
    }
    void endSection() {
        ImGui::EndGroup();
        ImGui::Dummy(ImVec2(0, Theme::Spacing::M));
    }
    // the next item ends at the right edge of the column
    void alignRight(float itemWidth) {
        ImGui::SameLine(); // then by cursor: SameLine's own offset counts from the group
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - itemWidth);
    }
    void wrapped(const ImVec4& color, const std::string& text) {
        ImGui::PushStyleColor(ImGuiCol_Text, color);
        ImGui::PushTextWrapPos(ImGui::GetWindowContentRegionMax().x);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
} // namespace

void App::installAgent(const AcpAgentDef& def) {
    if (registry.isBusy()) {
        return;
    }
    installing = def.id;
    if (!def.npmPackage.empty()) {
        registry.startInstallBun(); // npm agents run on the managed bun
    } else {
        registry.startFetch(); // then its binary, once the registry is here
    }
}

void App::pollInstall() {
    if (installing.empty() || !registry.poll()) {
        return;
    }
    const AcpAgentDef* def = AcpAgents::find(installing);
    if (!registry.error().empty()) {
        ui::Toast(registry.error().c_str(), ui::Tone::Danger, 6);
        installing.clear();
        return;
    }
    // the registry list arrived: now the binary itself
    if (def && def->npmPackage.empty() && registry.installedName().empty()) {
        if (const AcpRegistryAgent* agent = registry.find(def->registryId)) {
            registry.startInstall(*agent);
            return;
        }
        ui::Toast((def->name + " has no download for this machine").c_str(), ui::Tone::Danger, 6);
        installing.clear();
        return;
    }
    if (!agentOn(installing)) {
        data.agents.push_back(installing);
        save();
    }
    installing.clear();
    startScan(); // it is launchable now
}

void App::onboarding() {
    const auto& colors = Theme::Current()->colors;
    const bool narrow = ImGui::GetContentRegionAvail().x < 560;
    const float width = std::min(720.0f, ImGui::GetContentRegionAvail().x - 2 * Theme::Spacing::XL);
    ImGui::SetCursorPos(ImVec2((ImGui::GetWindowWidth() - width) * 0.5f, narrow ? 24 : 72));
    ImGui::BeginChild("onboarding", ImVec2(width, 0));

    char hostName[256] = "this machine";
    gethostname(hostName, sizeof(hostName));
    if (char* dot = std::strchr(hostName, '.')) {
        *dot = 0; // "name.local"
    }
    ImGui::PushFont(nullptr, 24.0f);
    ImGui::TextUnformatted("Welcome to imcode");
    ImGui::PopFont();
    ImGui::TextColored(colors.subtext0, "Setting up %s.", hostName);
    ImGui::Dummy(ImVec2(0, Theme::Spacing::XL));

    beginSection("Agents", narrow);
    if (!scanned) {
        ui::Progress(-1, "Looking for agents");
    }
    const auto& catalog = AcpAgents::catalog();
    for (size_t i = 0; scanned && i < catalog.size(); ++i) {
        const AcpAgentDef& def = catalog[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::AlignTextToFramePadding();
        agentIcon(def.id);
        ImGui::SameLine(0, 10);
        ImGui::TextUnformatted(def.name.c_str());
        const bool on = agentOn(def.id);
        if (installing == def.id) {
            alignRight(160);
            ui::Progress(-1, "Installing", 160);
        } else if (!scan.ready[i]) {
            alignRight(90);
            if (mydear::Button("Install", mydear::ButtonVariant::Secondary, ImVec2(90, 0))) {
                installAgent(def);
            }
        } else {
            if (on) {
                alignRight(narrow ? 90 + 28 : 90 + 80);
                ImGui::TextColored(colors.green, narrow ? ICON_FA_CHECK : ICON_FA_CHECK " Ready");
            }
            alignRight(90);
            if (mydear::Button(on ? "Turn off" : "Turn on", mydear::ButtonVariant::Secondary,
                               ImVec2(90, 0))) {
                if (on) {
                    std::erase(data.agents, def.id);
                } else {
                    data.agents.push_back(def.id);
                }
                save();
            }
        }
        ImGui::PopID();
    }
    endSection();

    // folders and conversations the agents' own clis left here
    std::set<std::string> folders;
    for (const auto& found : scan.history) {
        folders.insert(found.cwd);
    }
    beginSection("Projects", narrow);
    if (folders.empty()) {
        ImGui::TextColored(colors.subtext0, "%s",
                           scanned ? "No earlier agent sessions found on this machine."
                                   : "Looking for earlier sessions");
    } else {
        ImGui::Checkbox((std::format("Import {} projects", folders.size()) +
                         (narrow ? "" : " and their threads"))
                            .c_str(),
                        &importHistory);
        wrapped(colors.subtext0, narrow ? "And their threads, from your Claude Code and Codex history"
                                        : "From your Claude Code and Codex history");
    }
    endSection();

    beginSection("Phone", narrow);
    wrapped(colors.subtext0, ICON_FA_MOBILE_SCREEN "  Use imcode on your phone: not yet");
    endSection();
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0, Theme::Spacing::L));

    const bool import = importHistory && !folders.empty();
    const std::string label =
        import ? std::format("Import {} projects and start", folders.size()) : "Start";
    const float buttonWidth = ImGui::CalcTextSize(label.c_str()).x + 2 * Theme::Spacing::XL;
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - buttonWidth);
    ImGui::BeginDisabled(!scanned || !installing.empty());
    if (mydear::Button(label.c_str(), mydear::ButtonVariant::Primary, ImVec2(buttonWidth, 0))) {
        for (const auto& found : import ? scan.history : std::vector<History::Found>{}) {
            const bool have = std::ranges::any_of(data.threads, [&](const auto& t) {
                return t->sessionId == found.sessionId;
            });
            if (have) {
                continue;
            }
            addProject("", found.cwd);
            auto thread = std::make_unique<Thread>(found.cwd, found.agentId);
            thread->title = found.title;
            thread->sessionId = found.sessionId;
            thread->updatedAt = found.updatedAt;
            data.threads.push_back(std::move(thread));
        }
        data.onboarded = true;
        save();
        project = data.projects.empty() ? -1 : 0;
        if (project >= 0 && !data.agents.empty()) {
            newThread(project, data.agents.front());
        }
    }
    ImGui::EndDisabled();
    ImGui::EndChild();
}
