#include "app.hpp"

#include "code_colors.hpp"
#include "imshell/markdown.hpp"
#include "notify.hpp"
#include "imgui_internal.h"
#include "imshell/alert.hpp"
#include "imshell/button.hpp"
#include "imshell/components.hpp"
#include "imshell/embedded.hpp"
#include "imshell/fonts.hpp"
#include "imshell/image.hpp"
#include "imshell/splitter.hpp"
#include "imshell/frames.hpp"
#include "imshell/themes.hpp"
#include "imshell/title_bar.hpp"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <format>
#include <iomanip>
#include <sstream>

namespace Theme = imshell::Theme;
namespace ui = imshell::ui;

constexpr float barHeight = 40; // taller than the os default: air above and below its row

int runHeadless(App& app, const char* out, int width, int height, int frames); // headless.cpp

bool rowButton(const char* id, bool chosen, float height, ImGuiSelectableFlags flags) {
    const auto& colors = Theme::Current()->colors;
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    // the selectable only takes the click: its own square highlight is not drawn
    for (const ImGuiCol col : {ImGuiCol_Header, ImGuiCol_HeaderHovered, ImGuiCol_HeaderActive}) {
        ImGui::PushStyleColor(col, ImVec4(0, 0, 0, 0));
    }
    const bool clicked = ImGui::Selectable(id, false, flags, ImVec2(0, height));
    ImGui::PopStyleColor(3);
    if (chosen || ImGui::IsItemHovered()) {
        ImVec4 fill = colors.surface0;
        fill.w = chosen ? 1.0f : 0.5f;
        ImGui::GetWindowDrawList()->AddRectFilled(min, ImVec2(min.x + width, min.y + height),
                                                  ImGui::GetColorU32(fill), 8);
    }
    return clicked;
}

void projectBadge(const std::string& name, float size) {
    const auto& colors = Theme::Current()->colors;
    const ImVec4 tints[] = {colors.blue, colors.peach, colors.green, colors.purple, colors.teal, colors.rose};
    size_t hash = 0;
    for (const char c : name) {
        hash = hash * 31 + static_cast<unsigned char>(c);
    }
    const ImVec4 tint = tints[hash % std::size(tints)];
    // the first two letters or digits, upper case
    std::string letters;
    for (const char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) && letters.size() < 2) {
            letters += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
    }
    const float line = ImGui::GetTextLineHeight();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size + 4, line));
    const ImVec2 min(at.x, std::floor(at.y + (line - size) * 0.5f));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, ImVec2(min.x + size + 4, min.y + size),
                        ImGui::GetColorU32(ImVec4(tint.x, tint.y, tint.z, 0.22f)), 4);
    ImGui::PushFont(nullptr, size * 0.62f);
    const ImVec2 text = ImGui::CalcTextSize(letters.c_str());
    draw->AddText(ImVec2(min.x + (size + 4 - text.x) * 0.5f, min.y + (size - text.y) * 0.5f),
                  ImGui::GetColorU32(tint), letters.c_str());
    ImGui::PopFont();
}

std::string ago(long long unixSeconds) {
    const long long s = std::max(0LL, static_cast<long long>(std::time(nullptr)) - unixSeconds);
    return unixSeconds == 0 ? ""
           : s < 60         ? "now"
           : s < 3600       ? std::format("{}m", s / 60)
           : s < 86400      ? std::format("{}h", s / 3600)
                            : std::format("{}d", s / 86400);
}

void App::setup() {
    static const ImWchar iconRanges[] = {ICON_MIN_FA, ICON_MAX_16_FA, 0};
    const EmbeddedFont* text = findEmbeddedFont("JetBrainsMono-Medium");
    const EmbeddedFont* icons = findEmbeddedFont("fa-solid-900");
    if (text && icons) {
        imshell::AddFont({{text->data, text->size}, {icons->data, icons->size, iconRanges, true}},
                        15.0f);
    }
    // replies: our fonts, our colours for code, icon glyphs on the copy button
    auto face = [](const char* name) {
        const EmbeddedFont* font = findEmbeddedFont(name);
        return font ? imshell::Markdown::Font{font->data, font->size} : imshell::Markdown::Font{};
    };
    imshell::Markdown::Configure({.regular = face("JetBrainsMono-Medium"),
                                  .bold = face("JetBrainsMono-Bold"),
                                  .italic = face("JetBrainsMono-MediumItalic"),
                                  .boldItalic = face("JetBrainsMono-BoldItalic"),
                                  .code = face("JetBrainsMono-Medium"),
                                  .highlighter = [](const std::string& language) {
                                      return codeColors(fenceLang(language));
                                  },
                                  .copyIcon = ICON_FA_COPY,
                                  .copiedIcon = ICON_FA_CHECK});
    AcpHost::setWake([this] {
        imshell::requestFrames();
        hostWake.notify_one();
    });
    // IMCODE_DATA_DIR: another folder for the saved data (a second profile, ui checks)
    const char* dataDir = std::getenv("IMCODE_DATA_DIR");
    const std::filesystem::path dir = dataDir ? dataDir : imshell::DataDir("imcode");
    AcpRegistry::setInstallRoot(dir / "agents");
    dataFile = dir / "threads.json";
    data = Store::load(dataFile);
    applyTheme(data.theme);
    project = data.projects.empty() ? -1 : 0;
    startScan();
    // back where the user was: the thread that was open comes up again, with its history
    for (int i = 0; data.onboarded && !data.lastSession.empty() && i < static_cast<int>(data.threads.size()); ++i) {
        if (data.threads[i]->sessionId == data.lastSession && !data.threads[i]->archived) {
            select(i);
            break;
        }
    }
    // ui checks: IMCODE_DEV_SEND=<text> starts a thread in the first project before any frame
    if (const char* text = std::getenv("IMCODE_DEV_SEND"); text && !data.projects.empty()) {
        const Store::Project& p = data.projects.front();
        data.threads.push_back(std::make_unique<Thread>(p.cwd, agentsOn().front()->id, p.host));
        data.threads.back()->send(text);
    }
    host = std::thread([this] { hostLoop(); });
}

void App::agentIcon(const std::string& agentId, float size, float rowHeight) {
    auto it = icons.find(agentId);
    if (it == icons.end()) {
        const EmbeddedImage* image = findEmbeddedImage(("agent-" + agentId).c_str());
        const ImTextureID texture =
            image && upload ? imshell::LoadTexture(image->data, image->size, upload) : ImTextureID{};
        it = icons.emplace(agentId, texture).first;
    }
    // an item as tall as its row, the mark drawn in its middle: no cursor nudging, which
    // drifts by a pixel or two against buttons
    const float height = rowHeight > 0 ? rowHeight : ImGui::GetTextLineHeight();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(size, height));
    const ImVec2 min(at.x, std::floor(at.y + (height - size) * 0.5f));
    // the agent's own colour where its mark has one; the others are drawn like text
    static const std::map<std::string, ImVec4> brand = {
        {"claude-code", ImVec4(0.85f, 0.47f, 0.34f, 1)}, // #d97757
        {"gemini", ImVec4(0.28f, 0.59f, 0.89f, 1)},      // #4796e3
        {"antigravity-acp", ImVec4(0.26f, 0.52f, 0.96f, 1)},
    };
    ImVec4 tint = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    if (const auto colour = brand.find(agentId); colour != brand.end()) {
        tint = colour->second;
    }
    if (it->second == ImTextureID{}) {
        ImGui::GetWindowDrawList()->AddText(min, ImGui::GetColorU32(tint), ICON_FA_ROBOT);
        return;
    }
    ImGui::GetWindowDrawList()->AddImage(it->second, min, ImVec2(min.x + size, min.y + size), ImVec2(0, 0),
                                         ImVec2(1, 1), ImGui::GetColorU32(tint));
}

void App::projectPopup() {
    const auto& colors = Theme::Current()->colors;
    ImGui::SetNextWindowSize(ImVec2(380, 0));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, colors.mantle);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 10);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    const bool shown = ImGui::BeginPopup("projects");
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    if (!shown) {
        return;
    }
    const float line = ImGui::GetTextLineHeight();
    auto row = [&](const std::string& id, const char* icon, const std::string& title,
                   const std::string& detail, bool chosen) {
        const ImVec2 at = ImGui::GetCursorPos();
        const float height = detail.empty() ? line + 12 : line * 2 + 14;
        const bool clicked = rowButton(("##" + id).c_str(), chosen, height);
        const ImVec2 next = ImGui::GetCursorPos();
        ImGui::SetCursorPos(ImVec2(at.x + 10, at.y + 6));
        ImGui::TextColored(colors.subtext0, "%s", icon);
        ImGui::SameLine(at.x + 38);
        ImGui::TextUnformatted(title.c_str());
        if (!detail.empty()) {
            ImGui::SetCursorPos(ImVec2(at.x + 38, at.y + 6 + line + 2));
            ImGui::TextColored(colors.overlay1, "%s", detail.c_str());
        }
        ImGui::SetCursorPos(ImVec2(next.x, next.y - ImGui::GetStyle().ItemSpacing.y + 2));
        ImGui::Dummy(ImVec2(0, 0)); // imgui wants an item after a cursor move
        return clicked;
    };
    for (int i = 0; i < static_cast<int>(data.projects.size()); ++i) {
        const Store::Project& p = data.projects[i];
        if (row(std::to_string(i), p.host.empty() ? ICON_FA_FOLDER : ICON_FA_SERVER,
                std::filesystem::path(p.cwd).filename().string(),
                p.host.empty() ? p.cwd : p.host + ":" + p.cwd, i == project)) {
            switchProjectTo = i;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::Separator();
    if (row("add", ICON_FA_FOLDER_PLUS, "Add project...", "", false)) {
        paletteOpen();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void App::applyTheme(const std::string& id) {
    Theme::Current() = &Theme::themeById(id);
    Theme::ApplyTheme(Theme::Current()->colors);
    ImGui::GetStyle().ScrollbarSize = 8; // thin: the lists are narrow and many
    data.theme = Theme::Current()->id;
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

void App::openFile(const std::string& path) {
    attachments.push_back(path); // ui thread only, like the composer that reads it
    focusInput = true;
}

bool App::wantsFrames() {
    // called by the loop, outside a frame: the host may be changing the threads
    std::unique_lock lock(hostMutex, std::try_to_lock);
    if (!lock) {
        return true;
    }
    return scanOp.isRunning() || palette.op.isRunning() || registry.isBusy() ||
           gitOp.isRunning() || picker.op.isRunning() || files.listOp.isRunning() || files.indexOp.isRunning() || files.readOp.isRunning() ||
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
        project = addProject(t.host, t.projectDir());
        t.open();
        if (!t.sessionId.empty() && t.sessionId != data.lastSession) {
            data.lastSession = t.sessionId;
            save();
        }
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
        // sessions an agent reported for its folder, and the thread that asked
        std::vector<acp::SessionInfo> fresh;
        const Thread* from = nullptr;
        const bool watched = nowMs() - lastFrameMs < 1500;
        std::map<const Thread*, Seen> now;
        for (auto& t : data.threads) {
            t->poll(); // every thread, shown or not
            changed |= t->takeChanged();
            fresh.insert(fresh.end(), std::make_move_iterator(t->found.begin()),
                         std::make_move_iterator(t->found.end()));
            if (!t->found.empty()) {
                from = t.get();
                t->found.clear();
            }
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
        // each one not listed yet becomes a thread of the same project and agent: what was
        // started in a terminal can be continued here
        for (const auto& session : from ? fresh : std::vector<acp::SessionInfo>{}) {
            const bool have = std::ranges::any_of(data.threads, [&](const auto& t) {
                return t->sessionId == session.sessionId;
            });
            if (have || session.sessionId.empty() || session.title.empty()) {
                continue;
            }
            auto thread = std::make_unique<Thread>(from->cwd, from->agentId, from->host);
            thread->root = from->root;
            thread->title = session.title.substr(0, 80);
            thread->sessionId = session.sessionId;
            // "2026-11-11T02:07:30.123Z": to the second is enough to sort by
            std::tm when{};
            std::istringstream stamp(session.updatedAt);
            if (stamp >> std::get_time(&when, "%Y-%m-%dT%H:%M:%S")) {
                thread->updatedAt = timegm(&when);
            }
            data.threads.push_back(std::move(thread));
            changed = true;
        }
        // a new thread gets its session id a moment after it is opened
        if (changed && current >= 0 && !data.threads[current]->sessionId.empty()) {
            data.lastSession = data.threads[current]->sessionId;
        }
        if (changed) {
            save();
            imshell::requestFrames();
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
    const imshell::Rect content =
        headless ? imshell::Rect{ImVec2(0, barHeight), ImVec2(display.x, display.y - barHeight)}
                 : imshell::ContentRect();
    const bool narrow = content.size.x < 720; // one pane at a time
    imshell::TitleBar bar{ImVec2(0, 0), ImVec2(display.x, barHeight), 78, 0};
    if (headless) {
        ImGui::SetNextWindowPos(bar.pos);
        ImGui::SetNextWindowSize(bar.size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, (barHeight - ImGui::GetFrameHeight()) * 0.5f));
        ImGui::Begin("##bar", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleVar();
        ImGui::SetCursorPosX(bar.leftInset);
    } else {
        bar = imshell::BeginTitleBar();
    }
    if (data.onboarded && narrow) {
        if (imshell::IconButton(ICON_FA_BARS, imshell::ButtonVariant::Secondary, ImVec2(30, 22))) {
            sidebarOpen = !sidebarOpen;
        }
        ImGui::SameLine();
    }
    ImGui::AlignTextToFramePadding();
    if (data.onboarded && current >= 0) {
        const Thread& t = *data.threads[current];
        // the project: click to start a thread in another one
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.surface0);
        ImGui::PushStyleColor(ImGuiCol_Text, colors.subtext0);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
        if (ImGui::Button((std::filesystem::path(t.projectDir()).filename().string() + "  " ICON_FA_CHEVRON_DOWN "##project").c_str())) {
            ImGui::OpenPopup("projects");
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        projectPopup();
        ImGui::SameLine();
        ImGui::TextColored(colors.overlay0, "/");
        ImGui::SameLine();
        // as much of the title as fits before the buttons at the right
        const float room = bar.pos.x + bar.size.x - bar.rightInset - 76 - 12 - ImGui::GetCursorScreenPos().x;
        const size_t fits = static_cast<size_t>(std::max(0.0f, room / ImGui::CalcTextSize("0").x));
        const std::string title = t.title.empty() ? "New thread" : t.title;
        ImGui::TextUnformatted(title.size() > fits ? (title.substr(0, fits > 2 ? fits - 2 : 0) + "..").c_str()
                                                   : title.c_str());
        // at the right end, before the os's own buttons
        ImGui::SameLine();
        ImGui::SetCursorScreenPos(ImVec2(bar.pos.x + bar.size.x - bar.rightInset - 76,
                                         ImGui::GetCursorScreenPos().y));
        // flat, with room around the icon; filled while its panel is open
        auto barButton = [&](const char* icon, bool on) {
            ImGui::PushStyleColor(ImGuiCol_Button, on ? colors.surface1 : ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors.surface0);
            ImGui::PushStyleColor(ImGuiCol_Text, on ? colors.text : colors.subtext0);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6);
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0);
            const bool pressed = ImGui::Button(icon, ImVec2(30, 22));
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(3);
            return pressed;
        };
        if (barButton(ICON_FA_TERMINAL, terminalOpen)) {
            toggleTerminal();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Terminal (Ctrl+`)");
        }
        ImGui::SameLine(0, 6);
        if (barButton(ICON_FA_FOLDER_TREE, files.open)) {
            files.open = !files.open;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Files (Ctrl+B)");
        }
    } else {
        ImGui::TextColored(colors.subtext0, "imcode");
    }
    if (headless) {
        ImGui::End();
    } else {
        imshell::EndTitleBar();
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
                    imshell::Splitter("##terminal", &terminalSplit, area.y, 160, 80);
                    terminalPanel(t);
                if (std::exchange(terminalExited, false)) {
                    terminals.erase(&t); // opening the panel again starts a new shell
                    terminalOpen = false;
                    focusInput = true;
                }
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
    if (const char* dev = std::getenv("IMCODE_DEV"); dev) {
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
            data.threads[current]->send(all.substr(at + 6, all.find(';', at + 1) - at - 6),
                                        std::exchange(attachments, {}));
        }
        devOpenModels = frames == 40 && (all.find(";models;") != std::string::npos);
        if (const auto at = all.find(";remote:"); frames == 40 && at != std::string::npos) {
            paletteOpen();
            palette.step = Palette::Step::Host;
            palette.host = all.substr(at + 8, all.find(';', at + 1) - at - 8);
            paletteGo(palette.host, "");
        }
        if (frames == 40 && all.find(";installing;") != std::string::npos) {
            installing = "cursor"; // the row's progress, without a download
        }
        if (frames == 39 && all.find(";worktree;") != std::string::npos && current >= 0) {
            data.threads[current]->wantWorktree = true;
        }
        if (const auto at = all.find(";attach:"); frames == 39 && at != std::string::npos) {
            attachments.push_back(all.substr(at + 8, all.find(';', at + 1) - at - 8));
        }
        if (frames == 40 && all.find(";settings;") != std::string::npos) {
            settingsWant = true;
        }
        if (frames == 40 && all.find(";hosts;") != std::string::npos) {
            paletteOpen();
            palette.step = Palette::Step::Host;
        }
        // one of each kind of transcript item
        if (frames == 40 && all.find(";demo;") != std::string::npos && current >= 0) {
            Thread& t = *data.threads[current];
            t.title = "Rename the config loader";
            t.items.push_back({.kind = Item::Kind::User, .text = "Rename the config loader and update its callers."});
            t.items.push_back({.kind = Item::Kind::Thought, .text = "The loader is used in three files; the tests name it too."});
            Item plan{.kind = Item::Kind::Plan};
            plan.plan = {{"Find the callers", "high", "completed", {}},
                         {"Rename the function", "medium", "in_progress", {}},
                         {"Run the tests", "medium", "pending", {}}};
            t.items.push_back(plan);
            t.items.push_back({.kind = Item::Kind::Tool, .toolId = "1", .toolTitle = "grep -rn loadConfig src",
                               .toolStatus = "completed", .toolOutput = "src/a.cpp:12\nsrc/b.cpp:40"});
            Item edit{.kind = Item::Kind::Tool, .toolId = "2", .toolTitle = "Edit src/config.cpp",
                      .toolStatus = "completed"};
            const std::string before = "#include \"config.hpp\"\n\n// reads the file\nConfig loadConfig(const std::string& path) {\n"
                                       "    Config out;\n    out.path = path;\n    return out;\n}\n";
            const std::string after = "#include \"config.hpp\"\n\n// reads the file, or the defaults when it is missing\n"
                                      "Config readConfig(const std::string& path) {\n    Config out;\n    out.path = path;\n"
                                      "    out.retries = 3;\n    return out;\n}\n";
            edit.changes.push_back({"src/store.cpp", Diff::lines(before, after), 3, 2});
            edit.toolPath = "src/store.cpp";
            t.items.push_back(edit);
            Item ask{.kind = Item::Kind::Permission};
            ask.permission.title = "Run `ctest --test-dir build`";
            ask.permission.options = {{"a", "Allow once", "allow_once", {}},
                                      {"b", "Always allow", "allow_always", {}},
                                      {"c", "Reject", "reject_once", {}}};
            t.items.push_back(ask);
            t.items.push_back({.kind = Item::Kind::Assistant, .text = "Renamed `loadConfig` to `readConfig` in **3 files**.\n\n```cpp\n"
                                        "// was loadConfig\nConfig readConfig(const std::string& path);\n"
                                        "const int retries = 3;\n```"});
            Item auth{.kind = Item::Kind::Auth};
            auth.authMethods.resize(2);
            auth.authMethods[0].name = "Log in with your account";
            auth.authMethods[0].type = "terminal";
            auth.authMethods[0].args = {"--login"};
            auth.authMethods[1].name = "Use an API key";
            t.items.push_back(auth);
            t.items.push_back({.kind = Item::Kind::Info, .text = "Turn stopped: max_tokens"});
            t.items.push_back({.kind = Item::Kind::Error, .text = "The agent exited: connection reset"});
        }
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
        imshell::requestFrames();
    }
    // a sign-in the open thread wants run: in its terminal, opened for it
    if (current >= 0 && !data.threads[current]->terminalCommand.empty()) {
        terminalRun = std::exchange(data.threads[current]->terminalCommand, {});
        terminalOpen = true;
        focusTerminal = true;
    }
    paletteDraw();
    settingsDraw();
    if (switchProjectTo >= 0) {
        newThread(std::exchange(switchProjectTo, -1));
    }
    if (!switchAgentTo.empty()) {
        newThread(project, std::exchange(switchAgentTo, {}));
    }
    ImGui::End();
    ui::RenderToasts();
    imshell::Alert::render(); // last: over everything
}

int main() {
    imshell::Config config;
    config.title = "imcode";
    config.appName = "imcode";
    config.titleBar = true;
    config.titleBarHeight = barHeight;
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
    // not imshell::Run: markdown and icon textures are freed between run() and destroy()
    static imshell::App shell;
    app.shell = &shell;
    app.upload = [](const uint8_t* rgba, int w, int h) { return shell.createTexture(rgba, w, h); };
    if (!shell.create(config, app)) {
        return 1;
    }
    shell.run();
    app.icons.clear();
    imshell::Markdown::Shutdown();
    // stop the shells and the agents while the loop they wake still exists, not during static teardown
    app.stopHost();
    app.terminals.clear();
    app.data.threads.clear();
    AcpHost::setWake(nullptr);
    shell.destroy();
    return 0;
}
