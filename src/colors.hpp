#pragma once

#include "imshell/themes.hpp"

// the current theme under the names the views draw with
struct Colors {
    ImVec4 text, subtext1, subtext0, overlay1, overlay0, surface2, surface1, surface0, mantle, crust;
    ImVec4 blue, green, red, yellow, peach, teal, purple, rose, sky;
};

inline Colors themeColors() {
    const imshell::Theme& t = imshell::CurrentTheme();
    return {.text = t.foreground,
            .subtext1 = imshell::Mix(t.foreground, t.mutedForeground, 0.5f),
            .subtext0 = t.mutedForeground,
            .overlay1 = imshell::Mix(t.mutedForeground, t.background, 0.3f),
            .overlay0 = t.border,
            .surface2 = imshell::Mix(t.border, t.foreground, 0.15f),
            .surface1 = t.border,
            .surface0 = t.muted,
            .mantle = t.sidebar,
            .crust = t.card,
            .blue = t.hues.blue,
            .green = t.hues.green,
            .red = t.hues.red,
            .yellow = t.hues.yellow,
            .peach = t.hues.orange,
            .teal = t.hues.teal,
            .purple = t.hues.purple,
            .rose = t.hues.pink,
            .sky = t.hues.sky};
}
