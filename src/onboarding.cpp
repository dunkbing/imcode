#include "app.hpp"

#include "imshell/button.hpp"
#include "imshell/components.hpp"
#include "imshell/embedded.hpp"
#include "imshell/image.hpp"
#include "imshell/resizable.hpp"
#include "colors.hpp"
#include "imshell/updater.hpp"
#include "trace.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <set>
#ifndef _WIN32
#include <unistd.h>
#endif

using Theme = imshell::Theme;
namespace ui = imshell::ui;

namespace {
    // a section: its name in the left column, the rows to the right of it
    // (a narrow window stacks them: the name above its rows)
    void beginSection(const char* name, bool narrow) {
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, Theme::Spacing::M));
        ImGui::TextColored(themeColors().subtext0, "%s", name);
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
    const Colors colors = themeColors();
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

namespace {
    // this computer's name, as the network knows it
    std::string computerName() {
        char name[256] = "this machine";
#ifdef _WIN32
        if (const char* env = std::getenv("COMPUTERNAME")) {
            std::snprintf(name, sizeof(name), "%s", env);
        }
#else
        gethostname(name, sizeof(name));
#endif
        if (char* dot = std::strchr(name, '.')) {
            *dot = 0; // "name.local"
        }
        return name;
    }
} // namespace

// true while the dialog is up. it is drawn twice a frame, once inside the settings and once
// outside: a popup opens at the nesting level it was asked for at, and only that call finds it
bool App::machineDialog(bool inSettings) {
    const Colors colors = themeColors();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // adding a machine: a dialog of its own, drawn where it was asked for (over the settings, or over the app)
    static std::vector<SSHHostEntry> configured; // ~/.ssh/config, read when the dialog opens
    if (std::exchange(inSettings ? machineAddingIn : machineAdding, false)) {
        const char* config = std::getenv("IMCODE_SSH_CONFIG");
        configured = parseSSHConfig(config ? config : "");
        ImGui::OpenPopup("##addMachine");
    }
    // a machine is saved only once ssh reached it; an answer for a dialog closed meanwhile is dropped
    machineOp.check([&](MachineTry r) {
        if (r.host != machineTrying) {
            return;
        }
        if (r.ok) {
            if (std::ranges::find(data.machines, r.host) == data.machines.end()) {
                data.machines.push_back(r.host);
            }
            save();
            machineTrying.clear();
            offerImport(r.host);
            machineScanning = hostImport.op.isRunning();
            machineDone = !machineScanning;
        } else {
            machineError = r.error.message;
            machineAskSecret = r.error.needsSecret;
            if (machineAskSecret) {
                machineSecret.clear();
                machineFocus = true;
            }
        }
    });
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(520.0f, vp->WorkSize.x - 32), 0));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, colors.mantle);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Round::Dialog);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 18));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1);
    const bool adding = ImGui::BeginPopupModal("##addMachine", nullptr,
                                               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    if (adding) {
        const bool done = std::exchange(machineDone, false) || (machineScanning && !hostImport.op.isRunning());
        if (done) {
            machineScanning = false;
        }
        const auto known = [&](const std::string& host) {
            return std::ranges::find(data.machines, host) != data.machines.end() ||
                   std::ranges::any_of(data.projects, [&](const Store::Project& p) { return p.host == host; });
        };
        const bool connecting = machineOp.isRunning();
        const bool busy = connecting || machineScanning;
        // connects first; the answer is handled above, next frame
        const auto add = [&](const std::string& typed, const std::string& host) {
            // words are only for a pasted ssh command: anything else with a space is not a target
            const bool spaced = typed.find_first_of(" \t") != std::string::npos;
            if (connecting) {
                return;
            }
            if (host.empty() || (spaced && !typed.starts_with("ssh "))) {
                machineError = "bad ssh target \"" + typed + "\"";
            } else if (known(host) && !machineAskSecret) {
                machineError = host + " is already there";
            } else {
                machineError.clear();
                machineTrying = host;
                machineOp.start([host, secret = machineSecret] {
                    MachineTry out{.host = host};
                    out.ok = Ssh::session(host, secret, out.error) != nullptr;
                    return out;
                });
            }
        };
        ImGui::PushFont(nullptr, 18.0f);
        ImGui::TextUnformatted("Add a machine");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize("Esc").x);
        ImGui::TextColored(colors.overlay1, "Esc");
        ImGui::Dummy(ImVec2(0, Theme::Spacing::S));
        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
        ImGui::TextWrapped("A VPS, a Mac mini, a box at home: ImCode reaches it over ssh, and agents there keep running while your laptop sleeps.");
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, Theme::Spacing::M));
        const float addWidth = 64;
        bool header = false;
        for (const SSHHostEntry& entry : configured) {
            if (known(entry.alias)) {
                continue;
            }
            if (!header) {
                ImGui::TextColored(colors.overlay1, "From your ssh config");
                ImGui::Dummy(ImVec2(0, Theme::Spacing::S));
                header = true;
            }
            ImGui::PushID(entry.alias.c_str());
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(colors.overlay1, ICON_FA_SERVER);
            ImGui::SameLine(0, 12);
            ImGui::TextUnformatted(entry.alias.c_str());
            if (!entry.hostname.empty()) {
                ImGui::SameLine(0, 10);
                ImGui::TextColored(colors.overlay0, "%s", (entry.user.empty() ? entry.hostname : entry.user + "@" + entry.hostname).c_str());
            }
            ImGui::SameLine(ImGui::GetContentRegionMax().x - addWidth);
            ImGui::BeginDisabled(busy);
            if (imshell::Button(connecting && machineTrying == entry.alias ? "..." : "Add", imshell::ButtonVariant::Secondary,
                                ImVec2(addWidth, 0))) {
                std::snprintf(machineInput, sizeof(machineInput), "%s", entry.alias.c_str());
                add(entry.alias, entry.alias);
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        if (header) {
            ImGui::Dummy(ImVec2(0, Theme::Spacing::M));
        }
        ImGui::TextColored(colors.overlay1, header ? "Or type how ssh reaches it" : "Type how ssh reaches it");
        ImGui::Dummy(ImVec2(0, Theme::Spacing::S));
        if (std::exchange(machineFocus, false)) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - addWidth - 8);
        bool enter = ImGui::InputTextWithHint("##typed", "user@host, or ssh user@host", machineInput, sizeof(machineInput),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine(0, 8);
        ImGui::BeginDisabled(busy);
        enter |= imshell::Button("Add##typed", imshell::ButtonVariant::Primary, ImVec2(addWidth, 0));
        ImGui::EndDisabled();
        if (machineAskSecret) {
            ImGui::Dummy(ImVec2(0, Theme::Spacing::S));
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - addWidth - 8);
            if (std::exchange(machineFocus, false)) {
                ImGui::SetKeyboardFocusHere();
            }
            enter |= ui::TextField("Password or key passphrase", machineSecret,
                                   {.password = true, .flags = ImGuiInputTextFlags_EnterReturnsTrue});
        }
        if (enter) {
            add(machineInput, Ssh::destination(machineInput));
        }
        if (busy) {
            ImGui::Dummy(ImVec2(0, Theme::Spacing::S));
            ui::Progress(-1, (connecting ? "Connecting to " + machineTrying : "Looking for earlier sessions on " + hostImport.host).c_str());
        } else if (!machineError.empty()) {
            ImGui::Dummy(ImVec2(0, Theme::Spacing::S));
            ImGui::PushStyleColor(ImGuiCol_Text, machineAskSecret ? colors.yellow : colors.red);
            ImGui::TextWrapped("%s", machineError.c_str());
            ImGui::PopStyleColor();
        }
        if (done || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
            (ImGui::IsMouseClicked(0) && !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                                                                 ImGuiHoveredFlags_AllowWhenBlockedByActiveItem))) {
            if (!done) {
                machineTrying.clear(); // abandoned: a late answer is dropped
                machineScanning = false;
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return adding;
}

// the settings, over the app, one page at a time: closed by its x, escape, or a click outside
void App::settingsDraw() {
    const Colors colors = themeColors();
    if (std::exchange(settingsWant, false)) {
        ImGui::OpenPopup("##settings");
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const bool narrow = vp->WorkSize.x < 640;
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(760.0f, vp->WorkSize.x - 32), std::min(440.0f, vp->WorkSize.y - 32)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, colors.mantle);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Round::Dialog);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 18));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1);
    const bool shown = ImGui::BeginPopupModal(
        "##settings", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(3);
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

    // the pages: a list at the left, a row of them when the window is narrow
    struct Page {
        const char* icon;
        const char* name;
    };
    static constexpr Page pages[] = {{ICON_FA_PALETTE, "Appearance"}, {ICON_FA_ROBOT, "Agents"}, {ICON_FA_SERVER, "Machines"},
                                     {ICON_FA_BELL, "Alerts"},        {ICON_FA_CIRCLE_INFO, "About"}};
    if (narrow) {
        for (int i = 0; i < 5; ++i) {
            if (i > 0) {
                ImGui::SameLine();
            }
            if (ImGui::Selectable((std::string(pages[i].icon) + "##page" + std::to_string(i)).c_str(), settingsTab == i, 0, ImVec2(34, 0))) {
                settingsTab = i;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("%s", pages[i].name);
            }
        }
    }
    // the list at the left and the page beside it, with a divider between that does not move
    const auto navRows = [&] {
        for (int i = 0; i < 5; ++i) {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable(("##page" + std::to_string(i)).c_str(), settingsTab == i, 0, ImVec2(0, 30))) {
                settingsTab = i;
            }
            // the icon and the name each at their own column: the glyphs differ in width
            const float textY = at.y + (30 - ImGui::GetTextLineHeight()) / 2;
            const ImU32 ink = ImGui::GetColorU32(settingsTab == i ? colors.text : colors.subtext0);
            ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + 10, textY), ink, pages[i].icon);
            ImGui::GetWindowDrawList()->AddText(ImVec2(at.x + 38, textY), ink, pages[i].name);
        }
    };
    const auto pageBody = [&] {
    const auto heading = [&](const char* title, const char* about) {
        ImGui::TextColored(colors.overlay1, "%s", title);
        if (about) {
            ImGui::PushStyleColor(ImGuiCol_Text, colors.overlay0);
            ImGui::TextWrapped("%s", about);
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, Theme::Spacing::M));
    };
    switch (settingsTab) {
    case 0: { // the look: one of imshell's themes
        heading("APPEARANCE", nullptr);
        ImGui::AlignTextToFramePadding(); // level with the list beside it
        ImGui::TextColored(colors.subtext0, "Theme");
        ImGui::SameLine(110);
        ImGui::SetNextItemWidth(220);
        if (ImGui::BeginCombo("##theme", imshell::CurrentTheme().name)) {
            for (const auto& theme : imshell::Themes()) {
                if (ImGui::Selectable(theme.name, theme.id == data.theme)) {
                    applyTheme(theme.id);
                    save();
                }
            }
            ImGui::EndCombo();
        }
        break;
    }
    case 1:
        heading("AGENTS", "Turn on the ones you use, and install what is missing.");
        agentRows(true);
        break;
    case 2: { // the computers reached over ssh, and this one
        heading("MACHINES", "Your computers, reached over ssh. Their agents and terminals keep running when you disconnect.");
        // a row: the name and what is known of it, a dot (green: connected) and, at the right, a trash button
        const auto machine = [&](const std::string& name, const std::string& detail, bool live, bool removable) {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float right = at.x + ImGui::GetContentRegionAvail().x;
            const float line = ImGui::GetTextLineHeight();
            const float rowH = line * 2 + 14;
            ImGui::TextUnformatted(name.c_str());
            ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + line + 3));
            ImGui::TextColored(colors.overlay1, "%s", detail.c_str());
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(right - 40, at.y + rowH / 2 - 4), 4, // the same column on every row
                                                        ImGui::GetColorU32(live ? colors.green : colors.surface2));
            bool forget = false;
            if (removable) {
                const float small = ImGui::GetFrameHeightWithSpacing() - ImGui::GetStyle().ItemSpacing.y - 4;
                ImGui::SetCursorScreenPos(ImVec2(right - 24, at.y + rowH / 2 - 4 - small / 2));
                ImGui::PushID(name.c_str());
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                ImGui::PushStyleColor(ImGuiCol_Text, colors.overlay1);
                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
                forget = ImGui::SmallButton(ICON_FA_TRASH);
                ImGui::PopStyleColor(2);
                ImGui::PopStyleVar();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Forget this machine");
                }
                ImGui::PopID();
            }
            ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + rowH));
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 2));
            return forget;
        };
        machine(computerName(), "ImCode " IMCODE_VERSION, true, false);
        const std::vector<std::string> hosts = machineHosts();
        if (hosts.empty()) {
            ImGui::TextColored(colors.overlay1, "No other machines yet.");
        }
        for (const auto& host : hosts) {
            const auto there = std::ranges::count(data.projects, host, &Store::Project::host);
            const bool live = Ssh::connected(host);
            std::string detail = live ? "Connected" : "Not connected";
            if (there > 0) {
                detail += "  ·  " + std::to_string(there) + (there == 1 ? " project" : " projects");
            }
            if (machine(host, detail, live, there == 0)) {
                std::erase(data.machines, host);
                save();
                break; // the list changed under the loop
            }
        }
        ImGui::Dummy(ImVec2(0, Theme::Spacing::M));
        if (imshell::Button("Add a machine", imshell::ButtonVariant::Secondary)) {
            machineAddingIn = true; // over the settings
            machineFocus = true;
            machineError.clear();
            machineAskSecret = false;
            machineSecret.clear();
            machineTrying.clear();
            machineInput[0] = 0;
        }
        break;
    }
    case 3: { // what the system tells the user
        heading("ALERTS", "Notify when");
        bool changed = ImGui::Checkbox("A thread needs your answer", &data.notifyAnswer);
        changed |= ImGui::Checkbox("A thread finishes", &data.notifyFinished);
        changed |= ImGui::Checkbox("Also for threads in the background while the window is open", &data.notifyOthers);
        if (changed) {
            save();
        }
        break;
    }
    default: // which build this is: what to quote when something is off
        heading("ABOUT", nullptr);
        {
            // 64 points of a 64 pixel picture: whole pixels at 1x and 2x, so the edges stay sharp
            auto logo = icons.find("imcode");
            if (logo == icons.end()) {
                const EmbeddedImage* image = findEmbeddedImage("imcode");
                logo = icons.emplace("imcode", image && upload ? imshell::LoadTexture(image->data, image->size, upload) : ImTextureID{}).first;
            }
            if (logo->second != ImTextureID{}) {
                const ImVec2 at = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddImage(logo->second, at, ImVec2(at.x + 64, at.y + 64));
                ImGui::Dummy(ImVec2(64, 64));
                ImGui::Dummy(ImVec2(0, Theme::Spacing::S));
            }
        }
        {
            // click to copy what to quote in a report: the version and the commit
            const ImVec2 from = ImGui::GetCursorScreenPos();
            ImGui::TextColored(colors.subtext0, "ImCode " IMCODE_VERSION "  " IMCODE_COMMIT);
            ImGui::SameLine(0, Theme::Spacing::S);
            ImGui::TextColored(colors.overlay0, ICON_FA_COPY);
            if (ImGui::IsMouseHoveringRect(from, ImGui::GetItemRectMax())) { // the line and its icon are one target
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                ImGui::SetTooltip("Copy the version and commit");
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    ImGui::SetClipboardText("ImCode " IMCODE_VERSION " " IMCODE_COMMIT);
                    ui::Toast("Copied", ui::Tone::Success, 2);
                }
            }
        }
        ImGui::Dummy(ImVec2(0, Theme::Spacing::S));
        if (imshell::Button("Copy Trace Log", imshell::ButtonVariant::Secondary)) {
            // the end of this run's log: timings and names only, never message text
            const std::string log = trace::tail(1 << 20);
            if (log.empty()) {
                ui::Toast("The trace log is off or empty", ui::Tone::Warning, 3);
            } else {
                ImGui::SetClipboardText(log.c_str());
                ui::Toast("Trace copied", ui::Tone::Success, 2);
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Copies the end of this run's trace log (at most the last 1 MB; timings and names, no message text)");
        }
#ifdef IMCODE_UPDATER
        ImGui::Dummy(ImVec2(0, Theme::Spacing::S));
        const bool available = imshell::updater::Available();
        if (imshell::Button(available ? ("Update to " + imshell::updater::LatestVersion()).c_str() : "Check for Updates",
                            available ? imshell::ButtonVariant::Primary : imshell::ButtonVariant::Secondary)) {
            imshell::updater::Check();
        }
#endif
        break;
    }
    };
    static float navWidth = 160;
    if (narrow) {
        ImGui::BeginChild("##page", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
        pageBody();
        ImGui::EndChild();
    } else if (imshell::BeginResizable("##settingsSplit", {.disabled = true,
                                                            .line = ImGui::GetColorU32(colors.surface1),
                                                            .lit = ImGui::GetColorU32(colors.surface1)})) {
        if (imshell::ResizablePanel({.size = &navWidth, .fixed = true})) {
            navRows();
        }
        if (imshell::ResizablePanel({.childFlags = ImGuiChildFlags_AlwaysUseWindowPadding})) {
            pageBody();
        }
        imshell::EndResizable();
    }
    const bool nested = machineDialog(true); // the dialog over it takes escape and a click outside
    if (!nested) { // the dialog over it takes these
        close |= ImGui::IsKeyPressed(ImGuiKey_Escape);
        close |= ImGui::IsMouseClicked(0) &&
                 !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows |
                                         ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    }
    if (close) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::onboarding() {
    const Colors colors = themeColors();
    const bool narrow = ImGui::GetContentRegionAvail().x < 560;
    const float width = std::min(720.0f, ImGui::GetContentRegionAvail().x - 2 * Theme::Spacing::XL);
    ImGui::SetCursorPos(ImVec2((ImGui::GetWindowWidth() - width) * 0.5f, narrow ? 24 : 72));
    ImGui::BeginChild("onboarding", ImVec2(width, 0));

    const std::string hostName = computerName();
    ImGui::PushFont(nullptr, 24.0f);
    ImGui::TextUnformatted("Welcome to ImCode");
    ImGui::PopFont();
    ImGui::TextColored(colors.subtext0, "Setting up %s.", hostName.c_str());
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

// the first time a machine is reached: its agents' earlier sessions are looked for there
void App::offerImport(const std::string& host) {
    if (std::ranges::find(data.importAsked, host) != data.importAsked.end()) {
        return;
    }
    data.importAsked.push_back(host);
    save();
    if (projectOn(host) >= 0 || hostImport.op.isRunning()) {
        return; // it has projects already
    }
    hostImport.host = host;
    hostImport.found.clear();
    hostImport.op.start([host] { return History::scanRemote(host); });
}

// found something: a dialog offers them as projects and threads. nothing found: nothing shows
void App::importDialog() {
    HostImport& in = hostImport;
    in.op.check([&](std::vector<History::Found> found) {
        std::erase_if(found, [&](const History::Found& f) {
            return std::ranges::any_of(data.threads, [&](const auto& t) { return t->sessionId == f.sessionId; });
        });
        in.found = std::move(found);
        in.wantOpen = !in.found.empty();
    });
    if (std::exchange(in.wantOpen, false)) {
        ImGui::OpenPopup("##importThreads");
    }
    const Colors colors = themeColors();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(520.0f, vp->WorkSize.x - 32), 0));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, colors.mantle);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.overlay0);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Round::Dialog);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 18));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1);
    const bool shown = ImGui::BeginPopupModal("##importThreads", nullptr,
                                              ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    if (!shown) {
        return;
    }
    std::set<std::string> folders;
    for (const auto& f : in.found) {
        folders.insert(f.cwd);
    }
    ImGui::PushFont(nullptr, 18.0f);
    ImGui::TextUnformatted(("Import from " + in.host).c_str());
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, Theme::Spacing::S));
    ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
    ImGui::TextWrapped("Your Claude Code and Codex history on this machine has %zu project%s with %zu thread%s. "
                       "Import them to pick up where you left off.",
                       folders.size(), folders.size() == 1 ? "" : "s", in.found.size(), in.found.size() == 1 ? "" : "s");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, Theme::Spacing::L));
    const float width = 96;
    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - 2 * width - ImGui::GetStyle().ItemSpacing.x);
    bool close = ImGui::IsKeyPressed(ImGuiKey_Escape);
    if (imshell::Button("Skip", imshell::ButtonVariant::Secondary, ImVec2(width, 0))) {
        close = true;
    }
    ImGui::SameLine();
    if (imshell::Button("Import", imshell::ButtonVariant::Primary, ImVec2(width, 0))) {
        for (const auto& found : in.found) {
            addProject(in.host, found.cwd);
            auto thread = std::make_unique<Thread>(found.cwd, found.agentId, in.host);
            thread->title = found.title;
            thread->sessionId = found.sessionId;
            thread->updatedAt = found.updatedAt;
            data.threads.push_back(std::move(thread));
        }
        save();
        selectMachine(in.host); // the list shows that machine's threads
        close = true;
    }
    if (close) {
        in.found.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
