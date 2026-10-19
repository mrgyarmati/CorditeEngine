#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/Image.h"
#include "cordite/assets/ImageCompare.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Uuid.h"
#include "cordite/editor/EditorApp.h"
#include "cordite/editor/Gizmo.h"
#include "cordite/editor/ViewportPanel.h"
#include "cordite/platform/Window.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/EnvironmentProbes.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/renderer/RenderResources.h"
#include "cordite/renderer/SceneRenderer.h"
#include "cordite/rhi/TextureReadback.h"

#include "render/EditorTestSupport.h"
#include "render/RenderTestSupport.h"
#include "support/MathTestSupport.h"

#include <fmt/format.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace cordite;
using cordite::test::checkNoValidationIssues;
using cordite::test::createEditor;
using cordite::test::dragPointer;
using cordite::test::headlessDesc;
using cordite::test::json;
using cordite::test::pressShortcut;
using cordite::test::quoted;
using cordite::test::refusal;
using cordite::test::regionOf;
using cordite::test::run;
using cordite::test::showcaseProject;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

/// Where the picture of a viewport is on the screen, in the pixels of the interface - which
/// are pixels of the picture, without a window.
struct ScreenRect {
    Vec2 min{0.0f};
    Vec2 max{0.0f};

    [[nodiscard]] Vec2 center() const { return 0.5f * (min + max); }
};

ScreenRect pictureOf(const ViewportPanel& viewport) {
    const Vec2 origin = viewport.imageOrigin();
    return {.min = origin, .max = origin + Vec2(viewport.targets().size())};
}

/// How many pixels of two pictures of the same size differ by more than a step or two.
uint64_t differingPixels(const Image& actual, const Image& expected) {
    return unwrap(compareImages(actual, expected)).differingPixels;
}

/// The window of a viewport, as Dear ImGui has it.
ImGuiWindow* windowOf(const ViewportPanel& viewport) {
    ImGuiWindow* window = ImGui::FindWindowByName(viewport.windowId().c_str());
    REQUIRE(window != nullptr);
    return window;
}

/// The numbers of the viewports viewport.list says are open, in its order.
std::vector<uint32_t> openViewports(EditorApp& editor) {
    const Json listed = run(editor, "viewport.list");
    std::vector<uint32_t> numbers;
    for (const Json& viewport : listed["viewports"]) {
        numbers.push_back(viewport["viewport"].get<uint32_t>());
    }
    return numbers;
}

/// The pointer, moved to a place of the interface, with the frames it takes to be seen there.
void pointerTo(EditorApp& editor, Vec2 at) {
    run(editor, "input.inject", fmt::format(R"({{"mouseMove": [{}, {}]}})", at.x, at.y));
    REQUIRE(editor.runFrames(2).has_value());
}

/// A button of the mouse pressed and let go of where the pointer is.
void click(EditorApp& editor, std::string_view button) {
    run(editor, "input.inject", fmt::format(R"({{"mouseButton": "{}", "down": true}})", button));
    REQUIRE(editor.runFrames(1).has_value());
    run(editor, "input.inject", fmt::format(R"({{"mouseButton": "{}", "down": false}})", button));
    REQUIRE(editor.runFrames(2).has_value());
}

} // namespace

TEST_SUITE("EditorViewports") {
    TEST_CASE("viewports are opened beside the first, up to four, each with the lowest number that is free") {
        const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(1280, 720)));
        // A new editor has the first viewport alone, and it is the one used last.
        CHECK(run(*editor, "viewport.list") ==
              json(R"({"active": 1, "viewports": [{"viewport": 1, "camera": "editor", "showsGame": false,
                                                   "debugView": "none", "colliders": false}]})"));

        // Opened before there was a frame to lay it out in, the second goes to the right of
        // the first, docked, and is the one used last from then on.
        CHECK(run(*editor, "viewport.open")["viewport"] == 2);
        CHECK(editor->activeViewport() == 2);
        REQUIRE(editor->runFrames(3).has_value());
        ViewportPanel& first = editor->viewport();
        ViewportPanel* const second = editor->findViewport(2);
        REQUIRE(second != nullptr);
        CHECK(second->number() == 2);
        CHECK(second->windowId() == "###Viewport 2");
        CHECK(windowOf(*second)->DockNode != nullptr);
        CHECK(windowOf(first)->DockNode != nullptr);
        CHECK(pictureOf(*second).min.x >= pictureOf(first).max.x);
        CHECK(pictureOf(*second).min.y == pictureOf(first).min.y);
        CHECK(pictureOf(*second).max.y == pictureOf(first).max.y);
        // It is the one with the focus, as a window that appears is: still the one used last.
        CHECK(editor->activeViewport() == 2);

        // The third below the first, the fourth below the second: two by two.
        CHECK(run(*editor, "viewport.open")["viewport"] == 3);
        CHECK(run(*editor, "viewport.open")["viewport"] == 4);
        CHECK(editor->activeViewport() == 4);
        REQUIRE(editor->runFrames(3).has_value());
        ViewportPanel* const third = editor->findViewport(3);
        ViewportPanel* const fourth = editor->findViewport(4);
        REQUIRE(third != nullptr);
        REQUIRE(fourth != nullptr);
        CHECK(pictureOf(*third).min.y >= pictureOf(first).max.y);
        CHECK(pictureOf(*third).min.x == pictureOf(first).min.x);
        CHECK(pictureOf(*fourth).min.y >= pictureOf(*second).max.y);
        CHECK(pictureOf(*fourth).min.x == pictureOf(*second).min.x);
        CHECK(pictureOf(*fourth).min.x >= pictureOf(*third).max.x);
        CHECK(openViewports(*editor) == std::vector<uint32_t>{1, 2, 3, 4});

        // There is no fifth: a frame has room for so many views.
        const Error full = refusal(*editor, "viewport.open");
        CHECK(full.code == ErrorCode::InvalidState);
        CHECK(full.message.find("4 viewports are open") != std::string::npos);

        // A viewport that is closed gives its number back, and the room it had to what is
        // beside it; opened again, it has its place again.
        const ScreenRect secondWas = pictureOf(*second);
        CHECK(run(*editor, "viewport.close", R"({"viewport": 2})")["viewports"].size() == 3);
        CHECK(editor->findViewport(2) == nullptr);
        CHECK(openViewports(*editor) == std::vector<uint32_t>{1, 3, 4});
        REQUIRE(editor->runFrames(3).has_value());
        CHECK(pictureOf(*fourth).min.y == secondWas.min.y);
        CHECK(run(*editor, "viewport.open")["viewport"] == 2);
        REQUIRE(editor->runFrames(3).has_value());
        ViewportPanel* const reopened = editor->findViewport(2);
        REQUIRE(reopened != nullptr);
        CHECK(windowOf(*reopened)->DockNode != nullptr);
        CHECK(pictureOf(*reopened).min.x == secondWas.min.x);
        CHECK(pictureOf(*reopened).min.y == secondWas.min.y);
        CHECK(pictureOf(*fourth).min.y >= pictureOf(*reopened).max.y);

        // Closed and opened again twice between two frames, it is where it goes all the same.
        run(*editor, "viewport.close", R"({"viewport": 4})");
        run(*editor, "viewport.open");
        run(*editor, "viewport.close", R"({"viewport": 4})");
        run(*editor, "viewport.open");
        REQUIRE(editor->runFrames(3).has_value());
        REQUIRE(editor->findViewport(4) != nullptr);
        CHECK(windowOf(*editor->findViewport(4))->DockNode != nullptr);
        CHECK(pictureOf(*editor->findViewport(4)).min.y >= pictureOf(*reopened).max.y);
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("a command about a viewport is about the one it names, or the one used last") {
        const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(640, 360)));
        run(*editor, "viewport.lookAt", R"({"position": [0, 2, 10], "target": [0, 2, 0]})");
        const Json firstCamera = run(*editor, "viewport.camera");

        // A viewport starts where the camera of the one used last is, and is then the one
        // used last: what names no viewport is about it.
        CHECK(run(*editor, "viewport.open")["viewport"] == 2);
        CHECK(run(*editor, "viewport.camera") == firstCamera);
        run(*editor, "viewport.lookAt", R"({"position": [8, 1, 0], "target": [0, 1, 0]})");
        CHECK(run(*editor, "viewport.camera", R"({"viewport": 1})") == firstCamera);
        CHECK(run(*editor, "viewport.camera", R"({"viewport": 2})")["position"] == json("[8, 1, 0]"));
        CHECK(editor->findViewport(2)->camera().position() == Vec3(8.0f, 1.0f, 0.0f));
        CHECK(editor->camera().position() == Vec3(0.0f, 2.0f, 10.0f));

        // Each has its own render view, colliders and camera to look through.
        run(*editor, "viewport.debugView", R"({"view": "normals"})");
        run(*editor, "viewport.show", R"({"colliders": true, "viewport": 1})");
        run(*editor, "viewport.show", R"({"camera": "game", "viewport": 2})");
        CHECK(run(*editor, "viewport.list") == json(R"({"active": 2, "viewports": [
            {"viewport": 1, "camera": "editor", "showsGame": false, "debugView": "none", "colliders": true},
            {"viewport": 2, "camera": "game", "showsGame": false, "debugView": "normals", "colliders": false}]})"));
        CHECK(run(*editor, "viewport.debugView", R"({"viewport": 1})")["view"] == "none");
        // A number written with a point is a number all the same.
        CHECK(run(*editor, "viewport.debugView", R"({"viewport": 2.0})")["view"] == "normals");
        run(*editor, "viewport.lookAt", R"({"position": [0, 3, 6], "target": [0, 0, 0], "viewport": 1})");
        CHECK(editor->camera().position() == Vec3(0.0f, 3.0f, 6.0f));

        // What is no viewport that is open is refused, with the ones that are.
        for (const char* number : {"0", "3", "5", "4000000000", "-1"}) {
            CAPTURE(std::string(number));
            const Error error =
                refusal(*editor, "viewport.lookAt",
                        fmt::format(R"({{"position": [0, 0, 1], "target": [0, 0, 0], "viewport": {}}})", number));
            CHECK(error.code == ErrorCode::NotFound);
            CHECK(error.message.find("the open ones are 1 and 2") != std::string::npos);
        }
        CHECK(refusal(*editor, "viewport.camera", R"({"viewport": 1.5})").code == ErrorCode::InvalidArgument);
        CHECK(refusal(*editor, "viewport.camera", R"({"viewport": "2"})").code == ErrorCode::InvalidArgument);
        CHECK(editor->camera().position() == Vec3(0.0f, 3.0f, 6.0f));
        // The first is always there.
        const Error first = refusal(*editor, "viewport.close", R"({"viewport": 1})");
        CHECK(first.code == ErrorCode::InvalidArgument);
        CHECK(first.message.find("first viewport") != std::string::npos);
        CHECK(refusal(*editor, "viewport.close").code == ErrorCode::InvalidArgument);
        CHECK(refusal(*editor, "viewport.open", R"({"camera": "drone"})").code == ErrorCode::InvalidArgument);
        CHECK(openViewports(*editor) == std::vector<uint32_t>{1, 2});

        // A picture of one viewport alone, by its number - and of one that is not open, none.
        const TempDirectory directory;
        const Json saved = run(*editor, "editor.screenshot",
                               fmt::format(R"({{"path": {}, "viewportOnly": true, "viewport": 2,
                                                "width": 64, "height": 32}})",
                                           quoted(directory.path() / "second.png")));
        CHECK(saved["width"] == 64);
        CHECK(refusal(*editor, "editor.screenshot",
                      fmt::format(R"({{"path": {}, "viewportOnly": true, "viewport": 3}})",
                                  quoted(directory.path() / "third.png")))
                  .code == ErrorCode::NotFound);
        const Result<Image> none = editor->captureViewport(UVec2(64, 32), 3);
        REQUIRE_FALSE(none.has_value());
        CHECK(none.error().code == ErrorCode::NotFound);
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("every viewport draws its own view of the scene, at the quality of all of them") {
        EditorAppDesc desc = headlessDesc(UVec2(320, 180));
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        run(*editor, "viewport.lookAt", R"({"position": [-5.5, 3.2, 6.5], "target": [0.5, 0.8, -1.5]})");
        run(*editor, "viewport.open");
        REQUIRE(editor->runFrames(2).has_value());
        const UVec2 size(160, 90);
        const Image firstView = unwrap(editor->captureViewport(size, 1));

        // Another render view, or another place, is another picture - and leaves the first's.
        run(*editor, "viewport.debugView", R"({"view": "normals", "viewport": 2})");
        const Image normals = unwrap(editor->captureViewport(size, 2));
        CHECK(differingPixels(normals, firstView) > size.x * size.y / 2);
        run(*editor, "viewport.debugView", R"({"view": "none", "viewport": 2})");
        run(*editor, "viewport.lookAt", R"({"position": [4, 1.5, 4], "target": [0, 1, 0], "viewport": 2})");
        CHECK(differingPixels(unwrap(editor->captureViewport(size, 2)), firstView) > size.x * size.y / 4);
        CHECK(differingPixels(unwrap(editor->captureViewport(size, 1)), firstView) == 0);

        // The quality is that of all of them: with two shadow qualities the renderer would make
        // its atlas anew twice a frame. The atlas of the low quality is 2048 texels a side.
        CHECK(run(*editor, "viewport.quality", R"({"preset": "low"})")["shadows"] == "low");
        CHECK(editor->viewportSettings().shadowQuality == ShadowQuality::Low);
        REQUIRE(editor->runFrames(2).has_value());
        CHECK(run(*editor, "editor.statistics")["scene"]["shadows"]["atlasBytes"] == uint64_t{2048} * 2048 * 4);
        // And so is what the gizmo does.
        run(*editor, "viewport.gizmo", R"({"mode": "scale", "space": "local"})");
        CHECK(editor->viewportSettings().gizmoMode == GizmoMode::Scale);
        CHECK(editor->viewportSettings().gizmoSpace == GizmoSpace::Local);
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("the scene captures and the probes are drawn once a frame, however many viewports draw the scene") {
        EditorAppDesc desc = headlessDesc(UVec2(320, 180));
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        const auto scene = [&] { return run(*editor, "editor.statistics")["scene"]; };
        REQUIRE(editor->runFrames(3).has_value());
        // The viewport, and before it the security camera's capture of the stage.
        CHECK(scene()["views"] == 2);
        CHECK(scene()["captures"]["count"] == 1);

        // Three viewports draw three views, and the capture is drawn once for them all.
        run(*editor, "viewport.open");
        run(*editor, "viewport.open");
        REQUIRE(editor->runFrames(3).has_value());
        CHECK(scene()["views"] == 3 + 1);
        CHECK(scene()["captures"]["count"] == 1);
        CHECK(scene()["probes"]["captures"] == 0);
        // A probe that is asked for is taken once, in one frame: six views more.
        run(*editor, "probes.capture");
        REQUIRE(editor->runFrames(1).has_value());
        CHECK(scene()["probes"]["captures"] == 1);
        CHECK(scene()["views"] == 3 + 1 + kCubeFaceCount);
        REQUIRE(editor->runFrames(1).has_value());
        CHECK(scene()["probes"]["captures"] == 0);
        CHECK(scene()["views"] == 3 + 1);

        // A viewport whose tab is behind another's draws nothing, and leaves the captures to
        // the next one that draws: the first, put beside the second as a tab behind it.
        ImGui::DockBuilderDockWindow(ViewportPanel::kWindowId, windowOf(*editor->findViewport(2))->DockId);
        run(*editor, "editor.focusPanel", R"({"panel": "viewport2"})");
        REQUIRE(editor->runFrames(3).has_value());
        CHECK(windowOf(editor->viewport())->DockNode == windowOf(*editor->findViewport(2))->DockNode);
        CHECK(scene()["views"] == 2 + 1);
        CHECK(scene()["captures"]["count"] == 1);
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("a viewport opened while a camera is steered starts where that camera is, standing still") {
        const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(640, 360)));
        REQUIRE(editor->runFrames(3).has_value());
        pointerTo(*editor, pictureOf(editor->viewport()).center());
        run(*editor, "input.inject", R"({"mouseButton": "Right", "down": true})");
        REQUIRE(editor->runFrames(1).has_value());
        REQUIRE(editor->camera().isNavigating());

        run(*editor, "viewport.open");
        const EditorCamera& opened = editor->findViewport(2)->camera();
        CHECK(opened.position() == editor->camera().position());
        CHECK(opened.yawDegrees() == editor->camera().yawDegrees());
        CHECK_FALSE(opened.isNavigating());
        // The pointer, moved with the button held, turns the camera that was being steered.
        const float openedYaw = opened.yawDegrees();
        pointerTo(*editor, pictureOf(editor->viewport()).center() + Vec2(30.0f, 0.0f));
        CHECK(editor->findViewport(2)->camera().yawDegrees() == openedYaw);
        CHECK(editor->camera().yawDegrees() != openedYaw);
        run(*editor, "input.inject", R"({"mouseButton": "Right", "down": false})");
        REQUIRE(editor->runFrames(1).has_value());
        CHECK_FALSE(editor->camera().isNavigating());
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("a scene capture is drawn as the game draws it, whoever's camera a viewport looks through") {
        EditorAppDesc desc = headlessDesc(UVec2(320, 180));
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        // Without the sky, the security camera sees the background where nothing is drawn.
        run(*editor, "component.remove", R"({"entity": "Sky", "component": "Environment"})");
        REQUIRE(editor->runFrames(2).has_value());
        // The target the security camera draws into.
        const std::optional<Uuid> target = Uuid::parse("535d4668-3c0e-8e9a-ab90-cf9a08e41bde");
        REQUIRE(target.has_value());
        // A picture taken between frames draws the capture first, at one moment every time.
        const auto captured = [&] {
            REQUIRE(editor->captureViewport(UVec2(160, 90)).has_value());
            nvrhi::ITexture* const texture = editor->engine().sceneRenderer().resources().renderTarget(*target);
            REQUIRE(texture != nullptr);
            TexturePixels pixels = unwrap(readTexture(editor->engine().graphics(), texture));
            return unwrap(Image::fromPixels(pixels.width, pixels.height, ImageFormat::Rgba8, std::move(pixels.data)));
        };
        const Image gameBackground = captured();
        // The background is the game's: another one is another picture...
        editor->engine().renderSettings().backgroundColor = Vec3(0.8f, 0.1f, 0.1f);
        const Image red = captured();
        CHECK(differingPixels(red, gameBackground) > 100);
        // ...and not the viewport's, which is the editor's own through the editor's camera.
        run(*editor, "viewport.show", R"({"camera": "game"})");
        CHECK(differingPixels(captured(), red) == 0);
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("playing shows the game in the first viewport, and the others keep their cameras") {
        EditorAppDesc desc = headlessDesc(UVec2(320, 180));
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        run(*editor, "viewport.open");
        run(*editor, "play.start");
        REQUIRE(editor->runFrames(2).has_value());
        const Json playing = run(*editor, "viewport.list")["viewports"];
        CHECK(playing[0]["camera"] == "game");
        CHECK(playing[0]["showsGame"] == true);
        CHECK(playing[1]["camera"] == "editor");
        CHECK(playing[1]["showsGame"] == false);
        run(*editor, "play.stop");
        REQUIRE(editor->runFrames(1).has_value());
        CHECK(run(*editor, "viewport.list")["viewports"][0]["camera"] == "editor");
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("the viewport used last is the one clicked into, brought to the front or opened last") {
        const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(1280, 720)));
        run(*editor, "entity.create", R"({"name": "Target", "components": {"Transform": {"position": [3, 0, -2]}}})");
        run(*editor, "viewport.open");
        REQUIRE(editor->runFrames(3).has_value());
        CHECK(editor->activeViewport() == 2);

        // Any button of the mouse in its picture: the right one turns its camera, the middle
        // one slides it.
        pointerTo(*editor, pictureOf(editor->viewport()).center());
        click(*editor, "Right");
        CHECK(editor->activeViewport() == 1);
        CHECK(run(*editor, "viewport.list")["active"] == 1);
        pointerTo(*editor, pictureOf(*editor->findViewport(2)).center());
        click(*editor, "Middle");
        CHECK(editor->activeViewport() == 2);
        pointerTo(*editor, pictureOf(editor->viewport()).center());
        click(*editor, "Left");
        CHECK(editor->activeViewport() == 1);

        // Brought to the front by name, at once.
        run(*editor, "editor.focusPanel", R"({"panel": "viewport2"})");
        CHECK(editor->activeViewport() == 2);
        REQUIRE(editor->runFrames(2).has_value());
        CHECK(editor->activeViewport() == 2);
        run(*editor, "editor.focusPanel", R"({"panel": "viewport"})");
        CHECK(editor->activeViewport() == 1);
        REQUIRE(editor->runFrames(2).has_value());
        CHECK(editor->activeViewport() == 1);
        // With the focus on another panel, the one used last is still the one used last.
        run(*editor, "editor.focusPanel", R"({"panel": "hierarchy"})");
        REQUIRE(editor->runFrames(2).has_value());
        CHECK(editor->activeViewport() == 1);

        // F frames what is selected in the one used last, and in no other.
        run(*editor, "selection.set", R"({"entities": ["Target"]})");
        run(*editor, "editor.focusPanel", R"({"panel": "viewport2"})");
        REQUIRE(editor->runFrames(2).has_value());
        const Vec3 firstWas = editor->camera().position();
        const Vec3 secondWas = editor->findViewport(2)->camera().position();
        run(*editor, "input.inject", R"({"key": "F", "down": true})");
        REQUIRE(editor->runFrames(1).has_value());
        run(*editor, "input.inject", R"({"key": "F", "down": false})");
        REQUIRE(editor->runFrames(1).has_value());
        CHECK(editor->camera().position() == firstWas);
        CHECK(editor->findViewport(2)->camera().position() != secondWas);

        // Closing the one used last makes the first the one used last - and the one with the
        // focus, which Dear ImGui would otherwise give to whatever is in front.
        run(*editor, "viewport.open");
        CHECK(editor->activeViewport() == 3);
        REQUIRE(editor->runFrames(3).has_value());
        CHECK(run(*editor, "viewport.close", R"({"viewport": 3})")["active"] == 1);
        for (int frame = 0; frame < 4; ++frame) {
            REQUIRE(editor->runFrames(1).has_value());
            CHECK(editor->activeViewport() == 1);
        }
        CHECK(windowOf(editor->viewport()) == ImGui::GetCurrentContext()->NavWindow);

        // A viewport that is not open is no panel.
        const Error closed = refusal(*editor, "editor.focusPanel", R"({"panel": "viewport3"})");
        CHECK(closed.code == ErrorCode::InvalidArgument);
        CHECK(closed.message.find("\"viewport2\"") != std::string::npos);
        CHECK(closed.message.find("\"viewport3\"") == std::string::npos);
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("the close button of a viewport's tab closes it, and a drag of the gizmo in it is a step of the "
              "history") {
        const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(1280, 720)));
        run(*editor, "entity.create", R"({"name": "Box", "components": {"Transform": {"position": [0, 0, 0]}}})");
        run(*editor, "viewport.open");
        run(*editor, "viewport.lookAt", R"({"position": [0, 0, 10], "target": [0, 0, 0], "viewport": 2})");
        run(*editor, "selection.set", R"({"entities": ["Box"]})");
        REQUIRE(editor->runFrames(3).has_value());
        const World& world = editor->context().world();
        const Entity box = world.findByName("Box");

        // A handle of the gizmo in the second viewport, taken hold of and moved.
        const ViewportPanel& second = *editor->findViewport(2);
        const GizmoFrame frame = gizmoFrame(world.worldMatrix(box), GizmoSpace::World, GizmoMode::Translate,
                                            second.view(), Vec2(second.targets().size()));
        const Vec2 grab =
            second.imageOrigin() +
            worldToScreen(second.view(), frame.origin + frame.x * frame.size * 0.7f, Vec2(second.targets().size()))
                .value();
        pointerTo(*editor, grab);
        run(*editor, "input.inject", R"({"mouseButton": "Left", "down": true})");
        REQUIRE(editor->runFrames(2).has_value());
        pointerTo(*editor, grab + Vec2(60.0f, 0.0f));
        REQUIRE(editor->findViewport(2)->isDraggingGizmo());
        // While it is dragged, no key changes what the gizmo does.
        pressShortcut(*editor, ImGuiKey_E);
        CHECK(editor->viewportSettings().gizmoMode == GizmoMode::Translate);

        // Closed in the middle of the drag, the drag ends where it is, as one step.
        const float movedTo = world.transform(box).position.x;
        CHECK(movedTo > 0.1f);
        run(*editor, "viewport.close", R"({"viewport": 2})");
        CHECK(editor->context().history().undoLabel() == "Edit Transform of 'Box'");
        run(*editor, "input.inject", R"({"mouseButton": "Left", "down": false})");
        REQUIRE(editor->runFrames(2).has_value());
        CHECK(world.transform(box).position.x == movedTo);
        run(*editor, "editor.undo");
        CHECK(world.transform(box).position == Vec3(0.0f));

        // The button on the tab of one that is open closes it, by the command.
        run(*editor, "viewport.open");
        REQUIRE(editor->runFrames(3).has_value());
        const ImRect tab = windowOf(*editor->findViewport(2))->DC.DockTabItemRect;
        REQUIRE(tab.GetWidth() > 0.0f);
        const float closeButton = tab.Max.x - ImGui::GetStyle().FramePadding.x - 0.5f * ImGui::GetFontSize();
        pointerTo(*editor, Vec2(closeButton, tab.GetCenter().y));
        click(*editor, "Left");
        CHECK(editor->findViewport(2) == nullptr);
        CHECK(openViewports(*editor) == std::vector<uint32_t>{1});
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("the keys of the gizmo choose for every viewport, and wait while any of them steers its camera") {
        const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(1280, 720)));
        run(*editor, "viewport.open");
        REQUIRE(editor->runFrames(3).has_value());
        pressShortcut(*editor, ImGuiKey_E);
        CHECK(editor->viewportSettings().gizmoMode == GizmoMode::Rotate);
        pressShortcut(*editor, ImGuiKey_R);
        CHECK(editor->viewportSettings().gizmoMode == GizmoMode::Scale);
        pressShortcut(*editor, ImGuiKey_X);
        CHECK(editor->viewportSettings().gizmoSpace == GizmoSpace::Local);
        pressShortcut(*editor, ImGuiKey_X);
        CHECK(editor->viewportSettings().gizmoSpace == GizmoSpace::World);
        pressShortcut(*editor, ImGuiKey_W);
        CHECK(editor->viewportSettings().gizmoMode == GizmoMode::Translate);

        // The right button held in the second viewport: the same keys fly its camera.
        pointerTo(*editor, pictureOf(*editor->findViewport(2)).center());
        run(*editor, "input.inject", R"({"mouseButton": "Right", "down": true})");
        REQUIRE(editor->runFrames(1).has_value());
        REQUIRE(editor->findViewport(2)->camera().isNavigating());
        pressShortcut(*editor, ImGuiKey_E);
        CHECK(editor->viewportSettings().gizmoMode == GizmoMode::Translate);
        // Let go of, the next key is the gizmo's again - also the very first, pressed in the
        // frame right after.
        run(*editor, "input.inject", R"({"mouseButton": "Right", "down": false})");
        REQUIRE(editor->runFrames(1).has_value());
        pressShortcut(*editor, ImGuiKey_E);
        CHECK(editor->viewportSettings().gizmoMode == GizmoMode::Rotate);
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("a viewport that is closed while its camera is steered gives the pointer back") {
        // A window of its own, hidden: the pointer is locked to a window.
        EditorAppDesc desc = headlessDesc(UVec2(640, 360));
        desc.engine.headless = false;
        desc.engine.window.width = 640;
        desc.engine.window.height = 360;
        desc.engine.window.visible = false;
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        Window* const window = editor->engine().window();
        REQUIRE(window != nullptr);
        run(*editor, "viewport.open");
        REQUIRE(editor->runFrames(3).has_value());

        // With a window the interface follows the pointer of the system, which no test can
        // move: the right button is pressed as over the second viewport, and from then on
        // the viewport steers its camera, and locks the pointer, by itself.
        run(*editor, "input.inject", R"({"mouseButton": "Right", "down": true})");
        editor->findViewport(2)->camera().update(editor->engine().input(), 0.0f, true);
        REQUIRE(editor->runFrames(2).has_value());
        REQUIRE(editor->findViewport(2)->camera().isNavigating());
        CHECK(window->cursorMode() == CursorMode::Captured);
        run(*editor, "viewport.close", R"({"viewport": 2})");
        CHECK(window->cursorMode() == CursorMode::Normal);
        run(*editor, "input.inject", R"({"mouseButton": "Right", "down": false})");
        REQUIRE(editor->runFrames(2).has_value());
        CHECK(window->cursorMode() == CursorMode::Normal);
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE(
        "what can be placed lands where it is dropped onto a viewport, and a material on the mesh it is dropped on") {
        const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(1280, 720)));
        const World& world = editor->context().world();
        // Down at the origin, from ten metres up and five back: the middle of the picture.
        run(*editor, "viewport.lookAt", R"({"position": [0, 10, 5], "target": [0, 0, 0]})");
        run(*editor, "editor.focusPanel", R"({"panel": "assets"})");
        // The cube that the engine has built in, alone among the contents.
        run(*editor, "assets.browse", R"({"folder": "builtin", "search": "cube", "view": "grid"})");
        REQUIRE(editor->runFrames(3).has_value());
        const Vec2 tile = regionOf("###Assets/contents_") + Vec2(20.0f);
        const Vec2 middle = pictureOf(editor->viewport()).center();
        const auto materialsOf = [&world](Entity entity) {
            const auto* renderer = world.tryGet<MeshRendererComponent>(entity);
            REQUIRE(renderer != nullptr);
            return renderer->materials;
        };

        // Where nothing is in the way, on the grid.
        dragPointer(*editor, tile, middle);
        REQUIRE(world.roots().size() == 1);
        const Entity first = world.roots()[0];
        CHECK(world.name(first) == "Cube");
        CHECK(world.transform(first).position.y == 0.0f);
        CHECK(glm::length(world.transform(first).position) < 0.02f);

        // Where the first is in the way, on it: on its top, a quarter of a metre towards the
        // camera from its middle.
        dragPointer(*editor, tile, middle);
        REQUIRE(world.roots().size() == 2);
        const Entity second = world.roots()[1];
        CHECK_NEAR(world.transform(second).position, Vec3(0.0f, 0.5f, 0.25f), 0.02f);

        // A material, onto the mesh it is dropped on - the second cube, which is in front now.
        run(*editor, "assets.browse", R"({"search": "default"})");
        REQUIRE(editor->runFrames(3).has_value());
        dragPointer(*editor, tile, middle);
        CHECK(materialsOf(second) == std::vector<Uuid>{defaultMaterialId()});
        CHECK(materialsOf(first).empty());
        CHECK(world.roots().size() == 2);

        // Undone, the material is gone again; the cubes are steps of their own.
        run(*editor, "editor.undo");
        CHECK(materialsOf(second).empty());
        checkNoValidationIssues(editor->engine().graphics());
    }
}
