#include "cordite/editor/EditorApp.h"

#include "cordite/core/FileSystem.h"

#include "render/RenderTestSupport.h"
#include "support/MathTestSupport.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace cordite;
using cordite::test::checkAgainstReference;
using cordite::test::checkNoValidationIssues;
using cordite::test::TempDirectory;
using cordite::test::unwrap;

namespace {

std::filesystem::path showcaseScene() {
    return test::sourceDirectory() / "samples" / "showcase" / "showcase.scene.json";
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
    REQUIRE_MESSAGE(editor.has_value(), editor.error().describe());
    return std::move(*editor);
}

Json json(std::string_view text) {
    return unwrap(parseJson(text));
}

/// Runs a command that is expected to succeed.
Json run(EditorApp& editor, std::string_view command, std::string_view parameters = "{}") {
    const Result<Json> result = editor.commands().execute(command, json(parameters));
    REQUIRE_MESSAGE(result.has_value(), result.error().describe());
    return *result;
}

/// A JSON string literal holding a path, quoted and escaped.
std::string quoted(const std::filesystem::path& path) {
    return Json(pathToUtf8(path)).dump();
}

} // namespace

TEST_SUITE("EditorApp") {
    TEST_CASE("the editor showing the showcase scene matches the reference image") {
        EditorAppDesc desc = headlessDesc();
        desc.scene = showcaseScene();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        run(*editor, "selection.set", R"({"entities": ["Cameras/Main camera"]})");

        // Windows settle into their docked places over the first frames.
        REQUIRE(editor->runFrames(4).has_value());
        CHECK(editor->engine().frameIndex() == 4);

        checkAgainstReference(unwrap(editor->engine().captureFrame()), "editor-showcase-1280x720");
        checkNoValidationIssues(editor->engine().graphics());
    }

    TEST_CASE("the viewport alone matches its reference image") {
        EditorAppDesc desc = headlessDesc();
        desc.scene = showcaseScene();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        run(*editor, "selection.set", R"({"entities": ["Crate"]})");
        run(*editor, "viewport.lookAt", R"({"position": [7, 5, 9], "target": [0, 0.5, -1]})");

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

    TEST_CASE("the application adds its commands to the catalogue") {
        const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(640, 360)));
        for (const char* name : {"editor.runFrames", "editor.screenshot", "editor.exit", "viewport.lookAt",
                                 "viewport.frame", "viewport.camera", "entity.create", "scene.save"}) {
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
        desc.scene = showcaseScene();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);

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
        CHECK(editor->context().world().entityCount() == 9);
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
