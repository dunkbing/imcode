#include "markdown.hpp"

#include "IconsFontAwesome6.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "mydear/embedded.hpp"
#include "mydear/themes.hpp"
#include <format>
#include <imgui_rich_md/rich_md.h>
#include <vector>

namespace Theme = mydear::Theme;

namespace {
    // the render() in progress: code blocks are drawn from inside RichMd::Render
    struct Scope {
        const std::string* idPrefix = nullptr;
        int codeBlocks = 0;
        ImFont* font = nullptr; // the caller's, before rich_md pushes its own
        float fontSize = 0.0f;
    };
    Scope gScope;

    // shortcut: plain monospace, no highlighting; a highlighter (tree-sitter, as DearSQL's
    // editor) when code review in the transcript matters
    void renderCode(const std::string& rawCode, const std::string&) {
        const auto& colors = Theme::Current()->colors;
        std::string code = rawCode;
        while (!code.empty() && code.back() == '\n') {
            code.pop_back();
        }
        // rich_md scopes the block's ids by its text, which changes while it streams:
        // key them by position instead
        const std::string key =
            std::format("{}_{}", gScope.idPrefix ? *gScope.idPrefix : "md", gScope.codeBlocks++);
        ImGui::PushOverrideID(ImHashStr(key.c_str()));
        ImGui::PushFont(gScope.font, gScope.fontSize); // ours: monospace, with the icons
        ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.crust);
        ImGui::PushStyleColor(ImGuiCol_Border, colors.surface1);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
        if (ImGui::BeginChild("##md_code", ImVec2(-1, 0),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
                                  ImGuiChildFlags_AlwaysUseWindowPadding,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
            const float right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
            const ImVec2 at = ImGui::GetCursorPos();
            ImGui::TextUnformatted(code.c_str());
            const ImVec2 after = ImGui::GetCursorPos();
            // copy, over the top right corner; a check for a moment after
            static ImGuiID copied = 0;
            static double copiedAt = -10.0;
            const ImGuiID id = ImGui::GetID("copy");
            const bool done = id == copied && ImGui::GetTime() - copiedAt < 1.5;
            ImGui::SetCursorPos(ImVec2(right - ImGui::GetFrameHeight() - 12, at.y));
            ImGui::PushStyleColor(ImGuiCol_Text, done ? colors.green : colors.subtext0);
            if (ImGui::SmallButton(done ? ICON_FA_CHECK "##copy" : ICON_FA_COPY "##copy")) {
                ImGui::SetClipboardText(code.c_str());
                copied = id;
                copiedAt = ImGui::GetTime();
            }
            ImGui::PopStyleColor();
            ImGui::SetCursorPos(after);
            ImGui::Dummy(ImVec2(0, 0)); // imgui wants an item after a cursor move
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
        ImGui::PopFont();
        ImGui::PopID();
    }

    void ensureContext() {
        if (RichMd::GetCurrentContext()) {
            return;
        }
        RichMd::HostServices host = RichMd::GetHostServices();
        host.RenderCodeBlock = renderCode;
        // the app's font: rich_md asks for <base>-{Regular,Bold}[Italic].ttf and a code font
        host.ReadAsset = [fallback = host.ReadAsset](const std::string& path) -> RichMd::AssetBytes {
            if (path.starts_with("fonts/") && path.ends_with(".ttf") &&
                !path.starts_with("fonts/latex/")) {
                const bool bold = path.ends_with("-Bold.ttf") || path.ends_with("-BoldItalic.ttf");
                const bool italic = path.ends_with("Italic.ttf");
                const char* face = bold ? (italic ? "JetBrainsMono-BoldItalic" : "JetBrainsMono-Bold")
                                        : (italic ? "JetBrainsMono-MediumItalic"
                                                  : "JetBrainsMono-Medium");
                if (const EmbeddedFont* font = findEmbeddedFont(face)) {
                    return std::vector<uint8_t>(font->data, font->data + font->size);
                }
            }
            return fallback ? fallback(path) : RichMd::ReadAssetDefault(path);
        };
        RichMd::SetHostServices(host);
        RichMd::MarkdownOptions options;
        options.hardSoftBreaks = true; // chat text: a newline is a line break
        RichMd::CreateContext(options);
    }
} // namespace

void Markdown::render(const std::string& markdown, const std::string& idPrefix) {
    ensureContext();
    RichMd::GetStyle().codeColor = Theme::Current()->colors.peach; // inline code
    const Scope saved = gScope;
    gScope = {.idPrefix = &idPrefix,
              .font = ImGui::GetFont(),
              .fontSize = ImGui::GetCurrentContext()->FontSizeBase}; // unscaled, as PushFont takes it
    ImGui::PushID(idPrefix.c_str());
    RichMd::RenderRaw(markdown);
    ImGui::PopID();
    gScope = saved;
}

void Markdown::shutdown() {
    if (RichMd::GetCurrentContext()) {
        RichMd::DestroyContext();
    }
}
