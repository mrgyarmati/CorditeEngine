#pragma once

/// @file
/// How the interface looks: the colors and the sizes of a theme, put into ImGui's style.
///
/// @code
/// ImGuiStyle style;
/// applyTheme(style, UiTheme::Light, 1.5f);        // the colors, and the sizes at 150%
///
/// const UiPalette& colors = paletteOf(UiTheme::Dark);
/// const float readable = contrastRatio(colors.text, colors.panel);    // 12.8 : 1
/// @endcode
///
/// Both themes are flat: surfaces are told apart by their shade, not by gradients or
/// heavy borders, and one warm accent marks what is selected, active or held - the
/// color of the engine. Each color is chosen for the contrast against what it is drawn
/// on that the tests hold it to (WCAG 2): text 7 to 1 or more, secondary text and the
/// colors of the log 4.5 to 1, the accent 3 to 1, as the parts of a control need.
///
/// Colors are display colors - sRGB, as a screen shows them - like everything the
/// interface draws (it is drawn onto the finished picture).

#include <imgui.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace cordite {

enum class UiTheme : uint8_t {
    /// Light text on dark grays: what an editor of games is usually looked at in.
    Dark,
    /// Dark text on light grays and white fields.
    Light,
};

inline constexpr std::array<UiTheme, 2> kUiThemes = {UiTheme::Dark, UiTheme::Light};

/// The lowercase name a theme has in commands and settings: "dark", "light".
[[nodiscard]] std::string_view nameOf(UiTheme theme);
/// The name people know a theme by, as a menu shows it: "Dark", "Light".
[[nodiscard]] std::string_view displayNameOf(UiTheme theme);
/// The theme of a name, whatever its case. None for a name no theme has.
[[nodiscard]] std::optional<UiTheme> uiThemeFromName(std::string_view name);

/// The colors of a theme, by what they are for.
struct UiPalette {
    /// The deepest surface: the menu bar, the bars of tabs, what is behind docked panels.
    ImVec4 base;
    /// The surface of a panel.
    ImVec4 panel;
    /// The surface of a menu or a window that floats over the others.
    ImVec4 popup;
    /// What a value is edited in, and what a button is: a field.
    ImVec4 field;
    ImVec4 fieldHovered;
    ImVec4 fieldActive;
    /// Lines between things: separators, the edges of tables and of floating windows.
    ImVec4 border;
    ImVec4 text;
    /// What is said in passing, and what cannot be used now.
    ImVec4 textDisabled;
    /// What is selected, checked, active or held.
    ImVec4 accent;
    /// The accent where it is pressed or dragged: a little stronger.
    ImVec4 accentStrong;
    /// What is drawn on the accent: the icon of a button that is on.
    ImVec4 onAccent;
    /// How much of the accent a selected row is, over the panel.
    float selectionAlpha = 0.3f;
    /// The colors of a warning and of an error, as text on a panel.
    ImVec4 warning;
    ImVec4 error;
};

[[nodiscard]] const UiPalette& paletteOf(UiTheme theme);

/// The colors of what is drawn over the picture of a scene - which can be of any color -
/// in every theme: light text, on a dark backdrop that lets the picture through.
inline constexpr ImVec4 kOverlayText{0.93f, 0.94f, 0.95f, 1.0f};
inline constexpr ImVec4 kOverlayBackdrop{0.06f, 0.07f, 0.08f, 0.7f};

/// Puts a theme into a style: every color, and the sizes - paddings, spacings, roundings,
/// borders - multiplied by `scale`, the scale of the display (1 at 100%). Everything else
/// of the style is left as it is, the size of the font among it.
void applyTheme(ImGuiStyle& style, UiTheme theme, float scale);

/// The contrast of two colors as WCAG 2 defines it: from 1 (the same) to 21 (black and
/// white), whichever is the lighter. Alpha is not looked at.
[[nodiscard]] float contrastRatio(const ImVec4& first, const ImVec4& second);

/// `top` laid over `bottom` with the opacity `alpha`, as the interface draws it.
[[nodiscard]] ImVec4 blendColors(const ImVec4& top, float alpha, const ImVec4& bottom);

} // namespace cordite
