#include "cordite/ui/UiTheme.h"

#include "support/TestSupport.h"

#include <cctype>
#include <limits>
#include <string>

using namespace cordite;

namespace {

/// A color of a picture editor, 0xRRGGBB.
ImVec4 rgb(uint32_t value) {
    return {static_cast<float>((value >> 16U) & 0xFFU) / 255.0f, static_cast<float>((value >> 8U) & 0xFFU) / 255.0f,
            static_cast<float>(value & 0xFFU) / 255.0f, 1.0f};
}

bool sameColor(const ImVec4& first, const ImVec4& second) {
    return first.x == second.x && first.y == second.y && first.z == second.z && first.w == second.w;
}

/// Text needs 7 : 1 against what it is on to be read by everyone (WCAG 2, AAA), text in
/// passing and the colors of the log 4.5 : 1 (AA), and what shows the state of a control
/// 3 : 1 (WCAG 2.1, non-text contrast).
constexpr float kText = 7.0f;
constexpr float kSecondaryText = 4.5f;
constexpr float kControl = 3.0f;

} // namespace

TEST_SUITE("UiTheme") {
    TEST_CASE("every theme has a name that leads back to it, whatever its case") {
        for (const UiTheme theme : kUiThemes) {
            const std::string name(nameOf(theme));
            CHECK(uiThemeFromName(name) == theme);
            std::string upper = name;
            for (char& letter : upper) {
                letter = static_cast<char>(std::toupper(static_cast<unsigned char>(letter)));
            }
            CHECK(uiThemeFromName(upper) == theme);
        }
        CHECK(nameOf(UiTheme::Dark) == "dark");
        CHECK(nameOf(UiTheme::Light) == "light");
        CHECK(displayNameOf(UiTheme::Dark) == "Dark");
        CHECK(displayNameOf(UiTheme::Light) == "Light");
    }

    TEST_CASE("a name that no theme has is none") {
        CHECK_FALSE(uiThemeFromName("").has_value());
        CHECK_FALSE(uiThemeFromName("blue").has_value());
        CHECK_FALSE(uiThemeFromName("dark ").has_value());
        CHECK_FALSE(uiThemeFromName("darkness").has_value());
    }

    TEST_CASE("contrast is measured as WCAG 2 does") {
        const ImVec4 black = rgb(0x000000);
        const ImVec4 white = rgb(0xFFFFFF);
        CHECK(contrastRatio(black, white) == doctest::Approx(21.0f));
        CHECK(contrastRatio(white, black) == doctest::Approx(21.0f));
        CHECK(contrastRatio(white, white) == doctest::Approx(1.0f));
        // The gray that the guidelines name as the lightest that passes on white.
        CHECK(contrastRatio(rgb(0x767676), white) == doctest::Approx(4.54f).epsilon(0.002));
        CHECK(contrastRatio(rgb(0x777777), white) < 4.5f);
        // A channel this dark is on the straight part of the curve of sRGB: 10 / 255 / 12.92.
        CHECK(contrastRatio(rgb(0x0A0A0A), white) == doctest::Approx(19.798f).epsilon(0.001));
        // Alpha is not looked at, and neither is what lies outside a color.
        ImVec4 seeThrough = black;
        seeThrough.w = 0.0f;
        CHECK(contrastRatio(seeThrough, white) == doctest::Approx(21.0f));
        CHECK(contrastRatio(ImVec4(-1.0f, -2.0f, -3.0f, 1.0f), ImVec4(2.0f, 3.0f, 4.0f, 1.0f)) ==
              doctest::Approx(21.0f));
    }

    TEST_CASE("blending lays one color over another") {
        const ImVec4 blended = blendColors(rgb(0xFF0000), 0.25f, rgb(0x0000FF));
        CHECK(blended.x == doctest::Approx(0.25f));
        CHECK(blended.y == doctest::Approx(0.0f));
        CHECK(blended.z == doctest::Approx(0.75f));
        CHECK(blended.w == 1.0f);
        CHECK(sameColor(blendColors(rgb(0x123456), 1.0f, rgb(0xABCDEF)), rgb(0x123456)));
        CHECK(sameColor(blendColors(rgb(0x123456), 0.0f, rgb(0xABCDEF)), rgb(0xABCDEF)));
        // What is not an opacity is brought to the nearest that is.
        CHECK(sameColor(blendColors(rgb(0x123456), 2.0f, rgb(0xABCDEF)), rgb(0x123456)));
        CHECK(sameColor(blendColors(rgb(0x123456), -1.0f, rgb(0xABCDEF)), rgb(0xABCDEF)));
    }

    TEST_CASE("text of every theme can be read on every surface it is drawn on") {
        for (const UiTheme theme : kUiThemes) {
            CAPTURE(std::string(nameOf(theme)));
            const UiPalette& palette = paletteOf(theme);
            for (const ImVec4& surface : {palette.base, palette.panel, palette.popup, palette.field,
                                          palette.fieldHovered, palette.fieldActive}) {
                CHECK(contrastRatio(palette.text, surface) >= kText);
            }
            // A selected row is the accent laid over the panel.
            const ImVec4 selected = blendColors(palette.accent, palette.selectionAlpha, palette.panel);
            CHECK(contrastRatio(palette.text, selected) >= kText);
        }
    }

    TEST_CASE("secondary text, warnings and errors of every theme can be read on panels and fields") {
        for (const UiTheme theme : kUiThemes) {
            CAPTURE(std::string(nameOf(theme)));
            const UiPalette& palette = paletteOf(theme);
            for (const ImVec4& surface : {palette.panel, palette.field, palette.popup}) {
                CHECK(contrastRatio(palette.textDisabled, surface) >= kSecondaryText);
                CHECK(contrastRatio(palette.warning, surface) >= kSecondaryText);
                CHECK(contrastRatio(palette.error, surface) >= kSecondaryText);
                CHECK(contrastRatio(palette.prefab, surface) >= kSecondaryText);
            }
            // What is part of a prefab is read on a selected row of the hierarchy as well.
            const ImVec4 selected = blendColors(palette.accent, palette.selectionAlpha, palette.panel);
            CHECK(contrastRatio(palette.prefab, selected) >= kSecondaryText);
        }
    }

    TEST_CASE("the accent of every theme stands out from what it marks") {
        for (const UiTheme theme : kUiThemes) {
            CAPTURE(std::string(nameOf(theme)));
            const UiPalette& palette = paletteOf(theme);
            for (const ImVec4& surface : {palette.base, palette.panel, palette.field}) {
                CHECK(contrastRatio(palette.accent, surface) >= kControl);
                CHECK(contrastRatio(palette.accentStrong, surface) >= kControl);
            }
            // The icon of a button that is on, on the accent, and on it pressed.
            CHECK(contrastRatio(palette.onAccent, palette.accent) >= kControl);
            CHECK(contrastRatio(palette.onAccent, palette.accentStrong) >= kControl);
            // Surfaces differ by a shade, which is what a flat look tells them apart by;
            // a shade, not a contrast that draws the eye.
            CHECK(contrastRatio(palette.panel, palette.base) > 1.05f);
            CHECK(contrastRatio(palette.field, palette.panel) > 1.05f);
            CHECK(contrastRatio(palette.panel, palette.base) < 1.5f);
            CHECK(palette.selectionAlpha > 0.0f);
            CHECK(palette.selectionAlpha < 1.0f);
        }
    }

    TEST_CASE("what is drawn over a scene can be read over any picture") {
        // The backdrop lets the picture through: over white and over black, and over
        // the grays between, the text stands out from what is behind it.
        for (const uint32_t picture : {0xFFFFFFU, 0x000000U, 0x808080U, 0xFF4040U, 0x40FF40U}) {
            CAPTURE(picture);
            const ImVec4 behind = blendColors(kOverlayBackdrop, kOverlayBackdrop.w, rgb(picture));
            CHECK(contrastRatio(kOverlayText, behind) >= kSecondaryText);
        }
        CHECK(kOverlayBackdrop.w < 1.0f);
    }

    TEST_CASE("the dark theme is dark and the light theme is light") {
        const UiPalette& dark = paletteOf(UiTheme::Dark);
        const UiPalette& light = paletteOf(UiTheme::Light);
        CHECK(contrastRatio(dark.panel, rgb(0x000000)) < contrastRatio(dark.panel, rgb(0xFFFFFF)));
        CHECK(contrastRatio(light.panel, rgb(0x000000)) > contrastRatio(light.panel, rgb(0xFFFFFF)));
        CHECK(contrastRatio(dark.text, rgb(0x000000)) > contrastRatio(dark.text, rgb(0xFFFFFF)));
        CHECK(contrastRatio(light.text, rgb(0x000000)) < contrastRatio(light.text, rgb(0xFFFFFF)));
    }

    TEST_CASE("a theme puts its colors into a style") {
        for (const UiTheme theme : kUiThemes) {
            CAPTURE(std::string(nameOf(theme)));
            const UiPalette& palette = paletteOf(theme);
            ImGuiStyle style;
            applyTheme(style, theme, 1.0f);

            CHECK(sameColor(style.Colors[ImGuiCol_Text], palette.text));
            CHECK(sameColor(style.Colors[ImGuiCol_TextDisabled], palette.textDisabled));
            CHECK(sameColor(style.Colors[ImGuiCol_WindowBg], palette.panel));
            CHECK(sameColor(style.Colors[ImGuiCol_PopupBg], palette.popup));
            CHECK(sameColor(style.Colors[ImGuiCol_MenuBarBg], palette.base));
            CHECK(sameColor(style.Colors[ImGuiCol_FrameBg], palette.field));
            CHECK(sameColor(style.Colors[ImGuiCol_Button], palette.field));
            CHECK(sameColor(style.Colors[ImGuiCol_CheckMark], palette.accent));
            CHECK(sameColor(style.Colors[ImGuiCol_TabSelectedOverline], palette.accent));
            CHECK(style.Colors[ImGuiCol_Header].w == doctest::Approx(palette.selectionAlpha));

            // Every color is one, and opaque wherever text is drawn on it.
            for (int index = 0; index < ImGuiCol_COUNT; ++index) {
                CAPTURE(std::string(ImGui::GetStyleColorName(index)));
                const ImVec4& color = style.Colors[index];
                for (const float channel : {color.x, color.y, color.z, color.w}) {
                    CHECK(channel >= 0.0f);
                    CHECK(channel <= 1.0f);
                }
            }
            for (const ImGuiCol surface : {ImGuiCol_WindowBg, ImGuiCol_PopupBg, ImGuiCol_MenuBarBg, ImGuiCol_FrameBg,
                                           ImGuiCol_Button, ImGuiCol_Tab, ImGuiCol_TabSelected}) {
                CHECK(style.Colors[surface].w == 1.0f);
            }
        }
    }

    TEST_CASE("a theme is flat") {
        ImGuiStyle dark;
        applyTheme(dark, UiTheme::Dark, 1.0f);
        CHECK(dark.FrameBorderSize == 0.0f);
        CHECK(dark.TabBorderSize == 0.0f);
        CHECK(dark.ChildBorderSize == 0.0f);
        CHECK(dark.Colors[ImGuiCol_BorderShadow].w == 0.0f);
        // Docked panels have no menu button in front of their tabs, and no button after them
        // that closes them all: a panel that can be closed has a button on its tab.
        CHECK(dark.WindowMenuButtonPosition == ImGuiDir_None);
        CHECK_FALSE(dark.DockingNodeHasCloseButton);

        // On white a field needs an edge to be seen at all.
        ImGuiStyle light;
        applyTheme(light, UiTheme::Light, 1.0f);
        CHECK(light.FrameBorderSize == 1.0f);
    }

    TEST_CASE("the sizes of a theme follow the scale of the display") {
        ImGuiStyle normal;
        applyTheme(normal, UiTheme::Dark, 1.0f);
        ImGuiStyle doubled;
        applyTheme(doubled, UiTheme::Dark, 2.0f);
        CHECK(doubled.FramePadding.x == normal.FramePadding.x * 2.0f);
        CHECK(doubled.FramePadding.y == normal.FramePadding.y * 2.0f);
        CHECK(doubled.WindowPadding.x == normal.WindowPadding.x * 2.0f);
        CHECK(doubled.ItemSpacing.y == normal.ItemSpacing.y * 2.0f);
        CHECK(doubled.FrameRounding == normal.FrameRounding * 2.0f);
        CHECK(doubled.ScrollbarSize == normal.ScrollbarSize * 2.0f);
        CHECK(doubled.WindowMinSize.x == normal.WindowMinSize.x * 2.0f);
        // The colors are the same at any scale.
        CHECK(sameColor(doubled.Colors[ImGuiCol_WindowBg], normal.Colors[ImGuiCol_WindowBg]));
    }

    TEST_CASE("a theme put into a style twice is the theme once") {
        // ImGui multiplies sizes by a scale; a theme that scaled what the style held would
        // grow every time it is chosen again.
        ImGuiStyle once;
        applyTheme(once, UiTheme::Light, 1.5f);
        ImGuiStyle twice;
        applyTheme(twice, UiTheme::Dark, 1.5f);
        applyTheme(twice, UiTheme::Light, 1.5f);
        CHECK(twice.FramePadding.x == once.FramePadding.x);
        CHECK(twice.WindowPadding.y == once.WindowPadding.y);
        CHECK(twice.WindowMinSize.x == once.WindowMinSize.x);
        CHECK(twice.DisplaySafeAreaPadding.x == once.DisplaySafeAreaPadding.x);
        CHECK(twice.FrameBorderSize == once.FrameBorderSize);
        for (int index = 0; index < ImGuiCol_COUNT; ++index) {
            CAPTURE(std::string(ImGui::GetStyleColorName(index)));
            CHECK(sameColor(twice.Colors[index], once.Colors[index]));
        }
    }

    TEST_CASE("a theme leaves the size of the text as it is") {
        ImGuiStyle style;
        style.FontSizeBase = 17.0f;
        style.FontScaleMain = 1.25f;
        style.FontScaleDpi = 2.0f;
        applyTheme(style, UiTheme::Light, 2.0f);
        CHECK(style.FontSizeBase == 17.0f);
        CHECK(style.FontScaleMain == 1.25f);
        CHECK(style.FontScaleDpi == 2.0f);
    }

#if defined(CORDITE_ENABLE_ASSERTS)
    TEST_CASE("a scale that is no size is a mistake") {
        ImGuiStyle style;
        CHECK_THROWS_AS(applyTheme(style, UiTheme::Dark, 0.0f), test::AssertionFailure);
        CHECK_THROWS_AS(applyTheme(style, UiTheme::Dark, -1.0f), test::AssertionFailure);
        CHECK_THROWS_AS(applyTheme(style, UiTheme::Dark, std::numeric_limits<float>::quiet_NaN()),
                        test::AssertionFailure);
    }
#endif
}
