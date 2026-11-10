#pragma once

#include <string>

// chat markdown through imgui_rich_md: inline styles, lists, links, tables, and text that
// can be selected and copied. code blocks are ours: framed, with a copy button
namespace Markdown {
    // idPrefix keeps ImGui ids unique per message
    void render(const std::string& markdown, const std::string& idPrefix);
    // frees the renderer's fonts and textures; call while the graphics backend is alive
    void shutdown();
} // namespace Markdown
