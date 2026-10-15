#include "cordite/ui/UiFonts.h"

#include "cordite/core/Assert.h"

#include <algorithm>
#include <iterator>

// Written at build time by cmake/CorditeEmbedFile.cmake, from the files that the
// CMakeLists.txt of this module names. They are defined in those sources, which is what
// misc-use-internal-linkage cannot see.
// NOLINTBEGIN(misc-use-internal-linkage)
namespace cordite::embedded {
std::span<const unsigned char> interRegular();
std::span<const unsigned char> interSemiBold();
std::span<const unsigned char> jetBrainsMonoRegular();
std::span<const unsigned char> lucide();
} // namespace cordite::embedded

// NOLINTEND(misc-use-internal-linkage)

namespace cordite {
namespace {

/// The private use area of the basic plane, where Lucide has its icons, as ImGui takes a
/// list of ranges: pairs of first and last, ended by a zero. ImGui keeps a copy.
constexpr std::array<ImWchar, 3> kIconRange = {0xE000, 0xF8FF, 0};

/// Where an icon is drawn against the text around it, in heights of the text: Lucide draws
/// its square from the ascent of the font down, which is a little high beside Inter.
/// Lowered by this much, an icon sits in the middle of a line of text (the reference
/// pictures of UiTests show it).
constexpr float kIconDrop = 0.1f;
/// How wide an icon is at the least, in heights of the text: icons above each other in
/// a list line up, and the text after them too.
constexpr float kIconWidth = 1.25f;

void nameSource(ImFontConfig& source, std::string_view name) {
    const size_t length = std::min(name.size(), sizeof(source.Name) - 1);
    std::copy_n(name.begin(), length, std::begin(source.Name));
    source.Name[length] = '\0';
}

/// ImGui takes the bytes of a font as `void*`, because by default it takes them over and
/// frees them. These it is told it does not own, and it never writes them.
void* fontBytes(std::span<const unsigned char> file) {
    return const_cast<unsigned char*>(file.data());
}

ImFont* addTypeface(ImFontAtlas& atlas, UiFont font, float size) {
    const std::span<const unsigned char> file = fontFile(font);
    ImFontConfig face;
    face.FontDataOwnedByAtlas = false;
    face.GlyphExcludeRanges = kIconRange.data();
    nameSource(face, nameOf(font));
    ImFont* added = atlas.AddFontFromMemoryTTF(fontBytes(file), static_cast<int>(file.size()), size, &face);

    const std::span<const unsigned char> iconFile = iconFontFile();
    ImFontConfig icons;
    icons.FontDataOwnedByAtlas = false;
    icons.MergeMode = true;
    icons.GlyphMinAdvanceX = size * kIconWidth;
    icons.GlyphOffset = ImVec2(0.0f, size * kIconDrop);
    nameSource(icons, "icons");
    atlas.AddFontFromMemoryTTF(fontBytes(iconFile), static_cast<int>(iconFile.size()), size, &icons);
    return added;
}

} // namespace

std::string_view nameOf(UiFont font) {
    switch (font) {
        case UiFont::Regular:
            return "regular";
        case UiFont::Strong:
            return "strong";
        case UiFont::Monospace:
            return "monospace";
    }
    return "unknown";
}

std::span<const unsigned char> fontFile(UiFont font) {
    switch (font) {
        case UiFont::Regular:
            return embedded::interRegular();
        case UiFont::Strong:
            return embedded::interSemiBold();
        case UiFont::Monospace:
            return embedded::jetBrainsMonoRegular();
    }
    CORDITE_ASSERT(false, "no font file for UiFont {}", static_cast<int>(font));
    return embedded::interRegular();
}

std::span<const unsigned char> iconFontFile() {
    return embedded::lucide();
}

std::array<ImFont*, kUiFonts.size()> addInterfaceFonts(ImFontAtlas& atlas, float size) {
    std::array<ImFont*, kUiFonts.size()> fonts{};
    for (size_t index = 0; index < kUiFonts.size(); ++index) {
        fonts[index] = addTypeface(atlas, kUiFonts[index], size);
    }
    return fonts;
}

} // namespace cordite
