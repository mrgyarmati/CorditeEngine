#pragma once

/// @file
/// The typefaces of the interface, compiled into the program: Inter for what is read, in
/// two weights, JetBrains Mono for what is typed and logged, and the icons of Lucide
/// (UiIcons.h) merged into each of them.
///
/// @code
/// ImGui::PushFont(UiSystem::font(UiFont::Monospace), 0.0f);    // another face, same size
/// ImGui::TextUnformatted(line.c_str());
/// ImGui::PopFont();
/// @endcode
///
/// The files are pinned dependencies (cmake/CorditeDependencies.cmake) whose bytes
/// cmake/CorditeEmbed.cmake compiles into this module: nothing is looked for on the disk
/// when a program starts, from whichever directory it runs.

#include <imgui.h>

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace cordite {

enum class UiFont : uint8_t {
    /// Inter, regular: everything that is none of the others.
    Regular,
    /// Inter, semibold: the names of sections, and what a title says.
    Strong,
    /// JetBrains Mono, without ligatures: what is typed or logged, ids, code.
    Monospace,
};

inline constexpr std::array<UiFont, 3> kUiFonts = {UiFont::Regular, UiFont::Strong, UiFont::Monospace};

/// The name of a typeface as tests and logs say it: "regular", "strong", "monospace".
[[nodiscard]] std::string_view nameOf(UiFont font);

/// The bytes of the TrueType file of a typeface. They are part of the program, and there
/// for as long as it runs.
[[nodiscard]] std::span<const unsigned char> fontFile(UiFont font);

/// The bytes of the TrueType file of Lucide's icons.
[[nodiscard]] std::span<const unsigned char> iconFontFile();

/// Adds the typefaces to `atlas` in the order of UiFont - the first is the one ImGui uses
/// unless told otherwise - each with the icons merged into it, and returns them in that
/// order. `size` is the height of text in pixels that the icons are fitted to (ImGui
/// scales them with the text at any other size). The private use area U+E000 to U+F8FF
/// is the icons': what a typeface has there is not used.
[[nodiscard]] std::array<ImFont*, kUiFonts.size()> addInterfaceFonts(ImFontAtlas& atlas, float size);

} // namespace cordite
