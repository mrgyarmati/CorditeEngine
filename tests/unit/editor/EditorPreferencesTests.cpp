#include "cordite/editor/EditorPreferences.h"

#include "cordite/core/FileSystem.h"
#include "cordite/core/Platform.h"

#include "support/TestSupport.h"

#include <fmt/format.h>

#include <string>

using namespace cordite;

namespace {

Json preferencesWith(std::string_view member, const Json& value) {
    Json document = Json::parse(R"({"format": "cordite.preferences", "version": 1})");
    document[std::string(member)] = value;
    return document;
}

} // namespace

TEST_SUITE("EditorPreferences") {
    TEST_CASE("preferences are written with every member, and read back as they were") {
        for (const UiTheme theme : kUiThemes) {
            for (const AssetBrowserView view : kAssetBrowserViews) {
                const EditorPreferences preferences{.theme = theme, .assetView = view};
                const Json document = editorPreferencesDocument(preferences);
                CHECK(document == Json::parse(fmt::format(R"({{"format": "cordite.preferences", "version": 1, )"
                                                          R"("theme": "{}", "assetView": "{}"}})",
                                                          nameOf(theme), nameOf(view))));
                CHECK(test::unwrap(parseEditorPreferences(document)) == preferences);
            }
        }
    }

    TEST_CASE("what a document leaves out keeps its default") {
        const Json document = Json::parse(R"({"format": "cordite.preferences", "version": 1})");
        CHECK(test::unwrap(parseEditorPreferences(document)) == EditorPreferences{});
        CHECK(EditorPreferences{}.theme == UiTheme::Dark);
        // The assets as tiles, as the browsers of Unity and Unreal Engine show them at first.
        CHECK(EditorPreferences{}.assetView == AssetBrowserView::Grid);
        // A file of an editor that had no view of the assets to remember is read as it was.
        const EditorPreferences light =
            test::unwrap(parseEditorPreferences(preferencesWith("theme", std::string("light"))));
        CHECK(light.theme == UiTheme::Light);
        CHECK(light.assetView == AssetBrowserView::Grid);
    }

    TEST_CASE("a view of the assets is found by its name, whatever its case") {
        CHECK(test::unwrap(parseEditorPreferences(preferencesWith("assetView", "List"))).assetView ==
              AssetBrowserView::List);
        CHECK(test::unwrap(parseEditorPreferences(preferencesWith("assetView", "grid"))).assetView ==
              AssetBrowserView::Grid);
    }

    TEST_CASE("a view of the assets that is not one is an error that says which there are") {
        const Result<EditorPreferences> parsed = parseEditorPreferences(preferencesWith("assetView", "tiles"));
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().code == ErrorCode::ParseError);
        CHECK(test::errorOf(parsed).find("'tiles'") != std::string::npos);
        CHECK(test::errorOf(parsed).find("list, grid") != std::string::npos);
        for (const Json& value : {Json(1), Json(false), Json::object(), Json(nullptr)}) {
            CAPTURE(dumpJson(value));
            const Result<EditorPreferences> notName = parseEditorPreferences(preferencesWith("assetView", value));
            REQUIRE_FALSE(notName.has_value());
            CHECK(notName.error().code == ErrorCode::ParseError);
            CHECK(test::errorOf(notName).find("'assetView'") != std::string::npos);
        }
    }

    TEST_CASE("a theme is found by its name, whatever its case") {
        CHECK(test::unwrap(parseEditorPreferences(preferencesWith("theme", "Light"))).theme == UiTheme::Light);
    }

    TEST_CASE("a theme that has no such name is an error that says which there are") {
        const Result<EditorPreferences> parsed = parseEditorPreferences(preferencesWith("theme", "blue"));
        REQUIRE_FALSE(parsed.has_value());
        CHECK(parsed.error().code == ErrorCode::ParseError);
        CHECK(test::errorOf(parsed).find("'blue'") != std::string::npos);
        CHECK(test::errorOf(parsed).find("dark, light") != std::string::npos);
    }

    TEST_CASE("a theme that is no name is an error") {
        for (const Json& value : {Json(3), Json(true), Json::array(), Json(nullptr)}) {
            CAPTURE(dumpJson(value));
            const Result<EditorPreferences> parsed = parseEditorPreferences(preferencesWith("theme", value));
            REQUIRE_FALSE(parsed.has_value());
            CHECK(parsed.error().code == ErrorCode::ParseError);
            CHECK(test::errorOf(parsed).find("'theme'") != std::string::npos);
        }
    }

    TEST_CASE("a member that is not known is an error that names it") {
        const Result<EditorPreferences> parsed = parseEditorPreferences(preferencesWith("colour", "dark"));
        REQUIRE_FALSE(parsed.has_value());
        CHECK(test::errorOf(parsed).find("colour") != std::string::npos);
    }

    TEST_CASE("a document that is something else, or of a newer editor, is refused") {
        CHECK_FALSE(parseEditorPreferences(Json::parse(R"({"theme": "dark"})")).has_value());
        CHECK_FALSE(parseEditorPreferences(Json::parse(R"({"format": "cordite.scene", "version": 1})")).has_value());
        CHECK_FALSE(parseEditorPreferences(Json::parse("[]")).has_value());
        const Result<EditorPreferences> newer =
            parseEditorPreferences(Json::parse(R"({"format": "cordite.preferences", "version": 2})"));
        REQUIRE_FALSE(newer.has_value());
        CHECK(newer.error().code == ErrorCode::VersionMismatch);
    }

    TEST_CASE("preferences are saved to a file, with its directories, and loaded from it") {
        test::TempDirectory directory;
        const std::filesystem::path file = directory.path() / "deeper" / "still" / "preferences.json";
        REQUIRE_MESSAGE(saveEditorPreferences(file, {.theme = UiTheme::Light}).has_value(), "cannot save");
        CHECK(test::unwrap(loadEditorPreferences(file)).theme == UiTheme::Light);

        // Saved again, the file is replaced whole.
        const EditorPreferences listed{.theme = UiTheme::Dark, .assetView = AssetBrowserView::List};
        REQUIRE(saveEditorPreferences(file, listed).has_value());
        CHECK(test::unwrap(loadEditorPreferences(file)) == listed);
        CHECK(test::unwrap(readJsonFile(file)) == editorPreferencesDocument(listed));
    }

    TEST_CASE("a file that is not there gives the defaults") {
        test::TempDirectory directory;
        CHECK(test::unwrap(loadEditorPreferences(directory.path() / "never-written.json")) == EditorPreferences{});
    }

    TEST_CASE("a file that is broken is an error that names it") {
        test::TempDirectory directory;
        const std::filesystem::path wrong = directory.path() / "wrong.json";
        REQUIRE(
            writeTextFile(wrong, R"({"format": "cordite.preferences", "version": 1, "theme": "blue"})").has_value());
        const Result<EditorPreferences> loaded = loadEditorPreferences(wrong);
        REQUIRE_FALSE(loaded.has_value());
        CHECK(test::errorOf(loaded).find("wrong.json") != std::string::npos);
        CHECK(test::errorOf(loaded).find("'blue'") != std::string::npos);

        const std::filesystem::path garbled = directory.path() / "garbled.json";
        REQUIRE(writeTextFile(garbled, "{\"format\": ").has_value());
        CHECK_FALSE(loadEditorPreferences(garbled).has_value());
    }

    TEST_CASE("the preferences of a person are kept in their configuration directory") {
        const std::filesystem::path file = editorPreferencesFile();
        REQUIRE_FALSE(file.empty());
        CHECK(file.filename() == "editor-preferences.json");
        CHECK(file.parent_path().filename() == "Cordite");
        CHECK(file.parent_path().parent_path() == userConfigurationDirectory());
    }
}
