#include "markdown.hpp"

#include "IconsFontAwesome6.h"
#include "code_colors.hpp"
#include "imgui.h"
#include "imgui_internal.h"
#include "imshell/embedded.hpp"
#include "imshell/themes.hpp"
#include <algorithm>
#include <format>
#include <memory>
#include <unordered_map>
#include <imgui_rich_md/rich_md.h>
#include <vector>

namespace Theme = imshell::Theme;

namespace {
    // the render() in progress: code blocks are drawn from inside RichMd::Render
    struct Scope {
        const std::string* idPrefix = nullptr;
        int codeBlocks = 0;
        ImFont* font = nullptr; // the caller's, before rich_md pushes its own
        float fontSize = 0.0f;
    };
    Scope gScope;

    // one read-only editor per code block, for colours and selection; dropped once its
    // block has not been drawn for a while
    struct CodeView {
        std::unique_ptr<imshell::TextEditor> editor;
        std::string text;
        int lastFrame = 0;
    };
    std::unordered_map<std::string, CodeView>& codeViews() {
        static std::unordered_map<std::string, CodeView> views;
        return views;
    }

    // the fence's language name as one of ours
    Syntax::Lang langOf(const std::string& name) {
        static const std::pair<const char*, const char*> files[] = {
            {"c", "a.c"},       {"cpp", "a.cpp"},   {"c++", "a.cpp"},  {"h", "a.h"},
            {"hpp", "a.hpp"},   {"cmake", "CMakeLists.txt"},          {"json", "a.json"},
            {"md", "a.md"},     {"markdown", "a.md"}, {"sh", "a.sh"},  {"bash", "a.sh"},
            {"zsh", "a.sh"},    {"shell", "a.sh"}};
        for (const auto& [lang, file] : files) {
            if (name == lang) {
                return Syntax::langOf(file);
            }
        }
        return Syntax::Lang::None;
    }

    void renderCode(const std::string& rawCode, const std::string& lang) {
        const auto& colors = Theme::Current()->colors;
        std::string code = rawCode;
        while (!code.empty() && code.back() == '\n') {
            code.pop_back();
        }
        const int frame = ImGui::GetFrameCount();
        auto& views = codeViews();
        if (frame % 600 == 0) {
            std::erase_if(views, [&](const auto& v) { return frame - v.second.lastFrame > 600; });
        }
        // rich_md scopes the block's ids by its text, which changes while it streams:
        // key them by position instead, so the windows below stay the same
        const std::string key =
            std::format("{}_{}", gScope.idPrefix ? *gScope.idPrefix : "md", gScope.codeBlocks++);
        CodeView& view = views[key];
        if (!view.editor) {
            view.editor = std::make_unique<imshell::TextEditor>();
            view.editor->SetReadOnly(true);
            view.editor->SetShowLineNumbers(false); // a snippet, not a file
            view.editor->SetHighlighter(codeColors(langOf(lang)));
        }
        // the frame's own colour behind the text: one box, not two
        auto palette = imshell::TextEditor::FromTheme(colors);
        palette.background = ImGui::ColorConvertFloat4ToU32(colors.crust);
        view.editor->SetPalette(palette);
        if (view.text != code) { // streaming: the block grows
            view.text = code;
            view.editor->SetText(code);
        }
        view.lastFrame = frame;

        ImGui::PushOverrideID(ImHashStr(key.c_str()));
        ImGui::PushFont(gScope.font, gScope.fontSize); // ours: monospace, with the icons
        ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.crust);
        ImGui::PushStyleColor(ImGuiCol_Border, colors.surface1);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
        if (ImGui::BeginChild("##md_code", ImVec2(-1, 0),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY)) {
            const ImVec2 topRight(ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x,
                                  ImGui::GetCursorScreenPos().y);
            const int lines = static_cast<int>(std::ranges::count(code, '\n')) + 1;
            view.editor->Render("##md_editor",
                                ImVec2(-1, static_cast<float>(lines) * ImGui::GetTextLineHeightWithSpacing() +
                                               6.0f)); // the editor's margins
            // copy floats over the code, top right: a later sibling child draws on top
            static ImGuiID copied = 0;
            static double copiedAt = -10.0;
            const ImGuiID id = ImGui::GetID("copy");
            const bool done = id == copied && ImGui::GetTime() - copiedAt < 1.5;
            const float side = ImGui::GetFrameHeight();
            ImGui::SetCursorScreenPos(ImVec2(topRight.x - side - 4, topRight.y));
            if (ImGui::BeginChild("##md_code_actions", ImVec2(side + 4, side), ImGuiChildFlags_None,
                                  ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar)) {
                ImGui::PushStyleColor(ImGuiCol_Text, done ? colors.green : colors.subtext0);
                if (ImGui::SmallButton(done ? ICON_FA_CHECK "##copy" : ICON_FA_COPY "##copy")) {
                    ImGui::SetClipboardText(code.c_str());
                    copied = id;
                    copiedAt = ImGui::GetTime();
                }
                ImGui::PopStyleColor();
            }
            ImGui::EndChild();
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
    codeViews().clear();
    if (RichMd::GetCurrentContext()) {
        RichMd::DestroyContext();
    }
}
