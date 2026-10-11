#include "cordite/editor/EditorApp.h"

#include "cordite/core/FileSystem.h"
#include "cordite/core/Log.h"
#include "cordite/core/StringUtils.h"
#include "cordite/editor/EditorActions.h"
#include "cordite/editor/EditorCommands.h"
#include "cordite/editor/EditorFeedback.h"
#include "cordite/math/MathJson.h"
#include "cordite/rhi/TextureReadback.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <limits>
#include <span>
#include <utility>

namespace cordite {
namespace {

using Type = CommandParameterType;

constexpr const char* kEditorName = "Cordite Editor";
constexpr const char* kFileDialogId = "Scene file";
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

    ImGui::DockBuilderDockWindow("Hierarchy", left);
    ImGui::DockBuilderDockWindow("Inspector", right);
    ImGui::DockBuilderDockWindow("Console", bottom);
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderFinish(dockSpaceId);
}

} // namespace

Result<std::unique_ptr<EditorApp>> EditorApp::create(const EditorAppDesc& desc) {
    std::unique_ptr<EditorApp> app(new EditorApp());

    EngineDesc engineDesc = desc.engine;
    std::filesystem::path scene = desc.scene;
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

    app->m_context =
        std::make_unique<EditorContext>(app->m_engine->world(), app->m_engine->components(), app->m_engine->assets());
    CORDITE_TRY(registerEditorCommands(app->m_commands, *app->m_context));
    CORDITE_TRY(app->registerApplicationCommands());
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
    m_engine->world().updateWorldTransforms();
    drawInterface();
    CORDITE_TRY(m_engine->endFrame());

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
            CORDITE_LOG_ERROR("Console", "{}: the parameters are not valid JSON ({})", name, parsed.error().message);
            return;
        }
        parameters = std::move(*parsed);
    }

    const Result<Json> result = m_commands.execute(name, parameters);
    if (!result) {
        CORDITE_LOG_ERROR("Console", "{}", result.error().message);
    } else if (result->is_null()) {
        CORDITE_LOG_INFO("Console", "{}: done", name);
    } else {
        CORDITE_LOG_INFO("Console", "{}:\n{}", name, dumpJson(*result, 2));
    }
}

Result<Image> EditorApp::captureViewport(UVec2 size) {
    CORDITE_ASSERT(!m_engine->isFrameOpen(), "the viewport cannot be captured during a frame");
    if (m_engine->isFrameOpen()) {
        return makeError(ErrorCode::InvalidState, "the viewport cannot be captured during a frame");
    }

    m_engine->world().updateWorldTransforms();

    // Rendered with a command list of its own, so that capturing neither needs nor
    // disturbs a frame of the editor.
    nvrhi::IDevice* device = m_engine->graphics().device();
    const nvrhi::CommandListHandle commandList = device->createCommandList();
    commandList->open();
    const Status rendered = m_viewport->render(device, commandList, *m_context, size);
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
        .description = "Moves the viewport camera so that an entity is in view, keeping the viewing direction.",
        .parameters = {{"entity", Type::String, "The entity, by id, path or name."},
                       {"radius", Type::Number, "The size of the area to show around it, in metres. Defaults to 1.",
                        false}},
        .handler = [this](const Json& parameters) -> Result<Json> {
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

void EditorApp::drawInterface() {
    handleShortcuts();
    drawMainMenu();

    // A layout remembered from an earlier session wins; otherwise the default is built.
    const ImGuiID dockSpaceId = ImGui::GetID("EditorDockSpace");
    if (ImGui::DockBuilderGetNode(dockSpaceId) == nullptr) {
        buildDefaultLayout(dockSpaceId);
    }
    ImGui::DockSpaceOverViewport(dockSpaceId, ImGui::GetMainViewport());

    m_viewport->draw(*m_engine, *m_context);
    m_hierarchy.draw(*m_context);
    m_inspector.draw(*m_context);
    m_console.draw(m_submittedLines);
    drawFileDialog();

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

    const std::vector<Entity> selection = m_context->selection();
    if (!selection.empty()) {
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D, route)) {
            reportIfFailed(EditorActions(*m_context).duplicateEntities(selection));
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
            m_context->newScene();
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

    if (ImGui::BeginMenu("Help")) {
        // Every widget ImGui has, with its source: the reference for writing panels.
        ImGui::MenuItem("Interface demo", nullptr, &m_showInterfaceDemo);
        ImGui::EndMenu();
    }

    // The scene and whether it has unsaved changes, at the right end of the bar.
    const std::string status =
        fmt::format("{}{}", m_context->world().sceneName(), m_context->hasUnsavedChanges() ? " *" : "");
    const float statusWidth = ImGui::CalcTextSize(status.c_str()).x + ImGui::GetStyle().ItemSpacing.x * 2.0f;
    if (ImGui::GetContentRegionAvail().x > statusWidth) {
        ImGui::SameLine(ImGui::GetWindowWidth() - statusWidth);
        ImGui::TextDisabled("%s", status.c_str());
    }
    ImGui::EndMainMenuBar();
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
    ImGui::EndMenu();
}

void EditorApp::drawAssetMenu() {
    if (!ImGui::BeginMenu("Assets")) {
        return;
    }
    if (ImGui::MenuItem("Refresh")) {
        reportIfFailed(m_context->assets().refresh());
    }
    if (ImGui::BeginMenu("Place model")) {
        // Collected first: placing a model must not happen while the list is walked.
        Uuid chosen;
        bool any = false;
        for (const AssetInfo& asset : m_context->assets().database().assets()) {
            if (asset.type != AssetType::Model) {
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
            ImGui::TextDisabled("The project has no models.");
        }
        if (!chosen.isNil()) {
            reportIfFailed(
                EditorActions(*m_context).instantiateModel(chosen, {.parent = m_context->primarySelection()}));
        }
        ImGui::EndMenu();
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
    std::string title = fmt::format("{}{} - {}{}", sceneLabel, m_context->hasUnsavedChanges() ? " *" : "",
                                    m_project ? m_project->settings.name + " - " : std::string(), kEditorName);
    if (title != m_shownTitle) {
        window->setTitle(title);
        m_shownTitle = std::move(title);
    }
}

} // namespace cordite
