#include "cordite/editor/EditorApp.h"

#include "cordite/core/FileSystem.h"
#include "cordite/core/Log.h"
#include "cordite/core/StringUtils.h"
#include "cordite/editor/AssetCommands.h"
#include "cordite/editor/AssetFiles.h"
#include "cordite/editor/AssetMove.h"
#include "cordite/editor/AssetPreview.h"
#include "cordite/editor/EditorActions.h"
#include "cordite/editor/EditorCommands.h"
#include "cordite/editor/EditorFeedback.h"
#include "cordite/editor/EditorPreferences.h"
#include "cordite/editor/EditorToolbar.h"
#include "cordite/editor/GameExport.h"
#include "cordite/editor/InputCommands.h"
#include "cordite/editor/NewProject.h"
#include "cordite/editor/PhysicsCommands.h"
#include "cordite/editor/ScriptCommands.h"
#include "cordite/math/MathJson.h"
#include "cordite/rhi/TextureReadback.h"
#include "cordite/shader_compiler/EngineShaderCache.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <span>
#include <system_error>
#include <utility>

namespace cordite {
namespace {

using Type = CommandParameterType;

constexpr const char* kEditorName = "Cordite Editor";
/// The sides of a picture that asset.preview draws when it is not told, in pixels.
constexpr int64_t kPreviewSize = 256;
/// The largest error a level of detail may be allowed in the viewport, in pixels.
constexpr float kMaxLodErrorPixels = 64.0f;
/// What the items of the File menu that ask for a path say of themselves without dialogs.
constexpr const char* kNoDialogs = "Asks for a path in a dialog of the system, which an editor without a window "
                                   "has not got: the commands that take a path - scene.open, scene.save, "
                                   "project.new, project.open, project.export - do the same.";
/// What the parameter of the project commands that lets the changes of the scene go says.
constexpr const char* kDiscardChangesDescription =
    "Let the changes of the scene that are not saved go, instead of refusing: what a person who answers "
    "\"Don't save\" asks for. Defaults to false.";
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

/// The parameter by which a command that is about one viewport is told which.
CommandParameter viewportParameter() {
    return {"viewport", Type::Integer, "Which viewport, by its number: 1 is the first. Defaults to the one used last.",
            false};
}

/// Where a viewport goes in the layout when it has no place of its own: beside which other
/// one, and on which side of it.
struct ViewportPlace {
    uint32_t beside = 1;
    ImGuiDir side = ImGuiDir_Right;
};

/// The second to the right of the first, the third below the first, the fourth below the
/// second: four are a grid of two by two. A viewport is given the lowest number that is not
/// open, so the one it goes beside is open.
ViewportPlace placeOfViewport(uint32_t number) {
    switch (number) {
        case 2:
            return {.beside = 1, .side = ImGuiDir_Right};
        case 3:
            return {.beside = 1, .side = ImGuiDir_Down};
        default:
            return {.beside = 2, .side = ImGuiDir_Down};
    }
}

/// The dock node a window is in, or goes back to when it is drawn again: what the window, or
/// what Dear ImGui remembers of it, says. 0 for none.
ImGuiID dockIdOf(const std::string& windowId) {
    if (const ImGuiWindow* window = ImGui::FindWindowByName(windowId.c_str())) {
        return window->DockId;
    }
    if (const ImGuiWindowSettings* settings = ImGui::FindWindowSettingsByID(ImHashStr(windowId.c_str()))) {
        return settings->DockId;
    }
    return 0;
}

/// The dock node of that id if a window can be docked into it: one that is there and is not
/// split. Null otherwise.
ImGuiDockNode* leafNode(ImGuiID id) {
    ImGuiDockNode* node = id == 0 ? nullptr : ImGui::DockBuilderGetNode(id);
    return node != nullptr && node->IsLeafNode() ? node : nullptr;
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
        engineDesc.textureBudgetBytes = uint64_t{project.settings.textureBudgetMegabytes} << 20;
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
    // An editor compiles the engine's shaders: what changes in them is seen at once.
    if (!engineDesc.shaderCompiler) {
        engineDesc.shaderCompiler = makeSlangShaderCompiler;
    }
    // And keeps what it compiled where it keeps what it remembers, checked against the
    // sources every time it is read. Kept in memory alone, every shader was compiled anew
    // whenever a project opened: nine tenths of the three and a half seconds the showcase
    // took to open.
    if (engineDesc.cacheDirectory.empty()) {
        engineDesc.cacheDirectory = stateDirectory;
    }
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
    app->m_assets.browser().setView(app->m_preferences.assetView);
    // The project is the newest of those File > Open recent lists, the next time too.
    if (const std::optional<Project>& project = app->m_project; project) {
        rememberRecentProject(app->m_preferences, project->directory);
        app->savePreferences("the project is open, but not among the recent ones next time");
    }

    // Paths are asked for in the dialogs of the system - where there is a window to show
    // them over, and a person to answer - or in those a test gives.
    if (desc.fileDialogs != nullptr) {
        app->m_fileDialogs = desc.fileDialogs;
    } else if (app->m_engine->window() != nullptr) {
        if (Result<std::unique_ptr<FileDialogs>> dialogs = makeSystemFileDialogs()) {
            app->m_systemDialogs = std::move(*dialogs);
            app->m_fileDialogs = app->m_systemDialogs.get();
        } else {
            // The commands that take paths are left, and say so where the menu does.
            CORDITE_LOG_WARN("Editor", "the File menu asks for no paths: {}", dialogs.error().message);
        }
    }

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
    CORDITE_TRY(app->registerViewportCommands());
    CORDITE_TRY(app->registerPlayCommands());
    CORDITE_TRY(app->registerProjectCommands());
    CORDITE_TRY_ASSIGN(app->m_viewports.front(), ViewportPanel::create(*app->m_engine, app->m_viewportSettings));

    if (!desc.scene.empty()) {
        // A scene that was asked for by name is what the editor was started for.
        CORDITE_TRY(app->m_context->openScene(scene));
    } else if (!scene.empty()) {
        // The project's start scene is not: a project whose scene cannot be read opens all
        // the same, with an empty scene, so that it can be mended in the editor. What is
        // wrong is said in the console, which comes to the front for it.
        reportIfFailed(app->m_context->openScene(scene).transform_error([&scene](const Error& error) {
            return error.withContext(fmt::format("the start scene {} of the project", pathToUtf8(scene)));
        }));
    }
    // A scene that the editor before this one kept, when it could not go on, is offered back:
    // not opened in place of the one that opens now until somebody says so.
    app->m_stateDirectory = stateDirectory;
    if (const std::filesystem::path kept = stateDirectory / pathFromUtf8(kSceneRecoveryFileName); fileExists(kept)) {
        if (Result<SceneRecovery> recovery = readSceneRecovery(kept)) {
            CORDITE_LOG_WARN(
                "Editor", "the editor before kept changes of {} that were not saved: scene.recover opens them",
                recovery->scenePath.empty() ? std::string("a new scene") : pathToUtf8(recovery->scenePath));
            app->m_keptScene = std::move(*recovery);
        } else {
            CORDITE_LOG_WARN("Editor", "a kept scene is there, but cannot be read: {}", recovery.error().message);
        }
    }
    if (const std::optional<Project>& project = app->m_project; desc.watchAssets && project) {
        app->m_assetWatcher = std::make_unique<DirectoryWatcher>(
            DirectoryWatcherDesc{.directory = project->assetDirectory(), .interval = desc.assetWatchInterval});
        app->m_watchedRevision = app->m_context->assets().database().revision();
        app->m_watchedSaves = app->m_context->saveCount();
    }
    return app;
}

EditorApp::~EditorApp() = default;

Result<bool> EditorApp::runFrame() {
    Result<bool> ran = drawFrame();
    if (!ran && ran.error().code == ErrorCode::DeviceLost) {
        // Nothing more can be drawn, but the scene is on the CPU, as it was.
        return makeError(ErrorCode::DeviceLost, "{}; {}", ran.error().message, keepScene());
    }
    return ran;
}

std::string EditorApp::keepScene() {
    if (!m_context->hasUnsavedChanges()) {
        return "the scene had no changes that were not saved";
    }
    const std::filesystem::path file = m_stateDirectory / pathFromUtf8(kSceneRecoveryFileName);
    if (const Status kept =
            writeSceneRecovery(file, {.scenePath = m_context->scenePath(), .scene = m_context->documentScene()});
        !kept) {
        return fmt::format("the changes of the scene that were not saved could not be kept: {}", kept.error().message);
    }
    return fmt::format("the changes of the scene that were not saved were kept in {}, which the editor offers to "
                       "open the next time it opens this project",
                       pathToUtf8(file));
}

Result<bool> EditorApp::drawFrame() {
    followAssetFiles();
    // A dialog that asks for a path holds the program until it is answered: here, between
    // frames, it stops none half drawn. What it is answered with may be a line to run.
    showPathDialog();
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

Result<Image> EditorApp::captureViewport(UVec2 size, uint32_t viewport) {
    CORDITE_ASSERT(!m_engine->isFrameOpen(), "the viewport cannot be captured during a frame");
    if (m_engine->isFrameOpen()) {
        return makeError(ErrorCode::InvalidState, "the viewport cannot be captured during a frame");
    }
    ViewportPanel* const panel = findViewport(viewport);
    if (panel == nullptr) {
        return makeError(ErrorCode::NotFound, "viewport {} is not open; the open ones are {}", viewport,
                         openViewportNumbers());
    }

    m_engine->updateWorld(WorldClock::Stopped);

    // Rendered with a command list of its own, so that capturing neither needs nor
    // disturbs a frame of the editor.
    nvrhi::IDevice* device = m_engine->graphics().device();
    const nvrhi::CommandListHandle commandList = device->createCommandList();
    commandList->open();
    // A picture of this moment: no time has passed since the viewport was last drawn.
    const Status rendered =
        panel->render(device, commandList, *m_context, size, 0.0f, m_engine->timeSeconds(), WorldClock::Stopped);
    commandList->close();
    CORDITE_TRY(rendered);
    device->executeCommandList(commandList);

    CORDITE_TRY_ASSIGN(TexturePixels pixels, readTexture(m_engine->graphics(), panel->targets().display()));
    return Image::fromPixels(pixels.width, pixels.height, ImageFormat::Rgba8, std::move(pixels.data));
}

ViewportPanel* EditorApp::findViewport(uint32_t number) {
    return number >= 1 && number <= kMaxViewports ? m_viewports[number - 1].get() : nullptr;
}

ViewportPanel& EditorApp::activePanel() {
    ViewportPanel* const active = findViewport(m_activeViewport);
    // Closing the one used last makes the first the one used last.
    CORDITE_ASSERT(active != nullptr, "the viewport used last is open");
    return active != nullptr ? *active : viewport();
}

std::string EditorApp::openViewportNumbers() const {
    std::vector<uint32_t> open;
    for (const std::unique_ptr<ViewportPanel>& viewport : m_viewports) {
        if (viewport) {
            open.push_back(viewport->number());
        }
    }
    std::string numbers;
    for (size_t index = 0; index < open.size(); ++index) {
        if (index > 0) {
            numbers += index + 1 == open.size() ? " and " : ", ";
        }
        numbers += std::to_string(open[index]);
    }
    return numbers;
}

Result<ViewportPanel*> EditorApp::viewportOf(const Json& parameters) {
    const auto named = parameters.find("viewport");
    if (named == parameters.end()) {
        return &activePanel();
    }
    // Checked to be a whole number already; one far out of range is no number of a viewport.
    const auto number = named->get<double>();
    ViewportPanel* const viewport =
        number >= 1.0 && number <= kMaxViewports ? findViewport(static_cast<uint32_t>(number)) : nullptr;
    if (viewport == nullptr) {
        return makeError(ErrorCode::NotFound, "there is no viewport {} open; the open ones are {}",
                         named->is_number_integer() ? named->dump() : fmt::format("{}", number), openViewportNumbers());
    }
    return viewport;
}

Result<uint32_t> EditorApp::openViewport(ViewportCamera camera) {
    const auto free = std::ranges::find_if(m_viewports, [](const auto& viewport) { return viewport == nullptr; });
    if (free == m_viewports.end()) {
        return makeError(ErrorCode::InvalidState, "{} viewports are open, as many as there can be; close one first",
                         kMaxViewports);
    }
    const auto number = static_cast<uint32_t>(free - m_viewports.begin()) + 1;
    CORDITE_TRY_ASSIGN(std::unique_ptr<ViewportPanel> viewport,
                       ViewportPanel::create(*m_engine, m_viewportSettings, number));
    // It looks from where the one used last does - and Dear ImGui gives its window the focus
    // when it first appears, which keeps it the one used last.
    viewport->camera() = activePanel().camera().atRest();
    viewport->setViewCamera(camera);
    *free = std::move(viewport);
    m_activeViewport = number;
    m_viewportsToPlace.push_back(number);
    return number;
}

Status EditorApp::closeViewport(uint32_t number) {
    if (number == 1) {
        return makeError(ErrorCode::InvalidArgument, "the first viewport is always there and cannot be closed");
    }
    ViewportPanel* const viewport = findViewport(number);
    if (viewport == nullptr) {
        return makeError(ErrorCode::NotFound, "viewport {} is not open; the open ones are {}", number,
                         openViewportNumbers());
    }
    // A drag of its gizmo ends where it is, and is a step of the history; a pointer that steers
    // its camera is free again.
    viewport->release(*m_context, m_engine->window());
    m_viewports[number - 1].reset();
    std::erase(m_viewportsToPlace, number);
    if (m_activeViewport == number) {
        m_activeViewport = 1;
        // And has the focus: Dear ImGui would give it to whichever window is in front, which
        // may be another viewport.
        m_panelToFocus = ViewportPanel::kWindowId;
    }
    return {};
}

Json EditorApp::describeViewports() const {
    Json viewports = Json::array();
    for (const std::unique_ptr<ViewportPanel>& viewport : m_viewports) {
        if (viewport) {
            Json entry = Json::object();
            entry["viewport"] = viewport->number();
            entry["camera"] = nameOf(viewport->viewCamera());
            entry["showsGame"] = viewport->showsGame();
            entry["debugView"] = nameOf(viewport->debugView());
            entry["colliders"] = viewport->showsColliders();
            viewports.push_back(std::move(entry));
        }
    }
    Json result = Json::object();
    result["active"] = m_activeViewport;
    result["viewports"] = std::move(viewports);
    return result;
}

void EditorApp::placeOpenedViewports(ImGuiID dockSpaceId) {
    bool split = false;
    for (const uint32_t number : std::exchange(m_viewportsToPlace, {})) {
        const ViewportPanel* const viewport = findViewport(number);
        // One that was open before goes back where it was: Dear ImGui keeps the place of a
        // window that is closed, and gives the room to what is beside it meanwhile.
        if (viewport == nullptr || leafNode(dockIdOf(viewport->windowId())) != nullptr) {
            continue;
        }
        const ViewportPlace place = placeOfViewport(number);
        const ViewportPanel* const neighbour = findViewport(place.beside);
        ImGuiDockNode* node = neighbour == nullptr ? nullptr : leafNode(dockIdOf(neighbour->windowId()));
        if (node == nullptr) {
            // Beside one that floats: beside the first, if that is docked.
            node = leafNode(dockIdOf(ViewportPanel::kWindowId));
        }
        if (node == nullptr) {
            // Nothing to be docked beside: it floats, where Dear ImGui puts a new window.
            continue;
        }
        // What was in the node goes to the other side - and what windows remember of it goes
        // with them, those that are not drawn yet too.
        ImGuiID placed = 0;
        ImGui::DockBuilderSplitNode(node->ID, place.side, 0.5f, &placed, nullptr);
        ImGui::DockBuilderDockWindow(viewport->windowId().c_str(), placed);
        split = true;
    }
    if (split) {
        ImGui::DockBuilderFinish(dockSpaceId);
    }
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
        .description = "Saves a PNG image of the editor window, or of the scene alone as a viewport shows it. "
                       "Returns the path and the size of the image.",
        .parameters = {{"path", Type::String, "Where to save the image. Missing directories are created."},
                       {"viewportOnly", Type::Boolean,
                        "Save the scene without the interface around it. Defaults to false.", false},
                       {"viewport", Type::Integer,
                        "With viewportOnly: which viewport, by its number. Defaults to the one used last.", false},
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
                CORDITE_TRY_ASSIGN(const ViewportPanel* const viewport, viewportOf(parameters));
                const UVec2 windowSize = m_engine->renderSize();
                CORDITE_TRY_ASSIGN(const uint32_t width,
                                   countParameter(parameters, "width", windowSize.x, kMaxCaptureDimension));
                CORDITE_TRY_ASSIGN(const uint32_t height,
                                   countParameter(parameters, "height", windowSize.y, kMaxCaptureDimension));
                image = captureViewport(UVec2(width, height), viewport->number());
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
                       "frame has been drawn. A viewport brought to the front is the one used last from then on.",
        .parameters = {{"panel", Type::String,
                        "The panel: \"hierarchy\", \"inspector\", \"viewport\", \"console\" or \"assets\" - or "
                        "\"viewport2\" and on, the viewports opened besides the first, while they are open."}},
        .handler = [this](const Json& parameters) -> Result<Json> {
            const std::string panel = parameters.at("panel").get<std::string>();
            // The viewports besides the first are panels while they are open.
            std::vector<std::string> openViewports;
            for (const std::unique_ptr<ViewportPanel>& viewport : m_viewports) {
                if (viewport && viewport->number() > 1) {
                    openViewports.push_back(fmt::format("viewport{}", viewport->number()));
                    if (panel == openViewports.back()) {
                        m_panelToFocus = viewport->windowId();
                        m_activeViewport = viewport->number();
                        Json result = Json::object();
                        result["panel"] = panel;
                        return result;
                    }
                }
            }
            const auto found =
                std::ranges::find(kDockedPanels, std::string_view(panel), &std::pair<const char*, const char*>::first);
            if (found == kDockedPanels.end()) {
                std::string viewports;
                for (const std::string& name : openViewports) {
                    viewports += fmt::format(", \"{}\"", name);
                }
                return makeError(ErrorCode::InvalidArgument,
                                 "there is no panel '{}'; use \"hierarchy\", \"inspector\", \"viewport\"{}, "
                                 "\"console\" or \"assets\"",
                                 panel, viewports);
            }
            if (found->second == std::string_view(ViewportPanel::kWindowId)) {
                m_activeViewport = 1;
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
        .name = "scene.recover",
        .description =
            "Says whether the editor before this one kept a scene it could not go on with - the GPU was lost "
            "while the scene had changes that were not saved - and which file it was of. With `open` true it "
            "opens it in place of the scene that is open, as changes that are not saved until it is saved; "
            "with `open` false it lets it go. Either way the kept file is deleted. Refused while the scene is "
            "played, without a kept scene, and - to open one - over changes that are not saved, unless told "
            "discardChanges. Returns whether a scene is kept, and its file.",
        .parameters = {{"open", Type::Boolean,
                        "true opens the kept scene, false lets it go; left out, nothing is done but saying.", false},
                       {"discardChanges", Type::Boolean,
                        "Open it over changes of the open scene that are not saved, which are lost then. Defaults "
                        "to false.",
                        false}},
        .handler = [this](const Json& parameters) -> Result<Json> {
            if (parameters.contains("open")) {
                if (!m_keptScene) {
                    return makeError(ErrorCode::NotFound, "no scene was kept");
                }
                if (m_context->isPlaying()) {
                    return makeError(ErrorCode::InvalidState, "the scene is being played: stop it first");
                }
                if (parameters.at("open").get<bool>()) {
                    if (m_context->hasUnsavedChanges() && !jsonGetOr<bool>(parameters, "discardChanges", false)) {
                        return makeError(ErrorCode::InvalidState,
                                         "the open scene has changes that are not saved, and would be lost: save "
                                         "it, undo them, or say discardChanges");
                    }
                    CORDITE_TRY(m_context->restoreScene(m_keptScene->scene, m_keptScene->scenePath));
                }
                const std::filesystem::path file = m_stateDirectory / pathFromUtf8(kSceneRecoveryFileName);
                std::error_code error;
                if (std::filesystem::remove(file, error); error) {
                    CORDITE_LOG_WARN("Editor", "the kept scene is done with, but its file {} is still there: {}",
                                     pathToUtf8(file), error.message());
                }
                m_keptScene.reset();
            }
            Json result = Json::object();
            result["kept"] = m_keptScene.has_value();
            if (m_keptScene) {
                result["scenePath"] = pathToUtf8(m_keptScene->scenePath);
            }
            return result;
        },
    }));
    return m_commands.add({
        .name = "probes.capture",
        .description = "Takes the pictures of all environment probes again, in the next frames: what is around them "
                       "has changed - a wall was moved, a light turned on - and they still show it as it was. A "
                       "probe whose entity moves is taken again by itself. Returns how many probes have their "
                       "pictures waiting to be taken.",
        .parameters = {},
        .handler = [this](const Json& /*parameters*/) -> Result<Json> {
            m_engine->sceneRenderer().retakeProbes();
            Json result = Json::object();
            result["probes"] = m_engine->sceneRenderer().probes().count();
            return result;
        },
    });
}

Status EditorApp::registerViewportCommands() {
    CORDITE_TRY(m_commands.add({
        .name = "viewport.open",
        .description = fmt::format(
            "Opens another viewport beside those that are open - up to {} - with a camera of its own, which starts "
            "where the camera of the viewport used last is. A viewport has its own camera, render view and "
            "colliders; what its picture may cost and what the gizmo does are those of every viewport. The one "
            "that is opened is the one used last from then on: a command of a viewport that names none is about "
            "it. Returns its number, the lowest that is not open.",
            kMaxViewports),
        .parameters = {{"camera", Type::String,
                        "Through whose eyes it shows the scene: \"editor\" (the default) or \"game\".", false}},
        .handler = [this](const Json& parameters) -> Result<Json> {
            ViewportCamera camera = ViewportCamera::Editor;
            if (const auto name = jsonGetOr<std::string>(parameters, "camera", ""); !name.empty()) {
                const std::optional<ViewportCamera> named = viewportCameraFromName(name);
                if (!named) {
                    return makeError(ErrorCode::InvalidArgument, "'{}' is no camera of a viewport; use editor or game",
                                     name);
                }
                camera = *named;
            }
            CORDITE_TRY_ASSIGN(const uint32_t number, openViewport(camera));
            Json result = Json::object();
            result["viewport"] = number;
            return result;
        },
    }));

    CORDITE_TRY(m_commands.add({
        .name = "viewport.close",
        .description = "Closes a viewport other than the first, which is always there. A drag of the gizmo in "
                       "it ends where it is, as a step of the history. After the viewport used last is closed, "
                       "the first is the one used last. Returns what viewport.list returns.",
        .parameters = {{"viewport", Type::Integer, "Which viewport, by its number: 2 and on."}},
        .handler = [this](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const ViewportPanel* const viewport, viewportOf(parameters));
            CORDITE_TRY(closeViewport(viewport->number()));
            return describeViewports();
        },
    }));

    CORDITE_TRY(m_commands.add({
        .name = "viewport.list",
        .description = "Lists the viewports that are open: for each its number (`viewport`), through whose camera "
                       "it shows the scene (`camera`, and `showsGame`: whether its last picture was the game's), "
                       "its render view (`debugView`) and whether it draws the colliders of all entities; and "
                       "`active`, the number of the one used last - clicked into or brought to the front - which "
                       "the View menu, F and the commands that name no viewport act on.",
        .parameters = {},
        .handler = [this](const Json& /*parameters*/) -> Result<Json> { return describeViewports(); },
    }));

    CORDITE_TRY(m_commands.add({
        .name = "viewport.lookAt",
        .description = "Places the camera of a viewport at a position, looking at a target point.",
        .parameters = {{"position", Type::Array, "Where the camera goes: [x, y, z] in metres."},
                       {"target", Type::Array, "The point it looks at: [x, y, z] in metres."},
                       viewportParameter()},
        .handler = [this](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(ViewportPanel* const viewport, viewportOf(parameters));
            Vec3 position(0.0f);
            Vec3 target(0.0f);
            if (!fromJson(parameters.at("position"), position) || !fromJson(parameters.at("target"), target)) {
                return makeError(ErrorCode::InvalidArgument, "'position' and 'target' must be arrays of 3 numbers");
            }
            viewport->camera().lookAt(position, target);
            return Json(nullptr);
        },
    }));

    CORDITE_TRY(m_commands.add({
        .name = "viewport.frame",
        .description = "Moves the camera of a viewport so that an entity is in view, keeping the viewing "
                       "direction - or, without one, what is selected, as F does. What is framed is what the "
                       "camera orbits about with Alt and the left button.",
        .parameters = {{"entity", Type::String, "The entity, by id, path or name. Without it, the selection.", false},
                       {"radius", Type::Number,
                        "The size of the area to show around the entity, in metres. Defaults to 1. Only with "
                        "`entity`: the selection is framed by its own size.",
                        false},
                       viewportParameter()},
        .handler = [this](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(ViewportPanel* const viewport, viewportOf(parameters));
            if (!parameters.contains("entity")) {
                if (parameters.contains("radius")) {
                    return makeError(ErrorCode::InvalidArgument,
                                     "'radius' is the size around an entity; the selection is framed by its own size");
                }
                if (!viewport->frameSelection(*m_context)) {
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
            viewport->camera().frame(Vec3(m_context->world().computeWorldMatrix(entity)[3]), radius);
            return Json(nullptr);
        },
    }));

    CORDITE_TRY(m_commands.add({
        .name = "viewport.show",
        .description = "Chooses through whose eyes a viewport shows the scene, and what it draws over it. "
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
                        "Draw the colliders, the joints and the characters of all entities.", false},
                       viewportParameter()},
        .handler = [this](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(ViewportPanel* const viewport, viewportOf(parameters));
            if (const auto name = jsonGetOr<std::string>(parameters, "camera", ""); !name.empty()) {
                const std::optional<ViewportCamera> camera = viewportCameraFromName(name);
                if (!camera) {
                    return makeError(ErrorCode::InvalidArgument,
                                     "'{}' is no camera of the viewport; use editor or game", name);
                }
                viewport->setViewCamera(*camera);
            }
            if (parameters.contains("colliders")) {
                viewport->setShowColliders(jsonGetOr<bool>(parameters, "colliders", false));
            }
            Json shown = Json::object();
            shown["camera"] = nameOf(viewport->viewCamera());
            shown["showsGame"] = viewport->showsGame();
            shown["colliders"] = viewport->showsColliders();
            return shown;
        },
    }));

    CORDITE_TRY(m_commands.add({
        .name = "viewport.quality",
        .description =
            "Chooses how much the pictures of the viewports may cost - of all of them: they share one "
            "atlas of shadow maps and the textures. \"preset\" sets everything by "
            "one word (\"low\", \"medium\", \"high\") - also the largest side the textures keep, 1024, "
            "2048 and 4096 texels, which every view of the editor shares; \"shadows\" sets the shadows alone (\"off\" "
            "draws none; the others give the shadow maps of a view an atlas of 2048, 4096 and 8192 "
            "texels a side to share), \"effects\" what is worked out from the screen - ambient "
            "occlusion (\"off\" draws none; \"low\" and \"medium\" work it out for one pixel in "
            "four, \"high\" for every pixel) - and \"postProcess\" the finishing of the picture "
            "alone (\"off\" shows the light as the renderer worked it out, without exposure, bloom, "
            "tone mapping or grading), and \"lodErrorPixels\" how large the error of a level of "
            "detail may look, in pixels (a preset sets 2, 1 and a half; 0 draws every mesh at its "
            "finest). \"antiAliasing\" says how the steps of slanted edges are "
            "smoothed in a finished picture: \"fxaa\" from the picture itself, \"off\" not at all; "
            "no preset changes it. Returns what is now in use and all there is to choose from; "
            "without parameters it changes nothing. A game sets its own quality; this is that of the "
            "editor's views of the scene.",
        .parameters = {{"preset", Type::String,
                        "Sets shadows, screen effects and finishing together, by one of the names above.", false},
                       {"shadows", Type::String, "The quality of the shadows; applied after the preset.", false},
                       {"effects", Type::String,
                        "The quality of what is worked out from the screen; applied after the preset.", false},
                       {"postProcess", Type::String,
                        "The quality of the finishing of the picture; applied after the preset.", false},
                       {"lodErrorPixels", Type::Number,
                        "How large the error of a level of detail may look, in pixels, from 0 to 64; applied "
                        "after the preset.",
                        false},
                       {"antiAliasing", Type::String, "How edges are smoothed: \"fxaa\" or \"off\".", false}},
        .handler = [this](const Json& parameters) -> Result<Json> {
            // All of them are checked before any of them is applied.
            SceneRenderSettings wanted;
            std::optional<RenderQuality> chosenPreset;
            wanted.shadowQuality = m_viewportSettings.shadowQuality;
            wanted.screenEffectsQuality = m_viewportSettings.screenEffectsQuality;
            wanted.postProcessQuality = m_viewportSettings.postProcessQuality;
            wanted.lodErrorPixels = m_viewportSettings.lodErrorPixels;
            if (const auto name = jsonGetOr<std::string>(parameters, "preset", ""); !name.empty()) {
                const std::optional<RenderQuality> preset = renderQualityFromName(name);
                if (!preset) {
                    return makeError(ErrorCode::InvalidArgument,
                                     "'{}' is not a quality preset; viewport.quality without parameters lists them",
                                     name);
                }
                applyRenderQuality(*preset, wanted);
                chosenPreset = preset;
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
            if (parameters.contains("lodErrorPixels")) {
                const auto pixels = jsonGetOr<float>(parameters, "lodErrorPixels", wanted.lodErrorPixels);
                if (!(pixels >= 0.0f && pixels <= kMaxLodErrorPixels)) {
                    return makeError(ErrorCode::OutOfRange, "'lodErrorPixels' must be between 0 and {}, got {}",
                                     kMaxLodErrorPixels, pixels);
                }
                wanted.lodErrorPixels = pixels;
            }
            wanted.antiAliasing = m_viewportSettings.antiAliasing;
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
            m_viewportSettings.shadowQuality = wanted.shadowQuality;
            m_viewportSettings.screenEffectsQuality = wanted.screenEffectsQuality;
            m_viewportSettings.postProcessQuality = wanted.postProcessQuality;
            m_viewportSettings.lodErrorPixels = wanted.lodErrorPixels;
            m_viewportSettings.antiAliasing = wanted.antiAliasing;
            RenderResources& resources = m_engine->sceneRenderer().resources();
            if (chosenPreset) {
                resources.setTextureLimit(textureLimitFor(*chosenPreset));
            }

            Json result = Json::object();
            const std::optional<RenderQuality> preset = renderQualityOf(wanted);
            result["preset"] = preset ? Json(nameOf(*preset)) : Json(nullptr);
            result["shadows"] = nameOf(wanted.shadowQuality);
            result["effects"] = nameOf(wanted.screenEffectsQuality);
            result["postProcess"] = nameOf(wanted.postProcessQuality);
            result["lodErrorPixels"] = jsonFromFloat(wanted.lodErrorPixels);
            result["antiAliasing"] = nameOf(wanted.antiAliasing);
            result["textureLimit"] = resources.textureLimit();
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
        .description =
            "Chooses what a viewport shows: the lit picture (\"none\") or a debug view of the "
            "renderer - one thing the G-buffer holds, with what decals laid on (baseColor, normals, "
            "roughness, metallic, specular, occlusion, emissive, depth), how many point, spot and rect lights "
            "each pixel has to ask (lightCount: dark for none, blue for one, through green to red for eight "
            "and more), which cascade of the sun's shadow each pixel is looked up in "
            "(shadowCascades: red, green, blue, yellow from the nearest on; grey where the sun "
            "casts none), how much of the light from all around gets to each surface past what "
            "stands around it (ambientOcclusion: white is all of it), how much of each pixel "
            "is fog (fog: white where nothing shows through), how far and which way each pixel "
            "moved since the picture before (motion: black where nothing moved, red to the right, "
            "yellow-green up, cyan to the left, blue-purple down, brighter the further), the "
            "light without the materials (lightingOnly), what the surroundings "
            "give every surface to reflect, each of them made a perfect mirror (reflections), the "
            "edges of the triangles (wireframe), how often each pixel is drawn (overdraw), how "
            "blurred each pixel is by the depth of field (focus: black where it is sharp, red in "
            "front of that and blue behind), the light the shafts of the sun add, alone "
            "(lightShafts), what the lights throw towards the viewer from the fog in front of "
            "each pixel, alone (fogLight: black where they throw nothing), which environment probes "
            "light each surface (probes: a color for each probe, grey for the environment), what the "
            "reflections from the screen put in the place of the surroundings' reflections, alone "
            "(screenReflections: black where they meet nothing), or the level of "
            "detail each mesh is drawn at (lod: the mesh itself "
            "grey, then green, yellow, orange and red as the levels get coarser). Returns the view "
            "now shown and all there are; without a parameter it changes nothing.",
        .parameters = {{"view", Type::String, "The view to show, by one of the names above.", false},
                       viewportParameter()},
        .handler = [this](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(ViewportPanel* const viewport, viewportOf(parameters));
            if (const auto name = jsonGetOr<std::string>(parameters, "view", ""); !name.empty()) {
                const std::optional<RenderDebugView> view = renderDebugViewFromName(name);
                if (!view) {
                    return makeError(ErrorCode::InvalidArgument,
                                     "'{}' is not a view; viewport.debugView without parameters lists them", name);
                }
                viewport->setDebugView(*view);
            }
            Json views = Json::array();
            for (const RenderDebugView view : kRenderDebugViews) {
                views.push_back(nameOf(view));
            }
            Json result = Json::object();
            result["view"] = nameOf(viewport->debugView());
            result["views"] = std::move(views);
            return result;
        },
    }));

    CORDITE_TRY(m_commands.add({
        .name = "viewport.gizmo",
        .description = "Says what the gizmo of the selection does - translate, rotate or scale it - and along "
                       "whose axes, the world's or the entity's own (scale is always along the entity's own); "
                       "given either, changes it - in every viewport. The gizmo is drawn on the entity selected "
                       "last and moves every selected entity that does not hang under another selected one. "
                       "Dragging it is an edit of their Transform, one step to undo, which component.set makes "
                       "as well.",
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
                m_viewportSettings.gizmoMode = *mode;
            }
            if (space) {
                m_viewportSettings.gizmoSpace = *space;
            }
            Json result = Json::object();
            result["mode"] = std::string(nameOf(m_viewportSettings.gizmoMode));
            result["space"] = std::string(nameOf(m_viewportSettings.gizmoSpace));
            return result;
        },
    }));

    return m_commands.add({
        .name = "viewport.camera",
        .description = "Returns where the camera of a viewport is and which way it looks.",
        .parameters = {viewportParameter()},
        .handler = [this](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const ViewportPanel* const viewport, viewportOf(parameters));
            const EditorCamera& camera = viewport->camera();
            Json result = Json::object();
            result["position"] = toJson(camera.position());
            result["forward"] = toJson(camera.forward());
            result["yawDegrees"] = jsonFromFloat(camera.yawDegrees());
            result["pitchDegrees"] = jsonFromFloat(camera.pitchDegrees());
            return result;
        },
    });
}

void EditorApp::chooseTheme(UiTheme theme) {
    m_engine->ui().setTheme(theme);
    m_preferences.theme = theme;
    // The interface is in the theme either way; only the next session would not know.
    savePreferences("the theme is used, but not remembered");
}

void EditorApp::rememberAssetView() {
    const AssetBrowserView view = m_assets.browser().view();
    if (view == m_preferences.assetView) {
        return;
    }
    // Remembered once, whether the file can be written or not: a file that cannot be is
    // said once, not in every frame.
    m_preferences.assetView = view;
    savePreferences("the view of the assets is used, but not remembered");
}

void EditorApp::savePreferences(std::string_view whatIsLost) {
    if (m_preferencesFile.empty()) {
        return;
    }
    if (const Status saved = saveEditorPreferences(m_preferencesFile, m_preferences); !saved) {
        CORDITE_LOG_WARN("Editor", "{}: {}", whatIsLost, saved.error().message);
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
    // Another project is another engine - other assets, another cache - so the editor
    // ends, and whoever runs it begins again on the project (projectToOpen()).
    // A script cannot answer the question a person is asked about changes that are not
    // saved, so a command refuses to lose them unless it is told to.
    const auto refuseToLeave = [this](const Json& parameters) -> Status {
        if (m_context->isPlaying()) {
            return makeError(ErrorCode::InvalidState, "the scene is being played: stop it first");
        }
        if (m_context->hasUnsavedChanges() && !jsonGetOr<bool>(parameters, "discardChanges", false)) {
            return makeError(ErrorCode::InvalidState,
                             "the scene has changes that are not saved, and would be lost: save it, undo them, or "
                             "say discardChanges");
        }
        return {};
    };
    CORDITE_TRY(m_commands.add({
        .name = "project.new",
        .description =
            "Makes a new project - a directory with a project.cordite and its assets - with a scene to "
            "begin with: a camera that looks at a box on a floor, under a sun and a sky. Then the "
            "editor closes, and the program that runs it opens it again on the new project. Refused "
            "while the scene is played or has changes that are not saved - unless told to discard "
            "them - and for a directory that holds a project already. Returns the project and its start scene.",
        .parameters = {{"path", Type::String,
                        "The directory of the project: not there yet, or one without a project in it."},
                       {"name", Type::String, "What the game is called. Defaults to the name of the directory.", false},
                       {"discardChanges", Type::Boolean, kDiscardChangesDescription, false}},
        .handler = [this, refuseToLeave](const Json& parameters) -> Result<Json> {
            CORDITE_TRY(refuseToLeave(parameters));
            CORDITE_TRY_ASSIGN(const Project made,
                               createStarterProject(pathFromUtf8(parameters.at("path").get<std::string>()),
                                                    jsonGetOr<std::string>(parameters, "name", "")));
            m_projectToOpen = made.directory;
            m_engine->requestExit();
            Json result = Json::object();
            result["project"] = pathToUtf8(made.directory);
            result["name"] = made.settings.name;
            result["startScene"] = made.settings.startScene;
            return result;
        },
    }));
    CORDITE_TRY(m_commands.add({
        .name = "project.open",
        .description =
            "Opens another project: the editor closes, and the program that runs it opens it again on "
            "that project. Refused while the scene is played or has changes that are not saved - unless told "
            "to discard them - for a path that holds no project, and for one whose controls cannot "
            "be read; a start scene that cannot be read is said in the next editor, which opens "
            "with an empty scene. Returns the project.",
        .parameters = {{"path", Type::String, "The project: its directory, or its project.cordite."},
                       {"discardChanges", Type::Boolean, kDiscardChangesDescription, false}},
        .handler = [this, refuseToLeave](const Json& parameters) -> Result<Json> {
            CORDITE_TRY(refuseToLeave(parameters));
            CORDITE_TRY_ASSIGN(const Project project,
                               loadProject(pathFromUtf8(parameters.at("path").get<std::string>())));
            // What would keep the next editor from starting is found here, while this one is
            // still there to say so: controls that cannot be read. A start scene that cannot
            // be read does not keep a project from opening.
            if (!project.settings.inputMap.empty()) {
                CORDITE_TRY(loadInputMap(project.inputMapPath()));
            }
            m_projectToOpen = project.directory;
            m_engine->requestExit();
            Json result = Json::object();
            result["project"] = pathToUtf8(project.directory);
            result["name"] = project.settings.name;
            return result;
        },
    }));
    CORDITE_TRY(m_commands.add({
        .name = "project.recent",
        .description = "Lists the projects opened last, as File > Open recent does: the newest first, each with "
                       "whether it is still there. With forgetMissing, takes those that are not out of the list "
                       "first. The list is the person's, kept in their preferences; an editor without a file of "
                       "preferences - a batch run - has its own, which it forgets. Open one with project.open.",
        .parameters = {{"forgetMissing", Type::Boolean,
                        "Take the projects that are no longer there out of the list first. Defaults to false.", false}},
        .handler = [this](const Json& parameters) -> Result<Json> {
            Json result = Json::object();
            size_t forgotten = 0;
            if (jsonGetOr<bool>(parameters, "forgetMissing", false)) {
                forgotten = forgetMissingRecentProjects(m_preferences);
                savePreferences("the missing projects are gone from the list, but come back next time");
            }
            Json projects = Json::array();
            for (const std::filesystem::path& directory : m_preferences.recentProjects) {
                Json project = Json::object();
                project["path"] = pathToUtf8(directory);
                project["exists"] = recentProjectExists(directory);
                projects.push_back(std::move(project));
            }
            result["projects"] = std::move(projects);
            result["forgotten"] = forgotten;
            return result;
        },
    }));
    CORDITE_TRY(m_commands.add({
        .name = "project.export",
        .description = "Makes a game of the project: a directory with its program - cordite-player, under the "
                       "name of the game - the project file, the assets with their ids, the engine's shaders "
                       "compiled, and the notices of the software the program is made of; what a person plays "
                       "without the editor. On macOS the directory is an application bundle, with the program in "
                       "Contents/MacOS and the rest of the game in Contents/Resources. It runs on the kind of "
                       "system that exported it. Refused while the scene is played, and when it has changes that "
                       "are not saved: they would not be in the game. Returns the program, the directory of the "
                       "game in it, and how many files and bytes were written.",
        .parameters = {{"path", Type::String,
                        "The directory the game goes into, outside the project: not there yet, empty, or one "
                        "that a game was exported into - that game is replaced, and nothing else there. On "
                        "macOS .app is added to a name that does not end in it."}},
        .handler = [this](const Json& parameters) -> Result<Json> {
            if (!m_project) {
                return makeError(ErrorCode::InvalidState, "there is no project to make a game of: open one");
            }
            if (m_context->isPlaying()) {
                return makeError(ErrorCode::InvalidState, "the scene is being played: stop it first");
            }
            if (m_context->hasUnsavedChanges()) {
                return makeError(ErrorCode::InvalidState,
                                 "the scene has changes that are not saved, and would not be in the game: save it");
            }
            // The sources this editor compiles its own shaders from.
            const std::vector<std::filesystem::path>& sources = m_engine->shaders().sourceRoots();
            const std::filesystem::path destination =
                gameDestinationFor(pathFromUtf8(parameters.at("path").get<std::string>()), nativeGameLayout());
            CORDITE_TRY_ASSIGN(
                const GameExport made,
                exportGame(*m_project, {.destination = destination,
                                        .shaderDirectory = sources.empty() ? std::filesystem::path() : sources.front(),
                                        // Where this editor keeps its own: what it
                                        // compiled is not compiled again.
                                        .shaderCache = m_engine->shaders().cacheDirectory()}));
            Json result = Json::object();
            result["program"] = pathToUtf8(made.program);
            result["game"] = pathToUtf8(made.game);
            result["files"] = made.files;
            result["bytes"] = made.bytes;
            return result;
        },
    }));

    CORDITE_TRY(m_commands.add({
        .name = "asset.listen",
        .description = "Plays a sound of the assets for whoever is at the editor: once, from its beginning, without a "
                       "place and as loud as it was made - also while the scene is not played, which is silent "
                       "otherwise. A sound that is heard already is replaced. Without `asset`, stops what is heard. "
                       "Returns the sound's length in seconds, its channels and its rate, and whether it plays.",
        .parameters = {{"asset", Type::String, "The sound: its id, or where it is. Left out, what plays stops.",
                        false}},
        .handler = [this](const Json& parameters) -> Result<Json> {
            Json result = Json::object();
            if (!parameters.contains("asset")) {
                m_engine->audio().stopPreview();
                result["playing"] = false;
                return result;
            }
            CORDITE_TRY_ASSIGN(const AssetInfo* asset,
                               m_context->resolveAsset(parameters.at("asset").get<std::string>()));
            if (asset->type != AssetType::Sound) {
                return makeError(ErrorCode::InvalidArgument, "{} is {}, which is not heard", asset->reference(),
                                 withArticle(asset->type));
            }
            const Uuid id = asset->id;
            CORDITE_TRY_ASSIGN(const std::shared_ptr<const SoundData> sound, m_context->assets().loadSound(id));
            CORDITE_TRY(m_engine->audio().preview(m_context->assets(), id));
            result["seconds"] = sound->seconds();
            result["channels"] = sound->channels;
            result["sampleRate"] = sound->sampleRate;
            result["playing"] = true;
            return result;
        },
    }));

    CORDITE_TRY(m_commands.add({
        .name = "asset.preview",
        .description = "Draws a picture of an asset as the Assets panel shows it - a mesh as it is, a material on a "
                       "ball, a model or a prefab as it is placed, whole, from in front and above - and saves it "
                       "as a PNG file. A texture is its own picture: its file. Returns the path and the size.",
        .parameters = {{"asset", Type::String, "The asset: its id, or where it is."},
                       {"path", Type::String, "Where to save the picture. Missing directories are made."},
                       {"size", Type::Integer,
                        fmt::format("The sides of the square picture, in pixels, from {} to {}. Defaults to {}.",
                                    AssetPreview::kMinSize, AssetPreview::kMaxSize, kPreviewSize),
                        false}},
        .handler = [this](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const AssetInfo* asset,
                               m_context->resolveAsset(parameters.at("asset").get<std::string>()));
            if (!AssetPreview::isDrawn(asset->type)) {
                return makeError(
                    ErrorCode::InvalidArgument, "{} is {}, {}", asset->reference(), withArticle(asset->type),
                    asset->type == AssetType::Texture ? "whose file is its picture" : "which has no picture");
            }
            const auto size = jsonGetOr<int64_t>(parameters, "size", kPreviewSize);
            if (size < AssetPreview::kMinSize || size > AssetPreview::kMaxSize) {
                return makeError(ErrorCode::OutOfRange, "'size' is from {} to {} pixels, not {}",
                                 AssetPreview::kMinSize, AssetPreview::kMaxSize, size);
            }
            const Uuid id = asset->id;
            CORDITE_TRY_ASSIGN(const Image image, previewImage(id, static_cast<uint32_t>(size)));
            const std::filesystem::path path = pathFromUtf8(parameters.at("path").get<std::string>());
            if (const std::filesystem::path directory = path.parent_path(); !directory.empty()) {
                CORDITE_TRY(ensureDirectory(directory));
            }
            CORDITE_TRY(image.savePng(path));
            Json result = Json::object();
            result["path"] = pathToUtf8(path);
            result["width"] = image.width();
            result["height"] = image.height();
            return result;
        },
    }));

    // Here and not with the other commands of assets: the project names its start scene by
    // where it is, and the application is what has the project.
    CORDITE_TRY(m_commands.add({
        .name = "asset.delete",
        .description =
            "Deletes the file of an asset and its .meta, and reads the assets again: a model goes with all "
            "that is in it; the buffers and images a glTF file names stay. Refused while the scene is played, "
            "for what is built in or inside a file (delete the file), for the scene that is open, and - "
            "unless told to force it - for an asset that something uses: another asset, a property of the "
            "open scene, or the project, whose start scene it is. The message lists what uses it. Returns the "
            "file, and what used it. Files are no part of the document: editor.undo does not bring it back.",
        .parameters = {{"asset", Type::String, "The asset whose file is deleted: its id, or where it is."},
                       {"force", Type::Boolean,
                        "Delete it although something uses it, which then refers to an asset that is not "
                        "there. Defaults to false.",
                        false}},
        .handler = [this](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const AssetInfo* asset,
                               m_context->resolveAsset(parameters.at("asset").get<std::string>()));
            const bool force = jsonGetOr<bool>(parameters, "force", false);
            const bool startScene = m_project && m_project->settings.startScene == asset->path;
            if (startScene && !force) {
                return makeError(ErrorCode::InvalidState,
                                 "{} is the start scene of the project: delete it anyway with force, or have the "
                                 "project start with another first",
                                 asset->path);
            }
            CORDITE_TRY_ASSIGN(const AssetDeletion deleted, deleteAsset(*m_context, asset->id, force));
            const AssetDatabase& database = m_context->assets().database();
            Json users = Json::array();
            for (const Uuid& user : deleted.users.assets) {
                // Other files, which are still there after the assets were read again.
                const AssetInfo* found = database.find(user);
                users.push_back(found != nullptr ? found->reference() : user.toString());
            }
            Json result = Json::object();
            result["path"] = deleted.path;
            result["usedBy"] = std::move(users);
            result["usedInScene"] = deleted.users.sceneReferences;
            result["wasStartScene"] = startScene;
            return result;
        },
    }));
    CORDITE_TRY(m_commands.add({
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
            "editor.undo does not move it back.",
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
    }));

    return m_commands.add({
        .name = "asset.moveDirectory",
        .description =
            "Moves or renames a directory of the assets, with all that is in it. Every asset in it keeps its id - a "
            "file without a .meta gets one first - so whatever refers to them by id follows, and so do the file of "
            "the open scene and the start scene and the input map of the project. Refused, moving nothing, for a "
            "directory that is not there, a place that holds something, a place inside the directory, a path "
            "outside the assets or with a part that begins with a dot, and for a move after which a glTF file would "
            "not find a file it names by where it is. Returns every asset that moved, each with `mentions`: lines "
            "of scripts, scenes and prefabs, and values in the open scene, that still name it by where it was - "
            "those follow by hand. Files are no part of the document: editor.undo does not move them back.",
        .parameters = {{"from", Type::String,
                        "The directory, inside the asset directory, with forward slashes: \"textures/old\"."},
                       {"to", Type::String, "Where it goes, likewise: \"art/textures\"."}},
        .handler = [this](const Json& parameters) -> Result<Json> {
            CORDITE_TRY_ASSIGN(const DirectoryMove moved,
                               moveAssetDirectory(*m_context, parameters.at("from").get<std::string>(),
                                                  parameters.at("to").get<std::string>()));
            Json result = describeDirectoryMove(*m_context, moved);
            // The project names its start scene and its controls by where they are.
            if (m_project) {
                bool changed = false;
                const auto follow = [&](std::string& setting, const char* key) {
                    if (setting.starts_with(moved.from + "/")) {
                        setting = moved.to + setting.substr(moved.from.size());
                        result[key] = setting;
                        changed = true;
                    }
                };
                follow(m_project->settings.startScene, "startScene");
                follow(m_project->settings.inputMap, "inputMap");
                if (const Status saved = changed ? saveProject(*m_project) : Status(); !saved) {
                    return tl::unexpected(saved.error().withContext(
                        fmt::format("{} was moved to {}, but the project still names what was in it by where it was",
                                    moved.from, moved.to)));
                }
            }
            for (const Json& asset : result["assets"]) {
                for (const Json& mention : asset["mentions"]) {
                    CORDITE_LOG_WARN("Editor", "{} still names {}: {}",
                                     jsonGetOr<std::string>(mention, "file", "the open scene"),
                                     asset["from"].get<std::string>(), mention.dump());
                }
            }
            return result;
        },
    });
}

Result<Image> EditorApp::previewImage(const Uuid& asset, uint32_t size) {
    if (m_engine->isFrameOpen()) {
        return makeError(ErrorCode::InvalidState, "a picture of an asset cannot be drawn during a frame");
    }
    // With a command list of its own, as the viewport is captured, and a preview of its
    // own: the panel's shows what is chosen there.
    nvrhi::IDevice* device = m_engine->graphics().device();
    const nvrhi::CommandListHandle commandList = device->createCommandList();
    AssetPreview preview;
    commandList->open();
    const Status shown = preview.show(m_engine->sceneRenderer(), device, commandList, m_context->assets(),
                                      m_context->components(), asset, size, m_viewportSettings.shadowQuality);
    commandList->close();
    CORDITE_TRY(shown);
    device->executeCommandList(commandList);
    CORDITE_TRY_ASSIGN(TexturePixels pixels, readTexture(m_engine->graphics(), preview.picture()));
    return Image::fromPixels(pixels.width, pixels.height, ImageFormat::Rgba8, std::move(pixels.data));
}

void EditorApp::followAssetFiles() {
    if (!m_assetWatcher) {
        return;
    }
    const uint64_t revision = m_context->assets().database().revision();
    const uint64_t saves = m_context->saveCount();
    if (revision != m_watchedRevision || saves != m_watchedSaves) {
        // What the editor wrote itself, or has just read - with the .meta files that reading
        // wrote - is no change of the files.
        m_assetWatcher->rebaseline();
        m_watchedRevision = revision;
        m_watchedSaves = saves;
        return;
    }
    const std::vector<std::string> changes = m_assetWatcher->takeChanges();
    if (changes.empty()) {
        return;
    }
    CORDITE_LOG_INFO("Editor", "{} changed: the assets are read again",
                     changes.size() == 1 ? changes.front() : fmt::format("{} files", changes.size()));
    // Before the next frame, as a command: what it finds is said in the console.
    m_submittedLines.emplace_back("asset.refresh");
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
                       "and its sounds are heard, and the first viewport shows it through the camera of the "
                       "game (viewport.show changes that); the others keep their cameras. Nothing of it stays: "
                       "play.stop puts the scene back as it was, and what is edited while it plays goes with "
                       "the game. Returns what play.status returns.",
        .parameters = {},
        .handler = [this](const Json& /*parameters*/) -> Result<Json> {
            CORDITE_TRY(m_play->start());
            // A game is looked at through its own camera, until somebody says otherwise - in
            // the first viewport, beside which the others go on showing what they showed.
            m_viewBeforePlay = viewport().viewCamera();
            viewport().setViewCamera(ViewportCamera::Game);
            return describePlay();
        },
    }));

    CORDITE_TRY(m_commands.add({
        .name = "play.stop",
        .description = "Stops playing: the scripts end, and the scene is what it was when play.start was called "
                       "- its entities, its history, what is selected of what is still there. The first "
                       "viewport looks through the camera it looked through before.",
        .parameters = {},
        .handler = [this](const Json& /*parameters*/) -> Result<Json> {
            CORDITE_TRY(m_play->stop());
            viewport().setViewCamera(m_viewBeforePlay);
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
    drawToolbar(documentButtons(*m_context), gizmoButtons(m_viewportSettings.gizmoMode, m_viewportSettings.gizmoSpace),
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
    // Before the dock space is laid out: where the viewports opened since the last frame go is
    // part of its layout.
    placeOpenedViewports(dockSpaceId);
    ImGui::DockSpaceOverViewport(dockSpaceId, ImGui::GetMainViewport());

    // The first of them that draws a picture draws the scene captures, for all of them. The
    // one whose window has the focus is the one used last; with the focus elsewhere, the one
    // used last stays what it was.
    ViewportFrame frame{.clock = m_play->clock()};
    for (const std::unique_ptr<ViewportPanel>& viewport : m_viewports) {
        if (viewport) {
            viewport->draw(*m_engine, *m_context, frame, m_submittedLines);
            if (viewport->isFocused()) {
                m_activeViewport = viewport->number();
            }
        }
    }
    handleFrameShortcut();
    m_hierarchy.draw(*m_context);
    m_inspector.draw(*m_context, m_submittedLines);
    m_console.draw(m_submittedLines);
    // In a layout that was remembered before there was an Assets panel it has no place
    // of its own: there it goes where the console is, the first time it is seen.
    if (const ImGuiWindow* console = ImGui::FindWindowByName(ConsolePanel::kWindowId);
        console != nullptr && console->DockId != 0) {
        ImGui::SetNextWindowDockID(console->DockId, ImGuiCond_FirstUseEver);
    }
    m_assets.draw(*m_engine, *m_context, m_viewportSettings.shadowQuality, m_submittedLines);
    rememberAssetView();
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
    // Last, so that it is in front of everything.
    drawUnsavedChangesQuestion();
    drawKeptSceneQuestion();
}

void EditorApp::handleShortcuts() {
    // Global shortcuts give way to a text field that has the focus, so that Ctrl+Z in
    // a name field undoes typing and Delete removes a character. Every shortcut is asked for
    // in every frame, and only then is it decided whether it applies: Dear ImGui gives a key
    // to a shortcut from the frame after the one it was asked for in, and would lose the
    // first press after a frame in which it was not.
    const ImGuiInputFlags route = ImGuiInputFlags_RouteGlobal;
    UndoStack& history = m_context->history();

    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, route) && history.canUndo()) {
        reportIfFailed(history.undo());
    }
    // Two shortcuts redo, and both are asked for when the first is pressed.
    const bool redoPressed = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, route);
    const bool redoWithZPressed = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, route);
    if ((redoPressed || redoWithZPressed) && history.canRedo()) {
        reportIfFailed(history.redo());
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, route)) {
        if (m_context->scenePath().empty()) {
            askForPath(PathDialog::SaveSceneAs);
        } else {
            reportIfFailed(m_context->saveScene());
        }
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, route)) {
        leave(Leaving::OpenScene);
    }

    // Playing begins and ends between two frames, where commands run: these are queued
    // like lines typed into the console, and run before the next frame.
    const PlayState playing = m_play->state();
    const bool stopPressed = ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_F5, route);
    const bool startPressed = ImGui::Shortcut(ImGuiKey_F5, route);
    if (stopPressed && playing != PlayState::Stopped) {
        m_submittedLines.emplace_back("play.stop");
    } else if (startPressed && playing == PlayState::Stopped) {
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
    // What the gizmo does, as in the editors people know - unless a camera is being flown,
    // which takes the same keys, or the gizmo is being dragged in a viewport. The keys are
    // asked for all the same (see handleShortcuts).
    const bool translate = ImGui::Shortcut(ImGuiKey_W, route);
    const bool rotate = ImGui::Shortcut(ImGuiKey_E, route);
    const bool scale = ImGui::Shortcut(ImGuiKey_R, route);
    const bool switchSpace = ImGui::Shortcut(ImGuiKey_X, route);
    const bool busy = std::ranges::any_of(m_viewports, [](const std::unique_ptr<ViewportPanel>& viewport) {
        return viewport && (viewport->camera().isNavigating() || viewport->isDraggingGizmo());
    });
    if (busy) {
        return;
    }
    if (translate) {
        m_viewportSettings.gizmoMode = GizmoMode::Translate;
    }
    if (rotate) {
        m_viewportSettings.gizmoMode = GizmoMode::Rotate;
    }
    if (scale) {
        m_viewportSettings.gizmoMode = GizmoMode::Scale;
    }
    if (switchSpace) {
        m_viewportSettings.gizmoSpace =
            m_viewportSettings.gizmoSpace == GizmoSpace::World ? GizmoSpace::Local : GizmoSpace::World;
    }
}

void EditorApp::handleFrameShortcut() {
    // F frames what is selected in the viewport used last, wherever the pointer is - but not
    // into a text being typed, nor while its camera is being steered, nor through the game's.
    ViewportPanel& viewport = activePanel();
    if (!viewport.showsGame() && m_engine->input().wasKeyPressed(Key::F) && !ImGui::GetIO().WantTextInput &&
        !viewport.camera().isNavigating()) {
        viewport.frameSelection(*m_context);
    }
}

void EditorApp::handleEntityShortcuts() {
    const ImGuiInputFlags route = ImGuiInputFlags_RouteGlobal;
    // Asked for also while nothing is selected (see handleShortcuts): the first key after a
    // click that selects something is pressed in the frame right after it.
    const bool pastePressed = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V, route);
    const bool duplicatePressed = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D, route);
    const bool copyPressed = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C, route);
    const bool cutPressed = ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_X, route);
    const bool deletePressed = ImGui::Shortcut(ImGuiKey_Delete, route);

    // Pasting needs nothing selected: what was copied goes where it came from.
    if (pastePressed && !m_context->clipboard().is_null()) {
        reportIfFailed(EditorActions(*m_context).pasteEntities());
    }
    const std::vector<Entity> selection = m_context->selection();
    if (selection.empty()) {
        return;
    }
    if (duplicatePressed) {
        reportIfFailed(EditorActions(*m_context).duplicateEntities(selection));
    }
    if (copyPressed) {
        reportIfFailed(EditorActions(*m_context).copyEntities(selection));
    }
    if (cutPressed) {
        reportIfFailed(EditorActions(*m_context).cutEntities(selection));
    }
    if (deletePressed) {
        reportIfFailed(EditorActions(*m_context).deleteEntities(selection));
    }
}

void EditorApp::drawFileMenu() {
    if (!ImGui::BeginMenu("File")) {
        return;
    }
    // An item that asks for a path in a dialog: none without dialogs, which it says.
    const bool dialogs = hasFileDialogs();
    const auto pathItem = [dialogs](const char* label, const char* shortcut, bool enabled, const auto& choose) {
        if (ImGui::MenuItem(label, shortcut, false, enabled && dialogs)) {
            choose();
        }
        if (!dialogs && ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip("%s", kNoDialogs);
        }
    };
    // What leaves the scene asks first whether to save its changes, if it has any.
    pathItem("New project...", nullptr, !m_context->isPlaying(), [this] { leave(Leaving::NewProject); });
    pathItem("Open project...", nullptr, !m_context->isPlaying(), [this] { leave(Leaving::OpenProject); });
    if (ImGui::BeginMenu("Open recent", !m_context->isPlaying())) {
        drawRecentProjectsMenu();
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem("New scene")) {
        leave(Leaving::NewScene);
    }
    pathItem("Open scene...", "Ctrl+O", true, [this] { leave(Leaving::OpenScene); });
    if (ImGui::MenuItem("Save", "Ctrl+S", false, !m_context->scenePath().empty())) {
        reportIfFailed(m_context->saveScene());
    }
    pathItem("Save as...", nullptr, true, [this] { askForPath(PathDialog::SaveSceneAs); });
    ImGui::Separator();
    pathItem("Export game...", nullptr, m_project.has_value() && !m_context->isPlaying(),
             [this] { askForPath(PathDialog::ExportGame); });
    ImGui::Separator();
    if (ImGui::MenuItem("Exit")) {
        leave(Leaving::Exit);
    }
    ImGui::EndMenu();
}

void EditorApp::drawMainMenu() {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    EditorActions actions(*m_context);
    UndoStack& history = m_context->history();
    const std::vector<Entity> selection = m_context->selection();

    drawFileMenu();

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
    // What is about one viewport is about the one used last - which is said, when there are
    // several of them.
    ViewportPanel& viewport = activePanel();
    const auto open =
        static_cast<uint32_t>(std::ranges::count_if(m_viewports, [](const auto& panel) { return panel != nullptr; }));
    if (open > 1) {
        ImGui::SeparatorText(viewport.title().c_str());
    }
    // What the viewport shows: the picture, or a look into the renderer.
    if (const auto view = chooseFromMenu("Render view", kRenderDebugViews, viewport.debugView())) {
        viewport.setDebugView(*view);
    }
    // Through whose eyes: the editor's camera, or the one the game renders from.
    const bool game = viewport.viewCamera() == ViewportCamera::Game;
    if (ImGui::MenuItem("Game camera", nullptr, game)) {
        viewport.setViewCamera(game ? ViewportCamera::Editor : ViewportCamera::Game);
    }
    // What is drawn over the scene that is not part of it.
    if (ImGui::MenuItem("Colliders", nullptr, viewport.showsColliders())) {
        viewport.setShowColliders(!viewport.showsColliders());
    }
    ImGui::Separator();
    // Another, beside those that are open: between frames, where commands run.
    if (ImGui::MenuItem("Open viewport", nullptr, false, open < kMaxViewports)) {
        m_submittedLines.emplace_back("viewport.open");
    }
    // What the pictures of the viewports may cost: everything by one word, or piece by piece.
    if (ImGui::BeginMenu("Quality")) {
        SceneRenderSettings current;
        current.shadowQuality = m_viewportSettings.shadowQuality;
        current.screenEffectsQuality = m_viewportSettings.screenEffectsQuality;
        current.postProcessQuality = m_viewportSettings.postProcessQuality;
        current.lodErrorPixels = m_viewportSettings.lodErrorPixels;
        const std::optional<RenderQuality> preset = renderQualityOf(current);
        for (const RenderQuality quality : kRenderQualities) {
            const std::string label(displayNameOf(quality));
            if (ImGui::MenuItem(label.c_str(), nullptr, preset == quality)) {
                // By the command: a preset is also the size of the textures, which every
                // view of the editor shares.
                m_submittedLines.push_back(fmt::format(R"(viewport.quality {{"preset": "{}"}})", nameOf(quality)));
            }
        }
        ImGui::Separator();
        if (const auto quality = chooseFromMenu("Shadows", kShadowQualities, m_viewportSettings.shadowQuality)) {
            m_viewportSettings.shadowQuality = *quality;
        }
        if (const auto quality =
                chooseFromMenu("Screen effects", kScreenEffectsQualities, m_viewportSettings.screenEffectsQuality)) {
            m_viewportSettings.screenEffectsQuality = *quality;
        }
        if (const auto quality =
                chooseFromMenu("Post-processing", kPostProcessQualities, m_viewportSettings.postProcessQuality)) {
            m_viewportSettings.postProcessQuality = *quality;
        }
        if (const auto antiAliasing =
                chooseFromMenu("Anti-aliasing", kAntiAliasingModes, m_viewportSettings.antiAliasing)) {
            m_viewportSettings.antiAliasing = *antiAliasing;
        }
        ImGui::EndMenu();
    }
    ImGui::MenuItem("Statistics", nullptr, &m_showStatistics);
    // The probes show what was around them when they were taken.
    if (ImGui::MenuItem("Capture probes")) {
        m_submittedLines.emplace_back("probes.capture");
    }
    ImGui::Separator();
    // The style changes between frames, where commands run: changed in the middle of one,
    // what is pushed onto it would be popped back to the old theme.
    if (const auto theme = chooseFromMenu("Theme", kUiThemes, m_engine->ui().theme())) {
        m_submittedLines.push_back(fmt::format(R"(editor.theme {{"theme": "{}"}})", nameOf(*theme)));
    }
    ImGui::EndMenu();
}

void EditorApp::drawRecentProjectsMenu() {
    if (m_preferences.recentProjects.empty()) {
        ImGui::TextDisabled("No project has been opened yet");
        return;
    }
    bool anyMissing = false;
    for (size_t index = 0; index < m_preferences.recentProjects.size(); ++index) {
        const std::filesystem::path& directory = m_preferences.recentProjects[index];
        const bool exists = recentProjectExists(directory);
        const bool open = m_project && sameProjectDirectory(m_project->directory, directory);
        anyMissing = anyMissing || !exists;
        // Known by its place: a path is no id, and may hold what ImGui reads in a label (###).
        const std::string label =
            fmt::format("{}{}###recent{}", pathToUtf8(directory), exists ? "" : " (missing)", index);
        if (ImGui::MenuItem(label.c_str(), nullptr, open, exists && !open)) {
            openRecentProject(directory);
        }
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Remove missing projects", nullptr, false, anyMissing)) {
        forgetMissingRecentProjects(m_preferences);
        savePreferences("the missing projects are gone from the list, but come back next time");
    }
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
        m_submittedLines.emplace_back("asset.refresh");
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

FileDialogRequest EditorApp::pathRequest(PathDialog dialog) const {
    const std::filesystem::path project = m_project ? m_project->directory : std::filesystem::path();
    const std::filesystem::path assets = m_project ? m_project->assetDirectory() : std::filesystem::path();
    switch (dialog) {
        case PathDialog::OpenScene:
            return openSceneRequest(assets, m_context->scenePath());
        case PathDialog::SaveSceneAs:
            return saveSceneRequest(assets, m_context->scenePath(), m_context->world().sceneName());
        case PathDialog::ExportGame:
            return exportGameRequest(project, m_project ? m_project->settings.name : std::string(), nativeGameLayout());
        case PathDialog::NewProject:
            return newProjectRequest(project);
        case PathDialog::OpenProject:
            return openProjectRequest(project);
        case PathDialog::None:
            break;
    }
    return {};
}

void EditorApp::showPathDialog() {
    const PathDialog dialog = std::exchange(m_pathDialog, PathDialog::None);
    // Both belong to this dialog alone: what a question about unsaved changes left for it
    // to do - leave once the scene is saved, give up its changes - is not done for another.
    const Leaving afterSave = std::exchange(m_leavingAfterSave, Leaving::None);
    const bool discardChanges = std::exchange(m_discardChanges, false);
    if (dialog == PathDialog::None) {
        return;
    }
    if (m_fileDialogs == nullptr) {
        // Said where what the user asked for and did not get is said: the console.
        CORDITE_LOG_ERROR(kInterfaceLogCategory, "{}", kNoDialogs);
        return;
    }
    const FileDialogRequest request = pathRequest(dialog);
    const SystemWindow parent = m_engine->window() != nullptr ? m_engine->window()->systemWindow() : SystemWindow{};
    const Result<std::optional<std::filesystem::path>> answer = m_fileDialogs->ask(request, parent);
    if (!reportIfFailed(answer) || !answer->has_value()) {
        // What could not be asked is in the console; what was cancelled changes nothing.
        return;
    }
    const std::filesystem::path& path = **answer;
    // What leaves this scene or this project runs as a command, which says what it did or
    // why it would not.
    Json parameters = Json::object();
    parameters["path"] = pathToUtf8(path);
    // A person who said not to save the changes has said what the project commands ask.
    Json leavingParameters = parameters;
    if (discardChanges) {
        leavingParameters["discardChanges"] = true;
    }
    switch (dialog) {
        case PathDialog::OpenScene:
            reportIfFailed(m_context->openScene(path));
            break;
        case PathDialog::SaveSceneAs:
            if (reportIfFailed(m_context->saveScene(path))) {
                goAhead(afterSave, false);
            }
            break;
        case PathDialog::ExportGame:
            m_submittedLines.push_back(fmt::format("project.export {}", parameters.dump()));
            break;
        case PathDialog::NewProject:
            // Named after the directory it is made in, which the dialog asked the name of.
            m_submittedLines.push_back(fmt::format("project.new {}", leavingParameters.dump()));
            break;
        case PathDialog::OpenProject:
            m_submittedLines.push_back(fmt::format("project.open {}", leavingParameters.dump()));
            break;
        case PathDialog::None:
            break;
    }
}

void EditorApp::openRecentProject(const std::filesystem::path& directory) {
    m_recentProjectToOpen = directory;
    leave(Leaving::OpenRecentProject);
}

void EditorApp::leave(Leaving leaving) {
    if (leaving == Leaving::None) {
        return;
    }
    if (!m_context->hasUnsavedChanges()) {
        goAhead(leaving, false);
        return;
    }
    m_leaving = leaving;
}

void EditorApp::answerUnsavedChanges(UnsavedChangesAnswer answer) {
    const Leaving leaving = std::exchange(m_leaving, Leaving::None);
    if (leaving == Leaving::None) {
        return;
    }
    switch (answer) {
        case UnsavedChangesAnswer::Save:
            if (m_context->scenePath().empty()) {
                // Saved where a dialog says first; the dialog goes ahead once it has.
                m_leavingAfterSave = leaving;
                askForPath(PathDialog::SaveSceneAs);
            } else if (reportIfFailed(m_context->saveScene())) {
                goAhead(leaving, false);
            }
            break;
        case UnsavedChangesAnswer::DontSave:
            goAhead(leaving, true);
            break;
        case UnsavedChangesAnswer::Cancel:
            break;
    }
}

void EditorApp::goAhead(Leaving leaving, bool discardChanges) {
    switch (leaving) {
        case Leaving::NewScene:
            reportIfFailed(m_context->newScene());
            break;
        case Leaving::OpenScene:
            // Opening a scene replaces the one there, and asks nothing of changes.
            askForPath(PathDialog::OpenScene);
            break;
        case Leaving::NewProject:
            askForPath(PathDialog::NewProject);
            m_discardChanges = discardChanges;
            break;
        case Leaving::OpenProject:
            askForPath(PathDialog::OpenProject);
            m_discardChanges = discardChanges;
            break;
        case Leaving::OpenRecentProject: {
            Json parameters = Json::object();
            parameters["path"] = pathToUtf8(std::exchange(m_recentProjectToOpen, {}));
            if (discardChanges) {
                parameters["discardChanges"] = true;
            }
            m_submittedLines.push_back(fmt::format("project.open {}", parameters.dump()));
            break;
        }
        case Leaving::Exit:
            m_engine->requestExit();
            break;
        case Leaving::None:
            break;
    }
}

void EditorApp::drawKeptSceneQuestion() {
    static constexpr const char* kTitle = "Kept scene###KeptScene";
    const bool asking = m_keptScene && !m_keptSceneLater && !m_context->isPlaying();
    if (asking && !ImGui::IsPopupOpen(kTitle)) {
        ImGui::OpenPopup(kTitle);
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kTitle, nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    if (!asking) {
        // Answered by a command since it was drawn last.
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const std::string scene =
        m_keptScene->scenePath.empty() ? std::string("a new scene") : pathToUtf8(m_keptScene->scenePath.filename());
    ImGui::TextUnformatted(
        fmt::format("The editor stopped - the GPU was lost - before the changes of {} were saved.", scene).c_str());
    ImGui::TextUnformatted("It kept them. Open them, in place of the scene that is open?");
    ImGui::Spacing();
    if (ImGui::Button("Open")) {
        m_submittedLines.emplace_back(R"(scene.recover {"open": true, "discardChanges": true})");
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Let them go")) {
        m_submittedLines.emplace_back(R"(scene.recover {"open": false})");
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Later")) {
        // Asked again the next time the project opens; scene.recover answers meanwhile.
        m_keptSceneLater = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void EditorApp::drawUnsavedChangesQuestion() {
    // Known by what follows ###, as every window of the editor is (see windowName).
    static constexpr const char* kTitle = "Unsaved changes###UnsavedChanges";
    if (m_leaving != Leaving::None && !ImGui::IsPopupOpen(kTitle)) {
        ImGui::OpenPopup(kTitle);
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal(kTitle, nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }
    if (m_leaving == Leaving::None) {
        // Answered from outside - by a test or another part of the editor - since it was
        // drawn last.
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::Text("The scene \"%s\" has changes that are not saved.", m_context->world().sceneName().c_str());
    ImGui::TextUnformatted("Save them first?");
    ImGui::Spacing();
    // Every key is asked for in every frame, and then decided on: a shortcut is heard from
    // the frame after the one that asked for it.
    const bool enterPressed = ImGui::Shortcut(ImGuiKey_Enter);
    const bool keypadEnterPressed = ImGui::Shortcut(ImGuiKey_KeypadEnter);
    const bool escapePressed = ImGui::Shortcut(ImGuiKey_Escape);
    const bool saveClicked = ImGui::Button("Save");
    ImGui::SameLine();
    const bool dontSaveClicked = ImGui::Button("Don't save");
    ImGui::SameLine();
    const bool cancelClicked = ImGui::Button("Cancel");
    std::optional<UnsavedChangesAnswer> answer;
    if (saveClicked || enterPressed || keypadEnterPressed) {
        answer = UnsavedChangesAnswer::Save;
    } else if (dontSaveClicked) {
        answer = UnsavedChangesAnswer::DontSave;
    } else if (cancelClicked || escapePressed) {
        answer = UnsavedChangesAnswer::Cancel;
    }
    if (answer) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    if (answer) {
        answerUnsavedChanges(*answer);
    }
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
