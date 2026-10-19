#pragma once

/// @file
/// Icons for the interface: glyphs of Lucide (lucide.dev), which UiSystem merges into each
/// of its fonts. An icon is text, and goes wherever text does - a button, a label, a menu.
///
/// @code
/// if (ImGui::Button(icons::kPlay.text())) {
///     play();
/// }
/// ImGui::Text("%s Hierarchy", icons::kHierarchy.text());
/// @endcode
///
/// The code points are those of the Lucide that is pinned (cmake/CorditeDependencies.cmake,
/// font/lucide.css in its archive). Another version of Lucide may give an icon another
/// one: look them up again when it is upgraded. UiIconsTests holds every icon to its
/// code point and its UTF-8.

#include <array>

namespace cordite {

/// An icon: its code point, and the same as UTF-8 text.
struct UiIcon {
    char32_t codepoint = 0;
    /// The three bytes of UTF-8, and a terminating zero.
    std::array<char, 4> utf8{};

    /// The icon as text, for wherever ImGui takes some.
    [[nodiscard]] constexpr const char* text() const { return utf8.data(); }
};

/// The icon of a code point of Lucide. Lucide has its glyphs in the private use area of the
/// basic plane, U+E000 to U+F8FF, which UiSystem keeps for them, and each is three bytes of
/// UTF-8 there; another code point does not compile.
template <char32_t Codepoint>
consteval UiIcon uiIcon() {
    static_assert(Codepoint >= 0xE000 && Codepoint <= 0xF8FF,
                  "an icon is a code point of the private use area, U+E000 to U+F8FF");
    return {Codepoint,
            {static_cast<char>(0xE0U | (Codepoint >> 12U)), static_cast<char>(0x80U | ((Codepoint >> 6U) & 0x3FU)),
             static_cast<char>(0x80U | (Codepoint & 0x3FU)), '\0'}};
}

namespace icons {

// Playing.
inline constexpr UiIcon kPlay = uiIcon<0xE13F>();
inline constexpr UiIcon kPause = uiIcon<0xE131>();
inline constexpr UiIcon kStepForward = uiIcon<0xE3ED>();
inline constexpr UiIcon kStop = uiIcon<0xE16A>();

// The panels.
inline constexpr UiIcon kHierarchy = uiIcon<0xE40C>();
inline constexpr UiIcon kInspector = uiIcon<0xE299>();
inline constexpr UiIcon kConsole = uiIcon<0xE184>();
inline constexpr UiIcon kViewport = uiIcon<0xE2FD>();
inline constexpr UiIcon kStatistics = uiIcon<0xE038>();

// Assets, and what they are in.
inline constexpr UiIcon kFolder = uiIcon<0xE0DC>();
inline constexpr UiIcon kFolderOpen = uiIcon<0xE246>();
inline constexpr UiIcon kFile = uiIcon<0xE0C4>();
inline constexpr UiIcon kTexture = uiIcon<0xE0F9>();
inline constexpr UiIcon kMesh = uiIcon<0xE065>();
inline constexpr UiIcon kMaterial = uiIcon<0xE1DC>();
inline constexpr UiIcon kModel = uiIcon<0xE2CF>();
inline constexpr UiIcon kScene = uiIcon<0xE231>();
inline constexpr UiIcon kSkeleton = uiIcon<0xE35B>();
inline constexpr UiIcon kAnimation = uiIcon<0xE29A>();
inline constexpr UiIcon kScript = uiIcon<0xE0C7>();
inline constexpr UiIcon kSound = uiIcon<0xE55E>();
inline constexpr UiIcon kPrefab = uiIcon<0xE12C>();
inline constexpr UiIcon kSequence = uiIcon<0xE0D4>();

// Things to do.
inline constexpr UiIcon kSearch = uiIcon<0xE154>();
inline constexpr UiIcon kRefresh = uiIcon<0xE148>();
inline constexpr UiIcon kAdd = uiIcon<0xE140>();
inline constexpr UiIcon kDelete = uiIcon<0xE18D>();
inline constexpr UiIcon kCopy = uiIcon<0xE0A2>();
inline constexpr UiIcon kSave = uiIcon<0xE150>();
inline constexpr UiIcon kUndo = uiIcon<0xE2A0>();
inline constexpr UiIcon kRedo = uiIcon<0xE29F>();
inline constexpr UiIcon kMove = uiIcon<0xE124>();
inline constexpr UiIcon kRotate = uiIcon<0xE2E9>();
inline constexpr UiIcon kScale = uiIcon<0xE2EA>();
inline constexpr UiIcon kSelect = uiIcon<0xE1C2>();
inline constexpr UiIcon kList = uiIcon<0xE10B>();
inline constexpr UiIcon kGrid = uiIcon<0xE104>();
inline constexpr UiIcon kChevronLeft = uiIcon<0xE072>();
inline constexpr UiIcon kChevronRight = uiIcon<0xE073>();

// What there is in a scene.
inline constexpr UiIcon kEntity = uiIcon<0xE528>();
inline constexpr UiIcon kCamera = uiIcon<0xE068>();
inline constexpr UiIcon kLight = uiIcon<0xE1C1>();
inline constexpr UiIcon kSun = uiIcon<0xE17B>();
inline constexpr UiIcon kEye = uiIcon<0xE0BE>();
inline constexpr UiIcon kEnvironment = uiIcon<0xE0EB>();
inline constexpr UiIcon kFog = uiIcon<0xE213>();
inline constexpr UiIcon kPostProcess = uiIcon<0xE416>();
inline constexpr UiIcon kCharacter = uiIcon<0xE21D>();
inline constexpr UiIcon kCollider = uiIcon<0xE4B7>();
inline constexpr UiIcon kJoint = uiIcon<0xE107>();
inline constexpr UiIcon kListener = uiIcon<0xE385>();

// What the log says.
inline constexpr UiIcon kInfo = uiIcon<0xE0FE>();
inline constexpr UiIcon kWarning = uiIcon<0xE192>();
inline constexpr UiIcon kError = uiIcon<0xE088>();

} // namespace icons

} // namespace cordite
