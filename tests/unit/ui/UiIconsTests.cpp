#include "cordite/ui/UiIcons.h"

#include "cordite/ui/UiFonts.h"

#include "support/TestSupport.h"

#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>

using namespace cordite;

namespace {

/// Every icon of UiIcons.h, with the name Lucide gives it in font/lucide.css of the pinned
/// version, and the code point that has there. A new icon is added here as well.
struct KnownIcon {
    UiIcon icon;
    const char* lucideName = nullptr;
    char32_t codepoint = 0;
};

const std::vector<KnownIcon>& knownIcons() {
    static const std::vector<KnownIcon> icons = {
        {icons::kPlay, "play", 0xE13F},
        {icons::kPause, "pause", 0xE131},
        {icons::kStepForward, "step-forward", 0xE3ED},
        {icons::kStop, "square", 0xE16A},
        {icons::kHierarchy, "list-tree", 0xE40C},
        {icons::kInspector, "sliders-horizontal", 0xE299},
        {icons::kConsole, "terminal", 0xE184},
        {icons::kViewport, "axis-3d", 0xE2FD},
        {icons::kStatistics, "activity", 0xE038},
        {icons::kFolder, "folder", 0xE0DC},
        {icons::kFolderOpen, "folder-open", 0xE246},
        {icons::kFile, "file", 0xE0C4},
        {icons::kTexture, "image", 0xE0F9},
        {icons::kMesh, "box", 0xE065},
        {icons::kMaterial, "palette", 0xE1DC},
        {icons::kModel, "boxes", 0xE2CF},
        {icons::kScene, "mountain-snow", 0xE231},
        {icons::kSkeleton, "bone", 0xE35B},
        {icons::kAnimation, "clapperboard", 0xE29A},
        {icons::kScript, "file-code", 0xE0C7},
        {icons::kSound, "audio-lines", 0xE55E},
        {icons::kPrefab, "package", 0xE12C},
        {icons::kSequence, "film", 0xE0D4},
        {icons::kSearch, "search", 0xE154},
        {icons::kRefresh, "refresh-cw", 0xE148},
        {icons::kAdd, "plus", 0xE140},
        {icons::kDelete, "trash-2", 0xE18D},
        {icons::kCopy, "copy", 0xE0A2},
        {icons::kSave, "save", 0xE150},
        {icons::kUndo, "undo-2", 0xE2A0},
        {icons::kRedo, "redo-2", 0xE29F},
        {icons::kMove, "move", 0xE124},
        {icons::kRotate, "rotate-3d", 0xE2E9},
        {icons::kScale, "scale-3d", 0xE2EA},
        {icons::kSelect, "mouse-pointer-2", 0xE1C2},
        {icons::kList, "list", 0xE10B},
        {icons::kGrid, "layout-grid", 0xE104},
        {icons::kChevronLeft, "chevron-left", 0xE072},
        {icons::kChevronRight, "chevron-right", 0xE073},
        {icons::kEntity, "cuboid", 0xE528},
        {icons::kCamera, "camera", 0xE068},
        {icons::kLight, "lightbulb", 0xE1C1},
        {icons::kSun, "sun", 0xE17B},
        {icons::kEye, "eye", 0xE0BE},
        {icons::kEnvironment, "globe", 0xE0EB},
        {icons::kFog, "cloud-fog", 0xE213},
        {icons::kPostProcess, "sparkles", 0xE416},
        {icons::kCharacter, "person-standing", 0xE21D},
        {icons::kCollider, "shapes", 0xE4B7},
        {icons::kJoint, "link", 0xE107},
        {icons::kListener, "ear", 0xE385},
        {icons::kInfo, "info", 0xE0FE},
        {icons::kWarning, "triangle-alert", 0xE192},
        {icons::kError, "circle-x", 0xE088},
    };
    return icons;
}

/// The UTF-8 of a code point of the basic plane, worked out the long way.
std::string utf8Of(char32_t codepoint) {
    std::string text;
    if (codepoint < 0x80) {
        text += static_cast<char>(codepoint);
    } else if (codepoint < 0x800) {
        text += static_cast<char>(0xC0 | (codepoint >> 6));
        text += static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
        text += static_cast<char>(0xE0 | (codepoint >> 12));
        text += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        text += static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    return text;
}

} // namespace

TEST_SUITE("UiIcons") {
    TEST_CASE("an icon is its code point, and the same as UTF-8 text") {
        // Written out by hand once, so that the arithmetic is not only held against itself.
        CHECK(std::string(icons::kPlay.text()) == "\xEE\x84\xBF");
        CHECK(std::string(icons::kStop.text()) == "\xEE\x85\xAA");
        CHECK(std::string(icons::kSound.text()) == "\xEE\x95\x9E");
        CHECK(icons::kPlay.utf8.size() == 4);
        CHECK(icons::kPlay.utf8[3] == '\0');

        // The two ends of the area.
        constexpr UiIcon first = uiIcon<0xE000>();
        constexpr UiIcon last = uiIcon<0xF8FF>();
        CHECK(std::string(first.text()) == "\xEE\x80\x80");
        CHECK(std::string(last.text()) == "\xEF\xA3\xBF");
    }

    TEST_CASE("every icon has the code point Lucide gives it, and different icons are different") {
        std::set<char32_t> seen;
        for (const KnownIcon& known : knownIcons()) {
            CAPTURE(std::string(known.lucideName));
            CHECK(known.icon.codepoint == known.codepoint);
            CHECK(std::string(known.icon.text()) == utf8Of(known.codepoint));
            CHECK(std::strlen(known.icon.text()) == 3);
            CHECK(seen.insert(known.codepoint).second);
        }
    }

    TEST_CASE("the icon font has every icon") {
        ImFontAtlas atlas;
        ImFontConfig source;
        source.FontDataOwnedByAtlas = false;
        const std::span<const unsigned char> file = iconFontFile();
        ImFont* lucide = atlas.AddFontFromMemoryTTF(const_cast<unsigned char*>(file.data()),
                                                    static_cast<int>(file.size()), 16.0f, &source);
        REQUIRE(lucide != nullptr);
        for (const KnownIcon& known : knownIcons()) {
            CAPTURE(std::string(known.lucideName));
            CHECK(lucide->IsGlyphInFont(static_cast<ImWchar>(known.codepoint)));
        }
    }

    TEST_CASE("every typeface of the interface has the icons, and the text") {
        ImFontAtlas atlas;
        const std::array<ImFont*, kUiFonts.size()> fonts = addInterfaceFonts(atlas, 15.0f);
        for (size_t index = 0; index < kUiFonts.size(); ++index) {
            CAPTURE(std::string(nameOf(kUiFonts[index])));
            ImFont* font = fonts[index];
            REQUIRE(font != nullptr);
            // Made of its own file, and of the icons.
            REQUIRE(font->Sources.Size == 2);
            CHECK(font->Sources[0]->FontData == static_cast<const void*>(fontFile(kUiFonts[index]).data()));
            CHECK(font->Sources[1]->FontData == static_cast<const void*>(iconFontFile().data()));
            for (const KnownIcon& known : knownIcons()) {
                CHECK(font->IsGlyphInFont(static_cast<ImWchar>(known.codepoint)));
            }
            // Everything that can be typed on a keyboard of the engine's language, and the
            // signs its units and its texts use.
            for (char32_t letter = 0x20; letter < 0x7F; ++letter) {
                CAPTURE(static_cast<uint32_t>(letter));
                CHECK(font->IsGlyphInFont(static_cast<ImWchar>(letter)));
            }
            for (const char32_t sign : {U'°', U'·', U'…', U'–', U'×'}) {
                CAPTURE(static_cast<uint32_t>(sign));
                CHECK(font->IsGlyphInFont(static_cast<ImWchar>(sign)));
            }
        }
        // The first is what ImGui uses unless told otherwise.
        CHECK(atlas.Fonts[0] == fonts[0]);
        CHECK(atlas.Fonts.Size == static_cast<int>(kUiFonts.size()));
    }

    TEST_CASE("the icons come from Lucide in every typeface, also where a typeface has a glyph of its own") {
        // JetBrains Mono has the symbols of Powerline in the private use area, one of them
        // where Lucide has its icon for copying.
        ImFontAtlas alone;
        ImFontConfig source;
        source.FontDataOwnedByAtlas = false;
        const std::span<const unsigned char> file = fontFile(UiFont::Monospace);
        ImFont* monospace = alone.AddFontFromMemoryTTF(const_cast<unsigned char*>(file.data()),
                                                       static_cast<int>(file.size()), 15.0f, &source);
        REQUIRE(monospace != nullptr);
        CHECK(monospace->IsGlyphInFont(static_cast<ImWchar>(icons::kCopy.codepoint)));

        ImFontAtlas atlas;
        const std::array<ImFont*, kUiFonts.size()> fonts = addInterfaceFonts(atlas, 15.0f);
        for (size_t index = 0; index < kUiFonts.size(); ++index) {
            CAPTURE(std::string(nameOf(kUiFonts[index])));
            ImFontBaked* baked = fonts[index]->GetFontBaked(15.0f, 1.0f);
            REQUIRE(baked != nullptr);
            // The first source of a font is its typeface, the second the icons merged into it.
            for (const KnownIcon& known : knownIcons()) {
                CAPTURE(std::string(known.lucideName));
                const ImFontGlyph* glyph = baked->FindGlyphNoFallback(static_cast<ImWchar>(known.codepoint));
                REQUIRE(glyph != nullptr);
                CHECK(glyph->SourceIdx == 1);
            }
            const ImFontGlyph* letter = baked->FindGlyphNoFallback(static_cast<ImWchar>('A'));
            REQUIRE(letter != nullptr);
            CHECK(letter->SourceIdx == 0);
        }
    }

    TEST_CASE("the font files are TrueType files") {
        const auto isTrueType = [](std::span<const unsigned char> file) {
            // The version of an sfnt: 1.0 for TrueType outlines.
            return file.size() > 12 && file[0] == 0x00 && file[1] == 0x01 && file[2] == 0x00 && file[3] == 0x00;
        };
        for (const UiFont font : kUiFonts) {
            CAPTURE(std::string(nameOf(font)));
            CHECK(isTrueType(fontFile(font)));
        }
        CHECK(isTrueType(iconFontFile()));
        // Three different files, and the icons a fourth.
        CHECK(fontFile(UiFont::Regular).data() != fontFile(UiFont::Strong).data());
        CHECK(fontFile(UiFont::Regular).data() != fontFile(UiFont::Monospace).data());
        CHECK(fontFile(UiFont::Regular).size() != iconFontFile().size());
        CHECK(nameOf(UiFont::Regular) == "regular");
        CHECK(nameOf(UiFont::Strong) == "strong");
        CHECK(nameOf(UiFont::Monospace) == "monospace");
    }
}
