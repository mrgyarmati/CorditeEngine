#include "cordite/editor/EditorApp.h"

#include "cordite/animation/AnimationComponents.h"
#include "cordite/assets/AssetDatabase.h"
#include "cordite/assets/GltfExporter.h"
#include "cordite/assets/ImageCompare.h"
#include "cordite/assets/Primitives.h"
#include "cordite/audio/AudioSystem.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Log.h"
#include "cordite/core/Platform.h"
#include "cordite/core/Uuid.h"
#include "cordite/editor/AssetBrowser.h"
#include "cordite/editor/AssetPreview.h"
#include "cordite/editor/EditorPreferences.h"
#include "cordite/editor/Gizmo.h"
#include "cordite/editor/NewProject.h"
#include "cordite/engine/Project.h"
#include "cordite/platform/InputMap.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/RenderDebugView.h"
#include "cordite/rhi/TextureReadback.h"
#include "cordite/shaders/EngineShaders.h"
#include "cordite/world/SceneSerializer.h"

#include "render/RenderTestSupport.h"
#include "support/MathTestSupport.h"

#include <imgui_internal.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

using namespace cordite;
using cordite::test::checkAgainstReference;
using cordite::test::checkNoValidationIssues;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

std::filesystem::path showcaseProject() {
    return test::sourceDirectory() / "samples" / "showcase";
}

EditorAppDesc headlessDesc(UVec2 size = UVec2(1280, 720)) {
    EditorAppDesc desc;
    desc.engine.headless = true;
    desc.engine.validation = true;
    desc.engine.renderSize = size;
    desc.engine.fixedTimeStep = 1.0 / 60.0;
    // Tests must not depend on where the engine would look for shaders by default.
    desc.engine.shaderDirectory = test::shaderDirectory();
    return desc;
}

std::unique_ptr<EditorApp> createEditor(const EditorAppDesc& desc) {
    Result<std::unique_ptr<EditorApp>> editor = EditorApp::create(desc);
    REQUIRE_MESSAGE(editor.has_value(), test::errorOf(editor));
    return std::move(*editor);
}

Json json(std::string_view text) {
    return unwrap(parseJson(text));
}

/// Runs a command that is expected to succeed.
Json run(EditorApp& editor, std::string_view command, std::string_view parameters = "{}") {
    const Result<Json> result = editor.commands().execute(command, json(parameters));
    REQUIRE_MESSAGE(result.has_value(), test::errorOf(result));
    return *result;
}

/// Runs a command that is expected to fail, and returns why it did.
Error refusal(EditorApp& editor, std::string_view command, std::string_view parameters = "{}") {
    const Result<Json> result = editor.commands().execute(command, json(parameters));
    REQUIRE_FALSE(result.has_value());
    return result.error();
}

/// An editor for a picture of its whole window. The console is one of its panels, and
/// shows what was logged - by this test or by any that ran before it in the same
/// process: the log is emptied first, so that the picture does not depend on which
/// tests those were.
std::unique_ptr<EditorApp> createEditorForPicture(const EditorAppDesc& desc) {
    Log::clearHistory();
    return createEditor(desc);
}

/// The rows of a picture from `first` up to `last`, as a picture of their own.
Image rowsOf(const Image& picture, uint32_t first, uint32_t last) {
    const std::span<const std::byte> pixels = picture.pixels();
    const auto rowBytes = static_cast<std::ptrdiff_t>(picture.rowBytes());
    std::vector<std::byte> rows(pixels.begin() + first * rowBytes, pixels.begin() + last * rowBytes);
    return unwrap(Image::fromPixels(picture.width(), last - first, picture.format(), std::move(rows)));
}

/// A JSON string literal holding a path, quoted and escaped.
std::string quoted(const std::filesystem::path& path) {
    return Json(pathToUtf8(path)).dump();
}

} // namespace

TEST_SUITE("EditorApp") {
    TEST_CASE("the editor showing the showcase scene matches the reference image") {
        EditorAppDesc desc = headlessDesc();
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditorForPicture(desc);
        run(*editor, "selection.set", R"({"entities": ["Cameras/Main camera"]})");

        // Windows settle into their docked places over the first frames.
        REQUIRE(editor->runFrames(4).has_value());
        CHECK(editor->engine().frameIndex() == 4);

        checkAgainstReference(unwrap(editor->engine().captureFrame()), "editor-showcase-1280x720");
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("the editor showing a selected light matches the reference image") {
        EditorAppDesc desc = headlessDesc();
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditorForPicture(desc);
        // The inspector shows the light: its kind as a list, its color, numbers with their
        // units, and its lighting channels as a row of switches. The viewport shows how far
        // it reaches.
        run(*editor, "selection.set", R"({"entities": ["Stage/Lamp post/Arm/Head/Lamp"]})");
        run(*editor, "component.set",
            R"({"entity": "Stage/Lamp post/Arm/Head/Lamp", "component": "Light", "properties": {"channels": 37}})");

        REQUIRE(editor->runFrames(4).has_value());
        checkAgainstReference(unwrap(editor->engine().captureFrame()), "editor-light-1280x720");
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("the editor showing the assets of the showcase matches the reference image") {
        EditorAppDesc desc = headlessDesc();
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditorForPicture(desc);

        // The panel shares its place with the console, and is in front once it is asked
        // for. What it lists is set by command: a mesh inside a model is chosen - which
        // opens the model, and is far down the list, where the list scrolls to.
        CHECK(run(*editor, "editor.focusPanel", R"({"panel": "assets"})")["panel"] == "assets");
        const Json listed = run(*editor, "assets.browse", R"({"select": "models/hoist.gltf#mesh/1"})");
        CHECK(listed["selected"]["name"] == "Cable");
        REQUIRE(editor->runFrames(4).has_value());
        checkAgainstReference(unwrap(editor->engine().captureFrame()), "editor-assets-1280x720");

        // What the panel asks for when its "Place" is clicked is a line the console runs:
        // the hoist, under nothing since nothing is selected - and one step to take back.
        const size_t before = editor->context().world().entityCount();
        const Uuid hoist =
            *Uuid::parse(run(*editor, "asset.get", R"({"asset": "models/hoist.gltf"})")["id"].get<std::string>());
        editor->runCommandLine(placeAssetCommand(editor->context(), hoist));
        CHECK(editor->context().world().entityCount() > before);
        CHECK(editor->context().world().name(editor->context().primarySelection()) == "hoist");
        run(*editor, "editor.undo");
        CHECK(editor->context().world().entityCount() == before);
        REQUIRE(editor->runFrames(2).has_value());
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("the assets are in front of the console until something fails, which the status bar says too") {
        // An editor without a project: nothing in its picture changes from frame to frame
        // but what is asked for here.
        std::unique_ptr<EditorApp> editor = createEditorForPicture(headlessDesc(UVec2(640, 360)));
        // The panels, and the status bar under them, which says what was logged last.
        const auto statusBarHeight = [] {
            const ImGuiWindow* bar = ImGui::FindWindowByName("##StatusBar");
            REQUIRE(bar != nullptr);
            return static_cast<uint32_t>(bar->Size.y);
        };
        const auto capture = [&editor](uint32_t frames = 3) {
            REQUIRE(editor->runFrames(frames).has_value());
            return unwrap(editor->engine().captureFrame());
        };
        const auto panels = [&statusBarHeight](const Image& picture) {
            return rowsOf(picture, 0, picture.height() - statusBarHeight());
        };
        const auto statusBar = [&statusBarHeight](const Image& picture) {
            return rowsOf(picture, picture.height() - statusBarHeight(), picture.height());
        };
        const auto differ = [](const Image& first, const Image& second) {
            return unwrap(compareImages(first, second)).differingPixels;
        };

        const Image first = capture(4);
        const Image withAssets = panels(first);
        const Image quietBar = statusBar(first);
        CHECK(differ(withAssets, panels(capture())) == 0);

        // The console, once a frame has been drawn.
        CHECK(run(*editor, "editor.focusPanel", R"({"panel": "console"})")["panel"] == "console");
        const Image withConsole = panels(capture());
        const uint64_t between = differ(withAssets, withConsole);
        CHECK(between > 1000);

        // What the assets list shows in them: with a filter that leaves nothing, less.
        run(*editor, "editor.focusPanel", R"({"panel": "assets"})");
        CHECK(differ(withAssets, panels(capture())) == 0);
        run(*editor, "assets.browse", R"({"type": "scene"})");
        CHECK(differ(withAssets, panels(capture())) > 200);
        run(*editor, "assets.browse", R"({"type": "all"})");
        CHECK(differ(withAssets, panels(capture())) == 0);

        // What could not be done is said in the console, which comes to the front for it -
        // the picture is the one with the console but for the line it has now - and in the
        // status bar.
        editor->runCommandLine(R"(asset.instantiate {"asset": "textures/none.png"})");
        const Image failed = capture();
        const uint64_t fromConsole = differ(withConsole, panels(failed));
        CHECK(fromConsole > 0);
        CHECK(fromConsole < between);
        CHECK(differ(withAssets, panels(failed)) > between / 2);
        const Image failureBar = statusBar(failed);
        CHECK(differ(quietBar, failureBar) > 100);

        // What a command did, and what the game that runs says, does not bring the console
        // forward; the status bar says the newest of it.
        run(*editor, "editor.focusPanel", R"({"panel": "assets"})");
        static_cast<void>(capture());
        editor->runCommandLine("selection.get");
        CORDITE_LOG_ERROR("Script", "a script of the game has stopped");
        const Image scriptError = capture();
        CHECK(differ(withAssets, panels(scriptError)) == 0);
        CHECK(differ(failureBar, statusBar(scriptError)) > 100);

        // The entities of the scene, and those that are selected, are counted at the right
        // end of the bar.
        run(*editor, "entity.create", R"({"name": "Marker"})");
        run(*editor, "selection.set", R"({"entities": []})");
        const Image oneEntity = statusBar(capture());
        CHECK(differ(statusBar(scriptError), oneEntity) > 20);
        run(*editor, "selection.set", R"({"entities": ["Marker"]})");
        CHECK(differ(oneEntity, statusBar(capture())) > 50);

        // Every panel that is always there has a name; what is none is told which there are.
        for (const char* panel : {"hierarchy", "inspector", "viewport", "console", "assets"}) {
            CAPTURE(std::string(panel));
            CHECK(run(*editor, "editor.focusPanel", fmt::format(R"({{"panel": "{}"}})", panel))["panel"] == panel);
        }
        REQUIRE(editor->runFrames(2).has_value());
        const Result<Json> unknown =
            editor->commands().execute("editor.focusPanel", json(R"({"panel": "statistics"})"));
        REQUIRE_FALSE(unknown.has_value());
        CHECK(unknown.error().code == ErrorCode::InvalidArgument);
        CHECK(unknown.error().message.find("\"assets\"") != std::string::npos);
        checkNoValidationIssues(editor->engine().graphics());

        // A panel that is asked for before the first frame is in front from the first:
        // another editor, asked at once, shows what this one did with its console in front.
        editor.reset();
        editor = createEditorForPicture(headlessDesc(UVec2(640, 360)));
        run(*editor, "editor.focusPanel", R"({"panel": "console"})");
        CHECK(differ(withConsole, panels(capture(4))) == 0);
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("the viewport alone matches its reference image") {
        EditorAppDesc desc = headlessDesc();
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        // A mesh and a light are selected: the box of the one and the reach of the other
        // are drawn over the picture.
        run(*editor, "selection.set", R"({"entities": ["Stage/Crate", "Stage/Lamp post/Arm/Head/Lamp"]})");
        run(*editor, "viewport.lookAt", R"({"position": [-5.5, 3.2, 6.5], "target": [0.5, 0.8, -1.5]})");

        // No frame of the editor is needed for it.
        const Image image = unwrap(editor->captureViewport(UVec2(640, 360)));
        CHECK(image.width() == 640);
        CHECK(image.height() == 360);
        CHECK(editor->engine().frameIndex() == 0);
        checkAgainstReference(image, "editor-viewport-640x360");

        // Capturing again gives the same picture, at whatever size is asked for.
        const Image again = unwrap(editor->captureViewport(UVec2(640, 360)));
        CHECK(unwrap(compareImages(image, again, {.channelTolerance = 0, .maxDifferingRatio = 0.0})).matches);
        CHECK(unwrap(editor->captureViewport(UVec2(200, 300))).height() == 300);
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("the editor starts with an empty scene and reports a scene it cannot open") {
        {
            const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(640, 360)));
            CHECK(editor->context().world().entityCount() == 0);
            CHECK(editor->context().scenePath().empty());
            REQUIRE(editor->runFrames(2).has_value());
            checkNoValidationIssues(editor->engine().graphics());
        }

        EditorAppDesc missing = headlessDesc(UVec2(640, 360));
        missing.scene = test::sourceDirectory() / "samples" / "no-such.scene.json";
        const Result<std::unique_ptr<EditorApp>> failed = EditorApp::create(missing);
        REQUIRE_FALSE(failed.has_value());
        CHECK(failed.error().message.find("no-such.scene.json") != std::string::npos);

        // The failed start released the engine: another editor can be created.
        const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(640, 360)));
        REQUIRE(editor->runFrames(1).has_value());
    }

    TEST_CASE("a script builds a scene, saves it and takes pictures of it") {
        const TempDirectory directory;
        const std::filesystem::path scenePath = directory.path() / "built.scene.json";
        const std::filesystem::path editorImage = directory.path() / "shots" / "editor.png";
        const std::filesystem::path viewportImage = directory.path() / "shots" / "viewport.png";
        const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(960, 540)));

        const Json script = json(fmt::format(R"([
            {{"command": "entity.create", "params": {{"name": "Rig"}}}},
            {{"command": "entity.create", "params": {{"name": "Eye", "parent": "Rig",
                "components": {{"Transform": {{"position": [0, 1.7, 4]}}, "Camera": {{}}}}}}}},
            {{"command": "component.set", "params": {{"entity": "Rig/Eye", "component": "Camera",
                "properties": {{"verticalFovDegrees": 42}}}}}},
            {{"command": "viewport.frame", "params": {{"entity": "Eye", "radius": 3}}}},
            {{"command": "editor.runFrames", "params": {{"count": 2}}}},
            {{"command": "editor.screenshot", "params": {{"path": {}}}}},
            {{"command": "editor.screenshot", "params": {{"path": {}, "viewportOnly": true, "width": 320, "height": 200}}}},
            {{"command": "scene.save", "params": {{"path": {}}}}},
            {{"command": "editor.state"}}
        ])",
                                             quoted(editorImage), quoted(viewportImage), quoted(scenePath)));

        const Json outcome = editor->runScript(script);
        CAPTURE(dumpJson(outcome));
        REQUIRE(outcome["ok"] == true);
        REQUIRE(outcome["results"].size() == 9);
        for (const Json& result : outcome["results"]) {
            CHECK(result["ok"] == true);
        }
        CHECK(outcome["results"][0]["command"] == "entity.create");
        CHECK(outcome["results"][1]["result"]["path"] == "Rig/Eye");
        CHECK(outcome["results"][4]["result"]["frameIndex"] == 2);
        CHECK(outcome["results"][5]["result"]["width"] == 960);
        CHECK(outcome["results"][6]["result"]["height"] == 200);
        CHECK(outcome["results"][8]["result"]["scene"]["unsavedChanges"] == false);

        // The pictures exist and have the sizes that were reported.
        const Image whole = unwrap(Image::load(editorImage));
        CHECK(whole.width() == 960);
        CHECK(whole.height() == 540);
        const Image sceneOnly = unwrap(Image::load(viewportImage));
        CHECK(sceneOnly.width() == 320);
        CHECK(sceneOnly.height() == 200);

        // The saved scene holds what the script made.
        const Json saved = unwrap(readJsonFile(scenePath));
        REQUIRE(saved["entities"].size() == 2);
        CHECK(saved["entities"][1]["name"] == "Eye");
        CHECK(saved["entities"][1]["components"]["Camera"]["verticalFovDegrees"] == 42.0);
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("a failing step ends the script and says why") {
        const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(640, 360)));

        const Json outcome = editor->runScript(json(R"([
            {"command": "entity.create", "params": {"name": "First"}},
            {"command": "entity.rename", "params": {"entity": "Missing", "name": "Second"}},
            {"command": "entity.create", "params": {"name": "Third"}}
        ])"));

        CHECK(outcome["ok"] == false);
        REQUIRE(outcome["results"].size() == 2);
        CHECK(outcome["results"][0]["ok"] == true);
        const Json& failed = outcome["results"][1];
        CHECK(failed["command"] == "entity.rename");
        CHECK(failed["ok"] == false);
        CHECK(failed["error"]["code"] == "NotFound");
        CHECK(failed["error"]["message"] == "entity.rename: no entity is named 'Missing'");
        CHECK_FALSE(failed.contains("result"));

        // What ran before the failure stays; what came after it never ran.
        CHECK_FALSE(editor->context().world().findByName("First").isNull());
        CHECK(editor->context().world().findByName("Third").isNull());
    }

    TEST_CASE("malformed scripts are rejected step by step") {
        const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(640, 360)));

        const Json notArray = editor->runScript(json(R"({"command": "scene.new"})"));
        CHECK(notArray["ok"] == false);
        CHECK(notArray["results"] == Json::array());
        CHECK(notArray["error"]["message"] == "a script must be an array of steps, found object");

        struct Case {
            const char* step;
            const char* fragment;
        };

        for (const Case& entry : {
                 Case{R"("scene.new")", "a script step must be an object, found string"},
                 Case{R"({"params": {}})", "needs a 'command'"},
                 Case{R"({"command": 5})", "needs a 'command'"},
                 Case{R"({"command": "scene.new", "parameters": {}})", "unknown member 'parameters'"},
                 Case{R"({"command": "scene.new", "params": []})", "parameters must be a JSON object"},
                 Case{R"({"command": "scene.destroy"})", "there is no command named 'scene.destroy'"},
             }) {
            CAPTURE(entry.step);
            const Json outcome = editor->runScript(json(fmt::format("[{}]", entry.step)));
            CHECK(outcome["ok"] == false);
            REQUIRE(outcome["results"].size() == 1);
            CHECK(outcome["results"][0]["error"]["message"].get<std::string>().find(entry.fragment) !=
                  std::string::npos);
        }

        // An empty script is fine, and so is leaving out the parameters.
        CHECK(editor->runScript(Json::array())["ok"] == true);
        CHECK(editor->runScript(
                  json(R"([{"command": "scene.new"}, {"command": "scene.new", "params": null}])"))["ok"] == true);
    }

    TEST_CASE("lines entered in the console run commands and report their outcome") {
        const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(640, 360)));
        World& world = editor->context().world();

        {
            test::LogCapture capture(LogLevel::Info);
            editor->runCommandLine(R"(entity.create {"name": "From console"})");
            editor->runCommandLine("  selection.get  ");
            CHECK_FALSE(world.findByName("From console").isNull());
            CHECK(capture.contains("entity.create:"));
            CHECK(capture.contains("From console"));
            CHECK(capture.count(LogLevel::Error) == 0);
        }
        {
            test::LogCapture capture(LogLevel::Error);
            editor->runCommandLine(R"(entity.create {"name": )");
            editor->runCommandLine("entity.frobnicate");
            editor->runCommandLine(R"(entity.rename {"entity": "Nobody", "name": "x"})");
            CHECK(capture.count(LogLevel::Error) == 3);
            CHECK(capture.contains("the parameters are not valid JSON"));
            CHECK(capture.contains("there is no command named 'entity.frobnicate'"));
            CHECK(capture.contains("no entity is named 'Nobody'"));
        }
        // An empty line does nothing.
        editor->runCommandLine("   ");
        CHECK(world.entityCount() == 1);
    }

    TEST_CASE("the viewport camera is steered by commands") {
        const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(640, 360)));

        run(*editor, "viewport.lookAt", R"({"position": [0, 2, 10], "target": [0, 2, 0]})");
        Json camera = run(*editor, "viewport.camera");
        CHECK(camera["position"] == json("[0.0, 2.0, 10.0]"));
        CHECK(camera["forward"][2].get<float>() == doctest::Approx(-1.0f));
        CHECK(camera["yawDegrees"].get<float>() == doctest::Approx(0.0f));
        CHECK(camera["pitchDegrees"].get<float>() == doctest::Approx(0.0f));

        run(*editor, "entity.create", R"({"name": "Target", "components": {"Transform": {"position": [0, 2, -40]}}})");
        run(*editor, "viewport.frame", R"({"entity": "Target", "radius": 2})");
        camera = run(*editor, "viewport.camera");
        // Still looking the same way, now from close to the entity.
        CHECK(camera["forward"][2].get<float>() == doctest::Approx(-1.0f));
        CHECK_NEAR(editor->camera().position(), Vec3(0.0f, 2.0f, -35.2f), 1e-3f);

        for (const char* parameters : {R"({"position": [0, 0], "target": [0, 0, 0]})",
                                       R"({"position": "here", "target": [0, 0, 0]})", R"({"position": [0, 0, 0]})"}) {
            CAPTURE(parameters);
            CHECK_FALSE(editor->commands().execute("viewport.lookAt", json(parameters)).has_value());
        }
        CHECK_FALSE(
            editor->commands().execute("viewport.frame", json(R"({"entity": "Target", "radius": 0})")).has_value());
        CHECK_FALSE(editor->commands().execute("viewport.frame", json(R"({"entity": "Nobody"})")).has_value());
    }

    TEST_CASE("markers in the viewport can be picked") {
        const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(640, 360)));
        run(*editor, "entity.create", R"({"name": "Near", "components": {"Transform": {"position": [0, 0, 0]}}})");
        run(*editor, "entity.create", R"({"name": "Far", "components": {"Transform": {"position": [0, 0, -20]}}})");
        run(*editor, "entity.create", R"({"name": "Aside", "components": {"Transform": {"position": [4, 0, 0]}}})");
        run(*editor, "entity.create", R"({"name": "Off", "components": {"Transform": {"position": [-4, 0, 0]}}})");
        run(*editor, "entity.setEnabled", R"({"entity": "Off", "enabled": false})");
        // Level with the entities and straight in front of two of them.
        run(*editor, "viewport.lookAt", R"({"position": [0, 0, 10], "target": [0, 0, 0]})");

        const Vec2 size(640.0f, 360.0f);
        const Image image = unwrap(editor->captureViewport(UVec2(size)));
        const World& world = editor->context().world();
        const ViewportPanel& viewport = editor->viewport();
        const CameraView view = editor->camera().view(size.x / size.y);
        const auto screenPosition = [&](const char* name) {
            const Entity entity = world.findByName(name);
            return worldToScreen(view, Vec3(world.worldMatrix(entity)[3]), size).value();
        };

        // Near and Far are behind one another: the one in front is picked.
        CHECK_NEAR(screenPosition("Near"), Vec2(320.0f, 180.0f), 0.01f);
        CHECK(world.name(viewport.pick(editor->context(), Vec2(320.0f, 180.0f))) == "Near");
        CHECK(world.name(viewport.pick(editor->context(), screenPosition("Aside") + Vec2(5.0f, -5.0f))) == "Aside");
        // Too far from any marker, and a disabled entity has none.
        CHECK(viewport.pick(editor->context(), Vec2(40.0f, 40.0f)).isNull());
        CHECK(viewport.pick(editor->context(), screenPosition("Off")).isNull());

        // The marker that was picked is really there: its pixel is not background.
        const Vec2 aside = screenPosition("Aside");
        const Vec4 background = image.pixel(5, 5);
        float brightest = 0.0f;
        for (int dy = -3; dy <= 3; ++dy) {
            for (int dx = -3; dx <= 3; ++dx) {
                const Vec4 pixel = image.pixel(static_cast<uint32_t>(static_cast<int>(aside.x) + dx),
                                               static_cast<uint32_t>(static_cast<int>(aside.y) + dy));
                brightest = std::max(brightest, std::max(pixel.r, std::max(pixel.g, pixel.b)));
            }
        }
        CHECK(brightest > std::max(background.r, std::max(background.g, background.b)) + 0.2f);
    }

    TEST_CASE("the editor opens a project: its assets, and its start scene") {
        EditorAppDesc desc = headlessDesc(UVec2(640, 360));
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);

        REQUIRE(editor->project().has_value());
        CHECK(editor->project()->settings.name == "Showcase");
        CHECK(editor->context().scenePath() == showcaseProject() / "assets" / "scenes" / "showcase.scene.json");
        CHECK(editor->context().world().sceneName() == "Showcase");
        CHECK(run(*editor, "asset.list", R"({"type": "model"})")["assets"].size() == 4);
        CHECK(run(*editor, "scene.validate")["valid"] == true);

        // The engine's assets know what its scenes use: a bollard placed from the prefab,
        // and nothing that is not there.
        const Json bollard = run(*editor, "asset.get", R"({"asset": "prefabs/bollard.prefab.json"})");
        bool usedByScene = false;
        for (const Json& user : bollard["usedBy"]) {
            usedByScene = usedByScene || user["path"] == "scenes/showcase.scene.json";
        }
        CHECK(usedByScene);
        CHECK_FALSE(bollard["usedInScene"].empty());
        const Json scene = run(*editor, "asset.get", R"({"asset": "scenes/showcase.scene.json"})");
        CHECK(scene["dependencies"].size() > 10);
        for (const Json& dependency : scene["dependencies"]) {
            CHECK_FALSE(dependency.contains("missing"));
        }
    }

    TEST_CASE("a project that is not one is reported, and a scene of choice replaces the start scene") {
        {
            EditorAppDesc missing = headlessDesc(UVec2(640, 360));
            missing.project = test::sourceDirectory() / "samples";
            const Result<std::unique_ptr<EditorApp>> editor = EditorApp::create(missing);
            REQUIRE_FALSE(editor.has_value());
            CHECK(editor.error().code == ErrorCode::NotFound);
            CHECK(editor.error().message.find("project.cordite") != std::string::npos);
        }

        const TempDirectory directory;
        const std::filesystem::path other = directory.path() / "other.scene.json";
        REQUIRE(writeTextFile(other, R"({"format": "cordite.scene", "version": 1, "name": "Other", "entities": []})")
                    .has_value());
        EditorAppDesc desc = headlessDesc(UVec2(640, 360));
        desc.project = showcaseProject();
        desc.scene = other;
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        CHECK(editor->context().world().sceneName() == "Other");
        // The project's assets are there all the same.
        CHECK(run(*editor, "asset.get", R"({"asset": "models/crate.gltf#mesh/0"})")["name"] == "Crate");
    }

    TEST_CASE("a layout remembered before there was an Assets panel gets the panel where the console is") {
        const TempDirectory directory;
        const Project project = unwrap(createProject(directory.path() / "game", "Game"));
        const std::filesystem::path layout = project.cacheDirectory() / "editor-layout.ini";
        // The editor with its assets in front: as it comes, or with a layout it remembers.
        const auto pictureWithAssets = [&project](bool rememberLayout) {
            EditorAppDesc desc = headlessDesc(UVec2(640, 360));
            desc.project = project.directory;
            desc.rememberLayout = rememberLayout;
            const std::unique_ptr<EditorApp> editor = createEditorForPicture(desc);
            REQUIRE(editor->runFrames(3).has_value());
            run(*editor, "editor.focusPanel", R"({"panel": "assets"})");
            REQUIRE(editor->runFrames(3).has_value());
            Image picture = unwrap(editor->engine().captureFrame());
            checkNoValidationIssues(editor->engine().graphics());
            return picture;
        };
        const Image fresh = pictureWithAssets(false);

        // A layout is remembered - and made one of an editor that had no Assets panel: the
        // file says nothing of such a window.
        static_cast<void>(pictureWithAssets(true));
        std::string remembered = unwrap(readTextFile(layout));
        const size_t entry = remembered.find("[Window][Assets]");
        REQUIRE(entry != std::string::npos);
        const size_t next = remembered.find("\n[", entry);
        REQUIRE(next != std::string::npos);
        remembered.erase(entry, next + 1 - entry);
        REQUIRE(remembered.find("Assets") == std::string::npos);
        REQUIRE(remembered.find("[Window][Console]") != std::string::npos);
        REQUIRE(writeTextFile(layout, remembered).has_value());

        // The panel is where it is in a fresh layout: with the console, and not afloat
        // over the viewport, where a window that nothing places would be.
        CHECK(unwrap(compareImages(fresh, pictureWithAssets(true))).differingPixels == 0);
        // And from then on the layout remembers it.
        CHECK(unwrap(readTextFile(layout)).find("[Window][Assets]") != std::string::npos);
    }

    TEST_CASE("an editor asked to remember its layout keeps it in the project") {
        const TempDirectory directory;
        const Project project = unwrap(createProject(directory.path() / "game", "Game"));
        const std::filesystem::path layout = project.cacheDirectory() / "editor-layout.ini";

        const auto forget = [&project] {
            std::error_code error;
            std::filesystem::remove_all(project.cacheDirectory(), error);
            REQUIRE_FALSE(error);
        };

        // The project may be named by its directory or by its file.
        for (const std::filesystem::path& named : {project.directory, project.file()}) {
            forget();
            {
                EditorAppDesc desc = headlessDesc(UVec2(640, 360));
                desc.project = named;
                desc.rememberLayout = true;
                const std::unique_ptr<EditorApp> editor = createEditor(desc);
                REQUIRE(editor->runFrames(2).has_value());
            }
            // Written when the editor closes.
            CHECK(fileExists(layout));
        }

        // Not asked, it leaves nothing behind.
        forget();
        {
            EditorAppDesc desc = headlessDesc(UVec2(640, 360));
            desc.project = project.directory;
            const std::unique_ptr<EditorApp> editor = createEditor(desc);
            REQUIRE(editor->runFrames(2).has_value());
        }
        CHECK_FALSE(directoryExists(project.cacheDirectory()));
    }

    TEST_CASE("meshes in the viewport are picked by what the pointer is really over") {
        const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(640, 360)));
        const std::string cube = builtinMeshId(BuiltinMesh::Cube).toString();
        const std::string sphere = builtinMeshId(BuiltinMesh::Sphere).toString();
        const auto create = [&](std::string_view name, std::string_view mesh, std::string_view transform) {
            run(*editor, "entity.create",
                fmt::format(R"({{"name": "{}", "components": {{"Transform": {}, "MeshRenderer": {{"mesh": "{}"}}}}}})",
                            name, transform, mesh));
        };
        create("Near", cube, R"({"position": [0, 0, 0]})");
        create("Far", cube, R"({"position": [0, 0, -6], "scale": [4, 4, 1]})");
        create("Ball", sphere, R"({"position": [3, 0, 0], "scale": [2, 2, 2]})");
        // Turned by 45 degrees: seen head-on, a diamond.
        create("Diamond", cube, R"({"position": [-3, 0, 0], "rotation": [0, 0, 0.38268343, 0.9238795]})");
        create("Hidden", cube, R"({"position": [0, 3, 0]})");
        run(*editor, "entity.setEnabled", R"({"entity": "Hidden", "enabled": false})");
        // A marker behind the far wall, and one in front of everything.
        run(*editor, "entity.create",
            R"({"name": "Behind", "components": {"Transform": {"position": [1.5, 1.5, -9]}}})");
        run(*editor, "entity.create",
            R"({"name": "In front", "components": {"Transform": {"position": [0.2, 0.2, 3]}}})");
        run(*editor, "viewport.lookAt", R"({"position": [0, 0, 10], "target": [0, 0, 0]})");

        const Vec2 size(640.0f, 360.0f);
        const Image image = unwrap(editor->captureViewport(UVec2(size)));
        const World& world = editor->context().world();
        const ViewportPanel& viewport = editor->viewport();
        const CameraView view = editor->camera().view(size.x / size.y);
        const auto pixelOf = [&](const Vec3& point) { return worldToScreen(view, point, size).value(); };
        const auto picked = [&](const Vec3& point) {
            const Entity entity = viewport.pick(editor->context(), pixelOf(point));
            return entity.isNull() ? std::string("(nothing)") : world.name(entity);
        };

        // The near cube hides the far wall behind it; next to it, the wall is hit.
        CHECK(picked(Vec3(0.3f, -0.3f, 0.5f)) == "Near");
        CHECK(picked(Vec3(1.2f, -1.2f, -5.5f)) == "Far");
        // Triangles, not bounding boxes: the corner of the box around the ball is empty,
        // and so are the corners of the box around the diamond.
        CHECK(picked(Vec3(3.0f, 0.0f, 1.0f)) == "Ball");
        CHECK(picked(Vec3(3.9f, 0.9f, 0.0f)) == "(nothing)");
        CHECK(picked(Vec3(-3.0f, 0.6f, 0.5f)) == "Diamond");
        CHECK(picked(Vec3(-3.45f, 0.45f, 0.5f)) == "(nothing)");
        // Disabled entities are not there.
        CHECK(picked(Vec3(0.0f, 3.0f, 0.5f)) == "(nothing)");
        // A marker can be clicked where it is seen, and not through a mesh in front of it.
        CHECK(picked(Vec3(0.2f, 0.2f, 3.0f)) == "In front");
        CHECK(picked(Vec3(1.5f, 1.5f, -9.0f)) == "Far");
        // Beside everything.
        CHECK(picked(Vec3(-6.5f, 3.0f, 0.0f)) == "(nothing)");

        // What was picked is what was drawn there: the near cube's pixel is lit, the
        // empty corner beside the diamond is background.
        const Vec4 background = image.pixel(3, 3);
        const Vec2 onCube = pixelOf(Vec3(0.3f, -0.3f, 0.5f));
        const Vec2 besideDiamond = pixelOf(Vec3(-3.45f, 0.45f, 0.5f));
        CHECK(image.pixel(static_cast<uint32_t>(onCube.x), static_cast<uint32_t>(onCube.y)).r > background.r + 0.2f);
        CHECK(image.pixel(static_cast<uint32_t>(besideDiamond.x), static_cast<uint32_t>(besideDiamond.y)).r ==
              doctest::Approx(background.r).epsilon(0.05));
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("a model is placed by command, drawn, and taken back") {
        EditorAppDesc desc = headlessDesc(UVec2(640, 360));
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        run(*editor, "scene.new");
        run(*editor, "viewport.lookAt", R"({"position": [3, 2.5, 6], "target": [0, 1.2, 0]})");
        const Image empty = unwrap(editor->captureViewport(UVec2(640, 360)));

        const Json placed = run(*editor, "asset.instantiate", R"({"asset": "models/lamp_post.gltf"})");
        CHECK(placed["entityCount"] == 4);
        const EditorContext& context = editor->context();
        CHECK(context.entityPath(context.world().findByName("Head")) == "lamp_post/Lamp post/Arm/Head");
        REQUIRE(editor->runFrames(2).has_value());
        const Image withLamp = unwrap(editor->captureViewport(UVec2(640, 360)));
        CHECK_FALSE(
            unwrap(compareImages(empty, withLamp, {.channelTolerance = 8, .maxDifferingRatio = 0.002})).matches);

        // Undone, the viewport shows exactly what it showed before.
        run(*editor, "editor.undo");
        const Image undone = unwrap(editor->captureViewport(UVec2(640, 360)));
        CHECK(unwrap(compareImages(empty, undone, {.channelTolerance = 0, .maxDifferingRatio = 0.0})).matches);
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("the editor runs frames on request and closes on request") {
        const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(640, 360)));

        CHECK(run(*editor, "editor.runFrames")["frameIndex"] == 1);
        CHECK(run(*editor, "editor.runFrames", R"({"count": 3})")["frameIndex"] == 4);
        CHECK_FALSE(editor->commands().execute("editor.runFrames", json(R"({"count": 0})")).has_value());
        CHECK_FALSE(editor->commands().execute("editor.runFrames", json(R"({"count": 1e9})")).has_value());

        CHECK(run(*editor, "editor.exit").is_null());
        CHECK_FALSE(unwrap(editor->runFrame()));
        // Asking for more frames after that is harmless.
        CHECK(editor->runFrames(5).has_value());
        CHECK(editor->engine().frameIndex() == 4);
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("the viewport blurs what moves only through the game's camera, while the game's time passes") {
        EditorAppDesc desc = headlessDesc(UVec2(640, 360));
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        // The memory motion blur takes, of the largest view that has it: the security
        // camera's capture blurs what moves, as a game's view does, with the smaller images.
        const auto blurBytes = [&] {
            return run(*editor, "editor.statistics")["scene"]["motionBlur"]["bytes"].get<uint64_t>();
        };
        const uint64_t capture = uint64_t{256} * 144 * 8 + uint64_t{64} * 36 * 4 * 2;

        // Through the editor's own camera nothing of the viewport is blurred.
        REQUIRE(editor->runFrames(3).has_value());
        CHECK(blurBytes() == capture);
        // Nor through the game's, while the scene is edited and no time passes for it.
        run(*editor, "viewport.show", R"({"camera": "game"})");
        REQUIRE(editor->runFrames(3).has_value());
        CHECK(blurBytes() == capture);
        // While it plays, the viewport is a game's view: blurred, with images as large as it.
        run(*editor, "play.start");
        REQUIRE(editor->runFrames(3).has_value());
        CHECK(blurBytes() > capture);
        // Paused, time stands still again.
        run(*editor, "play.pause");
        REQUIRE(editor->runFrames(3).has_value());
        CHECK(blurBytes() == capture);
        run(*editor, "play.stop");
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("the viewport sees the game shaken where its camera shakes, while the game plays") {
        EditorAppDesc desc = headlessDesc(UVec2(640, 360));
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        const ViewportPanel& viewport = editor->viewport();
        // Where the main camera of the showcase is - its entity, not its view.
        const auto camera = [&] { return editor->context().world().findByName("Main camera"); };
        const auto placed = [&] { return Vec3(editor->context().world().worldMatrix(camera())[3]); };

        run(*editor, "viewport.show", R"({"camera": "game"})");
        REQUIRE(editor->runFrames(2).has_value());
        REQUIRE(viewport.showsGame());
        CHECK(viewport.view().position == placed());

        // A game gives the camera trauma: the viewport sees through the shake, as the game
        // would, and it goes on from frame to frame.
        run(*editor, "play.start");
        REQUIRE(editor->runFrames(1).has_value());
        run(*editor, "component.set",
            R"({"entity": "Cameras/Main camera", "component": "CameraShake", "properties": {"trauma": 1.0}})");
        REQUIRE(editor->runFrames(2).has_value());
        const UVec2 size = viewport.targets().size();
        const CameraView shaken = makeCameraView(editor->context().world(), camera(),
                                                 static_cast<float>(size.x) / static_cast<float>(size.y));
        CHECK(viewport.view().position == shaken.position);
        CHECK(viewport.view().forward == shaken.forward);
        CHECK(glm::distance(viewport.view().position, placed()) > 1e-4f);
        const Vec3 before = viewport.view().position;
        REQUIRE(editor->runFrames(1).has_value());
        CHECK(viewport.view().position != before);

        // Paused, the shake holds where it is; stopped, the camera is as the scene has it.
        run(*editor, "play.pause");
        const Vec3 held = viewport.view().position;
        REQUIRE(editor->runFrames(2).has_value());
        CHECK(viewport.view().position == held);
        run(*editor, "play.stop");
        REQUIRE(editor->runFrames(1).has_value());
        CHECK(viewport.view().position == placed());
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("the editor plays sequences only while the scene plays, and its game view follows their cuts") {
        EditorAppDesc desc = headlessDesc(UVec2(640, 360));
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        const ViewportPanel& viewport = editor->viewport();
        const auto entity = [&](std::string_view name) { return editor->context().world().findByName(name); };
        const auto placed = [&](std::string_view name) {
            return Vec3(editor->context().world().worldMatrix(entity(name))[3]);
        };
        const std::string_view flyby = R"({"entity": "Cameras/Flyby", "component": "SequencePlayer"})";
        run(*editor, "viewport.show", R"({"camera": "game"})");
        REQUIRE(editor->runFrames(1).has_value());
        const Vec3 waiting = placed("Cinema camera");
        CHECK(waiting == Vec3(6.0f, 1.2f, 1.0f));

        // Told to play while the scene is edited, the flyby does nothing: an editor shows the
        // scene as it is, and what a sequence writes would change it behind its history.
        run(*editor, "component.set",
            R"({"entity": "Cameras/Flyby", "component": "SequencePlayer", "properties": {"playing": true}})");
        REQUIRE(editor->runFrames(3).has_value());
        CHECK(run(*editor, "component.get", flyby)["time"] == 0.0);
        CHECK(placed("Cinema camera") == waiting);
        CHECK(viewport.view().position == placed("Main camera"));

        // Played, it flies its camera, and the game is seen through it.
        run(*editor, "play.start");
        REQUIRE(editor->runFrames(3).has_value());
        CHECK(run(*editor, "component.get", flyby)["time"].get<double>() > 0.0);
        CHECK(placed("Cinema camera") != waiting);
        CHECK(viewport.view().position == placed("Cinema camera"));
        // Stopped, the scene is as it was, and so is the view.
        run(*editor, "play.stop");
        REQUIRE(editor->runFrames(1).has_value());
        CHECK(placed("Cinema camera") == waiting);
        CHECK(viewport.view().position == placed("Main camera"));
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("editor.statistics reports what the last frame cost") {
        EditorAppDesc desc = headlessDesc(UVec2(640, 360));
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        REQUIRE(editor->runFrames(3).has_value());

        const Json statistics = run(*editor, "editor.statistics");
        CHECK(statistics["frame"] == 2);
        CHECK(statistics["cpuMilliseconds"].get<double>() > 0.0);
        CHECK((statistics["gpuMilliseconds"].is_null() || statistics["gpuMilliseconds"].get<double>() > 0.0));

        // Two views draw the showcase, some of it instanced: the viewport, and before it the
        // security camera's capture of the stage, which the monitor on the wall shows.
        // What is counted for every view is counted twice.
        const Json& scene = statistics["scene"];
        CHECK(scene["views"] == 2);
        CHECK(scene["captures"]["count"] == 1);
        CHECK(scene["captures"]["dropped"] == 0);
        CHECK(scene["meshRenderers"] == 2 * 37);
        // The cable of the hoist is posed by its animator: one skeleton of six joints.
        CHECK(statistics["animation"]["animators"] == 1);
        CHECK(statistics["animation"]["joints"] == 6);
        // The bodies of the scene are there - nineteen of them, two with shapes made of
        // meshes and one that stands for the sentry, a character - and none is
        // simulated: the editor shows a scene, it does not run it. Its joints hold
        // nothing while no time passes, and are not made.
        CHECK(statistics["physics"]["bodies"] == 19);
        CHECK(statistics["physics"]["characters"] == 1);
        CHECK(statistics["physics"]["joints"] == 0);
        CHECK(statistics["physics"]["geometries"] == 2);
        CHECK(statistics["physics"]["steps"] == 0);
        // Nor does it run the scripts of the scene: the clock of the showcase is not
        // even loaded, and stands at the name it has in the file.
        CHECK(dumpJson(statistics["scripts"], -1) ==
              R"({"scripts":0,"instances":0,"stopped":0,"calls":0,"timers":0,"memoryBytes":0,"milliseconds":0.0})");
        // And nothing is heard: the lamp hums and the music plays in a game.
        CHECK(dumpJson(statistics["audio"], -1) ==
              R"({"sources":0,"voices":0,"waiting":0,"listeners":0,"sounds":0,"soundBytes":0})");
        CHECK(run(*editor, "entity.get", R"({"entity": "Stage/Clock"})")["name"] == "Clock");
        CHECK(scene["skinned"]["meshes"] == 2 * 1);
        CHECK(scene["skinned"]["jointMatrices"] == 2 * 6);
        CHECK(scene["drawCalls"].get<uint32_t>() > 0);
        CHECK(scene["drawCalls"].get<uint32_t>() < scene["instances"].get<uint32_t>());
        CHECK(scene["triangles"].get<uint64_t>() > 1000);
        // The sun and the three lamps of the showcase, each lamp in the lists of the
        // clusters it reaches.
        CHECK(scene["lights"]["count"] == 2 * 4);
        CHECK(scene["lights"]["listEntries"].get<uint32_t>() > 3);
        CHECK(scene["lights"]["dropped"] == 0);
        // Three cascades for the sun, one map for the lamp, six for the wall light.
        CHECK(scene["shadows"]["maps"] == 2 * 10);
        CHECK(scene["shadows"]["lightsWithout"] == 0);
        // Passes over the whole picture: the two of the ambient occlusion, the lighting,
        // the three of the depth of field, and what finishes the picture as the showcase
        // says - five levels of bloom down and up, the exposure, the final pass, the
        // smoothing of its edges - for each view; and for the capture, which looks towards
        // the sun, the five of its light shafts, and the three of its motion blur: a
        // capture is a game's view, while the editor's own camera blurs nothing.
        CHECK(scene["screenPasses"] == 2 * 18 + 5 + 3);
        // The ambient occlusion is worked out for one pixel in four of the viewport, in
        // two half floats each; the depth of field in two images of a texel for every four
        // pixels, of four half floats each.
        const UVec2 viewportSize = editor->viewport().targets().size();
        CHECK(scene["ambientOcclusion"]["bytes"] ==
              uint64_t{(viewportSize.x + 1) / 2} * ((viewportSize.y + 1) / 2) * 4);
        CHECK(scene["depthOfField"]["bytes"] ==
              uint64_t{(viewportSize.x + 1) / 2} * ((viewportSize.y + 1) / 2) * 8 * 2);
        // The sun is behind the editor's camera, but the capture looks towards it: its light
        // shafts are worked out at half its 256 by 144 pixels, four half floats a texel, in
        // two images. The decals of the showcase are in the viewport - the grime and the
        // warning strip on the floor, the grime on the wall - and read a copy of the normals
        // as large as the viewport, two 16-bit numbers a pixel, made again for the grime,
        // which comes to where the strip bent them; the capture, on the wall, sees the two
        // on the floor, and copies its normals twice as well.
        CHECK(scene["lightShafts"]["bytes"] == uint64_t{128} * 72 * 8 * 2);
        // The capture's motion blur: a copy of its picture, four half floats a pixel, and two
        // images of two half floats for every tile of four pixels a side - the longest blur
        // of a picture 144 pixels high.
        CHECK(scene["motionBlur"]["bytes"] == uint64_t{256} * 144 * 8 + uint64_t{64} * 36 * 4 * 2);
        CHECK(scene["decals"]["count"] == 3 + 2);
        CHECK(scene["decals"]["dropped"] == 0);
        CHECK(scene["decals"]["normalCopies"] == 2 + 2);
        CHECK(scene["decals"]["bytes"] == uint64_t{viewportSize.x} * viewportSize.y * 4);
        // Every mesh in view is drawn at one of its levels of detail.
        const Json& lods = scene["lods"];
        REQUIRE(lods.size() == kMaxMeshLods + 1);
        uint32_t inView = 0;
        for (const Json& count : lods) {
            inView += count.get<uint32_t>();
        }
        CHECK(inView == scene["meshRenderers"].get<uint32_t>() - scene["culled"].get<uint32_t>());
        // The atlas of the medium quality: 4096 texels a side, four bytes each.
        CHECK(scene["shadows"]["atlasBytes"] == 67108864);
        // The cube map of the showcase's sky, made in the first frame and in no frame
        // since.
        CHECK(scene["environment"]["bytes"] == 4193280);
        CHECK(scene["environment"]["made"] == 0);
        CHECK(scene["passes"]["shadow"]["drawCalls"].get<uint32_t>() > 10);
        // All passes together are what was drawn.
        CHECK(scene["drawCalls"].get<uint32_t>() == scene["passes"]["shadow"]["drawCalls"].get<uint32_t>() +
                                                        scene["passes"]["depth"]["drawCalls"].get<uint32_t>() +
                                                        scene["passes"]["base"]["drawCalls"].get<uint32_t>() +
                                                        scene["passes"]["forward"]["drawCalls"].get<uint32_t>());

        const Json& resources = statistics["resources"];
        CHECK(resources["meshes"].get<uint32_t>() >= 9);
        CHECK(resources["textures"].get<uint32_t>() >= 10);
        CHECK(resources["meshBytes"].get<uint64_t>() > 0);
        CHECK(resources["textureBytes"].get<uint64_t>() > 0);

        // A picture taken between frames draws the scene once more, which is not counted
        // as part of the frame that follows.
        const TempDirectory directory;
        run(*editor, "editor.screenshot",
            fmt::format(R"({{"path": {}, "viewportOnly": true}})", quoted(directory.path() / "view.png")));
        REQUIRE(editor->runFrames(1).has_value());
        CHECK(run(*editor, "editor.statistics")["scene"]["views"] == 2);
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("the editor shows what is animated at the time its animator holds, and plays nothing") {
        EditorAppDesc desc = headlessDesc(UVec2(640, 360));
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        const std::string animatorOfHoist = R"({"entity": "Stage/Hoist/Skeleton", "component": "Animator"})";
        // Where the joints of the cable are: what the animator of the hoist has made.
        World& world = editor->engine().world();
        const auto jointsOfCable = [&] {
            const Entity skeleton = world.findByName("Skeleton");
            const SkeletonPoseComponent* pose = world.tryGet<SkeletonPoseComponent>(skeleton);
            REQUIRE(pose != nullptr);
            REQUIRE(pose->joints.size() == 6);
            return pose->joints;
        };

        // The scene is posed as soon as it is shown: the cable hangs to one side, as
        // its clip has it three quarters of a second in.
        REQUIRE(editor->runFrames(1).has_value());
        const std::vector<Mat4> atFirst = jointsOfCable();
        const float anchorX = atFirst[0][3].x;
        CHECK(atFirst[5][3].x - anchorX > 0.15f);

        // Frames go by, and the animator's clocks stand where the scene has them: the
        // scene is a document here, and nothing has changed it.
        REQUIRE(editor->runFrames(20).has_value());
        const Json animator = run(*editor, "component.get", animatorOfHoist);
        CHECK(animator["time"] == 0.75);
        CHECK(animator["blendTime"] == 1.5);
        CHECK(animator["playing"] == true);
        CHECK(run(*editor, "editor.state")["scene"]["unsavedChanges"] == false);
        CHECK(jointsOfCable() == atFirst);

        // Close to the cable, so that where it hangs is most of the picture.
        run(*editor, "viewport.lookAt", R"({"position": [-1.2, 1.6, 3.4], "target": [-2.1, 1.6, 0.9]})");
        const ImageCompareOptions exactly{.channelTolerance = 0, .maxDifferingRatio = 0.0};
        const Image before = unwrap(editor->captureViewport(UVec2(320, 180)));

        // Set to another time, the cable hangs as the clip has it then: half a swing
        // later, on the other side - in the world, and in the picture.
        run(*editor, "component.set",
            R"({"entity": "Stage/Hoist/Skeleton", "component": "Animator", "properties": {"time": 2.75}})");
        const Image later = unwrap(editor->captureViewport(UVec2(320, 180)));
        CHECK(run(*editor, "component.get", animatorOfHoist)["time"] == 2.75);
        CHECK(jointsOfCable()[5][3].x - anchorX < -0.15f);
        CHECK_FALSE(unwrap(compareImages(before, later, exactly)).matches);

        // It is an edit like any other, and is taken back like one.
        run(*editor, "editor.undo");
        const Image undone = unwrap(editor->captureViewport(UVec2(320, 180)));
        CHECK(jointsOfCable() == atFirst);
        CHECK(unwrap(compareImages(before, undone, exactly)).matches);
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("the viewport picks a skinned mesh where its pose has it, and shows the skeleton that is selected") {
        EditorAppDesc desc = headlessDesc(UVec2(640, 360));
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        // The cable of the hoist, seen at right angles to the plane it swings in.
        run(*editor, "viewport.lookAt", R"({"position": [-0.38, 1.7, 3.36], "target": [-2.1, 1.7, 0.9]})");
        const Vec2 size(640.0f, 360.0f);
        const Image plain = unwrap(editor->captureViewport(UVec2(size)));
        const World& world = editor->context().world();
        const ViewportPanel& viewport = editor->viewport();
        const CameraView view = editor->camera().view(size.x / size.y);
        const auto pixelOf = [&](const Vec3& point) { return worldToScreen(view, point, size).value(); };
        const auto picked = [&](const Vec3& point) {
            const Entity entity = viewport.pick(editor->context(), pixelOf(point));
            return entity.isNull() ? std::string("(nothing)") : world.name(entity);
        };

        // Where the joints of the cable are: as the animator has posed them, placed by
        // its entity. The weight hangs on the last joint, a third of a metre below it.
        const Entity skeleton = world.findByName("Skeleton");
        const SkeletonPoseComponent* pose = world.tryGet<SkeletonPoseComponent>(skeleton);
        REQUIRE(pose != nullptr);
        REQUIRE(pose->joints.size() == 6);
        const Mat4 placement = world.worldMatrix(skeleton);
        const auto jointAt = [&](size_t joint) { return Vec3(placement * pose->joints[joint][3]); };
        const Vec3 weight(placement * pose->joints[5] * Vec4(0.0f, -0.35f, 0.0f, 1.0f));
        // Where the weight was modelled: straight below where the cable hangs from.
        const Vec3 modelled = jointAt(0) - Vec3(0.0f, 1.6f, 0.0f);
        REQUIRE(glm::distance(weight, modelled) > 0.2f);

        // The pointer is over the cable where the cable is drawn.
        CHECK(picked(weight) == "Cable");
        CHECK(picked(modelled) != "Cable");
        // A joint is on the cable: the middle of the tube.
        CHECK(picked(jointAt(3)) == "Cable");

        // The pixels that differ from the picture without a selection: the lines the
        // selection adds.
        const auto added = [&](const Image& image, uint32_t x, uint32_t y) {
            return glm::distance(Vec3(image.pixel(x, y)), Vec3(plain.pixel(x, y))) > 0.08f;
        };

        // Selected, the mesh has a box around it that fits it as it hangs: the weight is
        // inside, where the box of the mesh as it was modelled would end beside it.
        run(*editor, "selection.set", R"({"entities": ["Stage/Hoist/Skeleton/Cable"]})");
        const Image boxed = unwrap(editor->captureViewport(UVec2(size)));
        Vec2 low(size);
        Vec2 high(0.0f);
        for (uint32_t y = 0; y < boxed.height(); ++y) {
            for (uint32_t x = 0; x < boxed.width(); ++x) {
                if (added(boxed, x, y)) {
                    low = glm::min(low, Vec2(x, y));
                    high = glm::max(high, Vec2(x, y));
                }
            }
        }
        const Vec2 weightPixel = pixelOf(weight);
        CHECK(low.x < weightPixel.x - 4.0f);
        CHECK(high.x > weightPixel.x + 4.0f);
        CHECK(low.y < weightPixel.y);
        CHECK(high.y > weightPixel.y);

        // Selected, the animator shows its skeleton: a line through every joint, in a
        // color of its own, through whatever is in front.
        run(*editor, "selection.set", R"({"entities": ["Stage/Hoist/Skeleton"]})");
        const Image bones = unwrap(editor->captureViewport(UVec2(size)));
        const Vec3 boneColor(0.95f, 0.45f, 0.85f);
        for (size_t joint = 0; joint < pose->joints.size(); ++joint) {
            CAPTURE(joint);
            const Vec2 pixel = pixelOf(jointAt(joint));
            const auto x = static_cast<uint32_t>(pixel.x);
            const auto y = static_cast<uint32_t>(pixel.y);
            CHECK_NEAR(Vec3(bones.pixel(x, y)), boneColor, 0.05f);
            CHECK(added(bones, x, y));
        }
        // Half way between two joints there is the bone.
        const Vec2 between = pixelOf(0.5f * (jointAt(2) + jointAt(3)));
        CHECK(added(bones, static_cast<uint32_t>(between.x), static_cast<uint32_t>(between.y)));
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("the editor shows the colliders of a scene, and does not let its bodies fall") {
        const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(640, 360)));
        // A crate in the air: a body that would fall, in a game.
        run(*editor, "entity.create",
            R"({"name": "Crate", "components": {"Transform": {"position": [0, 2, 0]},
                "Collider": {"shape": "box", "size": [2, 1, 1]}, "RigidBody": {"mass": 10}}})");
        run(*editor, "selection.set", R"({"entities": []})");
        run(*editor, "viewport.lookAt", R"({"position": [0, 2, 8], "target": [0, 2, 0]})");
        const Vec2 size(640.0f, 360.0f);
        const CameraView view = editor->camera().view(size.x / size.y);
        // The middle of an edge of the collider that faces the viewer: its upper front one.
        const Vec2 onEdge = worldToScreen(view, Vec3(0.0f, 2.5f, 0.5f), size).value();
        const Vec3 colliderColor(0.40f, 0.95f, 0.50f);
        // Whether a line of a color runs through a point of the picture: a line is one
        // pixel wide, and lies on the pixel the point is in or on one beside it.
        const auto hasLine = [](const Image& image, const Vec2& point, const Vec3& color) {
            for (int32_t dy = -1; dy <= 1; ++dy) {
                for (int32_t dx = -1; dx <= 1; ++dx) {
                    const Vec4 pixel = image.pixel(static_cast<uint32_t>(static_cast<int32_t>(point.x) + dx),
                                                   static_cast<uint32_t>(static_cast<int32_t>(point.y) + dy));
                    if (glm::distance(Vec3(pixel), color) < 0.05f) {
                        return true;
                    }
                }
            }
            return false;
        };
        const auto hasColliderLine = [&](const Image& image, const Vec2& point) {
            return hasLine(image, point, colliderColor);
        };

        // Asking changes nothing: colliders are not shown until they are asked for.
        CHECK(run(*editor, "viewport.show")["colliders"] == false);
        const Image plain = unwrap(editor->captureViewport(UVec2(size)));
        CHECK_FALSE(hasColliderLine(plain, onEdge));

        CHECK(run(*editor, "viewport.show", R"({"colliders": true})")["colliders"] == true);
        CHECK(editor->viewport().showsColliders());
        const Image shown = unwrap(editor->captureViewport(UVec2(size)));
        CHECK(hasColliderLine(shown, onEdge));
        // Beside the collider there is none.
        CHECK_FALSE(hasColliderLine(shown, worldToScreen(view, Vec3(0.0f, 3.0f, 0.5f), size).value()));
        CHECK(run(*editor, "viewport.show", R"({"colliders": false})")["colliders"] == false);
        // What is no yes or no is refused.
        CHECK_FALSE(editor->commands().execute("viewport.show", json(R"({"colliders": "yes"})")).has_value());

        // The collider of what is selected is always drawn - and those of what hangs
        // below it, which are parts of the same body: here a handle on top of the crate.
        run(*editor, "entity.create",
            R"({"name": "Handle", "parent": "Crate", "components": {"Transform": {"position": [0, 1.5, 0]},
                "Collider": {"shape": "box", "size": [0.5, 0.5, 0.5]}}})");
        const Vec2 onHandle = worldToScreen(view, Vec3(0.0f, 3.75f, 0.25f), size).value();
        run(*editor, "selection.set", R"({"entities": []})");
        const Image unselected = unwrap(editor->captureViewport(UVec2(size)));
        CHECK_FALSE(hasColliderLine(unselected, onEdge));
        CHECK_FALSE(hasColliderLine(unselected, onHandle));
        run(*editor, "selection.set", R"({"entities": ["Crate"]})");
        const Image selected = unwrap(editor->captureViewport(UVec2(size)));
        CHECK(hasColliderLine(selected, onEdge));
        CHECK(hasColliderLine(selected, onHandle));

        // A collider that is made of a mesh is drawn as what the physics has made of
        // the mesh, once a frame has made it: here the hull around the cone, to whose
        // tip an edge runs from every corner of its base.
        Json marker = json(R"({"name": "Marker", "components": {"Transform": {"position": [3, 2, 0]},
                               "Collider": {"shape": "convexHull"}}})");
        marker["components"]["Collider"]["mesh"] = builtinMeshId(BuiltinMesh::Cone).toString();
        REQUIRE(editor->commands().execute("entity.create", marker).has_value());
        run(*editor, "selection.set", R"({"entities": ["Marker"]})");
        REQUIRE(editor->runFrames(1).has_value());
        const Vec2 onTip = worldToScreen(view, Vec3(3.0f, 2.5f, 0.0f), size).value();
        const Image marked = unwrap(editor->captureViewport(UVec2(size)));
        CHECK(hasColliderLine(marked, onTip));
        run(*editor, "selection.set", R"({"entities": ["Crate"]})");
        CHECK_FALSE(hasColliderLine(unwrap(editor->captureViewport(UVec2(size))), onTip));
        run(*editor, "entity.delete", R"({"entities": ["Marker"]})");

        // A joint is drawn too, with what is selected and what hangs below that: a mark
        // where it is - a metre and a half above the handle, here - and the axis of a
        // hinge through it.
        const Vec3 jointColor(0.45f, 0.75f, 1.00f);
        run(*editor, "component.add",
            R"({"entity": "Handle", "component": "Joint",
                "properties": {"type": "hinge", "anchor": [0, 1.5, 0], "axis": [1, 0, 0]}})");
        const Vec2 onAxis = worldToScreen(view, Vec3(0.3f, 5.0f, 0.0f), size).value();
        const Vec2 onTheWay = worldToScreen(view, Vec3(1.5f, 5.0f, 0.0f), size).value();
        const Image hinged = unwrap(editor->captureViewport(UVec2(size)));
        CHECK(hasLine(hinged, onAxis, jointColor));
        // Held to the world, it has no line to anything; held to an entity, one to where
        // that is.
        CHECK_FALSE(hasLine(hinged, onTheWay, jointColor));
        const Json post = run(*editor, "entity.create",
                              R"({"name": "Post", "components": {"Transform": {"position": [2.5, 5, 0]}}})");
        Json held = json(R"({"entity": "Handle", "component": "Joint", "properties": {}})");
        held["properties"]["connectedTo"] = post["id"];
        REQUIRE(editor->commands().execute("component.set", held).has_value());
        run(*editor, "selection.set", R"({"entities": ["Crate"]})");
        CHECK(hasLine(unwrap(editor->captureViewport(UVec2(size))), onTheWay, jointColor));
        // What is not selected shows its joint when the colliders of all entities are shown.
        run(*editor, "selection.set", R"({"entities": []})");
        CHECK_FALSE(hasLine(unwrap(editor->captureViewport(UVec2(size))), onAxis, jointColor));
        run(*editor, "viewport.show", R"({"colliders": true})");
        CHECK(hasLine(unwrap(editor->captureViewport(UVec2(size))), onAxis, jointColor));
        run(*editor, "viewport.show", R"({"colliders": false})");
        run(*editor, "entity.delete", R"({"entities": ["Post"]})");
        run(*editor, "component.remove", R"({"entity": "Handle", "component": "Joint"})");
        run(*editor, "selection.set", R"({"entities": ["Crate"]})");

        // A trigger is drawn in a color of its own.
        run(*editor, "entity.create",
            R"({"name": "Zone", "components": {"Transform": {"position": [-3, 2, 0]},
                "Collider": {"shape": "box", "size": [1, 1, 1], "trigger": true}}})");
        run(*editor, "selection.set", R"({"entities": ["Zone"]})");
        const Vec2 onZone = worldToScreen(view, Vec3(-3.0f, 2.5f, 0.5f), size).value();
        const Image zoned = unwrap(editor->captureViewport(UVec2(size)));
        CHECK_FALSE(hasColliderLine(zoned, onZone));
        CHECK(hasLine(zoned, onZone, Vec3(1.00f, 0.55f, 0.20f)));
        run(*editor, "entity.delete", R"({"entities": ["Zone"]})");
        run(*editor, "selection.set", R"({"entities": ["Crate"]})");

        // A character is drawn as the capsule it is: upright in the world however its
        // entity is turned, with its soles where the entity is. Here one of three
        // metres whose entity leans over by an eighth of a turn.
        const Vec3 characterColor(0.75f, 0.60f, 1.00f);
        run(*editor, "entity.create",
            R"({"name": "Guard", "components": {
                "Transform": {"position": [3, 0.5, 0], "rotation": [0, 0, 0.38268343, 0.92387953]},
                "CharacterController": {"radius": 0.5, "height": 3}}})");
        // The point nearest the viewer of the ring around its lower end, and the top of its head.
        const Vec2 onRing = worldToScreen(view, Vec3(3.0f, 1.0f, 0.5f), size).value();
        const Vec2 onHead = worldToScreen(view, Vec3(3.0f, 3.5f, 0.0f), size).value();
        run(*editor, "selection.set", R"({"entities": ["Guard"]})");
        const Image guarded = unwrap(editor->captureViewport(UVec2(size)));
        CHECK(hasLine(guarded, onRing, characterColor));
        CHECK(hasLine(guarded, onHead, characterColor));
        // Not selected, it is drawn when the colliders of all entities are.
        run(*editor, "selection.set", R"({"entities": []})");
        CHECK_FALSE(hasLine(unwrap(editor->captureViewport(UVec2(size))), onRing, characterColor));
        run(*editor, "viewport.show", R"({"colliders": true})");
        CHECK(hasLine(unwrap(editor->captureViewport(UVec2(size))), onRing, characterColor));
        run(*editor, "viewport.show", R"({"colliders": false})");
        // The editor has it as a character, where its entity is, and does not let it fall.
        REQUIRE(editor->runFrames(5).has_value());
        CHECK(run(*editor, "editor.statistics")["physics"]["characters"] == 1);
        CHECK(run(*editor, "component.get", R"({"entity": "Guard", "component": "Transform"})")["position"] ==
              json("[3.0, 0.5, 0.0]"));
        // What is not for a person to say - what the simulation says of a character - an edit does not say.
        CHECK_FALSE(editor->commands()
                        .execute("component.set", json(R"({"entity": "Guard", "component": "CharacterController",
                                                           "properties": {"ground": "onGround"}})"))
                        .has_value());
        run(*editor, "component.set",
            R"({"entity": "Guard", "component": "CharacterController", "properties": {"movement": [1, 0, 0]}})");
        run(*editor, "entity.delete", R"({"entities": ["Guard"]})");
        run(*editor, "selection.set", R"({"entities": ["Crate"]})");

        // Frames go by, and the crate hangs where the scene has it: nothing is simulated,
        // and nothing has changed the document since it was made.
        REQUIRE(editor->runFrames(30).has_value());
        const Json transform = run(*editor, "component.get", R"({"entity": "Crate", "component": "Transform"})");
        CHECK(transform["position"] == json("[0.0, 2.0, 0.0]"));
        const Json statistics = run(*editor, "editor.statistics");
        // One body: the crate, with its handle as a part of it.
        CHECK(statistics["physics"]["bodies"] == 1);
        CHECK(statistics["physics"]["steps"] == 0);
        // The speed of a body is not part of a scene, but can be asked for and set.
        run(*editor, "component.set",
            R"({"entity": "Crate", "component": "RigidBody", "properties": {"linearVelocity": [0, 3, 0]}})");
        const Json rigidBody = run(*editor, "component.get", R"({"entity": "Crate", "component": "RigidBody"})");
        CHECK(rigidBody["mass"] == 10.0);
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("the editor has the controls of its project, and commands press them") {
        EditorAppDesc desc = headlessDesc(UVec2(640, 360));
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        const Json map = run(*editor, "input.map");
        CHECK(map["file"] == pathToUtf8(showcaseProject() / "assets" / "input" / "controls.input.json"));
        CHECK(map["map"]["actions"][0]["name"] == "jump");
        CHECK(editor->engine().actions().hasAxis("moveX"));

        // What every control is doing, by its name.
        const auto stateOf = [&](std::string_view kind, std::string_view name) {
            const Json state = run(*editor, "input.state");
            for (const Json& entry : state[std::string(kind)]) {
                if (entry["name"].get<std::string>() == name) {
                    return entry;
                }
            }
            return Json();
        };
        // A frame goes by, as frames do before anybody touches anything: from then on the
        // controls know how things were, and what goes down has been pressed.
        run(*editor, "editor.runFrames");
        CHECK(stateOf("actions", "jump")["down"] == false);
        // A key and a gamepad, as a person would use them: seen by the frame that follows.
        run(*editor, "input.inject", R"({"key": "D", "down": true})");
        run(*editor, "input.inject", R"({"gamepad": 0, "buttons": ["A"], "axes": {"RightTrigger": 0.9}})");
        run(*editor, "editor.runFrames");
        CHECK(stateOf("actions", "jump") ==
              json(R"({"name": "jump", "down": true, "pressed": true, "released": false})"));
        CHECK(stateOf("actions", "fire")["down"] == true);
        CHECK(stateOf("axes", "moveX")["value"] == 1.0);
        run(*editor, "input.inject", R"({"key": "D", "down": false})");
        run(*editor, "input.inject", R"({"gamepad": 0, "connected": false})");
        run(*editor, "editor.runFrames");
        CHECK(stateOf("actions", "jump")["released"] == true);
        CHECK(stateOf("axes", "moveX")["value"] == 0.0);
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("a project that names controls that cannot be read does not open") {
        const TempDirectory directory;
        Project project = unwrap(createProject(directory.path(), "Broken"));
        project.settings.inputMap = "input/controls.input.json";
        REQUIRE(saveProject(project).has_value());
        EditorAppDesc desc = headlessDesc(UVec2(320, 180));
        desc.project = directory.path();

        // The file is not there.
        Result<std::unique_ptr<EditorApp>> editor = EditorApp::create(desc);
        REQUIRE_FALSE(editor.has_value());
        CHECK(editor.error().code == ErrorCode::NotFound);
        CHECK(editor.error().message.find("controls.input.json") != std::string::npos);

        // The file is there, and is no input map: what is wrong with it is said, and where.
        REQUIRE(ensureDirectory(project.assetDirectory() / "input").has_value());
        REQUIRE(writeTextFile(project.inputMapPath(),
                              R"({"format": "cordite.input", "version": 1,
                                  "actions": [{"name": "jump", "bindings": [{"key": "Spacebar"}]}]})")
                    .has_value());
        editor = EditorApp::create(desc);
        REQUIRE_FALSE(editor.has_value());
        CHECK(editor.error().code == ErrorCode::ParseError);
        CHECK(editor.error().message ==
              pathToUtf8(project.inputMapPath()) + ": action 'jump', binding 1: \"Spacebar\" is not the name of a key");

        // Put right, the project opens with its controls.
        REQUIRE(writeTextFile(project.inputMapPath(),
                              R"({"format": "cordite.input", "version": 1,
                                  "actions": [{"name": "jump", "bindings": [{"key": "Space"}]}]})")
                    .has_value());
        const std::unique_ptr<EditorApp> opened = createEditor(desc);
        CHECK(opened->engine().actions().hasAction("jump"));
        // And a project that names none has none.
        project.settings.inputMap.clear();
        REQUIRE(saveProject(project).has_value());
    }

    TEST_CASE("the viewport shows the debug view it is told to") {
        EditorAppDesc desc = headlessDesc(UVec2(640, 360));
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);

        // Asking changes nothing, and lists what there is.
        Json views = run(*editor, "viewport.debugView");
        CHECK(views["view"] == "none");
        REQUIRE(views["views"].size() == kRenderDebugViews.size());
        CHECK(views["views"][0] == "none");
        CHECK(views["views"][2] == "normals");
        CHECK(editor->viewport().debugView() == RenderDebugView::None);
        const Image lit = unwrap(editor->captureViewport(UVec2(320, 180)));

        // Every view can be shown, draws without complaint and differs from the picture.
        for (const RenderDebugView view : kRenderDebugViews) {
            if (view == RenderDebugView::None) {
                continue;
            }
            CAPTURE(nameOf(view));
            views = run(*editor, "viewport.debugView", fmt::format(R"({{"view": "{}"}})", nameOf(view)));
            CHECK(views["view"] == std::string(nameOf(view)));
            CHECK(editor->viewport().debugView() == view);
            const Image shown = unwrap(editor->captureViewport(UVec2(320, 180)));
            CHECK(unwrap(compareImages(lit, shown)).differingPixels > 1000);
        }

        // Names are not case-sensitive; an unknown one is refused and changes nothing.
        CHECK(run(*editor, "viewport.debugView", R"({"view": "BaseColor"})")["view"] == "baseColor");
        const Result<Json> unknown = editor->commands().execute("viewport.debugView", json(R"({"view": "xray"})"));
        REQUIRE_FALSE(unknown.has_value());
        CHECK(unknown.error().code == ErrorCode::InvalidArgument);
        CHECK(editor->viewport().debugView() == RenderDebugView::BaseColor);

        // Back to the picture: the same as before.
        run(*editor, "viewport.debugView", R"({"view": "none"})");
        CHECK(unwrap(compareImages(lit, unwrap(editor->captureViewport(UVec2(320, 180))))).differingPixels == 0);
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("the viewport draws its picture at the quality it is told to") {
        EditorAppDesc desc = headlessDesc(UVec2(640, 360));
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);

        // Asking changes nothing, and lists what there is.
        Json answer = run(*editor, "viewport.quality");
        CHECK(answer["preset"] == "medium");
        CHECK(answer["shadows"] == "medium");
        CHECK(answer["effects"] == "medium");
        CHECK(answer["postProcess"] == "medium");
        CHECK(answer["antiAliasing"] == "fxaa");
        CHECK(answer["antiAliasingModes"] == json(R"(["off", "fxaa"])"));
        CHECK(editor->viewport().antiAliasing() == AntiAliasing::Fxaa);
        CHECK(answer["presets"] == json(R"(["low", "medium", "high"])"));
        CHECK(answer["shadowQualities"] == json(R"(["off", "low", "medium", "high"])"));
        CHECK(answer["effectsQualities"] == json(R"(["off", "low", "medium", "high"])"));
        CHECK(answer["postProcessQualities"] == json(R"(["off", "low", "medium", "high"])"));
        CHECK(editor->viewport().shadowQuality() == ShadowQuality::Medium);
        CHECK(editor->viewport().screenEffectsQuality() == ScreenEffectsQuality::Medium);
        CHECK(editor->viewport().postProcessQuality() == PostProcessQuality::Medium);
        const Image medium = unwrap(editor->captureViewport(UVec2(320, 180)));

        // Without shadows the picture is another one: the showcase has three lights
        // that cast them. Shadows alone were set, so no preset names what is in use.
        answer = run(*editor, "viewport.quality", R"({"shadows": "off"})");
        CHECK(answer["shadows"] == "off");
        CHECK(answer["postProcess"] == "medium");
        CHECK(answer["preset"].is_null());
        CHECK(editor->viewport().shadowQuality() == ShadowQuality::Off);
        const Image without = unwrap(editor->captureViewport(UVec2(320, 180)));
        CHECK(unwrap(compareImages(medium, without)).differingPixels > 500);

        // Every preset draws without complaint, and the picture is nearly the same at
        // each: the same shadows with softer or sharper edges, the same look worked out
        // more or less finely.
        for (const auto [preset, lodErrorPixels] :
             std::initializer_list<std::pair<const char*, float>>{{"low", 2.0f}, {"high", 0.5f}, {"medium", 1.0f}}) {
            CAPTURE(preset);
            answer = run(*editor, "viewport.quality", fmt::format(R"({{"preset": "{}"}})", preset));
            CHECK(answer["preset"] == std::string(preset));
            CHECK(answer["shadows"] == std::string(preset));
            CHECK(answer["effects"] == std::string(preset));
            CHECK(answer["postProcess"] == std::string(preset));
            CHECK(answer["lodErrorPixels"] == lodErrorPixels);
            CHECK(editor->viewport().lodErrorPixels() == lodErrorPixels);
            // Asked again, the viewport is still at the preset: it keeps every part of it.
            CHECK(run(*editor, "viewport.quality")["preset"] == std::string(preset));
            const Image shown = unwrap(editor->captureViewport(UVec2(320, 180)));
            const ImageComparison difference =
                unwrap(compareImages(medium, shown, {.channelTolerance = 24, .maxDifferingRatio = 0.03}));
            CHECK(difference.matches);
        }

        // The levels of detail are drawn as the viewport allows: everything at its finest
        // when it allows no error, coarser when it allows much - which no preset does.
        const auto meshesAtFinest = [&] {
            REQUIRE(editor->runFrames(1).has_value());
            return run(*editor, "editor.statistics")["scene"]["lods"][0].get<uint32_t>();
        };
        answer = run(*editor, "viewport.quality", R"({"lodErrorPixels": 0})");
        CHECK(answer["lodErrorPixels"] == 0.0f);
        CHECK(answer["preset"].is_null());
        const uint32_t allFinest = meshesAtFinest();
        run(*editor, "viewport.quality", R"({"lodErrorPixels": 32})");
        CHECK(meshesAtFinest() < allFinest);
        for (const char* wrong : {R"({"lodErrorPixels": -1})", R"({"lodErrorPixels": 65})"}) {
            CAPTURE(std::string(wrong));
            CHECK_FALSE(editor->commands().execute("viewport.quality", json(wrong)).has_value());
        }
        CHECK(editor->viewport().lodErrorPixels() == 32.0f);
        run(*editor, "viewport.quality", R"({"preset": "medium"})");

        // A picture that is not finished is the light as the renderer worked it out: no
        // exposure, no curve, no grade - a different picture altogether.
        answer = run(*editor, "viewport.quality", R"({"postProcess": "off"})");
        CHECK(answer["postProcess"] == "off");
        CHECK(answer["shadows"] == "medium");
        CHECK(editor->viewport().postProcessQuality() == PostProcessQuality::Off);
        const Image unfinished = unwrap(editor->captureViewport(UVec2(320, 180)));
        CHECK(unwrap(compareImages(medium, unfinished, {.channelTolerance = 16})).differingPixels > 5000);

        // Without the screen effects the corners of the scene are a little lighter: the
        // light from the sky is kept out of them no longer.
        run(*editor, "viewport.quality", R"({"preset": "medium"})");
        answer = run(*editor, "viewport.quality", R"({"effects": "off"})");
        CHECK(answer["effects"] == "off");
        CHECK(answer["shadows"] == "medium");
        CHECK(answer["preset"].is_null());
        CHECK(editor->viewport().screenEffectsQuality() == ScreenEffectsQuality::Off);
        const Image withoutEffects = unwrap(editor->captureViewport(UVec2(320, 180)));
        CHECK(unwrap(compareImages(medium, withoutEffects, {.channelTolerance = 3})).differingPixels > 200);
        CHECK(unwrap(compareImages(medium, withoutEffects, {.channelTolerance = 40})).differingPixels == 0);

        // Without anti-aliasing the edges in the picture have their steps: some pixels
        // differ, a few of them by much, and most of the picture not at all. It is no
        // part of a preset: the picture is still at the medium quality, and choosing a
        // preset does not bring the smoothing back.
        run(*editor, "viewport.quality", R"({"preset": "medium"})");
        answer = run(*editor, "viewport.quality", R"({"antiAliasing": "off"})");
        CHECK(answer["antiAliasing"] == "off");
        CHECK(answer["preset"] == "medium");
        CHECK(editor->viewport().antiAliasing() == AntiAliasing::Off);
        const Image stepped = unwrap(editor->captureViewport(UVec2(320, 180)));
        const ImageComparison steps = unwrap(compareImages(medium, stepped, {.channelTolerance = 24}));
        CHECK(steps.differingPixels > 50);
        CHECK(steps.differingPixels < 320 * 180 / 10);
        answer = run(*editor, "viewport.quality", R"({"preset": "high"})");
        CHECK(answer["antiAliasing"] == "off");
        answer = run(*editor, "viewport.quality", R"({"antiAliasing": "FXAA"})");
        CHECK(answer["antiAliasing"] == "fxaa");
        CHECK(editor->viewport().antiAliasing() == AntiAliasing::Fxaa);

        // A preset and single settings in one call: the preset first, then the rest.
        answer = run(*editor, "viewport.quality", R"({"preset": "high", "shadows": "LOW"})");
        CHECK(answer["shadows"] == "low");
        CHECK(answer["effects"] == "high");
        CHECK(answer["postProcess"] == "high");
        CHECK(answer["preset"].is_null());

        // An unknown name is refused, and then nothing of the call is applied.
        for (const char* parameters :
             {R"({"preset": "ultra"})", R"({"shadows": "ultra"})", R"({"postProcess": "ultra"})",
              R"({"effects": "ultra"})", R"({"effects": "off", "shadows": "ultra"})", R"({"antiAliasing": "ultra"})",
              R"({"shadows": "off", "antiAliasing": "ultra"})", R"({"preset": "medium", "postProcess": "ultra"})",
              R"({"shadows": "off", "postProcess": "ultra"})"}) {
            CAPTURE(std::string(parameters));
            const Result<Json> unknown = editor->commands().execute("viewport.quality", json(parameters));
            REQUIRE_FALSE(unknown.has_value());
            CHECK(unknown.error().code == ErrorCode::InvalidArgument);
            CHECK(unknown.error().message.find("ultra") != std::string::npos);
            CHECK(editor->viewport().shadowQuality() == ShadowQuality::Low);
            CHECK(editor->viewport().screenEffectsQuality() == ScreenEffectsQuality::High);
            CHECK(editor->viewport().postProcessQuality() == PostProcessQuality::High);
            CHECK(editor->viewport().antiAliasing() == AntiAliasing::Fxaa);
        }
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("optional windows are opened and closed by command") {
        const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(640, 360)));
        REQUIRE(editor->runFrames(2).has_value());
        const Image closed = unwrap(editor->engine().captureFrame());

        Json panels = run(*editor, "editor.showPanel", R"({"panel": "statistics"})");
        CHECK(panels["statistics"] == true);
        CHECK(panels["interfaceDemo"] == false);
        REQUIRE(editor->runFrames(2).has_value());
        // The window is there to be seen.
        const ImageComparison shown = unwrap(compareImages(closed, unwrap(editor->engine().captureFrame())));
        CHECK(shown.differingPixels > 2000);

        panels = run(*editor, "editor.showPanel", R"({"panel": "statistics", "visible": false})");
        CHECK(panels["statistics"] == false);
        REQUIRE(editor->runFrames(2).has_value());
        CHECK(unwrap(compareImages(closed, unwrap(editor->engine().captureFrame()))).differingPixels == 0);

        const Result<Json> unknown = editor->commands().execute("editor.showPanel", json(R"({"panel": "nonsense"})"));
        REQUIRE_FALSE(unknown.has_value());
        CHECK(unknown.error().code == ErrorCode::InvalidArgument);
        CHECK(unknown.error().message.find("statistics") != std::string::npos);
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("the viewport shows the scene through the camera of the game, with nothing an editor adds") {
        EditorAppDesc desc = headlessDesc(UVec2(640, 360));
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        const UVec2 size(640, 360);
        const auto same = [](const Image& a, const Image& b) {
            return unwrap(compareImages(a, b, {.channelTolerance = 0, .maxDifferingRatio = 0.0})).matches;
        };
        const auto selectedNames = [&] {
            std::vector<std::string> names;
            for (const Json& entity : run(*editor, "selection.get")) {
                names.push_back(entity["name"].get<std::string>());
            }
            return names;
        };

        // Unless it is asked to, it looks through the editor's camera.
        CHECK(run(*editor, "viewport.show") == json(R"({"camera": "editor", "showsGame": false, "colliders": false})"));
        CHECK(editor->viewport().viewCamera() == ViewportCamera::Editor);
        run(*editor, "selection.set", R"({"entities": ["Stage/Crate"]})");
        const Image editorView = unwrap(editor->captureViewport(size));

        // Through the camera of the game: where the scene's main camera stands, with its
        // field of view, lit and cleared as the game is - and no grid, no markers, no
        // frame around the crate that is selected.
        CHECK(run(*editor, "viewport.show", R"({"camera": "game"})")["camera"] == "game");
        const Image gameView = unwrap(editor->captureViewport(size));
        CHECK(run(*editor, "viewport.show")["showsGame"] == true);
        CHECK(editor->viewport().showsGame());
        checkAgainstReference(gameView, "editor-game-view-640x360");
        CHECK_FALSE(same(gameView, editorView));

        // What is selected, and whether colliders are to be drawn, changes nothing in it.
        run(*editor, "selection.set", R"({"entities": ["Stage/Barrel", "Lighting/Sun"]})");
        run(*editor, "viewport.show", R"({"colliders": true})");
        CHECK(same(unwrap(editor->captureViewport(size)), gameView));
        // Nor does where the editor's camera is.
        run(*editor, "viewport.lookAt", R"({"position": [-5.5, 3.2, 6.5], "target": [0.5, 0.8, -1.5]})");
        CHECK(same(unwrap(editor->captureViewport(size)), gameView));
        // Where the camera of the scene is, does.
        run(*editor, "component.set",
            R"({"entity": "Cameras/Main camera", "component": "Transform", "properties": {"position": [4, 4, 6]}})");
        CHECK_FALSE(same(unwrap(editor->captureViewport(size)), gameView));
        run(*editor, "editor.undo");
        CHECK(same(unwrap(editor->captureViewport(size)), gameView));

        // A click in the game's view is the game's: it selects nothing, and takes no
        // selection away. In the editor's it picks what is under the pointer.
        run(*editor, "viewport.show", R"({"colliders": false})");
        run(*editor, "selection.set", R"({"entities": ["Stage/Crate"]})");
        const auto click = [&] {
            run(*editor, "input.inject", R"({"mouseMove": [280, 150]})");
            run(*editor, "editor.runFrames", R"({"count": 2})");
            run(*editor, "input.inject", R"({"mouseButton": "Left", "down": true})");
            run(*editor, "editor.runFrames");
            run(*editor, "input.inject", R"({"mouseButton": "Left", "down": false})");
            run(*editor, "editor.runFrames", R"({"count": 2})");
        };
        run(*editor, "editor.runFrames", R"({"count": 4})");
        click();
        CHECK(selectedNames() == std::vector<std::string>{"Crate"});
        run(*editor, "viewport.show", R"({"camera": "editor"})");
        run(*editor, "editor.runFrames");
        click();
        CHECK(selectedNames() != std::vector<std::string>{"Crate"});

        // Nor is the editor's camera flown from the game's view: the right button and
        // W turn and move it in its own view, and nothing in the game's.
        const auto fly = [&] {
            run(*editor, "input.inject", R"({"mouseMove": [280, 150]})");
            run(*editor, "editor.runFrames", R"({"count": 2})");
            run(*editor, "input.inject", R"({"mouseButton": "Right", "down": true})");
            run(*editor, "input.inject", R"({"key": "W", "down": true})");
            run(*editor, "editor.runFrames", R"({"count": 2})");
            run(*editor, "input.inject", R"({"mouseMove": [330, 170]})");
            run(*editor, "editor.runFrames", R"({"count": 3})");
            run(*editor, "input.inject", R"({"key": "W", "down": false})");
            run(*editor, "input.inject", R"({"mouseButton": "Right", "down": false})");
            run(*editor, "editor.runFrames", R"({"count": 2})");
        };
        run(*editor, "viewport.show", R"({"camera": "game"})");
        run(*editor, "editor.runFrames");
        const Vec3 stoodAt = editor->camera().position();
        const float lookedAlong = editor->camera().yawDegrees();
        fly();
        CHECK(editor->camera().position() == stoodAt);
        CHECK(editor->camera().yawDegrees() == lookedAlong);
        run(*editor, "viewport.show", R"({"camera": "editor"})");
        run(*editor, "editor.runFrames");
        fly();
        CHECK(editor->camera().position() != stoodAt);
        CHECK(editor->camera().yawDegrees() != lookedAlong);

        // A scene without a camera for the game is shown through the editor's, as an
        // editor shows it - whatever was asked for.
        run(*editor, "selection.set", R"({"entities": ["Stage/Crate"]})");
        const Image editorNow = unwrap(editor->captureViewport(size));
        run(*editor, "viewport.show", R"({"camera": "game"})");
        run(*editor, "component.set",
            R"({"entity": "Cameras/Main camera", "component": "Camera", "properties": {"primary": false}})");
        // (The camera that is not looked through any more is drawn, as cameras are.)
        const Image fallback = unwrap(editor->captureViewport(size));
        CHECK(run(*editor, "viewport.show") == json(R"({"camera": "game", "showsGame": false, "colliders": false})"));
        CHECK_FALSE(same(fallback, gameView));
        run(*editor, "editor.undo");
        run(*editor, "viewport.show", R"({"camera": "editor"})");
        CHECK(same(unwrap(editor->captureViewport(size)), editorNow));

        // A camera the viewport has not.
        const Error error = refusal(*editor, "viewport.show", R"({"camera": "player"})");
        CHECK(error.code == ErrorCode::InvalidArgument);
        CHECK(error.message == "viewport.show: 'player' is no camera of the viewport; use editor or game");
        CHECK(run(*editor, "viewport.show")["camera"] == "editor");

        // Played, a scene is looked at through its own camera, and stopped, as before.
        run(*editor, "play.start");
        CHECK(run(*editor, "viewport.show")["camera"] == "game");
        run(*editor, "editor.runFrames", R"({"count": 3})");
        CHECK(run(*editor, "viewport.show")["showsGame"] == true);
        run(*editor, "play.stop");
        CHECK(run(*editor, "viewport.show")["camera"] == "editor");
        run(*editor, "editor.runFrames");
        CHECK(run(*editor, "viewport.show")["showsGame"] == false);
        run(*editor, "play.start");
        run(*editor, "editor.runFrames");
        // Somebody who wants to fly around the game that runs says so, and may.
        CHECK(run(*editor, "viewport.show", R"({"camera": "editor"})")["camera"] == "editor");
        run(*editor, "editor.runFrames");
        CHECK(run(*editor, "viewport.show")["showsGame"] == false);
        run(*editor, "play.stop");
        CHECK(run(*editor, "viewport.show")["camera"] == "editor");
        // And whoever looked through the game's camera before goes on doing so after.
        run(*editor, "viewport.show", R"({"camera": "game"})");
        run(*editor, "play.start");
        run(*editor, "play.stop");
        CHECK(run(*editor, "viewport.show")["camera"] == "game");
        CHECK(nameOf(ViewportCamera::Editor) == "editor");
        CHECK(nameOf(ViewportCamera::Game) == "game");
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("the game's view is drawn as the game draws: with its background, and with its lights or none") {
        const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(640, 360)));
        const UVec2 size(320, 180);
        // A cube and a camera that looks at it, and no light at all.
        run(*editor, "entity.create",
            R"({"name": "Eye", "components": {"Transform": {"position": [0, 0, 5]}, "Camera": {}}})");
        const Uuid cube = builtinMeshId(BuiltinMesh::Cube);
        run(*editor, "entity.create",
            fmt::format(R"({{"name": "Cube", "components": {{"MeshRenderer": {{"mesh": "{}"}}}}}})", cube.toString()));
        run(*editor, "viewport.lookAt", R"({"position": [0, 0, 5], "target": [0, 0, 0]})");
        const auto brightness = [](const Vec4& pixel) { return (pixel.r + pixel.g + pixel.b) / 3.0f; };
        // The middle of the picture is the cube; a corner is what is behind everything.
        const auto middle = [&] { return unwrap(editor->captureViewport(size)).pixel(160, 90); };
        const auto corner = [&] { return unwrap(editor->captureViewport(size)).pixel(4, 4); };

        // An editor lights a scene that has no light of its own, so that it can be seen.
        const float editorCube = brightness(middle());
        const Vec4 editorCorner = corner();
        CHECK(editorCube > 0.2f);

        // A game does not, unless it says so: through its camera the cube is dark.
        run(*editor, "viewport.show", R"({"camera": "game"})");
        CHECK(brightness(middle()) < 0.05f);
        CHECK(editor->viewport().showsGame());
        editor->engine().renderSettings().defaultLighting = true;
        CHECK(brightness(middle()) > 0.2f);
        editor->engine().renderSettings().defaultLighting = false;

        // What is behind everything is the game's to say as well, and not the editor's grey.
        editor->engine().renderSettings().backgroundColor = Vec3(1.0f, 0.0f, 0.0f);
        const Vec4 gameCorner = corner();
        CHECK(gameCorner.r > gameCorner.g + 0.5f);
        CHECK(gameCorner.r > editorCorner.r + 0.5f);
        // The editor's own view is as it was, whatever the game says.
        run(*editor, "viewport.show", R"({"camera": "editor"})");
        CHECK(corner().r == doctest::Approx(editorCorner.r).epsilon(0.02));
        CHECK(brightness(middle()) == doctest::Approx(editorCube).epsilon(0.02));
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("the editor playing the showcase matches the reference image") {
        EditorAppDesc desc = headlessDesc();
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditorForPicture(desc);
        run(*editor, "selection.set", R"({"entities": ["Stage/Clock"]})");
        REQUIRE(editor->runFrames(4).has_value());

        // Three quarters of a second of the game, a sixtieth at a time - the same game on
        // every machine. The viewport shows it through its camera, the bar says that it
        // plays, and the clock that is selected has ticked: its name says so in the
        // hierarchy and in the inspector, where its sound is seen to have been played.
        run(*editor, "play.start");
        REQUIRE(editor->runFrames(45).has_value());
        checkAgainstReference(unwrap(editor->engine().captureFrame()), "editor-playing-1280x720");
        run(*editor, "play.stop");
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("the editor plays the scene it shows, holds it, and puts it back when it stops") {
        EditorAppDesc desc = headlessDesc(UVec2(640, 360));
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        Engine& engine = editor->engine();
        const auto scene = [&] { return dumpJson(serializeScene(engine.world(), engine.components()), 2); };
        // The clock of the showcase writes the time into its name; the ball lies on the floor.
        const auto clockName = [&] {
            return run(*editor, "entity.get", R"({"entity": "10000000-0000-4000-8000-000000000038"})")["name"]
                .get<std::string>();
        };
        const auto ballHeight = [&] {
            return engine.world()
                .transform(engine.world().findById(*Uuid::parse("10000000-0000-4000-8000-00000000001b")))
                .position.y;
        };

        // Shown, not run.
        run(*editor, "editor.runFrames", R"({"count": 2})");
        const std::string before = scene();
        CHECK(run(*editor, "play.status") == json(R"({"state": "stopped", "frames": 0, "scriptProblems": []})"));
        CHECK(clockName() == "Clock");
        CHECK(editor->play().state() == PlayState::Stopped);
        CHECK(engine.audio().output() == AudioOutput::Offline);
        run(*editor, "selection.set", R"({"entities": ["Stage/Ball", "Stage/Clock"]})");

        // Played: time passes for the world, a sixtieth of a second a frame.
        CHECK(run(*editor, "play.start")["state"] == "playing");
        CHECK(editor->context().isPlaying());
        run(*editor, "editor.runFrames", R"({"count": 90})");
        Json status = run(*editor, "play.status");
        CHECK(status["state"] == "playing");
        CHECK(status["frames"] == 90);
        CHECK(status["scriptProblems"].empty());
        // The scripts run - the clock has ticked twice in a second and a half - the
        // simulation steps and holds its joints, and the lamp and the music are heard by
        // the camera.
        CHECK(clockName() == "Clock (2)");
        Json statistics = run(*editor, "editor.statistics");
        CHECK(statistics["scripts"]["instances"] == 8);
        CHECK(statistics["scripts"]["stopped"] == 0);
        CHECK(statistics["physics"]["steps"] == 1);
        CHECK(statistics["physics"]["joints"] == 5);
        CHECK(statistics["audio"]["sources"].get<int>() >= 2);
        CHECK(statistics["audio"]["listeners"] == 1);
        CHECK_FALSE(engine.audio().mixed().empty());

        // What is pressed reaches the game: "fire" makes the ball hop.
        const float rest = ballHeight();
        run(*editor, "input.inject", R"({"mouseButton": "Left", "down": true})");
        run(*editor, "editor.runFrames", R"({"count": 12})");
        run(*editor, "input.inject", R"({"mouseButton": "Left", "down": false})");
        CHECK(ballHeight() > rest + 0.15f);

        // A scene that is played is not replaced and not saved, and not played twice.
        for (const char* command : {"scene.new", "scene.save", "play.start"}) {
            CAPTURE(command);
            const Error error = refusal(*editor, command);
            CHECK(error.code == ErrorCode::InvalidState);
            CHECK(error.message.starts_with(std::string(command) + ": the scene is being played"));
        }
        CHECK(refusal(*editor, "scene.open", R"({"path": "scenes/annex.scene.json"})").code == ErrorCode::InvalidState);
        // It can be edited, as it runs - which is undone like any edit, and goes with
        // the game in any case.
        run(*editor, "entity.create", R"({"name": "Marker", "parent": "Stage"})");
        run(*editor, "component.set",
            R"({"entity": "Stage/Marker", "component": "Transform", "properties": {"position": [1, 2, 3]}})");
        run(*editor, "editor.undo");

        // Held: nothing moves on, nothing is heard, and no script runs.
        CHECK(run(*editor, "play.pause")["state"] == "paused");
        CHECK(editor->play().state() == PlayState::Paused);
        const std::string heldName = clockName();
        const uint64_t heldFrames = run(*editor, "play.status")["frames"].get<uint64_t>();
        CHECK(heldFrames == 102);
        run(*editor, "editor.runFrames", R"({"count": 60})");
        CHECK(clockName() == heldName);
        CHECK(run(*editor, "play.status")["frames"] == heldFrames);
        statistics = run(*editor, "editor.statistics");
        CHECK(statistics["scripts"]["instances"] == 0);
        CHECK(statistics["physics"]["steps"] == 0);
        CHECK(statistics["audio"]["voices"] == 0);
        // Held, the game is silence.
        CHECK_FALSE(engine.audio().mixed().empty());
        CHECK(std::ranges::all_of(engine.audio().mixed(), [](float sample) { return sample == 0.0f; }));
        CHECK(refusal(*editor, "play.pause").message == "play.pause: there is nothing to pause: the scene is paused");

        // A frame at a time: it goes on for as many as are asked for, and is held again.
        status = run(*editor, "play.step");
        CHECK(status["state"] == "paused");
        CHECK(status["frames"] == heldFrames + 1);
        status = run(*editor, "play.step", R"({"frames": 59})");
        CHECK(status["state"] == "paused");
        CHECK(status["frames"] == heldFrames + 60);
        // A second has passed for the game, and the clock has ticked twice more.
        CHECK(clockName() == "Clock (5)");
        run(*editor, "editor.runFrames", R"({"count": 5})");
        CHECK(run(*editor, "play.status")["frames"] == heldFrames + 60);
        CHECK(refusal(*editor, "play.step", R"({"frames": 0})").code == ErrorCode::OutOfRange);

        // Let go, it goes on; and only what is held is stepped or let go.
        CHECK(run(*editor, "play.resume")["state"] == "playing");
        run(*editor, "editor.runFrames", R"({"count": 30})");
        CHECK(clockName() == "Clock (6)");
        CHECK(refusal(*editor, "play.resume").message ==
              "play.resume: there is nothing to resume: the scene is playing");
        CHECK(refusal(*editor, "play.step").message ==
              "play.step: only a game that is paused is played frame by frame; the scene is playing");

        // Stopped: the scripts end and say so, and the scene is what it was - to the
        // letter, with what was selected still selected.
        {
            const test::LogCapture log(LogLevel::Info);
            status = run(*editor, "play.stop");
            CHECK(log.contains("Clock (6) stopped"));
            CHECK(log.count(LogLevel::Error) == 0);
        }
        CHECK(status["state"] == "stopped");
        CHECK(editor->play().state() == PlayState::Stopped);
        CHECK_FALSE(editor->context().isPlaying());
        CHECK(scene() == before);
        CHECK(clockName() == "Clock");
        const Json selected = run(*editor, "selection.get");
        REQUIRE(selected.size() == 2);
        CHECK(selected[0]["name"] == "Ball");
        CHECK(selected[1]["name"] == "Clock");
        CHECK_FALSE(editor->context().hasUnsavedChanges());
        // And it is a document again: shown, and not run.
        run(*editor, "editor.runFrames", R"({"count": 30})");
        CHECK(clockName() == "Clock");
        statistics = run(*editor, "editor.statistics");
        CHECK(statistics["scripts"]["scripts"] == 0);
        CHECK(statistics["physics"]["bodies"] == 19);
        CHECK(statistics["physics"]["joints"] == 0);
        CHECK(statistics["audio"]["voices"] == 0);
        for (const char* command : {"play.stop", "play.pause", "play.resume", "play.step"}) {
            CAPTURE(command);
            CHECK(refusal(*editor, command).code == ErrorCode::InvalidState);
        }
        CHECK(refusal(*editor, "play.stop").message == "play.stop: the scene is not being played");
        // And scene.validate has nothing to say about it.
        CHECK(run(*editor, "scene.validate")["problems"].empty());
        checkNoValidationIssues(engine.graphics());
    }

    TEST_CASE("a scene that is played again begins again, and one that leaves for another comes back") {
        EditorAppDesc desc = headlessDesc(UVec2(640, 360));
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        Engine& engine = editor->engine();
        const auto clockName = [&] {
            return run(*editor, "entity.get", R"({"entity": "10000000-0000-4000-8000-000000000038"})")["name"]
                .get<std::string>();
        };
        // An edit before it is played: playing must not cost it, nor count as another.
        run(*editor, "entity.rename", R"({"entity": "Stage/Glass pane", "name": "Window"})");
        REQUIRE(editor->context().hasUnsavedChanges());
        const std::string before = dumpJson(serializeScene(engine.world(), engine.components()), 2);

        // Twice in a row: the second time is like the first.
        for (int round = 0; round < 2; ++round) {
            CAPTURE(round);
            run(*editor, "play.start");
            run(*editor, "editor.runFrames", R"({"count": 45})");
            CHECK(clockName() == "Clock (1)");
            CHECK(run(*editor, "play.status")["frames"] == 45);
            run(*editor, "play.stop");
            CHECK(clockName() == "Clock");
            CHECK(dumpJson(serializeScene(engine.world(), engine.components()), 2) == before);
        }
        // The edit from before is still the one edit there is to take back.
        CHECK(editor->context().hasUnsavedChanges());
        CHECK(editor->context().history().undoCount() == 1);

        // Stopped while it is held, too.
        run(*editor, "play.start");
        run(*editor, "editor.runFrames", R"({"count": 10})");
        run(*editor, "play.pause");
        run(*editor, "play.stop");
        CHECK(dumpJson(serializeScene(engine.world(), engine.components()), 2) == before);

        // The game leaves for its annex when "travel" is pressed - which is T. That is
        // the game's business: what is edited is the scene it was started from, and
        // stopping brings that back.
        run(*editor, "play.start");
        run(*editor, "editor.runFrames", R"({"count": 3})");
        run(*editor, "input.inject", R"({"key": "T", "down": true})");
        run(*editor, "editor.runFrames", R"({"count": 3})");
        run(*editor, "input.inject", R"({"key": "T", "down": false})");
        CHECK(engine.world().sceneName() == "Annex");
        CHECK(engine.world().findByName("Clock").isNull());
        CHECK(run(*editor, "play.status")["state"] == "playing");
        run(*editor, "editor.runFrames", R"({"count": 10})");
        run(*editor, "play.stop");
        CHECK(engine.world().sceneName() == "Showcase");
        CHECK(dumpJson(serializeScene(engine.world(), engine.components()), 2) == before);
        // Stopped in the very frame the game asked for another scene, none is loaded
        // into what is a document again.
        run(*editor, "play.start");
        run(*editor, "editor.runFrames", R"({"count": 3})");
        run(*editor, "input.inject", R"({"key": "T", "down": true})");
        run(*editor, "editor.runFrames", R"({"count": 1})");
        run(*editor, "input.inject", R"({"key": "T", "down": false})");
        run(*editor, "play.stop");
        run(*editor, "editor.runFrames", R"({"count": 3})");
        CHECK(engine.world().sceneName() == "Showcase");
        CHECK(dumpJson(serializeScene(engine.world(), engine.components()), 2) == before);
        checkNoValidationIssues(engine.graphics());
    }

    TEST_CASE("the theme of the interface is chosen by command, and a person's choice is remembered") {
        const TempDirectory directory;
        const std::filesystem::path preferences = directory.path() / "Cordite" / "editor-preferences.json";
        {
            EditorAppDesc desc = headlessDesc(UVec2(640, 360));
            desc.preferencesFile = preferences;
            const std::unique_ptr<EditorApp> editor = createEditor(desc);
            // Nothing was chosen yet: the default, and no file.
            const Json described = run(*editor, "editor.theme");
            CHECK(described["theme"] == "dark");
            CHECK(described["themes"] == json(R"(["dark", "light"])"));
            CHECK_FALSE(fileExists(preferences));

            // A name is a name whatever its case; what is said back is the theme's own.
            CHECK(run(*editor, "editor.theme", R"({"theme": "Light"})")["theme"] == "light");
            CHECK(editor->engine().ui().theme() == UiTheme::Light);
            REQUIRE(editor->runFrames(1).has_value());
            const ImVec4 panel = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
            CHECK(panel.x == paletteOf(UiTheme::Light).panel.x);
            CHECK(panel.z == paletteOf(UiTheme::Light).panel.z);
            CHECK(unwrap(loadEditorPreferences(preferences)).theme == UiTheme::Light);

            // A theme that does not exist changes nothing.
            const Error unknown = refusal(*editor, "editor.theme", R"({"theme": "blue"})");
            CHECK(unknown.code == ErrorCode::InvalidArgument);
            CHECK(unknown.message.find("'blue'") != std::string::npos);
            CHECK(editor->engine().ui().theme() == UiTheme::Light);
            CHECK(unwrap(loadEditorPreferences(preferences)).theme == UiTheme::Light);
        }
        // The next session starts in it.
        {
            EditorAppDesc desc = headlessDesc(UVec2(640, 360));
            desc.preferencesFile = preferences;
            const std::unique_ptr<EditorApp> editor = createEditor(desc);
            CHECK(run(*editor, "editor.theme")["theme"] == "light");
            CHECK(editor->engine().ui().theme() == UiTheme::Light);
        }
        // Without a file for them, an editor starts with the defaults, whatever was chosen
        // elsewhere, and what is chosen in it goes nowhere.
        {
            const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(640, 360)));
            CHECK(run(*editor, "editor.theme")["theme"] == "dark");
            CHECK(run(*editor, "editor.theme", R"({"theme": "light"})")["theme"] == "light");
        }
        CHECK(unwrap(loadEditorPreferences(preferences)).theme == UiTheme::Light);
    }

    TEST_CASE("preferences that cannot be read are said, and the editor opens with the defaults") {
        const TempDirectory directory;
        const std::filesystem::path preferences = directory.path() / "editor-preferences.json";
        REQUIRE(writeTextFile(preferences, R"({"format": "cordite.preferences", "version": 1, "theme": "blue"})")
                    .has_value());

        const test::LogCapture log(LogLevel::Warn);
        EditorAppDesc desc = headlessDesc(UVec2(640, 360));
        desc.preferencesFile = preferences;
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        CHECK(run(*editor, "editor.theme")["theme"] == "dark");
        CHECK(log.contains("preferences are not used"));
        CHECK(log.contains("'blue'"));

        // Choosing a theme puts a file there that can be read.
        run(*editor, "editor.theme", R"({"theme": "light"})");
        CHECK(unwrap(loadEditorPreferences(preferences)).theme == UiTheme::Light);
    }

    TEST_CASE("the editor in the light theme matches the reference image") {
        EditorAppDesc desc = headlessDesc();
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditorForPicture(desc);
        run(*editor, "editor.theme", R"({"theme": "light"})");
        // A light, for an inspector with a list, a color, numbers with units and a row of
        // switches; and the assets in front, for rows of a tree with one of them chosen.
        run(*editor, "selection.set", R"({"entities": ["Stage/Lamp post/Arm/Head/Lamp"]})");
        run(*editor, "editor.focusPanel", R"({"panel": "assets"})");
        run(*editor, "assets.browse", R"({"select": "models/hoist.gltf#mesh/1"})");

        REQUIRE(editor->runFrames(4).has_value());
        checkAgainstReference(unwrap(editor->engine().captureFrame()), "editor-theme-light-1280x720");
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("what the gizmo does is chosen by command") {
        const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(640, 360)));
        const Json first = run(*editor, "viewport.gizmo");
        CHECK(first["mode"] == "translate");
        CHECK(first["space"] == "world");
        const Json turned = run(*editor, "viewport.gizmo", R"({"mode": "Rotate", "space": "local"})");
        CHECK(turned["mode"] == "rotate");
        CHECK(turned["space"] == "local");
        CHECK(editor->viewport().gizmoMode() == GizmoMode::Rotate);
        CHECK(editor->viewport().gizmoSpace() == GizmoSpace::Local);

        // What is wrong changes nothing, not even what was right beside it.
        const Error wrongSpace = refusal(*editor, "viewport.gizmo", R"({"mode": "scale", "space": "parent"})");
        CHECK(wrongSpace.code == ErrorCode::InvalidArgument);
        CHECK(wrongSpace.message.find("'parent'") != std::string::npos);
        CHECK(refusal(*editor, "viewport.gizmo", R"({"mode": "move"})").code == ErrorCode::InvalidArgument);
        CHECK(editor->viewport().gizmoMode() == GizmoMode::Rotate);
        CHECK(editor->viewport().gizmoSpace() == GizmoSpace::Local);
    }

    TEST_CASE("a handle of the gizmo is dragged with the pointer, and moves what is selected") {
        const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(640, 360)));
        run(*editor, "entity.create", R"({"name": "Box", "components": {"Transform": {"position": [0, 0, 0]}}})");
        run(*editor, "entity.create", R"({"name": "Lid", "parent": "Box",
                                          "components": {"Transform": {"position": [0, 1, 0]}}})");
        run(*editor, "viewport.lookAt", R"({"position": [0, 0, 10], "target": [0, 0, 0]})");
        const World& world = editor->context().world();
        const Entity box = world.findByName("Box");
        const Entity lid = world.findByName("Lid");
        REQUIRE_FALSE(lid.isNull());

        const auto pointerTo = [&](Vec2 at) {
            run(*editor, "input.inject", fmt::format(R"({{"mouseMove": [{}, {}]}})", at.x, at.y));
            REQUIRE(editor->runFrames(2).has_value());
        };
        const auto button = [&](bool down) {
            run(*editor, "input.inject", fmt::format(R"({{"mouseButton": "Left", "down": {}}})", down));
            REQUIRE(editor->runFrames(2).has_value());
        };
        // Where a point of the world is on the screen, in the pixels of the interface: the
        // picture is drawn at its origin, a pixel of it for a pixel of the interface.
        const auto onScreen = [&](const Vec3& point) {
            const ViewportPanel& viewport = editor->viewport();
            return viewport.imageOrigin() +
                   worldToScreen(viewport.view(), point, Vec2(viewport.targets().size())).value();
        };
        const auto frameOf = [&](Entity entity, GizmoMode mode) {
            const ViewportPanel& viewport = editor->viewport();
            return gizmoFrame(world.worldMatrix(entity), GizmoSpace::World, mode, viewport.view(),
                              Vec2(viewport.targets().size()));
        };

        // The box and the lid on it are both selected, the lid last: the gizmo is the lid's,
        // and moving it moves the box, which carries the lid.
        run(*editor, "selection.set", R"({"entities": ["Box", "Box/Lid"]})");
        REQUIRE(editor->runFrames(3).has_value());
        const GizmoFrame frame = frameOf(lid, GizmoMode::Translate);
        const Vec2 grab = onScreen(frame.origin + frame.x * frame.size * 0.7f);
        pointerTo(grab);
        button(true);
        // To the right and down: along X alone counts.
        pointerTo(grab + Vec2(60.0f, 25.0f));
        CHECK(editor->viewport().isDraggingGizmo());
        button(false);
        CHECK_FALSE(editor->viewport().isDraggingGizmo());

        const float moved = 60.0f * frame.size / kGizmoScreenSize;
        CHECK(world.transform(box).position.x == doctest::Approx(moved).epsilon(0.02));
        CHECK(world.transform(box).position.y == doctest::Approx(0.0f));
        CHECK(world.transform(box).position.z == doctest::Approx(0.0f));
        CHECK(world.transform(lid).position == Vec3(0.0f, 1.0f, 0.0f));
        // Letting go on the gizmo chose nothing, and the drag is one step to undo.
        CHECK(run(*editor, "selection.get").size() == 2);
        CHECK(editor->context().history().undoLabel() == "Edit Transform of 'Box'");
        run(*editor, "editor.undo");
        CHECK(world.transform(box).position == Vec3(0.0f));

        // The ring about Z faces the camera: from the right of the box to above it is a
        // quarter turn about Z.
        run(*editor, "selection.set", R"({"entities": ["Box"]})");
        run(*editor, "viewport.gizmo", R"({"mode": "rotate"})");
        REQUIRE(editor->runFrames(2).has_value());
        const GizmoFrame rings = frameOf(box, GizmoMode::Rotate);
        // Taken hold of a little below its right end, away from the other two rings, which are
        // seen edge on; let go of above it: a quarter turn the other way to the clock.
        const auto onRing = [](const GizmoFrame& of, float degrees) {
            const float angle = glm::radians(degrees);
            return of.origin + Vec3(std::cos(angle), std::sin(angle), 0.0f) * of.size;
        };
        pointerTo(onScreen(onRing(rings, -30.0f)));
        button(true);
        pointerTo(onScreen(onRing(rings, 60.0f)));
        button(false);
        CHECK_NEAR(world.transform(box).rotation * Vec3(1.0f, 0.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), 0.02f);
        CHECK(editor->context().history().undoLabel() == "Edit Transform of 'Box'");

        // What hangs under something turned turns in the world as its ring says: the lid of a
        // box tipped over about X, given a quarter turn about Z, has its X along the world's Y.
        run(*editor, "component.set", R"({"entity": "Box", "component": "Transform",
                                           "properties": {"rotation": [0.70710678, 0, 0, 0.70710678]}})");
        run(*editor, "selection.set", R"({"entities": ["Box/Lid"]})");
        REQUIRE(editor->runFrames(2).has_value());
        const GizmoFrame lidRings = frameOf(lid, GizmoMode::Rotate);
        pointerTo(onScreen(onRing(lidRings, -30.0f)));
        button(true);
        pointerTo(onScreen(onRing(lidRings, 60.0f)));
        button(false);
        CHECK_NEAR(glm::normalize(Vec3(world.worldMatrix(lid)[0])), Vec3(0.0f, 1.0f, 0.0f), 0.03f);
        CHECK(editor->context().history().undoLabel() == "Edit Transform of 'Lid'");

        // And it moves along the world's axes as the gizmo shows them, whatever its parent's turn.
        run(*editor, "viewport.gizmo", R"({"mode": "translate"})");
        REQUIRE(editor->runFrames(2).has_value());
        const Vec3 lidWas = Vec3(world.worldMatrix(lid)[3]);
        const GizmoFrame lidArrows = frameOf(lid, GizmoMode::Translate);
        const Vec2 lidGrab = onScreen(lidArrows.origin + lidArrows.x * lidArrows.size * 0.7f);
        pointerTo(lidGrab);
        button(true);
        pointerTo(lidGrab + Vec2(40.0f, 0.0f));
        button(false);
        const Vec3 lidIs = Vec3(world.worldMatrix(lid)[3]);
        CHECK(lidIs.x - lidWas.x == doctest::Approx(40.0f * lidArrows.size / kGizmoScreenSize).epsilon(0.02));
        CHECK(lidIs.y == doctest::Approx(lidWas.y).epsilon(0.001));
        CHECK(lidIs.z == doctest::Approx(lidWas.z).epsilon(0.001));

        // A click on a handle that moves nothing is the gizmo's too: it does not choose what is
        // behind it, and is no step of the history.
        const size_t steps = editor->context().history().undoCount();
        const Vec2 handle = onScreen(Vec3(world.worldMatrix(lid)[3]) + lidArrows.x * lidArrows.size * 0.7f);
        pointerTo(handle);
        button(true);
        button(false);
        CHECK(run(*editor, "selection.get").size() == 1);
        CHECK(editor->context().history().undoCount() == steps);

        // Pressed beside every handle, in a corner of the picture, the pointer selects as it
        // always did: nothing is there.
        pointerTo(editor->viewport().imageOrigin() + Vec2(6.0f, Vec2(editor->viewport().targets().size()).y - 6.0f));
        button(true);
        button(false);
        CHECK(run(*editor, "selection.get").empty());
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("F frames what is selected, and Alt with the left button turns the camera around it") {
        const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(640, 360)));
        run(*editor, "entity.create", R"({"name": "Far", "components": {"Transform": {"position": [30, 2, -20]}}})");
        const Vec3 far(30.0f, 2.0f, -20.0f);
        const EditorCamera& camera = editor->camera();
        const auto key = [&](const char* name, bool down) {
            run(*editor, "input.inject", fmt::format(R"({{"key": "{}", "down": {}}})", name, down));
            REQUIRE(editor->runFrames(1).has_value());
        };
        const auto looksAt = [&](const Vec3& point) {
            return glm::dot(glm::normalize(point - camera.position()), camera.forward()) > 0.9999f;
        };
        REQUIRE(editor->runFrames(2).has_value());
        CHECK_FALSE(looksAt(far));

        // Nothing selected: nothing to frame.
        key("F", true);
        key("F", false);
        CHECK_FALSE(looksAt(far));
        CHECK(refusal(*editor, "viewport.frame").code == ErrorCode::InvalidArgument);

        run(*editor, "selection.set", R"({"entities": ["Far"]})");
        key("F", true);
        key("F", false);
        CHECK(looksAt(far));
        CHECK(glm::distance(camera.position(), far) == doctest::Approx(camera.focusDistance()));
        // What is framed is a metre around a point.
        CHECK(camera.focusDistance() < 2.0f);

        // Around it, with Alt held: the left button turns the camera and chooses nothing.
        const ViewportPanel& viewport = editor->viewport();
        const Vec2 middle = viewport.imageOrigin() + Vec2(viewport.targets().size()) * 0.5f;
        const Vec3 stoodAt = camera.position();
        run(*editor, "input.inject", fmt::format(R"({{"mouseMove": [{}, {}]}})", middle.x, middle.y));
        key("LeftAlt", true);
        run(*editor, "input.inject", R"({"mouseButton": "Left", "down": true})");
        REQUIRE(editor->runFrames(2).has_value());
        run(*editor, "input.inject", fmt::format(R"({{"mouseMove": [{}, {}]}})", middle.x + 60.0f, middle.y));
        REQUIRE(editor->runFrames(2).has_value());
        // Alt is claimed while it turns the camera, so that letting go of it does not take
        // the keyboard to the menus.
        CHECK(camera.isOrbiting());
        CHECK(ImGui::GetKeyOwner(ImGuiMod_Alt) != ImGuiKeyOwner_NoOwner);
        run(*editor, "input.inject", R"({"mouseButton": "Left", "down": false})");
        key("LeftAlt", false);
        REQUIRE(editor->runFrames(1).has_value());
        CHECK(ImGui::GetKeyOwner(ImGuiMod_Alt) == ImGuiKeyOwner_NoOwner);
        CHECK(glm::distance(camera.position(), stoodAt) > 0.1f);
        CHECK(looksAt(far));
        CHECK(glm::distance(camera.position(), far) == doctest::Approx(camera.focusDistance()));
        CHECK(run(*editor, "selection.get").size() == 1);

        // A mesh is framed by its size: a cube ten metres across is seen from further away
        // than a point.
        run(*editor, "entity.create",
            fmt::format(
                R"({{"name": "Block", "components": {{"Transform": {{"position": [30, 2, -20], "scale": [10, 10, 10]}}, "MeshRenderer": {{"mesh": "{}"}}}}}})",
                builtinMeshId(BuiltinMesh::Cube).toString()));
        run(*editor, "selection.set", R"({"entities": ["Block"]})");
        key("F", true);
        key("F", false);
        CHECK(looksAt(far));
        CHECK(camera.focusDistance() > 10.0f);
        run(*editor, "selection.set", R"({"entities": ["Far"]})");

        // The command frames the selection as F does - also what was made a moment ago,
        // before a frame has placed it.
        run(*editor, "viewport.lookAt", R"({"position": [0, 5, 10], "target": [0, 0, 0]})");
        CHECK_FALSE(looksAt(far));
        run(*editor, "viewport.frame");
        CHECK(looksAt(far));
        run(*editor, "entity.create", R"({"name": "New", "components": {"Transform": {"position": [-12, 3, 7]}}})");
        run(*editor, "selection.set", R"({"entities": ["New"]})");
        run(*editor, "viewport.frame");
        CHECK(looksAt(Vec3(-12.0f, 3.0f, 7.0f)));
        // A size goes with an entity; the selection has its own.
        const Error sized = refusal(*editor, "viewport.frame", R"({"radius": 3})");
        CHECK(sized.code == ErrorCode::InvalidArgument);
        CHECK(sized.message.find("'radius'") != std::string::npos);
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("a file of an asset is moved by command, and the project follows its start scene") {
        const TempDirectory directory;
        Project project = unwrap(createProject(directory.path(), "Moving"));
        REQUIRE(writeTextFile(project.assetDirectory() / "scenes" / "start.scene.json",
                              R"({"format": "cordite.scene", "version": 1, "name": "Start", "entities": []})")
                    .has_value());
        project.settings.startScene = "scenes/start.scene.json";
        REQUIRE(saveProject(project).has_value());
        EditorAppDesc desc = headlessDesc(UVec2(640, 360));
        desc.project = directory.path();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        REQUIRE(editor->context().world().sceneName() == "Start");

        const Json moved =
            run(*editor, "asset.move", R"({"asset": "scenes/start.scene.json", "to": "levels/first.scene.json"})");
        CHECK(moved["from"] == "scenes/start.scene.json");
        CHECK(moved["to"] == "levels/first.scene.json");
        CHECK(moved["startScene"] == "levels/first.scene.json");
        CHECK(moved["mentions"].empty());
        // The project file says so, and the open scene is saved where it went.
        CHECK(unwrap(loadProject(directory.path())).settings.startScene == "levels/first.scene.json");
        CHECK(editor->context().scenePath() == project.assetDirectory() / "levels" / "first.scene.json");
        CHECK(run(*editor, "asset.get", R"({"asset": "levels/first.scene.json"})")["type"] == "scene");

        // Another asset leaves the project as it is; what is refused changes nothing.
        REQUIRE(writeTextFile(project.assetDirectory() / "scripts" / "door.lua", "return {}\n").has_value());
        run(*editor, "asset.refresh");
        CHECK_FALSE(
            run(*editor, "asset.move", R"({"asset": "scripts/door.lua", "to": "door.lua"})").contains("startScene"));
        CHECK(refusal(*editor, "asset.move", R"({"asset": "door.lua", "to": "door.txt"})").code ==
              ErrorCode::InvalidArgument);
        CHECK(unwrap(loadProject(directory.path())).settings.startScene == "levels/first.scene.json");
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("files dropped onto the editor are brought into the assets, where the chosen asset is") {
        const TempDirectory directory;
        const Project project = unwrap(createProject(directory.path() / "project", "Dropping"));
        const Image image = unwrap(Image::create(2, 2, ImageFormat::Rgba8));
        REQUIRE(ensureDirectory(project.assetDirectory() / "textures").has_value());
        REQUIRE(image.savePng(project.assetDirectory() / "textures" / "wall.png").has_value());
        REQUIRE(image.savePng(directory.path() / "rust.png").has_value());
        REQUIRE(image.savePng(directory.path() / "moss.png").has_value());
        EditorAppDesc desc = headlessDesc(UVec2(640, 360));
        desc.project = project.directory;
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        const auto drop = [&](const std::filesystem::path& file) {
            Json files = Json::object();
            files["drop"] = Json::array({pathToUtf8(file)});
            run(*editor, "input.inject", files.dump());
            REQUIRE(editor->runFrames(2).has_value());
        };

        // Nothing chosen: to the top.
        drop(directory.path() / "rust.png");
        CHECK(run(*editor, "asset.get", R"({"asset": "rust.png"})")["type"] == "texture");
        // Beside what is chosen.
        run(*editor, "assets.browse", R"({"select": "textures/wall.png"})");
        drop(directory.path() / "moss.png");
        CHECK(run(*editor, "asset.get", R"({"asset": "textures/moss.png"})")["type"] == "texture");
        drop(directory.path() / "rust.png");
        CHECK(fileExists(project.assetDirectory() / "textures" / "rust.png"));
        // Once more, onto a place that holds it now: that is said, and the editor goes on.
        drop(directory.path() / "rust.png");
        CHECK(run(*editor, "asset.list", R"({"type": "texture"})")["assets"].size() == 4);
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("a picture of an asset is drawn by command, and beside the rows of the Assets panel") {
        const TempDirectory directory;
        const Project project = unwrap(createProject(directory.path() / "project", "Pictures"));
        const std::filesystem::path assets = project.assetDirectory();
        REQUIRE(writeTextFile(assets / "materials" / "red.material.json",
                              R"({"format": "cordite.material", "version": 1, "baseColor": [1, 0, 0, 1]})")
                    .has_value());
        REQUIRE(writeTextFile(assets / "scripts" / "door.lua", "return {}\n").has_value());
        // Red, all of it: what the picture of it must hold.
        std::vector<std::byte> redPixels;
        for (int pixel = 0; pixel < 8; ++pixel) {
            redPixels.insert(redPixels.end(), {std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}});
        }
        const Image texture = unwrap(Image::fromPixels(4, 2, ImageFormat::Rgba8, std::move(redPixels)));
        REQUIRE(texture.savePng(assets / "textures" / "wide.png").has_value());
        GltfDocument model;
        model.meshes.push_back({.mesh = makeBox()});
        model.nodes.push_back({.name = "Box", .mesh = 0});
        model.roots = {0};
        REQUIRE(exportGltf(model, assets / "models" / "box.glb").has_value());
        // Twenty metres across, a long way from where the camera of a picture would be
        // without looking: it is seen whole all the same.
        model.meshes[0].mesh = makeBox(Vec3(20.0f));
        model.nodes[0].transform.position = Vec3(100.0f, 0.0f, -50.0f);
        REQUIRE(exportGltf(model, assets / "models" / "big.glb").has_value());
        REQUIRE(writeTextFile(assets / "materials" / "broken.material.json", "not json").has_value());
        // Large enough for the panel to have room for a picture beside its rows.
        EditorAppDesc desc = headlessDesc(UVec2(1280, 720));
        desc.project = project.directory;
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        // A prefab, made as a person makes one.
        run(*editor, "entity.create",
            fmt::format(R"({{"name": "Post", "components": {{"MeshRenderer": {{"mesh": "{}"}}}}}})",
                        builtinMeshId(BuiltinMesh::Cylinder).toString()));
        run(*editor, "prefab.create", R"({"entity": "Post", "path": "prefabs/post.prefab.json"})");

        const std::filesystem::path file = directory.path() / "pictures" / "picture.png";
        const auto picture = [&](std::string_view asset, int64_t size) {
            Json parameters = Json::object();
            parameters["asset"] = asset;
            parameters["path"] = pathToUtf8(file);
            parameters["size"] = size;
            const Json saved = run(*editor, "asset.preview", parameters.dump());
            CHECK(saved["width"] == size);
            CHECK(saved["height"] == size);
            return unwrap(Image::load(file));
        };
        // Something in the middle, against a background that is the same in every corner -
        // but for the dither of a finished picture, a level at the most.
        const auto drawsSomething = [](const Image& image) {
            const uint32_t last = image.width() - 1;
            const Vec4 corner = image.pixel(0, 0);
            for (const Vec4& other : {image.pixel(last, 0), image.pixel(0, last), image.pixel(last, last)}) {
                CHECK(glm::length(Vec3(other) - Vec3(corner)) < 2.5f / 255.0f);
            }
            const Vec4 middle = image.pixel(last / 2, last / 2);
            CHECK(glm::length(Vec3(middle) - Vec3(corner)) > 0.05f);
            return middle;
        };
        drawsSomething(picture(builtinMeshId(BuiltinMesh::Cube).toString(), 64));
        drawsSomething(picture("models/box.glb", 48));
        drawsSomething(picture("models/big.glb", 48));
        drawsSomething(picture("prefabs/post.prefab.json", 32));
        // What hangs under something is placed by it: a beam eight times as long as it is
        // thick, lying along the X axis of a parent that is turned upright, is drawn standing.
        run(*editor, "entity.create",
            R"({"name": "Tower", "components": {"Transform": {"rotation": [0, 0, 0.7071068, 0.7071068]}}})");
        run(*editor, "entity.create",
            fmt::format(
                R"({{"name": "Beam", "parent": "Tower", "components": {{"Transform": {{"scale": [8, 1, 1]}}, "MeshRenderer": {{"mesh": "{}"}}}}}})",
                builtinMeshId(BuiltinMesh::Cube).toString()));
        run(*editor, "prefab.create", R"({"entity": "Tower", "path": "prefabs/tower.prefab.json"})");
        const Image tower = picture("prefabs/tower.prefab.json", 64);
        const Vec3 background(tower.pixel(0, 0));
        uint32_t across = 0;
        uint32_t down = 0;
        for (uint32_t step = 0; step < 64; ++step) {
            across += glm::length(Vec3(tower.pixel(step, 32)) - background) > 0.05f ? 1 : 0;
            down += glm::length(Vec3(tower.pixel(32, step)) - background) > 0.05f ? 1 : 0;
        }
        CHECK(down > 2 * across);
        // A material is drawn on a ball, and is seen to be red.
        const Vec4 red = drawsSomething(picture("materials/red.material.json", 64));
        CHECK(red.r > red.g + 0.1f);
        CHECK(red.r > red.b + 0.1f);

        // A texture is its own picture; a script has none; sizes have their limits.
        const auto refused = [&](std::string_view asset, int64_t size) {
            Json parameters = Json::object();
            parameters["asset"] = asset;
            parameters["path"] = pathToUtf8(file);
            parameters["size"] = size;
            return refusal(*editor, "asset.preview", parameters.dump());
        };
        CHECK(refused("textures/wide.png", 64).message.find("whose file is its picture") != std::string::npos);
        CHECK(refused("scripts/door.lua", 64).message.find("which has no picture") != std::string::npos);
        CHECK(refused("models/box.glb", 8).code == ErrorCode::OutOfRange);
        CHECK(refused("models/box.glb", 4096).code == ErrorCode::OutOfRange);

        // The panel draws what is chosen, and a texture as it is.
        const AssetPreview& preview = editor->assetBrowser().preview();
        run(*editor, "editor.focusPanel", R"({"panel": "assets"})");
        run(*editor, "assets.browse", R"({"select": "materials/red.material.json"})");
        REQUIRE(editor->runFrames(2).has_value());
        CHECK(preview.asset() == unwrap(editor->context().resolveAsset("materials/red.material.json"))->id);
        CHECK(preview.picture() != nullptr);
        // Asked in every frame, it draws once - and again when the assets are read again.
        const uint64_t made = preview.attempts();
        REQUIRE(editor->runFrames(3).has_value());
        CHECK(preview.attempts() == made);
        run(*editor, "asset.refresh");
        REQUIRE(editor->runFrames(2).has_value());
        CHECK(preview.attempts() == made + 1);
        // What cannot be pictured is tried once, not in every frame.
        run(*editor, "assets.browse", R"({"select": "materials/broken.material.json"})");
        REQUIRE(editor->runFrames(3).has_value());
        CHECK(preview.picture() == nullptr);
        CHECK(preview.attempts() == made + 2);
        run(*editor, "assets.browse", R"({"select": "textures/wide.png"})");
        // In the very frame it is first shown, it is on the GPU: the scene of that frame was
        // drawn before the panel asked for it, and sent nothing of it.
        REQUIRE(editor->runFrames(1).has_value());
        REQUIRE(preview.picture() != nullptr);
        const TexturePixels shown = unwrap(readTexture(editor->engine().graphics(), preview.picture()));
        REQUIRE(shown.data.size() >= 4);
        CHECK(shown.data[0] == std::byte{255});
        CHECK(shown.data[1] == std::byte{0});
        CHECK(shown.data[2] == std::byte{0});
        CHECK(preview.picture()->getDesc().width == 4);
        CHECK(preview.picture()->getDesc().height == 2);
        // What has no picture leaves the last one as it was.
        run(*editor, "assets.browse", R"({"select": "scripts/door.lua"})");
        REQUIRE(editor->runFrames(2).has_value());
        CHECK(preview.picture()->getDesc().width == 4);

        // Asked directly, a preview refuses what it cannot picture, and draws nothing for it.
        AssetPreview direct;
        Engine& engine = editor->engine();
        nvrhi::IDevice* device = engine.graphics().device();
        const nvrhi::CommandListHandle commandList = device->createCommandList();
        commandList->open();
        const auto refusal = [&](const Uuid& asset, uint32_t size) {
            const Status shown = direct.show(engine.sceneRenderer(), device, commandList, editor->context().assets(),
                                             editor->context().components(), asset, size, ShadowQuality::Medium);
            REQUIRE_FALSE(shown.has_value());
            CHECK(direct.picture() == nullptr);
            return shown.error();
        };
        const Uuid cube = builtinMeshId(BuiltinMesh::Cube);
        CHECK(refusal(cube, AssetPreview::kMinSize - 1).code == ErrorCode::InvalidArgument);
        CHECK(refusal(cube, AssetPreview::kMaxSize + 1).code == ErrorCode::InvalidArgument);
        CHECK(refusal(Uuid::generate(), 64).code == ErrorCode::NotFound);
        const Error script = refusal(unwrap(editor->context().resolveAsset("scripts/door.lua"))->id, 64);
        CHECK(script.code == ErrorCode::InvalidArgument);
        CHECK(script.message == "scripts/door.lua is a script, which has no picture");
        commandList->close();
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("the engine asks for the permutations of its shaders on its list, every one, and no other") {
        EditorAppDesc desc = headlessDesc(UVec2(640, 360));
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        // Every way a picture is made: each debug view, each quality of everything, edges
        // smoothed and not, and the scene played - its figures in their poses.
        for (const RenderDebugView view : kRenderDebugViews) {
            run(*editor, "viewport.debugView", fmt::format(R"({{"view": "{}"}})", nameOf(view)));
            REQUIRE(editor->runFrames(1).has_value());
        }
        run(*editor, "viewport.debugView", R"({"view": "none"})");
        for (const std::string_view quality : {"off", "low", "medium", "high"}) {
            run(*editor, "viewport.quality",
                fmt::format(R"({{"shadows": "{0}", "effects": "{0}", "postProcess": "{0}"}})", quality));
            REQUIRE(editor->runFrames(1).has_value());
        }
        run(*editor, "viewport.quality", R"({"antiAliasing": "off"})");
        REQUIRE(editor->runFrames(1).has_value());
        run(*editor, "play.start");
        REQUIRE(editor->runFrames(5).has_value());
        run(*editor, "play.stop");

        const auto keysOf = [](const std::vector<ShaderRequest>& requests) {
            std::set<std::string> keys;
            for (const ShaderRequest& request : requests) {
                keys.insert(request.key().toHex());
            }
            return keys;
        };
        const std::vector<ShaderRequest> listed = engineShaderRequests();
        const std::vector<ShaderRequest> asked = editor->engine().shaders().requested();
        const std::set<std::string> listedKeys = keysOf(listed);
        const std::set<std::string> askedKeys = keysOf(asked);
        // A game that is exported has these, and no compiler for another.
        for (const ShaderRequest& request : asked) {
            CAPTURE(request.describe());
            CHECK(listedKeys.contains(request.key().toHex()));
        }
        // And what is listed is asked for: the list has nothing that no longer is.
        for (const ShaderRequest& request : listed) {
            CAPTURE(request.describe());
            CHECK(askedKeys.contains(request.key().toHex()));
        }
    }

    TEST_CASE("files of the assets that another program changes are read again, and a played script goes on with "
              "its new text") {
        const TempDirectory directory;
        const Project project = unwrap(createProject(directory.path() / "watched", "Watched"));
        const std::filesystem::path script = project.assetDirectory() / "scripts" / "namer.lua";
        const auto namer = [](std::string_view name) {
            return fmt::format("return {{\n    update = function(self)\n        self.entity:setName(\"{}\")\n    "
                               "end,\n}}\n",
                               name);
        };
        REQUIRE(writeTextFile(script, namer("first")).has_value());

        EditorAppDesc desc = headlessDesc(UVec2(320, 180));
        desc.project = project.directory;
        desc.watchAssets = true;
        desc.assetWatchInterval = std::chrono::milliseconds(10);
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        const AssetDatabase& database = editor->context().assets().database();
        Json named = Json::object();
        named["name"] = "Named";
        named["components"]["Script"]["script"] = run(*editor, "asset.get", R"({"asset": "scripts/namer.lua"})")["id"];
        const std::string id = run(*editor, "entity.create", named.dump())["id"].get<std::string>();
        const auto nameOf = [&] {
            Json entity = Json::object();
            entity["entity"] = id;
            return run(*editor, "entity.get", entity.dump())["name"].get<std::string>();
        };
        // Frames until the name is what is expected, or ten seconds have passed: the files
        // are looked at on a thread of their own.
        const auto runUntilNamed = [&](std::string_view name) {
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (nameOf() != name && std::chrono::steady_clock::now() < end) {
                run(*editor, "editor.runFrames");
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            return nameOf();
        };

        run(*editor, "play.start");
        CHECK(runUntilNamed("first") == "first");
        // Written by another program while the game runs.
        REQUIRE(writeTextFile(script, namer("second, written while it ran")).has_value());
        CHECK(runUntilNamed("second, written while it ran") == "second, written while it ran");
        CHECK(editor->engine().scripts().problems().empty());
        run(*editor, "play.stop");

        // What the editor writes itself is no reason to read the assets again: a new scene
        // file is read by the save, and one saved again is not read at all.
        Json save = Json::object();
        save["path"] = pathToUtf8(project.assetDirectory() / "scenes" / "watched.scene.json");
        run(*editor, "scene.save", save.dump());
        run(*editor, "editor.runFrames");
        const uint64_t revision = database.revision();
        run(*editor, "entity.rename", fmt::format(R"({{"entity": "{}", "name": "Renamed"}})", id));
        run(*editor, "scene.save");
        for (int frame = 0; frame < 20; ++frame) {
            run(*editor, "editor.runFrames");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(database.revision() == revision);
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("an editor reads the assets again only when asked, unless it watches them") {
        const TempDirectory directory;
        const Project project = unwrap(createProject(directory.path() / "unwatched", "Unwatched"));
        EditorAppDesc desc = headlessDesc(UVec2(320, 180));
        desc.project = project.directory;
        desc.assetWatchInterval = std::chrono::milliseconds(10);
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        const uint64_t revision = editor->context().assets().database().revision();
        REQUIRE(writeTextFile(project.assetDirectory() / "scripts" / "late.lua", "return {}\n").has_value());
        for (int frame = 0; frame < 20; ++frame) {
            run(*editor, "editor.runFrames");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        CHECK(editor->context().assets().database().revision() == revision);
        run(*editor, "asset.refresh");
        CHECK(editor->context().assets().database().revision() != revision);
    }

    TEST_CASE("a new project is made by command, and the editor closes so that it is opened instead") {
        const TempDirectory directory;
        EditorAppDesc desc = headlessDesc(UVec2(320, 180));
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        Json parameters = Json::object();
        parameters["path"] = pathToUtf8(directory.path() / "fresh");
        parameters["name"] = "Fresh";

        // What is not saved would be lost; what is played is no scene to leave.
        run(*editor, "entity.create", R"({"name": "Unsaved"})");
        CHECK(refusal(*editor, "project.new", parameters.dump()).message.find("not saved") != std::string::npos);
        run(*editor, "editor.undo");
        run(*editor, "play.start");
        CHECK(refusal(*editor, "project.new", parameters.dump()).message.find("played") != std::string::npos);
        run(*editor, "play.stop");
        CHECK_FALSE(std::filesystem::exists(directory.path() / "fresh"));
        CHECK_FALSE(editor->projectToOpen().has_value());

        const Json made = run(*editor, "project.new", parameters.dump());
        CHECK(made["name"] == "Fresh");
        CHECK(made["startScene"] == "scenes/main.scene.json");
        REQUIRE(editor->projectToOpen().has_value());
        CHECK(*editor->projectToOpen() == directory.path() / "fresh");
        CHECK_FALSE(unwrap(editor->runFrame()));
        // There is a project there now.
        CHECK(refusal(*editor, "project.new", parameters.dump()).code == ErrorCode::AlreadyExists);
    }

    TEST_CASE("an editor opens another project by command, and nothing that is no project") {
        const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(320, 180)));
        const TempDirectory directory;
        Json nowhere = Json::object();
        nowhere["path"] = pathToUtf8(directory.path());
        CHECK(refusal(*editor, "project.open", nowhere.dump()).code == ErrorCode::NotFound);
        CHECK_FALSE(editor->projectToOpen().has_value());

        Json showcase = Json::object();
        showcase["path"] = pathToUtf8(showcaseProject() / "project.cordite");
        CHECK(run(*editor, "project.open", showcase.dump())["name"] == "Showcase");
        REQUIRE(editor->projectToOpen().has_value());
        CHECK(*editor->projectToOpen() == showcaseProject());
        CHECK_FALSE(unwrap(editor->runFrame()));
    }

    TEST_CASE("a new project opens with its scene, which the game's camera shows lit") {
        const TempDirectory directory;
        const Project made = unwrap(createStarterProject(directory.path() / "lit", "Lit"));
        EditorAppDesc desc = headlessDesc(UVec2(320, 180));
        desc.project = made.directory;
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        CHECK(editor->context().scenePath() == made.startScenePath());
        run(*editor, "viewport.show", R"({"camera": "game"})");
        const Image picture = unwrap(editor->captureViewport(UVec2(320, 180)));
        // The box in the middle, lit by the sun, and the floor below it, in its shadow or
        // not: neither is black, and they differ.
        const Vec4 box = picture.pixel(160, 95);
        const Vec4 floor = picture.pixel(160, 170);
        CHECK(box.r + box.g + box.b > 0.3f);
        CHECK(floor.r + floor.g + floor.b > 0.15f);
        CHECK(glm::length(Vec3(box) - Vec3(floor)) > 0.02f);
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("a directory of the assets is moved by command, and the project's start scene and controls follow") {
        const TempDirectory directory;
        Project project = unwrap(createStarterProject(directory.path() / "moving", "Moving"));
        REQUIRE(writeTextFile(project.assetDirectory() / "input" / "controls.input.json",
                              R"({"format": "cordite.input", "version": 1, "actions": [], "axes": []})")
                    .has_value());
        project.settings.inputMap = "input/controls.input.json";
        REQUIRE(saveProject(project).has_value());
        EditorAppDesc desc = headlessDesc(UVec2(320, 180));
        desc.project = project.directory;
        const std::unique_ptr<EditorApp> editor = createEditor(desc);

        const Json scenes = run(*editor, "asset.moveDirectory", R"({"from": "scenes", "to": "levels/first"})");
        CHECK(scenes["to"] == "levels/first");
        REQUIRE(scenes["assets"].size() == 1);
        CHECK(scenes["assets"][0]["to"] == "levels/first/main.scene.json");
        CHECK(scenes["startScene"] == "levels/first/main.scene.json");
        CHECK(editor->context().scenePath() == project.assetDirectory() / "levels" / "first" / "main.scene.json");
        CHECK(run(*editor, "asset.moveDirectory", R"({"from": "input", "to": "config"})")["inputMap"] ==
              "config/controls.input.json");
        const Project moved = unwrap(loadProject(project.directory));
        CHECK(moved.settings.startScene == "levels/first/main.scene.json");
        CHECK(moved.settings.inputMap == "config/controls.input.json");

        CHECK(refusal(*editor, "asset.moveDirectory", R"({"from": "levels", "to": "levels/first/deeper"})").code ==
              ErrorCode::InvalidArgument);
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("a sound of the assets is heard in the editor while its scene is not played, and stops on request") {
        EditorAppDesc desc = headlessDesc(UVec2(320, 180));
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        const AudioSystem& audio = editor->engine().audio();
        // Whether anything at all was heard in the frames that run.
        const auto heardIn = [&](int frames) {
            bool heard = false;
            for (int frame = 0; frame < frames; ++frame) {
                run(*editor, "editor.runFrames");
                heard = heard || std::ranges::any_of(audio.mixed(), [](float sample) { return sample != 0.0f; });
            }
            return heard;
        };
        // The showcase has sounds that play when its scene starts, which an editor holds.
        CHECK_FALSE(heardIn(3));

        const Json listened = run(*editor, "asset.listen", R"({"asset": "sounds/hop.wav"})");
        CHECK(listened["playing"] == true);
        // A quarter of a second, to the frame: 5513 at 22050 frames a second.
        CHECK(listened["seconds"].get<double>() == doctest::Approx(0.25).epsilon(1.0e-4));
        CHECK(listened["channels"] == 1);
        CHECK(listened["sampleRate"] == 22050);
        const Uuid hop = unwrap(editor->context().resolveAsset("sounds/hop.wav"))->id;
        CHECK(audio.previewing() == hop);
        CHECK(heardIn(3));
        // Stopped by the line of the panel's Stop button.
        editor->runCommandLine(listenCommand(std::nullopt));
        CHECK_FALSE(audio.previewing().has_value());
        CHECK_FALSE(heardIn(3));
        // Played by the line of its Play button, to its end - a quarter of a second - and
        // not again.
        editor->runCommandLine(listenCommand(hop));
        CHECK(audio.previewing() == hop);
        CHECK(heardIn(3));
        run(*editor, "editor.runFrames", R"({"count": 20})");
        CHECK_FALSE(audio.previewing().has_value());
        CHECK_FALSE(heardIn(3));

        const Error script = refusal(*editor, "asset.listen", R"({"asset": "scripts/hop.lua"})");
        CHECK(script.code == ErrorCode::InvalidArgument);
        CHECK(script.message == "asset.listen: scripts/hop.lua is a script, which is not heard");
        CHECK(refusal(*editor, "asset.listen", R"({"asset": "sounds/none.wav"})").code == ErrorCode::NotFound);
        CHECK(refusal(*editor, "asset.listen", R"({"asset": ""})").code == ErrorCode::InvalidArgument);
        CHECK(refusal(*editor, "asset.listen", R"({"asset": 5})").code == ErrorCode::InvalidArgument);
        // Nothing to stop: nothing happens.
        CHECK(run(*editor, "asset.listen")["playing"] == false);
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("a sound chosen in the Assets panel is played and stopped with the button beside the rows") {
        EditorAppDesc desc = headlessDesc(UVec2(1280, 720));
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        const AudioSystem& audio = editor->engine().audio();
        const Uuid hop = unwrap(editor->context().resolveAsset("sounds/hop.wav"))->id;
        run(*editor, "assets.browse", R"({"select": "sounds/hop.wav"})");
        REQUIRE(editor->runFrames(2).has_value());

        // The button is the last thing in the area beside the rows, under what is said of
        // the sound, in the pixels of the interface.
        const auto pressButton = [&] {
            const ImGuiWindow* area = nullptr;
            for (const ImGuiWindow* window : GImGui->Windows) {
                if (std::string_view(window->Name).find("###Assets/picture_") != std::string_view::npos) {
                    area = window;
                }
            }
            REQUIRE(area != nullptr);
            const Vec2 at(area->DC.CursorStartPos.x + 10.0f, area->DC.CursorMaxPos.y - ImGui::GetFrameHeight() * 0.5f);
            run(*editor, "input.inject", fmt::format(R"({{"mouseMove": [{}, {}]}})", at.x, at.y));
            REQUIRE(editor->runFrames(2).has_value());
            run(*editor, "input.inject", R"({"mouseButton": "Left", "down": true})");
            REQUIRE(editor->runFrames(2).has_value());
            run(*editor, "input.inject", R"({"mouseButton": "Left", "down": false})");
            REQUIRE(editor->runFrames(2).has_value());
        };
        // While another sound is heard, the button plays this one in its place.
        run(*editor, "asset.listen", R"({"asset": "sounds/call.wav"})");
        CHECK(audio.previewing() != hop);
        pressButton();
        CHECK(audio.previewing() == hop);
        // Now it says Stop, where it said Play.
        pressButton();
        CHECK_FALSE(audio.previewing().has_value());
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("a game is made of the project by command, and not of a scene that is not saved") {
        const TempDirectory directory;
        // Without a project there is nothing to make a game of.
        {
            const std::unique_ptr<EditorApp> bare = createEditor(headlessDesc(UVec2(640, 360)));
            CHECK(refusal(*bare, "project.export", R"({"path": "anywhere"})").code == ErrorCode::InvalidState);
        }
        EditorAppDesc desc = headlessDesc(UVec2(640, 360));
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        Json parameters = Json::object();
        parameters["path"] = pathToUtf8(directory.path() / "Showcase");
        // What is not saved would not be in it.
        run(*editor, "entity.create", R"({"name": "Unsaved"})");
        CHECK(refusal(*editor, "project.export", parameters.dump()).message.find("not saved") != std::string::npos);
        run(*editor, "editor.undo");
        // With the shaders this editor compiles its own from.
        const Json made = run(*editor, "project.export", parameters.dump());
        CHECK(made["program"] ==
              pathToUtf8(directory.path() / "Showcase" / pathFromUtf8(executableFileName("Showcase"))));
        CHECK(made["files"].get<uint64_t>() > 100);
        CHECK(fileExists(directory.path() / "Showcase" / "project.cordite"));
        CHECK(fileExists(directory.path() / "Showcase" / "licenses" / "THIRD-PARTY.md"));
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("the application adds its commands to the catalogue") {
        const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(640, 360)));
        for (const char* name :
             {"editor.runFrames",  "editor.screenshot",  "editor.statistics",   "editor.showPanel", "editor.exit",
              "editor.focusPanel", "editor.theme",       "assets.browse",       "viewport.lookAt",  "viewport.frame",
              "viewport.camera",   "viewport.debugView", "viewport.gizmo",      "entity.create",    "scene.save",
              "play.start",        "play.stop",          "play.pause",          "play.resume",      "play.step",
              "play.status",       "asset.move",         "asset.import",        "asset.preview",    "project.export",
              "project.new",       "project.open",       "asset.moveDirectory", "asset.listen"}) {
            CAPTURE(name);
            const CommandDesc* command = editor->commands().find(name);
            REQUIRE(command != nullptr);
            CHECK(command->description.size() > 20);
            for (const CommandParameter& parameter : command->parameters) {
                CHECK(parameter.description.size() > 5);
            }
        }
    }

    TEST_CASE("editing through the interface thread keeps rendering valid") {
        EditorAppDesc desc = headlessDesc(UVec2(960, 540));
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        const size_t entityCount = editor->context().world().entityCount();

        // A burst of edits with frames in between, as when an agent works on a scene:
        // entities come and go, the selection and the camera move, the history rewinds.
        for (int round = 0; round < 5; ++round) {
            run(*editor, "entity.create", fmt::format(R"({{"name": "Probe {}", "parent": "Stage"}})", round));
            run(*editor, "selection.set", fmt::format(R"({{"entities": ["Probe {}"]}})", round));
            REQUIRE(editor->runFrames(2).has_value());
            run(*editor, "component.add", fmt::format(R"({{"entity": "Probe {}", "component": "Camera"}})", round));
            run(*editor, "viewport.frame", fmt::format(R"({{"entity": "Probe {}"}})", round));
            REQUIRE(editor->runFrames(1).has_value());
            run(*editor, "editor.undo");
            run(*editor, "entity.delete", fmt::format(R"({{"entities": ["Probe {}"]}})", round));
            REQUIRE(editor->runFrames(1).has_value());
        }
        CHECK(editor->context().world().entityCount() == entityCount);
        checkNoValidationIssues(editor->engine().graphics());
    }

#if defined(CORDITE_ENABLE_ASSERTS)
    TEST_CASE("capturing the viewport in the middle of a frame is a programmer error") {
        const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(640, 360)));
        REQUIRE(unwrap(editor->engine().beginFrame()));
        CHECK_THROWS_AS((void)editor->captureViewport(UVec2(64, 64)), test::AssertionFailure);
        REQUIRE(editor->engine().endFrame().has_value());
    }
#endif
}
