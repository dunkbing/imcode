#include "app.hpp"

#include "imshell/button.hpp"
#include "imshell/components.hpp"
#include "imshell/themes.hpp"
#include <algorithm>
#include <cstring>
#include <format>
#include <set>
#include <unistd.h>

namespace Theme = imshell::Theme;
namespace ui = imshell::ui;

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

// each agent: its mark and name, then ready / turn on or off / install
void App::agentRows(bool narrow) {
    const auto& colors = Theme::Current()->colors;
    if (!scanned) {
        ui::Progress(-1, "Looking for agents");
    }
    const auto& catalog = AcpAgents::catalog();
    for (size_t i = 0; scanned && i < catalog.size(); ++i) {
        const AcpAgentDef& def = catalog[i];
        ImGui::PushID(static_cast<int>(i));
        agentIcon(def.id, 16, ImGui::GetFrameHeight());
        ImGui::SameLine(0, 10);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(def.name.c_str());
        const bool on = agentOn(def.id);
        if (installing == def.id) {
            // on the row, where the button was: the bar in the middle of the row's height
            alignRight(90 + 90);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(colors.subtext0, "Installing");
            alignRight(90);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetFrameHeight() * 0.5f - 3);
            ui::Progress(-1, nullptr, 90);
        } else if (!scan.ready[i]) {
            alignRight(90);
            if (imshell::Button("Install", imshell::ButtonVariant::Secondary, ImVec2(90, 0))) {
                installAgent(def);
            }
        } else {
            if (on) {
                alignRight(narrow ? 90 + 28 : 90 + 80);
                ImGui::TextColored(colors.green, narrow ? ICON_FA_CHECK : ICON_FA_CHECK " Ready");
            }
            alignRight(90);
            if (imshell::Button(on ? "Turn off" : "Turn on", imshell::ButtonVariant::Secondary,
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
}

// agents and setup, over the app: closed by its x, escape, or a click outside
void App::settingsDraw() {
    const auto& colors = Theme::Current()->colors;
    if (std::exchange(settingsWant, false)) {
        ImGui::OpenPopup("##settings");
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const bool narrow = vp->WorkSize.x < 560;
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(640.0f, vp->WorkSize.x - 32), 0));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, colors.mantle);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.surface1);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 18));
    const bool shown = ImGui::BeginPopupModal(
        "##settings", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    if (!shown) {
        return;
    }
    const float button = ImGui::GetFrameHeight();
    ImGui::PushFont(nullptr, 20.0f);
    ImGui::TextUnformatted("Settings");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - button);
    bool close = imshell::IconButton(ICON_FA_XMARK, imshell::ButtonVariant::Secondary, ImVec2(button, button));
    ImGui::Dummy(ImVec2(0, Theme::Spacing::M));
    // the look: one of imshell's themes
    ImGui::TextColored(colors.subtext0, "Theme");
    ImGui::SameLine(narrow ? 90.0f : 140.0f);
    ImGui::SetNextItemWidth(220);
    if (ImGui::BeginCombo("##theme", Theme::Current()->name)) {
        for (const auto& theme : Theme::THEMES) {
            if (ImGui::Selectable(theme.name, theme.id == data.theme)) {
                applyTheme(theme.id);
                save();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::Dummy(ImVec2(0, Theme::Spacing::M));
    ImGui::TextColored(colors.subtext0, "Agents");
    ImGui::Dummy(ImVec2(0, Theme::Spacing::S));
    agentRows(narrow);
    ImGui::Dummy(ImVec2(0, Theme::Spacing::S));
    close |= ImGui::IsKeyPressed(ImGuiKey_Escape);
    close |= ImGui::IsMouseClicked(0) &&
             !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                                     ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if (close) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
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
    agentRows(narrow);
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
    if (imshell::Button(label.c_str(), imshell::ButtonVariant::Primary, ImVec2(buttonWidth, 0))) {
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
