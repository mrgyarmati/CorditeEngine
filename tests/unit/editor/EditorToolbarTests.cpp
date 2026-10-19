#include "cordite/editor/EditorToolbar.h"

#include "cordite/editor/EditorActions.h"

#include "unit/editor/EditorTestSupport.h"

#include <string>

using namespace cordite;
using cordite::test::EditorFixture;

namespace {

const ToolbarButton& buttonFor(const std::vector<ToolbarButton>& buttons, std::string_view command) {
    for (const ToolbarButton& button : buttons) {
        if (button.command == command) {
            return button;
        }
    }
    FAIL("no button runs " << std::string(command));
    return buttons.front();
}

bool sameIcon(const UiIcon& first, const UiIcon& second) {
    return first.codepoint == second.codepoint;
}

} // namespace

TEST_SUITE("EditorToolbar") {
    TEST_CASE("play starts a scene that is stopped, and nothing else can be done to it") {
        const std::vector<ToolbarButton> buttons = playButtons(PlayState::Stopped);
        REQUIRE(buttons.size() == 3);
        CHECK(sameIcon(buttons[0].icon, icons::kPlay));
        CHECK(buttons[0].command == "play.start");
        CHECK(buttons[0].enabled);
        CHECK_FALSE(buttons[0].active);
        CHECK(buttons[0].tooltip == "Play (F5)");
        CHECK(sameIcon(buttons[1].icon, icons::kPause));
        CHECK_FALSE(buttons[1].enabled);
        CHECK(sameIcon(buttons[2].icon, icons::kStepForward));
        CHECK_FALSE(buttons[2].enabled);
    }

    TEST_CASE("while a scene plays, play becomes stop, and pause holds it") {
        const std::vector<ToolbarButton> buttons = playButtons(PlayState::Playing);
        REQUIRE(buttons.size() == 3);
        CHECK(sameIcon(buttons[0].icon, icons::kStop));
        CHECK(buttons[0].command == "play.stop");
        CHECK(buttons[0].active);
        CHECK(buttons[0].tooltip == "Stop (Shift+F5)");
        CHECK(buttons[1].command == "play.pause");
        CHECK(buttons[1].enabled);
        CHECK_FALSE(buttons[1].active);
        // A frame at a time only while it is held.
        CHECK_FALSE(buttons[2].enabled);
    }

    TEST_CASE("a scene that is held is resumed, stepped or stopped") {
        const std::vector<ToolbarButton> buttons = playButtons(PlayState::Paused);
        REQUIRE(buttons.size() == 3);
        CHECK(sameIcon(buttons[0].icon, icons::kStop));
        CHECK(buttons[0].command == "play.stop");
        CHECK(buttons[0].active);
        CHECK(buttons[1].command == "play.resume");
        CHECK(buttons[1].active);
        CHECK(buttons[1].tooltip == "Resume (F6)");
        CHECK(buttons[2].command == "play.step");
        CHECK(buttons[2].enabled);
        CHECK(buttons[2].tooltip == "Step a frame (F10)");
    }

    TEST_CASE("the tools say what the gizmo does, and switch it") {
        const std::vector<ToolbarButton> moving = gizmoButtons(GizmoMode::Translate, GizmoSpace::World);
        REQUIRE(moving.size() == 4);
        CHECK(sameIcon(moving[0].icon, icons::kMove));
        CHECK(moving[0].command == R"(viewport.gizmo {"mode": "translate"})");
        CHECK(moving[0].active);
        CHECK(moving[0].tooltip == "Move (W)");
        CHECK(sameIcon(moving[1].icon, icons::kRotate));
        CHECK(moving[1].command == R"(viewport.gizmo {"mode": "rotate"})");
        CHECK_FALSE(moving[1].active);
        CHECK(moving[2].command == R"(viewport.gizmo {"mode": "scale"})");
        CHECK(moving[2].tooltip == "Scale (R)");
        // The space shows what it is, and a click makes it the other.
        CHECK(sameIcon(moving[3].icon, icons::kEnvironment));
        CHECK(moving[3].command == R"(viewport.gizmo {"space": "local"})");
        CHECK_FALSE(moving[3].active);

        const std::vector<ToolbarButton> scaling = gizmoButtons(GizmoMode::Scale, GizmoSpace::Local);
        CHECK_FALSE(scaling[0].active);
        CHECK(scaling[2].active);
        CHECK(sameIcon(scaling[3].icon, icons::kEntity));
        CHECK(scaling[3].command == R"(viewport.gizmo {"space": "world"})");
        for (const ToolbarButton& button : scaling) {
            CHECK(button.enabled);
        }
    }

    TEST_CASE("the document's buttons can be clicked when what they do can be done") {
        EditorFixture fixture;
        EditorContext& context = fixture.context;
        std::vector<ToolbarButton> buttons = documentButtons(context);
        REQUIRE(buttons.size() == 3);
        CHECK(sameIcon(buttons[0].icon, icons::kSave));
        // A scene that has no file is saved under a name, which a button cannot ask for.
        CHECK_FALSE(buttonFor(buttons, "scene.save").enabled);
        CHECK(buttonFor(buttons, "scene.save").tooltip.find("Save as") != std::string::npos);
        CHECK_FALSE(buttonFor(buttons, "editor.undo").enabled);
        CHECK_FALSE(buttonFor(buttons, "editor.redo").enabled);

        // What would be undone is in the tooltip.
        REQUIRE(EditorActions(context).renameEntity(fixture.chair, "Stool").has_value());
        buttons = documentButtons(context);
        CHECK(buttonFor(buttons, "editor.undo").enabled);
        CHECK(buttonFor(buttons, "editor.undo").tooltip ==
              fmt::format("Undo {} (Ctrl+Z)", context.history().undoLabel()));
        CHECK_FALSE(buttonFor(buttons, "editor.redo").enabled);

        REQUIRE(context.history().undo().has_value());
        buttons = documentButtons(context);
        CHECK_FALSE(buttonFor(buttons, "editor.undo").enabled);
        CHECK(buttonFor(buttons, "editor.redo").enabled);
        CHECK(buttonFor(buttons, "editor.redo").tooltip ==
              fmt::format("Redo {} (Ctrl+Y)", context.history().redoLabel()));

        // A scene with a file is saved with a click - but not while it is played.
        const test::TempDirectory directory;
        REQUIRE(context.saveScene(directory.path() / "room.scene.json").has_value());
        CHECK(buttonFor(documentButtons(context), "scene.save").enabled);
        CHECK(buttonFor(documentButtons(context), "scene.save").tooltip == "Save (Ctrl+S)");
        REQUIRE(context.beginPlay().has_value());
        CHECK_FALSE(buttonFor(documentButtons(context), "scene.save").enabled);
        REQUIRE(context.endPlay().has_value());
        CHECK(buttonFor(documentButtons(context), "scene.save").enabled);
    }

    TEST_CASE("the status bar sums up the scene: whether it plays, its entities and the selection") {
        CHECK(statusSummary({.entityCount = 0}) == "0 entities");
        CHECK(statusSummary({.entityCount = 1}) == "1 entity");
        CHECK(statusSummary({.entityCount = 41, .selectionCount = 2}) == "41 entities   ·   2 selected");
        CHECK(statusSummary({.play = PlayState::Playing, .entityCount = 41}) == "Playing   ·   41 entities");
        CHECK(statusSummary({.play = PlayState::Paused, .entityCount = 3, .selectionCount = 1}) ==
              "Paused   ·   3 entities   ·   1 selected");
    }
}
