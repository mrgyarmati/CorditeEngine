#include "cordite/ui/UiTheme.h"

#include "cordite/core/Assert.h"
#include "cordite/core/StringUtils.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace cordite {
namespace {

/// A color written as it is in a picture editor: 0xRRGGBB.
constexpr ImVec4 rgb(uint32_t value, float alpha = 1.0f) {
    return {static_cast<float>((value >> 16U) & 0xFFU) / 255.0f, static_cast<float>((value >> 8U) & 0xFFU) / 255.0f,
            static_cast<float>(value & 0xFFU) / 255.0f, alpha};
}

constexpr ImVec4 withAlpha(const ImVec4& color, float alpha) {
    return {color.x, color.y, color.z, alpha};
}

const UiPalette kDark{
    .base = rgb(0x17181B),
    .panel = rgb(0x202226),
    .popup = rgb(0x26282D),
    .field = rgb(0x2C2F35),
    .fieldHovered = rgb(0x363A41),
    .fieldActive = rgb(0x40444C),
    .border = rgb(0x33363C),
    .text = rgb(0xE4E6EA),
    .textDisabled = rgb(0x979CA4),
    .accent = rgb(0xE8892B),
    .accentStrong = rgb(0xF29A42),
    .onAccent = rgb(0x17181B),
    .selectionAlpha = 0.32f,
    .warning = rgb(0xE6C07A),
    .error = rgb(0xF2777A),
    .prefab = rgb(0x8CC0FF),
};

const UiPalette kLight{
    .base = rgb(0xE1E3E7),
    .panel = rgb(0xF3F4F6),
    .popup = rgb(0xFFFFFF),
    .field = rgb(0xFFFFFF),
    .fieldHovered = rgb(0xE8EBEF),
    .fieldActive = rgb(0xDCE0E6),
    .border = rgb(0xC4C9D1),
    .text = rgb(0x1D2025),
    .textDisabled = rgb(0x5F6670),
    .accent = rgb(0xBF5A0C),
    .accentStrong = rgb(0xA84D08),
    .onAccent = rgb(0xFFFFFF),
    .selectionAlpha = 0.22f,
    .warning = rgb(0x8A5A00),
    .error = rgb(0xB42318),
    .prefab = rgb(0x1A4E92),
};

/// The sizes of both themes at 100%: room around things, rounded a little.
void applySizes(ImGuiStyle& style, UiTheme theme) {
    style.WindowPadding = ImVec2(10.0f, 8.0f);
    style.FramePadding = ImVec2(8.0f, 4.0f);
    style.CellPadding = ImVec2(6.0f, 3.0f);
    style.ItemSpacing = ImVec2(8.0f, 6.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.IndentSpacing = 18.0f;
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 10.0f;

    style.WindowRounding = 6.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 4.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 4.0f;

    // Flat: a field is a shade in the dark, and needs an edge only on white.
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 0.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = theme == UiTheme::Light ? 1.0f : 0.0f;
    style.TabBorderSize = 0.0f;
    style.TabBarBorderSize = 1.0f;
    style.TabBarOverlineSize = 2.0f;
    style.SeparatorTextBorderSize = 1.0f;
    style.DockingSeparatorSize = 2.0f;

    // No menu button in front of the tabs of docked panels: what it offers is done by
    // dragging a tab. And no button at the end of them that closes all of a place's panels
    // that can be closed: a tab closes its own, as in Unity and Unreal Engine, and one
    // button for each is enough.
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.DockingNodeHasCloseButton = false;
    style.ColorButtonPosition = ImGuiDir_Right;
}

/// Gives every color of ImGui its value in a theme. A color that a later version of ImGui
/// adds has none here until it is given one, which an assertion says - otherwise it would be
/// left in ImGui's own dark colors, whatever the theme.
void applyColors(ImGuiStyle& style, const UiPalette& palette) {
    std::array<bool, ImGuiCol_COUNT> named{};
    const auto set = [&style, &named](ImGuiCol slot, const ImVec4& color) {
        style.Colors[slot] = color;
        named[static_cast<size_t>(slot)] = true;
    };
    const ImVec4 none(0.0f, 0.0f, 0.0f, 0.0f);

    set(ImGuiCol_Text, palette.text);
    set(ImGuiCol_TextDisabled, palette.textDisabled);
    set(ImGuiCol_TextLink, palette.accent);
    set(ImGuiCol_TextSelectedBg, withAlpha(palette.accent, 0.35f));
    set(ImGuiCol_InputTextCursor, palette.text);
    set(ImGuiCol_UnsavedMarker, palette.text);

    set(ImGuiCol_WindowBg, palette.panel);
    set(ImGuiCol_ChildBg, none);
    set(ImGuiCol_PopupBg, palette.popup);
    set(ImGuiCol_MenuBarBg, palette.base);
    set(ImGuiCol_TitleBg, palette.base);
    set(ImGuiCol_TitleBgActive, palette.base);
    set(ImGuiCol_TitleBgCollapsed, palette.base);
    set(ImGuiCol_DockingEmptyBg, palette.base);
    set(ImGuiCol_DockingPreview, withAlpha(palette.accent, 0.45f));
    set(ImGuiCol_ModalWindowDimBg, withAlpha(palette.base, 0.6f));
    set(ImGuiCol_NavWindowingDimBg, withAlpha(palette.base, 0.6f));
    set(ImGuiCol_NavWindowingHighlight, palette.accent);
    set(ImGuiCol_NavCursor, palette.accent);

    set(ImGuiCol_Border, palette.border);
    set(ImGuiCol_BorderShadow, none);
    set(ImGuiCol_Separator, palette.border);
    set(ImGuiCol_SeparatorHovered, palette.accent);
    set(ImGuiCol_SeparatorActive, palette.accentStrong);
    set(ImGuiCol_ResizeGrip, none);
    set(ImGuiCol_ResizeGripHovered, palette.accent);
    set(ImGuiCol_ResizeGripActive, palette.accentStrong);

    // Fields and buttons are shades; what is on, held or dragged is the accent.
    set(ImGuiCol_FrameBg, palette.field);
    set(ImGuiCol_FrameBgHovered, palette.fieldHovered);
    set(ImGuiCol_FrameBgActive, palette.fieldActive);
    set(ImGuiCol_Button, palette.field);
    set(ImGuiCol_ButtonHovered, palette.fieldHovered);
    set(ImGuiCol_ButtonActive, palette.fieldActive);
    set(ImGuiCol_CheckboxSelectedBg, palette.fieldHovered);
    set(ImGuiCol_CheckMark, palette.accent);
    set(ImGuiCol_SliderGrab, palette.accent);
    set(ImGuiCol_SliderGrabActive, palette.accentStrong);
    set(ImGuiCol_DragDropTarget, palette.accent);
    set(ImGuiCol_DragDropTargetBg, withAlpha(palette.accent, 0.12f));

    // Rows of lists and trees: the accent shows what is selected, a shade what the pointer
    // is over. ImGui draws a collapsing header in these too; sectionHeader() (UiWidgets.h)
    // gives a section the shades of a field instead.
    set(ImGuiCol_Header, withAlpha(palette.accent, palette.selectionAlpha));
    set(ImGuiCol_HeaderHovered, palette.fieldHovered);
    set(ImGuiCol_HeaderActive, withAlpha(palette.accent, palette.selectionAlpha + 0.1f));
    set(ImGuiCol_TreeLines, palette.border);

    set(ImGuiCol_ScrollbarBg, none);
    set(ImGuiCol_ScrollbarGrab, palette.fieldHovered);
    set(ImGuiCol_ScrollbarGrabHovered, palette.fieldActive);
    set(ImGuiCol_ScrollbarGrabActive, palette.accent);

    // A tab in front is the panel it belongs to, with the accent along its top while its
    // panel has the focus.
    set(ImGuiCol_Tab, palette.base);
    set(ImGuiCol_TabHovered, palette.fieldHovered);
    set(ImGuiCol_TabSelected, palette.panel);
    set(ImGuiCol_TabSelectedOverline, palette.accent);
    set(ImGuiCol_TabDimmed, palette.base);
    set(ImGuiCol_TabDimmedSelected, palette.panel);
    set(ImGuiCol_TabDimmedSelectedOverline, palette.border);

    set(ImGuiCol_TableHeaderBg, palette.base);
    set(ImGuiCol_TableBorderStrong, palette.border);
    set(ImGuiCol_TableBorderLight, palette.border);
    set(ImGuiCol_TableRowBg, none);
    set(ImGuiCol_TableRowBgAlt, withAlpha(palette.field, 0.35f));

    set(ImGuiCol_PlotLines, palette.textDisabled);
    set(ImGuiCol_PlotLinesHovered, palette.accent);
    set(ImGuiCol_PlotHistogram, palette.accent);
    set(ImGuiCol_PlotHistogramHovered, palette.accentStrong);

    for (size_t index = 0; index < named.size(); ++index) {
        CORDITE_ASSERT(named[index], "the theme gives ImGui's color {} no value",
                       ImGui::GetStyleColorName(static_cast<ImGuiCol>(index)));
    }
}

/// The luminance of a display color, as WCAG 2 defines it.
float relativeLuminance(const ImVec4& color) {
    const auto linear = [](float channel) {
        const float value = std::clamp(channel, 0.0f, 1.0f);
        return value <= 0.04045f ? value / 12.92f : std::pow((value + 0.055f) / 1.055f, 2.4f);
    };
    return 0.2126f * linear(color.x) + 0.7152f * linear(color.y) + 0.0722f * linear(color.z);
}

} // namespace

std::string_view nameOf(UiTheme theme) {
    switch (theme) {
        case UiTheme::Dark:
            return "dark";
        case UiTheme::Light:
            return "light";
    }
    return "unknown";
}

std::string_view displayNameOf(UiTheme theme) {
    switch (theme) {
        case UiTheme::Dark:
            return "Dark";
        case UiTheme::Light:
            return "Light";
    }
    return "Unknown";
}

std::optional<UiTheme> uiThemeFromName(std::string_view name) {
    for (const UiTheme theme : kUiThemes) {
        if (equalsIgnoreCase(name, nameOf(theme))) {
            return theme;
        }
    }
    return std::nullopt;
}

const UiPalette& paletteOf(UiTheme theme) {
    return theme == UiTheme::Light ? kLight : kDark;
}

void applyTheme(ImGuiStyle& style, UiTheme theme, float scale) {
    const bool usable = std::isfinite(scale) && scale > 0.0f;
    CORDITE_ASSERT(usable, "the scale of an interface is more than nothing, got {}", scale);

    // From ImGui's defaults, not from what the style holds: ImGui multiplies sizes by a
    // scale, and a theme that is chosen twice would otherwise be scaled twice.
    ImGuiStyle themed;
    themed.FontSizeBase = style.FontSizeBase;
    themed.FontScaleMain = style.FontScaleMain;
    themed.FontScaleDpi = style.FontScaleDpi;
    applySizes(themed, theme);
    applyColors(themed, paletteOf(theme));
    themed.ScaleAllSizes(usable ? scale : 1.0f);
    style = themed;
}

float contrastRatio(const ImVec4& first, const ImVec4& second) {
    const float lighter = std::max(relativeLuminance(first), relativeLuminance(second));
    const float darker = std::min(relativeLuminance(first), relativeLuminance(second));
    return (lighter + 0.05f) / (darker + 0.05f);
}

ImVec4 blendColors(const ImVec4& top, float alpha, const ImVec4& bottom) {
    const float amount = std::clamp(alpha, 0.0f, 1.0f);
    return {top.x * amount + bottom.x * (1.0f - amount), top.y * amount + bottom.y * (1.0f - amount),
            top.z * amount + bottom.z * (1.0f - amount), 1.0f};
}

} // namespace cordite
