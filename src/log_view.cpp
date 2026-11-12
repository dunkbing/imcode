// every line sent to and from the open thread's agent (and what it printed on stderr);
// click one to read it laid out
#include "app.hpp"

#include "imshell/button.hpp"
#include "imshell/themes.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <ctime>

namespace Theme = imshell::Theme;

void App::logOpen(Thread& t) {
    log.want = true;
    log.thread = &t;
    log.follow = true;
}

void App::logDraw() {
    const auto& colors = Theme::Current()->colors;
    if (std::exchange(log.want, false)) {
        ImGui::OpenPopup("##log");
    }
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(1000.0f, vp->WorkSize.x - 32), std::min(700.0f, vp->WorkSize.y - 32)));
    ImGui::PushStyleColor(ImGuiCol_PopupBg, colors.mantle);
    ImGui::PushStyleColor(ImGuiCol_Border, colors.surface1);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, Round::Dialog);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    const bool shown = ImGui::BeginPopupModal("##log", nullptr,
                                              ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
    if (!shown) {
        return;
    }
    // the thread could have gone while the dialog was shut: look it up, do not trust the pointer
    const bool alive = std::ranges::any_of(data.threads, [&](const auto& t) { return t.get() == log.thread; });
    size_t count = 0;
    const auto lines = alive ? log.thread->wire(count) : std::vector<AcpClient::WireLine>{};

    const float button = ImGui::GetFrameHeight();
    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(nullptr, 18.0f);
    ImGui::TextUnformatted("Agent log");
    ImGui::PopFont();
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(colors.overlay1, "%zu lines", count);
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - button);
    bool close = imshell::IconButton(ICON_FA_XMARK, imshell::ButtonVariant::Secondary, ImVec2(button, button));
    ImGui::Spacing();

    static int selected = -1;
    static size_t seen = 0;
    const int total = static_cast<int>(lines.size());
    selected = std::min(selected, total - 1);
    const float detail = selected >= 0 ? 220.0f : 0.0f;
    if (total == 0) {
        ImGui::TextColored(colors.overlay1, "Nothing yet: the agent has not started.");
    } else if (ImGui::BeginChild("##lines", ImVec2(0, -detail), ImGuiChildFlags_None, ImGuiWindowFlags_None)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImGuiListClipper clipper;
        clipper.Begin(total, 22);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto& line = lines[static_cast<size_t>(i)];
                ImGui::PushID(i);
                if (rowButton("##line", i == selected, 22)) {
                    selected = i;
                }
                const ImVec2 start = ImGui::GetItemRectMin(), end = ImGui::GetItemRectMax();
                const ImVec4 tone = line.dir == '>' ? colors.blue : line.dir == '<' ? colors.green : colors.peach;
                const time_t secs = static_cast<time_t>(line.ms / 1000);
                std::tm local{};
#ifdef _WIN32
                localtime_s(&local, &secs);
#else
                localtime_r(&secs, &local);
#endif
                char stamp[16];
                std::strftime(stamp, sizeof stamp, "%H:%M:%S", &local);
                char head[32];
                std::snprintf(head, sizeof head, "%s.%03d", stamp, static_cast<int>(line.ms % 1000));
                ImDrawList* draw = ImGui::GetWindowDrawList();
                const float y = start.y + (22 - ImGui::GetTextLineHeight()) / 2;
                draw->AddText(ImVec2(start.x + 4, y), ImGui::GetColorU32(colors.overlay0), head);
                draw->AddText(ImVec2(start.x + 92, y), ImGui::GetColorU32(tone),
                              line.dir == '>' ? ICON_FA_ARROW_RIGHT : line.dir == '<' ? ICON_FA_ARROW_LEFT : ICON_FA_TRIANGLE_EXCLAMATION);
                draw->PushClipRect(ImVec2(start.x + 116, start.y), end, true);
                draw->AddText(ImVec2(start.x + 116, y), ImGui::GetColorU32(colors.text), line.text.c_str());
                draw->PopClipRect();
                ImGui::PopID();
            }
        }
        // stay at the end while lines arrive, until the user scrolls up
        if (count != seen && log.follow) {
            ImGui::SetScrollHereY(1.0f);
        }
        log.follow = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4;
        ImGui::PopStyleVar();
        ImGui::EndChild();
        seen = count;
    }
    if (selected >= 0 && selected < total) {
        const std::string& raw = lines[static_cast<size_t>(selected)].text;
        const auto parsed = nlohmann::json::parse(raw, nullptr, false);
        const std::string pretty = parsed.is_discarded() ? raw : parsed.dump(2);
        ImGui::Separator();
        if (ImGui::SmallButton(ICON_FA_COPY "  Copy")) {
            ImGui::SetClipboardText(raw.c_str());
        }
        if (ImGui::BeginChild("##detail", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar)) {
            ImGui::TextUnformatted(pretty.c_str());
        }
        ImGui::EndChild();
    }
    close |= ImGui::IsKeyPressed(ImGuiKey_Escape);
    close |= ImGui::IsMouseClicked(0) &&
             !ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if (close) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
