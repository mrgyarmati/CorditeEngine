#include "cordite/editor/EditorApp.h"

#include "cordite/core/FileSystem.h"
#include "cordite/core/Log.h"
#include "cordite/core/StringUtils.h"
#include "cordite/editor/AssetCommands.h"
#include "cordite/editor/AssetMove.h"
#include "cordite/editor/EditorActions.h"
#include "cordite/editor/EditorCommands.h"
#include "cordite/editor/EditorFeedback.h"
#include "cordite/editor/EditorPreferences.h"
#include "cordite/editor/EditorToolbar.h"
#include "cordite/editor/InputCommands.h"
#include "cordite/editor/PhysicsCommands.h"
#include "cordite/editor/ScriptCommands.h"
#include "cordite/math/MathJson.h"
#include "cordite/rhi/TextureReadback.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <span>
#include <utility>

namespace cordite {
namespace {

using Type = CommandParameterType;

constexpr const char* kEditorName = "Cordite Editor";
constexpr const char* kFileDialogId = "Scene file";
/// The windows that are closed until someone asks for them, as editor.showPanel names them.
constexpr const char* kStatisticsPanelName = "statistics";
constexpr const char* kInterfaceDemoPanelName = "interfaceDemo";
/// The panels that are always there, as editor.focusPanel names them, with what their
/// windows are known by.
constexpr std::array<std::pair<const char*, const char*>, 5> kDockedPanels = {{
    {"hierarchy", HierarchyPanel::kWindowId},
    {"inspector", InspectorPanel::kWindowId},
    {"viewport", ViewportPanel::kWindowId},
    {"console", ConsolePanel::kWindowId},
    {"assets", AssetBrowserPanel::kWindowId},
}};
/// Where the arrangement of the windows is remembered, inside a .cordite directory.
constexpr const char* kLayoutFileName = "editor-layout.ini";

/// Frames rendered before a screenshot of the whole editor, so that windows that were
/// just opened or resized have settled into their layout.
constexpr uint32_t kSettleFrames = 3;

/// Limits for commands that take sizes and counts from a script.
constexpr double kMaxScriptFrames = 100000.0;
constexpr double kMaxCaptureDimension = 16384.0;

/// The default layout: which share of the window each side panel takes.
constexpr float kHierarchyWidthShare = 0.18f;
constexpr float kInspectorWidthShare = 0.30f;
constexpr float kConsoleHeightShare = 0.26f;

Json describeError(const Error& error) {
    Json description = Json::object();
    description["code"] = nameOf(error.code);
    description["message"] = error.message;
    return description;
}

/// Reads a script step and runs it. `commandName` receives the name as soon as it is
/// known, so that a failure can be reported against it.
Result<Json> runStep(const CommandRegistry& commands, const Json& step, std::string& commandName) {
    if (!step.is_object()) {
        return makeError(ErrorCode::InvalidArgument, "a script step must be an object, found {}", jsonTypeName(step));
    }
    for (const auto& [key, value] : step.items()) {
        if (key != "command" && key != "params") {
            return makeError(ErrorCode::InvalidArgument,
                             "unknown member '{}' in a script step; a step has 'command' and 'params'", key);
        }
    }

    const auto command = step.find("command");
    if (command == step.end() || !command->is_string()) {
        return makeError(ErrorCode::InvalidArgument, "a script step needs a 'command' that names the command to run");
    }
    commandName = command->get<std::string>();

    const auto parameters = step.find("params");
    return commands.execute(commandName, parameters != step.end() ? *parameters : Json(nullptr));
}

/// Reads a whole number given as a command parameter and checks it against a range.
Result<uint32_t> countParameter(const Json& parameters, const char* name, double fallback, double maximum) {
    const auto value = jsonGetOr<double>(parameters, name, fallback);
    if (value < 1.0 || value > maximum) {
        return makeError(ErrorCode::OutOfRange, "'{}' must be between 1 and {}, got {}", name, maximum, value);
    }
    return static_cast<uint32_t>(value);
}

/// Docks the panels where they go when no layout was remembered from an earlier session.
void buildDefaultLayout(ImGuiID dockSpaceId) {
    ImGui::DockBuilderRemoveNode(dockSpaceId);
    ImGui::DockBuilderAddNode(dockSpaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockSpaceId, ImGui::GetMainViewport()->WorkSize);

    // Each split takes a side panel off what is left; the viewport gets the remainder.
    ImGuiID center = dockSpaceId;
    const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, kHierarchyWidthShare, nullptr, &center);
    const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, kInspectorWidthShare, nullptr, &center);
    const ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, kConsoleHeightShare, nullptr, &center);

    ImGui::DockBuilderDockWindow(HierarchyPanel::kWindowId, left);
    ImGui::DockBuilderDockWindow(InspectorPanel::kWindowId, right);
    // The assets share the place of the console. They are put there when their window
    // first appears (drawInterface), which is also what places them in a layout that was
    // remembered from before there was an Assets panel: one way for both.
    ImGui::DockBuilderDockWindow(ConsolePanel::kWindowId, bottom);
    ImGui::DockBuilderDockWindow(ViewportPanel::kWindowId, center);
    ImGui::DockBuilderFinish(dockSpaceId);
}

} // namespace

Result<std::unique_ptr<EditorApp>> EditorApp::create(const EditorAppDesc& desc) {
    std::unique_ptr<EditorApp> app(new EditorApp());

    EngineDesc engineDesc = desc.engine;
    std::filesystem::path scene = desc.scene;
    // The controls of the project, if it names any.
    std::filesystem::path inputMap;
    // What a person leaves behind lives in the .cordite directory of the project, or of
    // the working directory when there is no project.
    std::filesystem::path stateDirectory = ".cordite";
    if (!desc.project.empty()) {
        CORDITE_TRY_ASSIGN(Project project, loadProject(desc.project));
        engineDesc.assetDirectory = project.assetDirectory();
        stateDirectory = project.cacheDirectory();
        if (scene.empty()) {
            scene = project.startScenePath();
        }
        inputMap = project.inputMapPath();
        app->m_project = std::move(project);
    }
    if (desc.rememberLayout && engineDesc.uiSettingsFile.empty()) {
        engineDesc.uiSettingsFile = stateDirectory / kLayoutFileName;
    }
    // An editor is where assets get their ids.
    engineDesc.writeAssetMetaFiles = true;
    if (engineDesc.applicationName == EngineDesc().applicationName) {
        engineDesc.applicationName = kEditorName;
    }
    if (engineDesc.window.title == WindowDesc().title) {
        engineDesc.window.title = kEditorName;
    }
    CORDITE_TRY_ASSIGN(app->m_engine, Engine::create(engineDesc));

    // What the person at the editor chose about it. Preferences that cannot be read are no
    // reason not to open a project: they are said, and the defaults are used.
    if (!desc.preferencesFile.empty()) {
        app->m_preferencesFile = desc.preferencesFile;
        if (Result<EditorPreferences> preferences = loadEditorPreferences(desc.preferencesFile)) {
            app->m_preferences = *preferences;
        } else {
            CORDITE_LOG_WARN("Editor", "the editor's preferences are not used: {}", preferences.error().message);
        }
    }
    app->m_engine->ui().setTheme(app->m_preferences.theme);

    app->m_context =
        std::make_unique<EditorContext>(app->m_engine->world(), app->m_engine->components(), app->m_engine->assets());
    app->m_play = std::make_unique<PlayMode>(*app->m_engine, *app->m_context);
    CORDITE_TRY(registerEditorCommands(app->m_commands, *app->m_context));
    CORDITE_TRY(registerPhysicsCommands(app->m_commands, *app->m_context, app->m_engine->physics()));
    // Read as strictly as the scene is: a project whose controls cannot be read does not open.
    if (!inputMap.empty()) {
        CORDITE_TRY_ASSIGN(InputMap map, loadInputMap(inputMap));
        app->m_engine->actions().setMap(std::move(map));
    }
    CORDITE_TRY(registerInputCommands(app->m_commands, app->m_engine->input(), app->m_engine->actions(), inputMap));
    CORDITE_TRY(registerScriptCommands(app->m_commands, app->m_engine->scripts()));
    CORDITE_TRY(registerAssetBrowserCommands(app->m_commands, *app->m_context, app->m_assets.browser()));
    CORDITE_TRY(app->registerApplicationCommands());
    CORDITE_TRY(app->registerPlayCommands());
    CORDITE_TRY(app->registerProjectCommands());
    CORDITE_TRY_ASSIGN(app->m_viewport, ViewportPanel::create(*app->m_engine));

    if (!scene.empty()) {
        CORDITE_TRY(app->m_context->openScene(scene));
    }
    return app;
}

EditorApp::~EditorApp() = default;

Result<bool> EditorApp::runFrame() {
    // Lines entered in the console run here, between frames, where a command is free to
    // render frames of its own.
    const std::vector<std::string> lines = std::exchange(m_submittedLines, {});
    for (const std::string& line : lines) {
        runCommandLine(line);
    }

    CORDITE_TRY_ASSIGN(const bool keepRunning, m_engine->beginFrame());
    if (!keepRunning) {
        return false;
    }
    importDroppedFiles();
    // The scene is a document here, not a game that runs: nothing in it plays on, and
    // what is animated is shown at the times its animators hold - until it is played.
    m_engine->updateWorld(m_play->clock());
    drawInterface();
    CORDITE_TRY(m_engine->endFrame());
    m_play->frameEnded();

    updateWindowTitle();
    return true;
}

Status EditorApp::runFrames(uint32_t count) {
    for (uint32_t frame = 0; frame < count; ++frame) {
        CORDITE_TRY_ASSIGN(const bool keepRunning, runFrame());
        if (!keepRunning) {
            break;
        }
    }
    return {};
}

Json EditorApp::runScript(const Json& script) {
    Json outcome = Json::object();
    outcome["ok"] = true;
    outcome["results"] = Json::array();

    if (!script.is_array()) {
        outcome["ok"] = false;
        outcome["error"] =
            describeError({ErrorCode::InvalidArgument,
                           fmt::format("a script must be an array of steps, found {}", jsonTypeName(script))});
        return outcome;
    }

    for (const Json& step : script) {
        std::string commandName;
        const Result<Json> result = runStep(m_commands, step, commandName);

        Json entry = Json::object();
        entry["command"] = commandName;
        entry["ok"] = result.has_value();
        if (result) {
            entry["result"] = *result;
        } else {
            entry["error"] = describeError(result.error());
        }
        outcome["results"].push_back(std::move(entry));

        if (!result) {
            // Later steps were written assuming this one worked.
            outcome["ok"] = false;
            break;
        }
    }
    return outcome;
}

void EditorApp::runCommandLine(std::string_view line) {
    const std::string_view trimmed = trim(line);
    const size_t nameEnd = trimmed.find_first_of(" \t");
    const std::string_view name = trimmed.substr(0, nameEnd);
    const std::string_view rest =
        nameEnd == std::string_view::npos ? std::string_view() : trim(trimmed.substr(nameEnd));
    if (name.empty()) {
        return;
    }

    Json parameters = nullptr;
    if (!rest.empty()) {
        Result<Json> parsed = parseJson(rest, "parameters");
        if (!parsed) {
            CORDITE_LOG_ERROR(kConsoleLogCategory, "{}: the parameters are not valid JSON ({})", name,
                              parsed.error().message);
            return;
        }
        parameters = std::move(*parsed);
    }

    const Result<Json> result = m_commands.execute(name, parameters);
    if (!result) {
        CORDITE_LOG_ERROR(kConsoleLogCategory, "{}", result.error().message);
    } else if (result->is_null()) {
        CORDITE_LOG_INFO(kConsoleLogCategory, "{}: done", name);
    } else {
        CORDITE_LOG_INFO(kConsoleLogCategory, "{}:\n{}", name, dumpJson(*result, 2));
    }
}

Result<Image> EditorApp::captureViewport(UVec2 size) {
    CORDITE_ASSERT(!m_engine->isFrameOpen(), "the viewport cannot be captured during a frame");
    if (m_engine->isFrameOpen()) {
        return makeError(ErrorCode::InvalidState, "the viewport cannot be captured during a frame");
    }

    m_engine->updateWorld(WorldClock::Stopped);

    // Rendered with a command list of its own, so that capturing neither needs nor
    // disturbs a frame of the editor.
    nvrhi::IDevice* device = m_engine->graphics().device();
    const nvrhi::CommandListHandle commandList = device->createCommandList();
    commandList->open();
    // A picture of this moment: no time has passed since the viewport was last drawn.
    const Status rendered = m_viewport->render(device, commandList, *m_context, size, 0.0f, m_engine->timeSeconds());
    commandList->close();
    CORDITE_TRY(rendered);
    device->executeCommandList(commandList);

    CORDITE_TRY_ASSIGN(TexturePixels pixels, readTexture(m_engine->graphics(), m_viewport->targets().display()));
    return Image::fromPixels(pixels.width, pixels.height, ImageFormat::Rgba8, std::move(pixels.data));
}

Status EditorApp::registerApplicationCommands() {
    CORDITE_TRY(m_commands.add({
        .name = "editor.runFrames",
        .description = "Lets the editor run for a number of frames, so that what was changed is drawn.",
        .parameters = {{"count", Type::Integer, "How many frames to run. Defaults to 1.", false}},
        .handler = [this](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const uint32_t count, countParameter(parameters, "count", 1.0, kMaxScriptFrames));
            CORDITE_TRY(runFrames(count));
            Json result = Json::object();
            result["frameIndex"] = m_engine->frameIndex();
            return result;
        },
    }));

    CORDITE_TRY(m_commands.add({
        .name = "editor.screenshot",
        .description = "Saves a PNG image of the editor window, or of the scene alone as the viewport shows it. "
                       "Returns the path and the size of the image.",
        .parameters = {{"path", Type::String, "Where to save the image. Missing directories are created."},
                       {"viewportOnly", Type::Boolean,
                        "Save the scene without the interface around it. Defaults to false.", false},
                       {"width", Type::Integer,
                        "With viewportOnly: the image width in pixels. Defaults to the "
                        "width of the editor window.",
                        false},
                       {"height", Type::Integer,
                        "With viewportOnly: the image height in pixels. Defaults to the "
                        "height of the editor window.",
                        false}},
        .handler = [this](const Json& parameters) -> Result<Json> {
            const std::filesystem::path path = pathFromUtf8(parameters.at("path").get<std::string>());

            Result<Image> image = makeError(ErrorCode::Unknown, "no image");
            if (jsonGetOr<bool>(parameters, "viewportOnly", false)) {
                const UVec2 windowSize = m_engine->renderSize();
                CORDITE_TRY_ASSIGN(const uint32_t width,
                                   countParameter(parameters, "width", windowSize.x, kMaxCaptureDimension));
                CORDITE_TRY_ASSIGN(const uint32_t height,
                                   countParameter(parameters, "height", windowSize.y, kMaxCaptureDimension));
                image = captureViewport(UVec2(width, height));
            } else {
                CORDITE_TRY(runFrames(kSettleFrames));
                image = m_engine->captureFrame();
            }
            CORDITE_TRY(image);

            if (const std::filesystem::path directory = path.parent_path(); !directory.empty()) {
                CORDITE_TRY(ensureDirectory(directory));
            }
            CORDITE_TRY(image->savePng(path));

            Json result = Json::object();
            result["path"] = pathToUtf8(path);
            result["width"] = image->width();
            result["height"] = image->height();
            return result;
        },
    }));

    CORDITE_TRY(m_commands.add({
        .name = "editor.statistics",
        .description = "Reports what the last frame cost: milliseconds on the CPU and on the GPU (null where "
                       "the GPU cannot time its work, and before its first measurement has arrived), what the "
                       "scene renderer drew, the lights it drew it with (their number, the entries of the "
                       "cluster lists they are in, and how many were left out because a view holds only so "
                       "many), their shadow maps, the cube maps of environments (how many the frame had "
                       "to make - none, but for the first frame an environment is seen in - and the "
                       "memory of those held), and what it holds on the GPU. Run a few frames first: a "
                       "frame that loads assets is not typical.",
        .parameters = {},
        .handler = [this](const Json& /*parameters*/) -> Result<Json> {
            return describeFrameStatistics(m_engine->statistics());
        },
    }));

    CORDITE_TRY(m_commands.add({
        .name = "editor.showPanel",
        .description = "Opens or closes one of the editor's optional windows, so that it is part of what a "
                       "screenshot shows. Returns which of them are open.",
        .parameters = {{"panel", Type::String, "The window: \"statistics\" or \"interfaceDemo\"."},
                       {"visible", Type::Boolean, "Whether it is to be open. Defaults to true.", false}},
        .handler = [this](const Json& parameters) -> Result<Json> {
            const std::string panel = parameters.at("panel").get<std::string>();
            const bool visible = jsonGetOr<bool>(parameters, "visible", true);
            if (panel == kStatisticsPanelName) {
                m_showStatistics = visible;
            } else if (panel == kInterfaceDemoPanelName) {
                m_showInterfaceDemo = visible;
            } else {
                return makeError(ErrorCode::InvalidArgument, "there is no panel '{}'; use \"{}\" or \"{}\"", panel,
                                 kStatisticsPanelName, kInterfaceDemoPanelName);
            }
            Json result = Json::object();
            result[kStatisticsPanelName] = m_showStatistics;
            result[kInterfaceDemoPanelName] = m_showInterfaceDemo;
            return result;
        },
    }));

    CORDITE_TRY(m_commands.add({
        .name = "editor.theme",
        .description = "Says which theme the interface is drawn in - its colors - and, given one, draws it in that "
                       "one from the next frame on. A person's choice is remembered for every project they open; "
                       "a batch run remembers nothing. Returns the theme and the names of all of them.",
        .parameters = {{"theme", Type::String, "The theme to change to: \"dark\" or \"light\".", false}},
        .handler = [this](const Json& parameters) -> Result<Json> {
            if (const auto name = parameters.find("theme"); name != parameters.end()) {
                const std::string text = name->get<std::string>();
                const std::optional<UiTheme> theme = uiThemeFromName(text);
                if (!theme) {
                    return makeError(ErrorCode::InvalidArgument, "there is no theme '{}'; use \"dark\" or \"light\"",
                                     text);
                }
                chooseTheme(*theme);
            }
            return describeTheme();
        },
    }));

    CORDITE_TRY(m_commands.add({
        .name = "editor.focusPanel",
        .description = "Brings one of the editor's panels to the front of those it shares its place with, as a "
                       "click on its tab does, so that it is what a screenshot shows. It is in front once a "
                       "frame has been drawn.",
        .parameters = {{"panel", Type::String,
                        "The panel: \"hierarchy\", \"inspector\", \"viewport\", \"console\" or \"assets\"."}},
        .handler = [this](const Json& parameters) -> Result<Json> {
            const std::string panel = parameters.at("panel").get<std::string>();
            const auto found =
                std::ranges::find(kDockedPanels, std::string_view(panel), &std::pair<const char*, const char*>::first);
            if (found == kDockedPanels.end()) {
                return makeError(ErrorCode::InvalidArgument,
                                 "there is no panel '{}'; use \"hierarchy\", \"inspector\", \"viewport\", \"console\" "
                                 "or \"assets\"",
                                 panel);
            }
            m_panelToFocus = found->second;
            Json result = Json::object();
            result["panel"] = panel;
            return result;
        },
    }));

    CORDITE_TRY(m_commands.add({
        .name = "editor.exit",
        .description = "Closes the editor after the current frame. Unsaved changes are discarded.",
        .parameters = {},
        .handler = [this](const Json& /*parameters*/) -> Result<Json> {
            m_engine->requestExit();
            return Json(nullptr);
        },
    }));

    CORDITE_TRY(m_commands.add({
        .name = "viewport.lookAt",
        .description = "Places the viewport camera at a position, looking at a target point.",
        .parameters = {{"position", Type::Array, "Where the camera goes: [x, y, z] in metres."},
                       {"target", Type::Array, "The point it looks at: [x, y, z] in metres."}},
        .handler = [this](const Json& parameters) -> Result<Json> {
            Vec3 position(0.0f);
            Vec3 target(0.0f);
            if (!fromJson(parameters.at("position"), position) || !fromJson(parameters.at("target"), target)) {
                return makeError(ErrorCode::InvalidArgument, "'position' and 'target' must be arrays of 3 numbers");
            }
            camera().lookAt(position, target);
            return Json(nullptr);
        },
    }));

    CORDITE_TRY(m_commands.add({
        .name = "viewport.frame",
        .description = "Moves the viewport camera so that an entity is in view, keeping the viewing direction - "
                       "or, without one, what is selected, as F does. What is framed is what the camera "
                       "orbits about with Alt and the left button.",
        .parameters = {{"entity", Type::String, "The entity, by id, path or name. Without it, the selection.", false},
                       {"radius", Type::Number,
                        "The size of the area to show around the entity, in metres. Defaults to 1. Only with "
                        "`entity`: the selection is framed by its own size.",
                        false}},
        .handler = [this](const Json& parameters) -> Result<Json> {
            if (!parameters.contains("entity")) {
                if (parameters.contains("radius")) {
                    return makeError(ErrorCode::InvalidArgument,
                                     "'radius' is the size around an entity; the selection is framed by its own size");
                }
                if (!m_viewport->frameSelection(*m_context)) {
                    return makeError(ErrorCode::InvalidArgument, "nothing is selected to frame; name an entity");
                }
                return Json(nullptr);
            }
            CORDITE_TRY_ASSIGN(const Entity entity,
                               m_context->resolveEntity(parameters.at("entity").get<std::string>()));
            const auto radius = jsonGetOr<float>(parameters, "radius", 1.0f);
            if (!(radius > 0.0f)) {
                return makeError(ErrorCode::OutOfRange, "'radius' must be positive, got {}", radius);
            }
            camera().frame(Vec3(m_context->world().computeWorldMatrix(entity)[3]), radius);
            return Json(nullptr);
        },
    }));

    CORDITE_TRY(m_commands.add({
        .name = "viewport.show",
        .description = "Chooses through whose eyes the viewport shows the scene, and what it draws over it. "
                       "`camera` is \"editor\" - the editor's own camera, with the grid, the markers and the "
                       "frames around what is selected - or \"game\": the camera the game renders from, the "
                       "first enabled primary one, and nothing but what a player sees; a scene without "
                       "such a camera is shown through the editor's. `colliders` draws the collider and "
                       "the joint of every entity that has one, and the capsule of every character, as "
                       "lines (those of selected entities and of what hangs below them are always drawn). "
                       "Returns what is asked for now, and `showsGame`: whether the last picture was the "
                       "game's. Without a parameter it changes nothing.",
        .parameters = {{"camera", Type::String, "Whose camera to look through: \"editor\" or \"game\".", false},
                       {"colliders", Type::Boolean,
                        "Draw the colliders, the joints and the characters of all entities.", false}},
        .handler = [this](const Json& parameters) -> Result<Json> {
            if (const auto name = jsonGetOr<std::string>(parameters, "camera", ""); !name.empty()) {
                if (name != nameOf(ViewportCamera::Editor) && name != nameOf(ViewportCamera::Game)) {
                    return makeError(ErrorCode::InvalidArgument,
                                     "'{}' is no camera of the viewport; use editor or game", name);
                }
                m_viewport->setViewCamera(name == nameOf(ViewportCamera::Game) ? ViewportCamera::Game
                                                                               : ViewportCamera::Editor);
            }
            if (parameters.contains("colliders")) {
                m_viewport->setShowColliders(jsonGetOr<bool>(parameters, "colliders", false));
            }
            Json shown = Json::object();
            shown["camera"] = nameOf(m_viewport->viewCamera());
            shown["showsGame"] = m_viewport->showsGame();
            shown["colliders"] = m_viewport->showsColliders();
            return shown;
        },
    }));

    CORDITE_TRY(m_commands.add({
        .name = "viewport.quality",
        .description = "Chooses how much the picture in the viewport may cost. \"preset\" sets everything by "
                       "one word (\"low\", \"medium\", \"high\"); \"shadows\" sets the shadows alone (\"off\" "
                       "draws none; the others give the shadow maps of a view an atlas of 2048, 4096 and 8192 "
                       "texels a side to share), \"effects\" what is worked out from the screen - ambient "
                       "occlusion (\"off\" draws none; \"low\" and \"medium\" work it out for one pixel in "
                       "four, \"high\" for every pixel) - and \"postProcess\" the finishing of the picture "
                       "alone (\"off\" shows the light as the renderer worked it out, without exposure, bloom, "
                       "tone mapping or grading). \"antiAliasing\" says how the steps of slanted edges are "
                       "smoothed in a finished picture: \"fxaa\" from the picture itself, \"off\" not at all; "
                       "no preset changes it. Returns what is now in use and all there is to choose from; "
                       "without parameters it changes nothing. A game sets its own quality; this is the "
                       "editor's view of the scene.",
        .parameters = {{"preset", Type::String,
                        "Sets shadows, screen effects and finishing together, by one of the names above.", false},
                       {"shadows", Type::String, "The quality of the shadows; applied after the preset.", false},
                       {"effects", Type::String,
                        "The quality of what is worked out from the screen; applied after the preset.", false},
                       {"postProcess", Type::String,
                        "The quality of the finishing of the picture; applied after the preset.", false},
                       {"antiAliasing", Type::String, "How edges are smoothed: \"fxaa\" or \"off\".", false}},
        .handler = [this](const Json& parameters) -> Result<Json> {
            // All of them are checked before any of them is applied.
            SceneRenderSettings wanted;
            wanted.shadowQuality = m_viewport->shadowQuality();
            wanted.screenEffectsQuality = m_viewport->screenEffectsQuality();
            wanted.postProcessQuality = m_viewport->postProcessQuality();
            if (const auto name = jsonGetOr<std::string>(parameters, "preset", ""); !name.empty()) {
                const std::optional<RenderQuality> preset = renderQualityFromName(name);
                if (!preset) {
                    return makeError(ErrorCode::InvalidArgument,
                                     "'{}' is not a quality preset; viewport.quality without parameters lists them",
                                     name);
                }
                applyRenderQuality(*preset, wanted);
            }
            if (const auto name = jsonGetOr<std::string>(parameters, "shadows", ""); !name.empty()) {
                const std::optional<ShadowQuality> quality = shadowQualityFromName(name);
                if (!quality) {
                    return makeError(ErrorCode::InvalidArgument,
                                     "'{}' is not a shadow quality; viewport.quality without parameters lists them",
                                     name);
                }
                wanted.shadowQuality = *quality;
            }
            if (const auto name = jsonGetOr<std::string>(parameters, "effects", ""); !name.empty()) {
                const std::optional<ScreenEffectsQuality> quality = screenEffectsQualityFromName(name);
                if (!quality) {
                    return makeError(ErrorCode::InvalidArgument,
                                     "'{}' is not a quality of the screen effects; viewport.quality without "
                                     "parameters lists them",
                                     name);
                }
                wanted.screenEffectsQuality = *quality;
            }
            if (const auto name = jsonGetOr<std::string>(parameters, "postProcess", ""); !name.empty()) {
                const std::optional<PostProcessQuality> quality = postProcessQualityFromName(name);
                if (!quality) {
                    return makeError(ErrorCode::InvalidArgument,
                                     "'{}' is not a post-process quality; viewport.quality without parameters "
                                     "lists them",
                                     name);
                }
                wanted.postProcessQuality = *quality;
            }
            wanted.antiAliasing = m_viewport->antiAliasing();
            if (const auto name = jsonGetOr<std::string>(parameters, "antiAliasing", ""); !name.empty()) {
                const std::optional<AntiAliasing> antiAliasing = antiAliasingFromName(name);
                if (!antiAliasing) {
                    return makeError(ErrorCode::InvalidArgument,
                                     "'{}' is not a way of anti-aliasing; viewport.quality without parameters "
                                     "lists them",
                                     name);
                }
                wanted.antiAliasing = *antiAliasing;
            }
            m_viewport->setShadowQuality(wanted.shadowQuality);
            m_viewport->setScreenEffectsQuality(wanted.screenEffectsQuality);
            m_viewport->setPostProcessQuality(wanted.postProcessQuality);
            m_viewport->setAntiAliasing(wanted.antiAliasing);

            Json result = Json::object();
            const std::optional<RenderQuality> preset = renderQualityOf(wanted);
            result["preset"] = preset ? Json(nameOf(*preset)) : Json(nullptr);
            result["shadows"] = nameOf(wanted.shadowQuality);
            result["effects"] = nameOf(wanted.screenEffectsQuality);
            result["postProcess"] = nameOf(wanted.postProcessQuality);
            result["antiAliasing"] = nameOf(wanted.antiAliasing);
            result["presets"] = Json::array();
            for (const RenderQuality quality : kRenderQualities) {
                result["presets"].push_back(nameOf(quality));
            }
            result["shadowQualities"] = Json::array();
            for (const ShadowQuality quality : kShadowQualities) {
                result["shadowQualities"].push_back(nameOf(quality));
            }
            result["effectsQualities"] = Json::array();
            for (const ScreenEffectsQuality quality : kScreenEffectsQualities) {
                result["effectsQualities"].push_back(nameOf(quality));
            }
            result["postProcessQualities"] = Json::array();
            for (const PostProcessQuality quality : kPostProcessQualities) {
                result["postProcessQualities"].push_back(nameOf(quality));
            }
            result["antiAliasingModes"] = Json::array();
            for (const AntiAliasing antiAliasing : kAntiAliasingModes) {
                result["antiAliasingModes"].push_back(nameOf(antiAliasing));
            }
            return result;
        },
    }));

    CORDITE_TRY(m_commands.add({
        .name = "viewport.debugView",
        .description = "Chooses what the viewport shows: the lit picture (\"none\") or a debug view of the "
                       "renderer - one thing the G-buffer holds (baseColor, normals, roughness, metallic, "
                       "specular, occlusion, emissive, depth), how many point and spot lights each pixel "
                       "has to ask (lightCount: dark for none, blue for one, through green to red for eight "
                       "and more), which cascade of the sun's shadow each pixel is looked up in "
                       "(shadowCascades: red, green, blue, yellow from the nearest on; grey where the sun "
                       "casts none), how much of the light from all around gets to each surface past what "
                       "stands around it (ambientOcclusion: white is all of it), how much of each pixel "
                       "is fog (fog: white where nothing shows through), the light without the "
                       "materials (lightingOnly), what the surroundings "
                       "give every surface to reflect, each of them made a perfect mirror (reflections), the "
                       "edges of the triangles (wireframe) or how often each pixel is drawn (overdraw). "
                       "Returns the view now shown and all there are; without a parameter it changes "
                       "nothing.",
        .parameters = {{"view", Type::String, "The view to show, by one of the names above.", false}},
        .handler = [this](const Json& parameters) -> Result<Json> {
            if (const auto name = jsonGetOr<std::string>(parameters, "view", ""); !name.empty()) {
                const std::optional<RenderDebugView> view = renderDebugViewFromName(name);
                if (!view) {
                    return makeError(ErrorCode::InvalidArgument,
                                     "'{}' is not a view; viewport.debugView without parameters lists them", name);
                }
                m_viewport->setDebugView(*view);
            }
            Json views = Json::array();
            for (const RenderDebugView view : kRenderDebugViews) {
                views.push_back(nameOf(view));
            }
            Json result = Json::object();
            result["view"] = nameOf(m_viewport->debugView());
            result["views"] = std::move(views);
            return result;
        },
    }));

    CORDITE_TRY(m_commands.add({
        .name = "viewport.gizmo",
        .description = "Says what the gizmo of the selection does - translate, rotate or scale it - and along "
                       "whose axes, the world's or the entity's own (scale is always along the entity's own); "
                       "given either, changes it. The gizmo is drawn on the entity selected last and moves "
                       "every selected entity that does not hang under another selected one. Dragging it is "
                       "an edit of their Transform, one step to undo, which component.set makes as well.",
        .parameters = {{"mode", Type::String, "\"translate\", \"rotate\" or \"scale\".", false},
                       {"space", Type::String, "\"world\" or \"local\".", false}},
        .handler = [this](const Json& parameters) -> Result<Json> {
            // Both are checked before either is changed.
            std::optional<GizmoMode> mode;
            if (const auto name = jsonGetOr<std::string>(parameters, "mode", ""); !name.empty()) {
                mode = gizmoModeFromName(name);
                if (!mode) {
                    return makeError(ErrorCode::InvalidArgument,
                                     "'{}' is no mode of the gizmo; use \"translate\", \"rotate\" or \"scale\"", name);
                }
            }
            std::optional<GizmoSpace> space;
            if (const auto name = jsonGetOr<std::string>(parameters, "space", ""); !name.empty()) {
                space = gizmoSpaceFromName(name);
                if (!space) {
                    return makeError(ErrorCode::InvalidArgument,
                                     "'{}' is no space of the gizmo; use \"world\" or \"local\"", name);
                }
            }
            if (mode) {
                m_viewport->setGizmoMode(*mode);
            }
            if (space) {
                m_viewport->setGizmoSpace(*space);
            }
            Json result = Json::object();
            result["mode"] = std::string(nameOf(m_viewport->gizmoMode()));
            result["space"] = std::string(nameOf(m_viewport->gizmoSpace()));
            return result;
        },
    }));

    return m_commands.add({
        .name = "viewport.camera",
        .description = "Returns where the viewport camera is and which way it looks.",
        .parameters = {},
        .handler = [this](const Json& /*parameters*/) -> Result<Json> {
            Json result = Json::object();
            result["position"] = toJson(camera().position());
            result["forward"] = toJson(camera().forward());
            result["yawDegrees"] = jsonFromFloat(camera().yawDegrees());
            result["pitchDegrees"] = jsonFromFloat(camera().pitchDegrees());
            return result;
        },
    });
}

void EditorApp::chooseTheme(UiTheme theme) {
    m_engine->ui().setTheme(theme);
    m_preferences.theme = theme;
    if (m_preferencesFile.empty()) {
        return;
    }
    // The interface is in the theme either way; only the next session would not know.
    if (const Status saved = saveEditorPreferences(m_preferencesFile, m_preferences); !saved) {
        CORDITE_LOG_WARN("Editor", "the theme is used, but not remembered: {}", saved.error().message);
    }
}

Json EditorApp::describeTheme() const {
    Json result = Json::object();
    result["theme"] = std::string(nameOf(m_engine->ui().theme()));
    Json names = Json::array();
    for (const UiTheme theme : kUiThemes) {
        names.push_back(std::string(nameOf(theme)));
    }
    result["themes"] = std::move(names);
    return result;
}

Json EditorApp::describePlay() const {
    Json status = Json::object();
    status["state"] = nameOf(m_play->state());
    status["frames"] = m_play->playedFrames();
    // What keeps a script of the game from running, for whoever plays it without
    // looking at the log.
    Json problems = Json::array();
    for (const ScriptProblem& problem : m_engine->scripts().problems()) {
        problems.push_back(problem.message);
    }
    status["scriptProblems"] = std::move(problems);
    return status;
}

Status EditorApp::registerProjectCommands() {
    // Here and not with the other commands of assets: the project names its start scene by
    // where it is, and the application is what has the project.
    return m_commands.add({
        .name = "asset.move",
        .description =
            "Moves or renames the file of an asset, with its .meta: it keeps its id, so whatever refers to it by "
            "id follows it, and so do the file of the open scene and the start scene of the project. Refused "
            "for what it would break or cannot do: an asset that is built in or inside a file (move the file), "
            "a path outside the assets, with a part that begins with a dot, or with another ending, a place "
            "that holds a file, a glTF file that names its buffers and images by where they are and would "
            "leave them behind in another directory, and a texture that a glTF file names by where it is. "
            "Returns `mentions`: lines of scripts, scenes and prefabs, and values in the open scene, that "
            "still name it by where it was - those follow by hand. Files are no part of the document: "
            "edit.undo does not move it back.",
        .parameters = {{"asset", Type::String, "The asset whose file moves: its id, or where it is."},
                       {"to", Type::String,
                        "Where the file goes, inside the asset directory, with forward slashes: "
                        "\"textures/brick.png\"."}},
        .handler = [this](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const AssetInfo* asset,
                               m_context->resolveAsset(parameters.at("asset").get<std::string>()));
            CORDITE_TRY_ASSIGN(const AssetMove moved,
                               moveAsset(*m_context, asset->id, parameters.at("to").get<std::string>()));
            Json result = describeAssetMove(*m_context, moved);
            if (m_project && m_project->settings.startScene == moved.from) {
                m_project->settings.startScene = moved.to;
                if (const Status saved = saveProject(*m_project); !saved) {
                    return tl::unexpected(saved.error().withContext(fmt::format(
                        "{} was moved to {}, but the project still starts with where it was", moved.from, moved.to)));
                }
                result["startScene"] = moved.to;
            }
            for (const Json& mention : result["mentions"]) {
                CORDITE_LOG_WARN("Editor", "{} still names {}: {}",
                                 jsonGetOr<std::string>(mention, "file", "the open scene"), moved.from, mention.dump());
            }
            return result;
        },
    });
}

void EditorApp::importDroppedFiles() {
    // Run before the next frame, as what is typed in the console is: the assets are read again.
    const std::span<const std::filesystem::path> dropped = m_engine->input().droppedFiles();
    if (!dropped.empty()) {
        m_submittedLines.push_back(
            importAssetsCommand(dropped, importDirectory(m_assets.browser(), m_context->assets().database())));
    }
}

Status EditorApp::registerPlayCommands() {
    CORDITE_TRY(m_commands.add({
        .name = "play.start",
        .description = "Plays the scene in the editor: from the next frame on its scripts run, its bodies move "
                       "and its sounds are heard, and the viewport shows it through the camera of the game "
                       "(viewport.show changes that). Nothing of it stays: play.stop puts the scene back as "
                       "it was, and what is edited while it plays goes with the game. Returns what "
                       "play.status returns.",
        .parameters = {},
        .handler = [this](const Json& /*parameters*/) -> Result<Json> {
            CORDITE_TRY(m_play->start());
            // A game is looked at through its own camera, until somebody says otherwise.
            m_viewBeforePlay = m_viewport->viewCamera();
            m_viewport->setViewCamera(ViewportCamera::Game);
            return describePlay();
        },
    }));

    CORDITE_TRY(m_commands.add({
        .name = "play.stop",
        .description = "Stops playing: the scripts end, and the scene is what it was when play.start was called "
                       "- its entities, its history, what is selected of what is still there.",
        .parameters = {},
        .handler = [this](const Json& /*parameters*/) -> Result<Json> {
            CORDITE_TRY(m_play->stop());
            m_viewport->setViewCamera(m_viewBeforePlay);
            return describePlay();
        },
    }));

    CORDITE_TRY(m_commands.add({
        .name = "play.pause",
        .description = "Holds the game that is being played where it is: nothing in it moves on and nothing "
                       "is heard, and the editor shows it as it stands. play.step lets single frames pass.",
        .parameters = {},
        .handler = [this](const Json& /*parameters*/) -> Result<Json> {
            CORDITE_TRY(m_play->pause());
            return describePlay();
        },
    }));

    CORDITE_TRY(m_commands.add({
        .name = "play.resume",
        .description = "Lets a game that is paused go on.",
        .parameters = {},
        .handler = [this](const Json& /*parameters*/) -> Result<Json> {
            CORDITE_TRY(m_play->resume());
            return describePlay();
        },
    }));

    CORDITE_TRY(m_commands.add({
        .name = "play.step",
        .description = "Lets a game that is paused go on for a number of frames, runs those frames, and "
                       "leaves it paused: for looking at what a game does one frame at a time.",
        .parameters = {{"frames", Type::Integer, "How many frames to let pass. Defaults to 1.", false}},
        .handler = [this](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const uint32_t frames, countParameter(parameters, "frames", 1.0, kMaxScriptFrames));
            CORDITE_TRY(m_play->step(frames));
            CORDITE_TRY(runFrames(frames));
            return describePlay();
        },
    }));

    return m_commands.add({
        .name = "play.status",
        .description = "Says whether the scene is being played - `state` is \"stopped\", \"playing\" or "
                       "\"paused\" - for how many `frames` its time has passed since play.start, and what "
                       "keeps scripts of it from running (`scriptProblems`).",
        .parameters = {},
        .handler = [this](const Json& /*parameters*/) -> Result<Json> { return describePlay(); },
    });
}

void EditorApp::drawInterface() {
    handleShortcuts();
    drawMainMenu();
    // The bars come before the dock space, which has the room they leave.
    drawToolbar(documentButtons(*m_context), gizmoButtons(m_viewport->gizmoMode(), m_viewport->gizmoSpace()),
                playButtons(m_play->state()), m_submittedLines);
    const StatusBarState status{
        .message = m_console.newestMessage(LogLevel::Info),
        .play = m_play->state(),
        .entityCount = m_context->world().entityCount(),
        .selectionCount = m_context->selection().size(),
    };
    if (drawStatusBar(status)) {
        m_panelToFocus = ConsolePanel::kWindowId;
    }

    // A layout remembered from an earlier session wins; otherwise the default is built.
    const ImGuiID dockSpaceId = ImGui::GetID("EditorDockSpace");
    if (ImGui::DockBuilderGetNode(dockSpaceId) == nullptr) {
        buildDefaultLayout(dockSpaceId);
        // Of the panels that share a place, the assets are in front - unless another was
        // asked for before there was a frame to show it in. What goes wrong is in the
        // status bar, and brings the console forward.
        if (m_panelToFocus.empty()) {
            m_panelToFocus = AssetBrowserPanel::kWindowId;
        }
    }
    ImGui::DockSpaceOverViewport(dockSpaceId, ImGui::GetMainViewport());

    m_viewport->draw(*m_engine, *m_context);
    m_hierarchy.draw(*m_context);
    m_inspector.draw(*m_context);
    m_console.draw(m_submittedLines);
    // In a layout that was remembered before there was an Assets panel it has no place
    // of its own: there it goes where the console is, the first time it is seen.
    if (const ImGuiWindow* console = ImGui::FindWindowByName(ConsolePanel::kWindowId);
        console != nullptr && console->DockId != 0) {
        ImGui::SetNextWindowDockID(console->DockId, ImGuiCond_FirstUseEver);
    }
    m_assets.draw(*m_context, m_submittedLines);
    drawFileDialog();
    // What the user asked for and did not get is said in the console, which may be behind
    // the assets: it comes to the front for it.
    if (m_console.takeFailedAction()) {
        m_panelToFocus = ConsolePanel::kWindowId;
    }
    if (!m_panelToFocus.empty()) {
        ImGui::SetWindowFocus(m_panelToFocus.c_str());
        m_panelToFocus.clear();
    }

    if (m_showStatistics) {
        m_statistics.draw(m_engine->statistics(), m_showStatistics);
    }
    if (m_showInterfaceDemo) {
        ImGui::ShowDemoWindow(&m_showInterfaceDemo);
    }
}

void EditorApp::handleShortcuts() {
    // Global shortcuts give way to a text field that has the focus, so that Ctrl+Z in
    // a name field undoes typing and Delete removes a character.
    const ImGuiInputFlags route = ImGuiInputFlags_RouteGlobal;
    UndoStack& history = m_context->history();

    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, route) && history.canUndo()) {
        reportIfFailed(history.undo());
    }
    const bool redoPressed = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, route) ||
                             ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, route);
    if (redoPressed && history.canRedo()) {
        reportIfFailed(history.redo());
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, route)) {
        if (m_context->scenePath().empty()) {
            m_fileDialog = FileDialog::SaveAs;
            m_openFileDialog = true;
        } else {
            reportIfFailed(m_context->saveScene());
        }
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, route)) {
        m_fileDialog = FileDialog::Open;
        m_openFileDialog = true;
    }

    // Playing begins and ends between two frames, where commands run: these are queued
    // like lines typed into the console, and run before the next frame.
    const PlayState playing = m_play->state();
    if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_F5, route) && playing != PlayState::Stopped) {
        m_submittedLines.emplace_back("play.stop");
    } else if (ImGui::Shortcut(ImGuiKey_F5, route) && playing == PlayState::Stopped) {
        m_submittedLines.emplace_back("play.start");
    }
    if (ImGui::Shortcut(ImGuiKey_F6, route) && playing != PlayState::Stopped) {
        m_submittedLines.emplace_back(playing == PlayState::Playing ? "play.pause" : "play.resume");
    }
    if (ImGui::Shortcut(ImGuiKey_F10, route) && playing == PlayState::Paused) {
        m_submittedLines.emplace_back("play.step");
    }

    handleGizmoShortcuts();
    handleEntityShortcuts();
}

void EditorApp::handleGizmoShortcuts() {
    const ImGuiInputFlags route = ImGuiInputFlags_RouteGlobal;
    // What the gizmo does, as in the editors people know - unless the camera is being flown,
    // which takes the same keys.
    if (!m_viewport->camera().isNavigating() && !m_viewport->isDraggingGizmo()) {
        if (ImGui::Shortcut(ImGuiKey_W, route)) {
            m_viewport->setGizmoMode(GizmoMode::Translate);
        }
        if (ImGui::Shortcut(ImGuiKey_E, route)) {
            m_viewport->setGizmoMode(GizmoMode::Rotate);
        }
        if (ImGui::Shortcut(ImGuiKey_R, route)) {
            m_viewport->setGizmoMode(GizmoMode::Scale);
        }
        if (ImGui::Shortcut(ImGuiKey_X, route)) {
            m_viewport->setGizmoSpace(m_viewport->gizmoSpace() == GizmoSpace::World ? GizmoSpace::Local
                                                                                    : GizmoSpace::World);
        }
    }
}

void EditorApp::handleEntityShortcuts() {
    const ImGuiInputFlags route = ImGuiInputFlags_RouteGlobal;
    // Pasting needs nothing selected: what was copied goes where it came from.
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V, route) && !m_context->clipboard().is_null()) {
        reportIfFailed(EditorActions(*m_context).pasteEntities());
    }

    const std::vector<Entity> selection = m_context->selection();
    if (!selection.empty()) {
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D, route)) {
            reportIfFailed(EditorActions(*m_context).duplicateEntities(selection));
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C, route)) {
            reportIfFailed(EditorActions(*m_context).copyEntities(selection));
        }
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_X, route)) {
            reportIfFailed(EditorActions(*m_context).cutEntities(selection));
        }
        if (ImGui::Shortcut(ImGuiKey_Delete, route)) {
            reportIfFailed(EditorActions(*m_context).deleteEntities(selection));
        }
    }
}

void EditorApp::drawMainMenu() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    EditorActions actions(*m_context);
    UndoStack& history = m_context->history();
    const std::vector<Entity> selection = m_context->selection();

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New scene")) {
            reportIfFailed(m_context->newScene());
        }
        if (ImGui::MenuItem("Open scene...", "Ctrl+O")) {
            m_fileDialog = FileDialog::Open;
            m_openFileDialog = true;
        }
        if (ImGui::MenuItem("Save", "Ctrl+S", false, !m_context->scenePath().empty())) {
            reportIfFailed(m_context->saveScene());
        }
        if (ImGui::MenuItem("Save as...")) {
            m_fileDialog = FileDialog::SaveAs;
            m_openFileDialog = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) {
            m_engine->requestExit();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Edit")) {
        // The labels say what would be undone and redone.
        const std::string undoText = history.canUndo() ? fmt::format("Undo {}", history.undoLabel()) : "Undo";
        const std::string redoText = history.canRedo() ? fmt::format("Redo {}", history.redoLabel()) : "Redo";
        if (ImGui::MenuItem(undoText.c_str(), "Ctrl+Z", false, history.canUndo())) {
            reportIfFailed(history.undo());
        }
        if (ImGui::MenuItem(redoText.c_str(), "Ctrl+Y", false, history.canRedo())) {
            reportIfFailed(history.redo());
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Cut", "Ctrl+X", false, !selection.empty())) {
            reportIfFailed(actions.cutEntities(selection));
        }
        if (ImGui::MenuItem("Copy", "Ctrl+C", false, !selection.empty())) {
            reportIfFailed(actions.copyEntities(selection));
        }
        if (ImGui::MenuItem("Paste", "Ctrl+V", false, !m_context->clipboard().is_null())) {
            reportIfFailed(actions.pasteEntities());
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, !selection.empty())) {
            reportIfFailed(actions.duplicateEntities(selection));
        }
        if (ImGui::MenuItem("Delete", "Del", false, !selection.empty())) {
            reportIfFailed(actions.deleteEntities(selection));
        }
        ImGui::EndMenu();
    }

    drawEntityMenu();
    drawAssetMenu();
    drawPlayMenu();
    drawViewMenu();

    if (ImGui::BeginMenu("Help")) {
        // Every widget ImGui has, with its source: the reference for writing panels.
        ImGui::MenuItem("Interface demo", nullptr, &m_showInterfaceDemo);
        ImGui::EndMenu();
    }

    // The scene, whether it has unsaved changes, and whether it is being played, at
    // the right end of the bar.
    const PlayState playing = m_play->state();
    const std::string status =
        fmt::format("{}{}{}", m_context->world().sceneName(), m_context->hasUnsavedChanges() ? " *" : "",
                    playing == PlayState::Stopped ? std::string() : fmt::format(" - {}", nameOf(playing)));
    const float statusWidth = ImGui::CalcTextSize(status.c_str()).x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
    if (ImGui::GetContentRegionAvail().x > statusWidth) {
        ImGui::SameLine(ImGui::GetWindowWidth() - statusWidth);
        ImGui::TextDisabled("%s", status.c_str());
    }
    ImGui::EndMainMenuBar();
}

namespace {

/// A menu that offers the values of an enumeration by the names people know them by,
/// with a mark at the one in use. Returns the one that was picked, if one was.
template <typename Value, size_t Count>
std::optional<Value> chooseFromMenu(const char* label, const std::array<Value, Count>& values, Value inUse) {
    std::optional<Value> chosen;
    if (ImGui::BeginMenu(label)) {
        for (const Value value : values) {
            const std::string name(displayNameOf(value));
            if (ImGui::MenuItem(name.c_str(), nullptr, value == inUse)) {
                chosen = value;
            }
        }
        ImGui::EndMenu();
    }
    return chosen;
}

} // namespace

void EditorApp::drawViewMenu() {
    if (!ImGui::BeginMenu("View")) {
        return;
    }
    // What the viewport shows: the picture, or a look into the renderer.
    if (const auto view = chooseFromMenu("Render view", kRenderDebugViews, m_viewport->debugView())) {
        m_viewport->setDebugView(*view);
    }
    // Through whose eyes: the editor's camera, or the one the game renders from.
    const bool game = m_viewport->viewCamera() == ViewportCamera::Game;
    if (ImGui::MenuItem("Game camera", nullptr, game)) {
        m_viewport->setViewCamera(game ? ViewportCamera::Editor : ViewportCamera::Game);
    }
    // What is drawn over the scene that is not part of it.
    if (ImGui::MenuItem("Colliders", nullptr, m_viewport->showsColliders())) {
        m_viewport->setShowColliders(!m_viewport->showsColliders());
    }
    // What the picture in the viewport may cost: everything by one word, or piece by piece.
    if (ImGui::BeginMenu("Quality")) {
        SceneRenderSettings current;
        current.shadowQuality = m_viewport->shadowQuality();
        current.screenEffectsQuality = m_viewport->screenEffectsQuality();
        current.postProcessQuality = m_viewport->postProcessQuality();
        const std::optional<RenderQuality> preset = renderQualityOf(current);
        for (const RenderQuality quality : kRenderQualities) {
            const std::string label(displayNameOf(quality));
            if (ImGui::MenuItem(label.c_str(), nullptr, preset == quality)) {
                applyRenderQuality(quality, current);
                m_viewport->setShadowQuality(current.shadowQuality);
                m_viewport->setScreenEffectsQuality(current.screenEffectsQuality);
                m_viewport->setPostProcessQuality(current.postProcessQuality);
            }
        }
        ImGui::Separator();
        if (const auto quality = chooseFromMenu("Shadows", kShadowQualities, m_viewport->shadowQuality())) {
            m_viewport->setShadowQuality(*quality);
        }
        if (const auto quality =
                chooseFromMenu("Screen effects", kScreenEffectsQualities, m_viewport->screenEffectsQuality())) {
            m_viewport->setScreenEffectsQuality(*quality);
        }
        if (const auto quality =
                chooseFromMenu("Post-processing", kPostProcessQualities, m_viewport->postProcessQuality())) {
            m_viewport->setPostProcessQuality(*quality);
        }
        if (const auto antiAliasing = chooseFromMenu("Anti-aliasing", kAntiAliasingModes, m_viewport->antiAliasing())) {
            m_viewport->setAntiAliasing(*antiAliasing);
        }
        ImGui::EndMenu();
    }
    ImGui::MenuItem("Statistics", nullptr, &m_showStatistics);
    ImGui::Separator();
    // The style changes between frames, where commands run: changed in the middle of one,
    // what is pushed onto it would be popped back to the old theme.
    if (const auto theme = chooseFromMenu("Theme", kUiThemes, m_engine->ui().theme())) {
        m_submittedLines.push_back(fmt::format(R"(editor.theme {{"theme": "{}"}})", nameOf(*theme)));
    }
    ImGui::EndMenu();
}

void EditorApp::drawEntityMenu() {
    if (!ImGui::BeginMenu("Entity")) {
        return;
    }
    EditorActions actions(*m_context);
    const auto create = [&](const char* name, Json components) {
        const Result<Entity> created = actions.createEntity(
            {.name = name, .parent = m_context->primarySelection(), .components = std::move(components)});
        if (reportIfFailed(created)) {
            m_context->setSelection(std::span(&*created, 1));
        }
    };

    if (ImGui::MenuItem("Create empty")) {
        create("Entity", Json::object());
    }
    if (ImGui::MenuItem("Create camera")) {
        Json components = Json::object();
        components["Camera"] = Json::object();
        create("Camera", std::move(components));
    }
    if (ImGui::BeginMenu("Create light")) {
        const auto light = [&create](const char* name, const char* type, const Quat& rotation) {
            Json components = Json::object();
            components["Light"]["type"] = type;
            components["Transform"]["rotation"] = toJson(rotation);
            create(name, std::move(components));
        };
        // A light shines along its -Z axis. Left level, a directional light would only
        // graze a floor, so it starts out shining down at an angle; so does a spot
        // light, which would otherwise shine along the floor it stands on.
        const Quat downwards(glm::radians(Vec3(-50.0f, -30.0f, 0.0f)));
        if (ImGui::MenuItem("Directional light")) {
            light("Directional light", "directional", downwards);
        }
        if (ImGui::MenuItem("Point light")) {
            light("Point light", "point", kIdentityQuat);
        }
        if (ImGui::MenuItem("Spot light")) {
            light("Spot light", "spot", downwards);
        }
        if (ImGui::MenuItem("Ambient light")) {
            Json components = Json::object();
            components["AmbientLight"] = Json::object();
            create("Ambient light", std::move(components));
        }
        if (ImGui::MenuItem("Environment")) {
            // The image of the sky that is to light the scene is chosen in the inspector.
            Json components = Json::object();
            components["Environment"] = Json::object();
            create("Environment", std::move(components));
        }
        ImGui::EndMenu();
    }
    if (ImGui::MenuItem("Create fog")) {
        // As dense as it gets at the height of the entity: it is moved up and down like
        // anything else; its color and density are set in the inspector.
        Json components = Json::object();
        components["Fog"] = Json::object();
        create("Fog", std::move(components));
    }
    if (ImGui::MenuItem("Create post-process volume")) {
        // A box of a few metres in which the picture is finished differently; what
        // that is to be is set in the inspector.
        Json components = Json::object();
        components["PostProcess"]["global"] = false;
        components["Transform"]["scale"] = toJson(Vec3(4.0f));
        create("Post-process volume", std::move(components));
    }
    if (ImGui::BeginMenu("Create shape")) {
        // The built-in meshes: enough to block out a scene before models exist.
        for (const BuiltinMesh mesh : kBuiltinMeshes) {
            const AssetInfo* asset = m_context->assets().database().find(builtinMeshId(mesh));
            if (asset != nullptr && ImGui::MenuItem(asset->name.c_str())) {
                Json components = Json::object();
                components["MeshRenderer"]["mesh"] = asset->id.toString();
                create(asset->name.c_str(), std::move(components));
            }
        }
        ImGui::EndMenu();
    }

    ImGui::Separator();
    // Writes a file and reads the assets again, which is for between two frames: it is
    // asked for as a command, and done before the next one. The file is named after the
    // entity; prefab.create in the console takes a path.
    const Entity selected = m_context->primarySelection();
    if (ImGui::MenuItem("Make prefab", nullptr, false, !selected.isNull() && !m_context->isPlaying())) {
        Json parameters = Json::object();
        parameters["entity"] = m_context->world().id(selected).toString();
        m_submittedLines.push_back(fmt::format("prefab.create {}", parameters.dump()));
    }
    ImGui::EndMenu();
}

Uuid EditorApp::drawAssetChoice(const char* label, AssetType type, const char* nothing) const {
    Uuid chosen;
    if (!ImGui::BeginMenu(label)) {
        return chosen;
    }
    bool any = false;
    for (const AssetInfo& asset : m_context->assets().database().assets()) {
        if (asset.type != type) {
            continue;
        }
        any = true;
        ImGui::PushID(asset.path.c_str());
        if (ImGui::MenuItem(asset.name.c_str(), asset.path.c_str())) {
            chosen = asset.id;
        }
        ImGui::PopID();
    }
    if (!any) {
        ImGui::TextDisabled("%s", nothing);
    }
    ImGui::EndMenu();
    return chosen;
}

void EditorApp::drawPlayMenu() {
    if (!ImGui::BeginMenu("Play")) {
        return;
    }
    // Each is a command, queued to run before the next frame: see handleShortcuts().
    const PlayState state = m_play->state();
    if (ImGui::MenuItem("Play", "F5", false, state == PlayState::Stopped)) {
        m_submittedLines.emplace_back("play.start");
    }
    if (ImGui::MenuItem(state == PlayState::Paused ? "Resume" : "Pause", "F6", false, state != PlayState::Stopped)) {
        m_submittedLines.emplace_back(state == PlayState::Paused ? "play.resume" : "play.pause");
    }
    if (ImGui::MenuItem("Step a frame", "F10", false, state == PlayState::Paused)) {
        m_submittedLines.emplace_back("play.step");
    }
    if (ImGui::MenuItem("Stop", "Shift+F5", false, state != PlayState::Stopped)) {
        m_submittedLines.emplace_back("play.stop");
    }
    ImGui::EndMenu();
}

void EditorApp::drawAssetMenu() {
    if (!ImGui::BeginMenu("Assets")) {
        return;
    }
    if (ImGui::MenuItem("Refresh")) {
        reportIfFailed(m_context->assets().refresh());
    }
    // Placed once the menu has been drawn: the list of assets must not be walked while
    // something is placed from it.
    if (const Uuid model = drawAssetChoice("Place model", AssetType::Model, "The project has no models.");
        !model.isNil()) {
        reportIfFailed(EditorActions(*m_context).instantiateModel(model, {.parent = m_context->primarySelection()}));
    }
    if (const Uuid prefab = drawAssetChoice("Place prefab", AssetType::Prefab, "The project has no prefabs.");
        !prefab.isNil()) {
        reportIfFailed(EditorActions(*m_context).instantiatePrefab(prefab, {.parent = m_context->primarySelection()}));
    }

    // What is wrong with the asset directory, where it will be seen.
    const std::span<const std::string> problems = m_context->assets().database().problems();
    if (!problems.empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("%zu problem(s); see asset.list in the console", problems.size());
    }
    ImGui::EndMenu();
}

void EditorApp::drawFileDialog() {
    if (m_openFileDialog) {
        m_openFileDialog = false;
        m_fileDialogPath = pathToUtf8(m_context->scenePath());
        ImGui::OpenPopup(kFileDialogId);
    }

    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 36.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal(kFileDialogId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    const bool isOpen = m_fileDialog == FileDialog::Open;
    ImGui::TextUnformatted(isOpen ? "Scene file to open:" : "Save the scene as:");
    ImGui::SetNextItemWidth(-std::numeric_limits<float>::min());
    if (ImGui::IsWindowAppearing()) {
        ImGui::SetKeyboardFocusHere();
    }
    const bool entered = ImGui::InputText("##path", &m_fileDialogPath, ImGuiInputTextFlags_EnterReturnsTrue);

    const bool hasPath = !trim(m_fileDialogPath).empty();
    ImGui::BeginDisabled(!hasPath);
    const bool confirmed = ImGui::Button(isOpen ? "Open" : "Save") || (entered && hasPath);
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool cancelled = ImGui::Button("Cancel");

    if (confirmed) {
        const std::filesystem::path path = pathFromUtf8(trim(m_fileDialogPath));
        // The dialog closes either way; a failure is shown in the console.
        reportIfFailed(isOpen ? m_context->openScene(path) : m_context->saveScene(path));
    }
    if (confirmed || cancelled) {
        m_fileDialog = FileDialog::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void EditorApp::updateWindowTitle() {
    Window* window = m_engine->window();
    if (window == nullptr) {
        return;
    }

    const std::filesystem::path& scenePath = m_context->scenePath();
    const std::string sceneLabel =
        scenePath.empty() ? m_context->world().sceneName() : pathToUtf8(scenePath.filename());
    const PlayState playing = m_play->state();
    std::string title =
        fmt::format("{}{}{} - {}{}", sceneLabel, m_context->hasUnsavedChanges() ? " *" : "",
                    playing == PlayState::Stopped ? std::string() : fmt::format(" ({})", nameOf(playing)),
                    m_project ? m_project->settings.name + " - " : std::string(), kEditorName);
    if (title != m_shownTitle) {
        window->setTitle(title);
        m_shownTitle = std::move(title);
    }
}

} // namespace cordite
