#include "cordite/editor/ReferenceDocs.h"

#include "cordite/editor/EditorApp.h"

#include "render/RenderTestSupport.h"
#include "support/TestSupport.h"

#include <memory>
#include <optional>
#include <string>

using namespace cordite;

TEST_SUITE("ReferenceDocs") {
    TEST_CASE("the reference pages of the guide are what the engine has") {
        EditorAppDesc desc;
        desc.engine.headless = true;
        desc.engine.validation = true;
        desc.engine.renderSize = UVec2(320, 180);
        desc.engine.shaderDirectory = test::shaderDirectory();
        Result<std::unique_ptr<EditorApp>> created = EditorApp::create(desc);
        REQUIRE_MESSAGE(created.has_value(), test::errorOf(created));
        EditorApp& editor = **created;

        test::checkTextAgainstReference(componentReference(editor.engine().components()),
                                        "docs/reference/components.md");
        test::checkTextAgainstReference(scriptReference(editor.engine().scripts()), "docs/reference/script-api.md");
        test::checkTextAgainstReference(commandReference(editor.commands()), "docs/reference/editor-commands.md");
        test::checkTextAgainstReference(inputReference(), "docs/reference/input.md");
        test::checkNoValidationIssues(editor.engine().graphics());
    }

    TEST_CASE("where two texts first differ is said by its line") {
        CHECK_FALSE(test::firstDifference("one\ntwo\n", "one\ntwo\n").has_value());
        // Carriage returns, which Git may put into a file on Windows, are no difference.
        CHECK_FALSE(test::firstDifference("one\r\ntwo\r\n", "one\ntwo\n").has_value());

        const std::optional<std::string> other = test::firstDifference("one\ntwo\nthree\n", "one\n2\nthree\n");
        REQUIRE(other.has_value());
        CHECK(other.value_or("") == "line 2: the file has \"two\", the engine \"2\"");

        const std::optional<std::string> longer = test::firstDifference("one\n", "one\ntwo\n");
        REQUIRE(longer.has_value());
        CHECK(longer.value_or("") == "the file has 1 lines, the engine 2");
        CHECK(test::firstDifference("", "one\n").has_value());
    }
}
