#include "cordite/editor/EditorApp.h"

#include "cordite/animation/AnimationComponents.h"
#include "cordite/assets/ImageCompare.h"
#include "cordite/core/FileSystem.h"

#include "render/RenderTestSupport.h"
#include "support/MathTestSupport.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
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

/// A JSON string literal holding a path, quoted and escaped.
std::string quoted(const std::filesystem::path& path) {
    return Json(pathToUtf8(path)).dump();
}

} // namespace

TEST_SUITE("EditorApp") {
    TEST_CASE("the editor showing the showcase scene matches the reference image") {
        EditorAppDesc desc = headlessDesc();
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
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
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
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
    }

    TEST_CASE("a project that is not one is reported, and a scene of choice replaces the start scene") {
        {
            EditorAppDesc missing = headlessDesc(UVec2(640, 360));
            missing.project = test::sourceDirectory() / "samples";
            const Result<std::unique_ptr<EditorApp>> editor = EditorApp::create(missing);
            REQUIRE_FALSE(editor.has_value());
            CHECK(editor.error().code == ErrorCode::NotFound);
            CHECK(editor.error().message.find("cordite.project.json") != std::string::npos);
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

    TEST_CASE("editor.statistics reports what the last frame cost") {
        EditorAppDesc desc = headlessDesc(UVec2(640, 360));
        desc.project = showcaseProject();
        const std::unique_ptr<EditorApp> editor = createEditor(desc);
        REQUIRE(editor->runFrames(3).has_value());

        const Json statistics = run(*editor, "editor.statistics");
        CHECK(statistics["frame"] == 2);
        CHECK(statistics["cpuMilliseconds"].get<double>() > 0.0);
        CHECK((statistics["gpuMilliseconds"].is_null() || statistics["gpuMilliseconds"].get<double>() > 0.0));

        // The viewport is the one view; it draws the showcase, some of it instanced.
        const Json& scene = statistics["scene"];
        CHECK(scene["views"] == 1);
        CHECK(scene["meshRenderers"] == 23);
        // The cable of the hoist is posed by its animator: one skeleton of six joints.
        CHECK(statistics["animation"]["animators"] == 1);
        CHECK(statistics["animation"]["joints"] == 6);
        CHECK(scene["skinned"]["meshes"] == 1);
        CHECK(scene["skinned"]["jointMatrices"] == 6);
        CHECK(scene["drawCalls"].get<uint32_t>() > 0);
        CHECK(scene["drawCalls"].get<uint32_t>() < scene["instances"].get<uint32_t>());
        CHECK(scene["triangles"].get<uint64_t>() > 1000);
        // The sun and the three lamps of the showcase, each lamp in the lists of the
        // clusters it reaches.
        CHECK(scene["lights"]["count"] == 4);
        CHECK(scene["lights"]["listEntries"].get<uint32_t>() > 3);
        CHECK(scene["lights"]["dropped"] == 0);
        // Three cascades for the sun, one map for the lamp, six for the wall light.
        CHECK(scene["shadows"]["maps"] == 10);
        CHECK(scene["shadows"]["lightsWithout"] == 0);
        // Passes over the whole picture: the two of the ambient occlusion, the lighting,
        // and what finishes the picture as the showcase says - five levels of bloom down
        // and up, the exposure, the final pass, the smoothing of its edges.
        CHECK(scene["screenPasses"] == 15);
        // The ambient occlusion is worked out for one pixel in four of the viewport, in
        // two half floats each.
        const UVec2 viewportSize = editor->viewport().targets().size();
        CHECK(scene["ambientOcclusion"]["bytes"] ==
              uint64_t{(viewportSize.x + 1) / 2} * ((viewportSize.y + 1) / 2) * 4);
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
        CHECK(run(*editor, "editor.statistics")["scene"]["views"] == 1);
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
        for (const char* preset : {"low", "high", "medium"}) {
            CAPTURE(preset);
            answer = run(*editor, "viewport.quality", fmt::format(R"({{"preset": "{}"}})", preset));
            CHECK(answer["preset"] == std::string(preset));
            CHECK(answer["shadows"] == std::string(preset));
            CHECK(answer["effects"] == std::string(preset));
            CHECK(answer["postProcess"] == std::string(preset));
            const Image shown = unwrap(editor->captureViewport(UVec2(320, 180)));
            const ImageComparison difference =
                unwrap(compareImages(medium, shown, {.channelTolerance = 24, .maxDifferingRatio = 0.03}));
            CHECK(difference.matches);
        }

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

    TEST_CASE("the application adds its commands to the catalogue") {
        const std::unique_ptr<EditorApp> editor = createEditor(headlessDesc(UVec2(640, 360)));
        for (const char* name : {"editor.runFrames", "editor.screenshot", "editor.statistics", "editor.showPanel",
                                 "editor.exit", "viewport.lookAt", "viewport.frame", "viewport.camera",
                                 "viewport.debugView", "entity.create", "scene.save"}) {
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
