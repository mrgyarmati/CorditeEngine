#include "cordite/engine/Engine.h"

#include "cordite/core/Assert.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Log.h"
#include "cordite/core/Platform.h"
#include "cordite/core/Time.h"
#include "cordite/core/Uuid.h"
#include "cordite/engine/EngineComponents.h"
#include "cordite/engine/SceneChange.h"
#include "cordite/renderer/BlitPass.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/FramePacer.h"
#include "cordite/renderer/GpuTimer.h"
#include "cordite/renderer/TestPatternPass.h"
#include "cordite/rhi/Swapchain.h"
#include "cordite/rhi/TextureReadback.h"
#include "cordite/shader_compiler/SlangShaderCompiler.h"
#include "cordite/world/Components.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace cordite {
namespace {

/// Largest frame time fed into the simulation. A longer pause (a breakpoint, a window
/// drag) should not make everything jump ahead by seconds.
constexpr double kMaxFrameSeconds = 0.25;

/// How long a frame waits when the window is minimized and nothing can be shown, so
/// that the loop does not spin at full speed.
constexpr std::chrono::milliseconds kMinimizedFrameDelay(10);

Result<std::filesystem::path> locateShaderDirectory(const std::filesystem::path& requested) {
    if (!requested.empty()) {
        if (!directoryExists(requested)) {
            return makeError(ErrorCode::NotFound, "shader directory {} does not exist", pathToUtf8(requested));
        }
        return requested;
    }

    std::vector<std::filesystem::path> candidates;
#if defined(CORDITE_ENGINE_SHADER_DIR)
    // Development builds prefer the repository's copy, so that editing a shader there
    // takes effect without rebuilding.
    candidates.push_back(pathFromUtf8(CORDITE_ENGINE_SHADER_DIR));
#endif
    candidates.push_back(executableDirectory() / "shaders");

    for (const std::filesystem::path& candidate : candidates) {
        if (directoryExists(candidate)) {
            return candidate;
        }
    }
    return makeError(ErrorCode::NotFound, "cannot find the engine's shaders; expected a directory at {}",
                     pathToUtf8(candidates.back()));
}

} // namespace

struct Engine::Impl {
    EngineDesc desc;

    // Members are declared in dependency order; destruction runs in reverse, so GPU
    // resources go before the device, and the interface and the window before the
    // window system.
    std::unique_ptr<WindowSystem> windowSystem;
    std::unique_ptr<Window> window;
    std::unique_ptr<GraphicsDevice> graphics;
    std::unique_ptr<Swapchain> swapchain;
    std::unique_ptr<SlangShaderCompiler> shaderCompiler;
    std::unique_ptr<ShaderLibrary> shaderLibrary;
    std::unique_ptr<ShaderManager> shaderManager;

    nvrhi::CommandListHandle commandList;
    /// Times the frame's command list on the GPU.
    std::unique_ptr<GpuTimer> gpuTimer;
    /// Keeps frames from piling up where no display holds the loop back.
    std::unique_ptr<FramePacer> framePacer;
    std::unique_ptr<TestPatternPass> testPattern;
    std::unique_ptr<BlitPass> blit;

    // The frame is rendered here first; the swapchain only receives a copy. This keeps
    // windowed and headless rendering identical and gives captureFrame() a stable source.
    RenderTarget frameTarget;
    std::unique_ptr<UiSystem> ui;

    // The asset manager outlives the scene renderer, which holds GPU copies of assets.
    std::unique_ptr<AssetManager> assets;
    std::unique_ptr<SceneRenderer> sceneRenderer;
    /// What drawWorld() renders through; created when it is first needed.
    SceneTargets worldTargets;

    /// Stands in for the window's input when there is no window.
    Input headlessInput;
    /// What the controls of the game are doing, worked out of the input every frame.
    InputActions actions;
    /// The scene a script asked for in the frame before: loaded when the next begins.
    std::optional<Uuid> pendingScene;

    ComponentRegistry components;
    World world;
    std::unique_ptr<PhysicsSystem> physics;
    /// What the last updateWorld() since the frame began did.
    PhysicsFrameStatistics physicsStatistics;
    AnimationSystem animation;
    /// What the last updateWorld() since the frame began did.
    AnimationStatistics animationStatistics;
    std::unique_ptr<ScriptSystem> scripts;
    /// What the last updateWorld() since the frame began did.
    ScriptStatistics scriptStatistics;
    double scriptMilliseconds = 0.0;

    Stopwatch frameClock;
    /// Runs from beginFrame() to the moment the frame is handed to the GPU.
    Stopwatch workClock;
    FrameStatistics statistics;
    /// What runFrame() draws the world with.
    SceneRenderSettings renderSettings;
    double timeSeconds = 0.0;
    double deltaSeconds = 0.0;
    uint64_t frameIndex = 0;
    bool exitRequested = false;
    bool frameOpen = false;
    /// The window's image for the frame in progress; empty when headless or minimized.
    SwapchainFrame backBuffer;

    Status initialize();
};

Status Engine::Impl::initialize() {
    CORDITE_TRY_ASSIGN(windowSystem, WindowSystem::create({.headless = desc.headless}));

    GraphicsDeviceDesc graphicsDesc;
    graphicsDesc.applicationName = desc.applicationName;
    graphicsDesc.validation = desc.validation;
    graphicsDesc.preferredAdapter = desc.preferredAdapter;
    if (!desc.headless) {
        graphicsDesc.presentationExtensions = windowSystem->vulkanInstanceExtensions();
        if (graphicsDesc.presentationExtensions.empty()) {
            return makeError(ErrorCode::Unsupported, "this system cannot present Vulkan images to a window");
        }
        CORDITE_TRY_ASSIGN(window, windowSystem->createWindow(desc.window));
    }
    CORDITE_TRY_ASSIGN(graphics, GraphicsDevice::create(graphicsDesc));

    if (window) {
        CORDITE_TRY_ASSIGN(swapchain, Swapchain::create(*graphics, *window, {.vsync = desc.vsync}));
    }

    CORDITE_TRY_ASSIGN(const std::filesystem::path shaderDirectory, locateShaderDirectory(desc.shaderDirectory));
    CORDITE_TRY_ASSIGN(shaderCompiler, SlangShaderCompiler::create({.sourceRoots = {shaderDirectory}}));

    ShaderLibraryDesc libraryDesc;
    libraryDesc.sourceRoots = {shaderDirectory};
    libraryDesc.compiler = shaderCompiler.get();
    if (!desc.cacheDirectory.empty()) {
        libraryDesc.cacheDirectory = desc.cacheDirectory / "shaders";
    }
    shaderLibrary = std::make_unique<ShaderLibrary>(std::move(libraryDesc));
    shaderManager = std::make_unique<ShaderManager>(graphics->device(), *shaderLibrary);

    nvrhi::IDevice* device = graphics->device();
    commandList = device->createCommandList();
    gpuTimer = std::make_unique<GpuTimer>(device, graphics->adapter().supportsTimerQueries);
    framePacer = std::make_unique<FramePacer>(device);
    CORDITE_TRY_ASSIGN(TestPatternPass createdTestPattern, TestPatternPass::create(device, *shaderManager));
    testPattern = std::make_unique<TestPatternPass>(std::move(createdTestPattern));
    CORDITE_TRY_ASSIGN(BlitPass createdBlit, BlitPass::create(device, *shaderManager));
    blit = std::make_unique<BlitPass>(std::move(createdBlit));

    CORDITE_TRY_ASSIGN(
        frameTarget,
        RenderTarget::create(device, {.size = swapchain ? swapchain->size() : desc.renderSize, .debugName = "Frame"}));
    CORDITE_TRY_ASSIGN(
        ui, UiSystem::create(device, *shaderManager, {.window = window.get(), .settingsFile = desc.uiSettingsFile}));

    CORDITE_TRY_ASSIGN(assets, AssetManager::create(
                                   {.rootDirectory = desc.assetDirectory, .writeMetaFiles = desc.writeAssetMetaFiles}));
    CORDITE_TRY_ASSIGN(sceneRenderer,
                       SceneRenderer::create(device, *shaderManager, *assets,
                                             {.supportsWireframe = graphics->adapter().supportsWireframe}));

    CORDITE_TRY(registerEngineComponents(components));
    CORDITE_TRY_ASSIGN(physics, PhysicsSystem::create(desc.physics));
    CORDITE_TRY_ASSIGN(scripts, ScriptSystem::create(desc.scripts));
    frameClock.restart();
    return {};
}

Result<std::unique_ptr<Engine>> Engine::create(const EngineDesc& desc) {
    if (desc.renderSize.x == 0 || desc.renderSize.y == 0) {
        return makeError(ErrorCode::InvalidArgument, "render size must be positive, got {}x{}", desc.renderSize.x,
                         desc.renderSize.y);
    }
    if (desc.fixedTimeStep < 0.0) {
        return makeError(ErrorCode::InvalidArgument, "fixedTimeStep must not be negative, got {}", desc.fixedTimeStep);
    }

    auto impl = std::make_unique<Impl>();
    impl->desc = desc;
    CORDITE_TRY(impl->initialize());
    return std::unique_ptr<Engine>(new Engine(std::move(impl)));
}

Engine::Engine(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

Engine::~Engine() {
    if (m_impl->frameOpen) {
        // Abandon the frame in progress: nothing recorded in it will be executed.
        m_impl->ui->discardFrame();
        m_impl->commandList->close();
    }
    // Finish everything in flight before the members that own GPU resources go away.
    m_impl->graphics->waitIdle();
}

Result<bool> Engine::beginFrame() {
    Impl& impl = *m_impl;
    CORDITE_ASSERT(!impl.frameOpen, "beginFrame() called again before endFrame()");
    if (impl.frameOpen) {
        return makeError(ErrorCode::InvalidState, "the previous frame has not been ended");
    }

    impl.workClock.restart();
    impl.windowSystem->pollEvents();
    if (impl.exitRequested || (impl.window && impl.window->shouldClose())) {
        return false;
    }
    // Scenes change between frames: what a script asked for in the last one happens
    // now. A scene that cannot be loaded costs the frame nothing - the one that was
    // there goes on, and the log says why.
    if (const std::optional<Uuid> scene = std::exchange(impl.pendingScene, std::nullopt)) {
        if (const Status changed = loadScene(*scene); !changed) {
            CORDITE_LOG_ERROR("Engine", "the scene a script asked for was not loaded: {}", changed.error().message);
        }
    }
    // The events of the frame are in: what they make of the controls of the game is
    // the same for everything that asks during the frame.
    impl.actions.update(input());

    impl.deltaSeconds = impl.desc.fixedTimeStep > 0.0 ? impl.desc.fixedTimeStep
                                                      : std::min(impl.frameClock.elapsedSeconds(), kMaxFrameSeconds);
    impl.frameClock.restart();

    impl.backBuffer = {};
    if (impl.swapchain) {
        CORDITE_TRY_ASSIGN(impl.backBuffer, impl.swapchain->beginFrame());
        // A minimized window has no back buffer. The frame still runs, offscreen at the
        // size it last had, so that the program need not treat that case specially.
        if (impl.backBuffer.framebuffer != nullptr) {
            CORDITE_TRY(impl.frameTarget.resize(impl.graphics->device(), impl.backBuffer.size));
        }
    }

    impl.commandList->open();
    impl.gpuTimer->begin(impl.commandList);
    impl.frameTarget.clear(impl.commandList);

    if (!impl.window) {
        const Input& pointer = impl.headlessInput;
        impl.ui->setPointerState({.position = pointer.mousePosition(),
                                  .leftDown = pointer.isMouseButtonDown(MouseButton::Left),
                                  .rightDown = pointer.isMouseButtonDown(MouseButton::Right),
                                  .middleDown = pointer.isMouseButtonDown(MouseButton::Middle),
                                  .wheel = pointer.scrollDelta().y});
    }
    impl.ui->beginFrame(impl.frameTarget.size(), static_cast<float>(impl.deltaSeconds));

    // What was drawn between frames - a picture taken of a viewport, say - is not part
    // of this frame, so its numbers are dropped here.
    (void)impl.sceneRenderer->takeFrameStatistics();
    impl.physicsStatistics = {};
    impl.animationStatistics = {};
    impl.scriptStatistics = {};
    impl.scriptMilliseconds = 0.0;
    impl.frameOpen = true;
    return true;
}

Status Engine::endFrame() {
    Impl& impl = *m_impl;
    CORDITE_ASSERT(impl.frameOpen, "endFrame() called without a frame started by beginFrame()");
    if (!impl.frameOpen) {
        return makeError(ErrorCode::InvalidState, "no frame has been started");
    }

    impl.ui->render(impl.commandList, impl.frameTarget.framebuffer());
    const bool hasBackBuffer = impl.backBuffer.framebuffer != nullptr;
    if (hasBackBuffer) {
        impl.blit->draw(impl.commandList, impl.frameTarget.color(), impl.backBuffer.framebuffer);
    }
    impl.gpuTimer->end(impl.commandList);
    impl.commandList->close();
    impl.frameOpen = false;

    impl.graphics->device()->executeCommandList(impl.commandList);

    // Before the frame is shown: showing it may wait for the display, which is not work.
    impl.statistics.frameIndex = impl.frameIndex;
    impl.statistics.cpuMilliseconds = impl.workClock.elapsedSeconds() * 1000.0;
    impl.statistics.gpuMilliseconds = impl.gpuTimer->latestMilliseconds();
    impl.statistics.physics = impl.physicsStatistics;
    impl.statistics.animation = impl.animationStatistics;
    impl.statistics.scripts = impl.scriptStatistics;
    impl.statistics.scriptMilliseconds = impl.scriptMilliseconds;
    impl.statistics.scene = impl.sceneRenderer->takeFrameStatistics();
    impl.statistics.resources = impl.sceneRenderer->resources().statistics();

    // Without a display to wait for, a loop would hand the GPU frames faster than it
    // draws them. This waits only when it is several frames behind.
    impl.framePacer->frameSubmitted();

    if (hasBackBuffer) {
        CORDITE_TRY(impl.swapchain->present());
    } else if (impl.swapchain) {
        std::this_thread::sleep_for(kMinimizedFrameDelay);
    }
    impl.graphics->collectGarbage();

    if (!impl.window) {
        // This frame has seen what was injected; what is injected from now on belongs to
        // the next one.
        impl.headlessInput.beginFrame();
    }

    impl.timeSeconds += impl.deltaSeconds;
    ++impl.frameIndex;
    return {};
}

bool Engine::isFrameOpen() const {
    return m_impl->frameOpen;
}

void Engine::drawTestPattern() {
    const Impl& impl = *m_impl;
    CORDITE_ASSERT(impl.frameOpen, "drawTestPattern() needs a frame started by beginFrame()");
    if (!impl.frameOpen) {
        return;
    }
    impl.testPattern->draw(impl.commandList, impl.frameTarget.framebuffer(),
                           {.timeSeconds = static_cast<float>(impl.timeSeconds)});
}

Status Engine::drawWorld(const SceneRenderSettings& settings) {
    Impl& impl = *m_impl;
    CORDITE_ASSERT(impl.frameOpen, "drawWorld() needs a frame started by beginFrame()");
    if (!impl.frameOpen) {
        return makeError(ErrorCode::InvalidState, "no frame has been started");
    }

    const Entity camera = findPrimaryCamera(impl.world);
    if (camera.isNull()) {
        drawTestPattern();
        return {};
    }

    const UVec2 size = impl.frameTarget.size();
    if (!impl.worldTargets.isValid()) {
        CORDITE_TRY_ASSIGN(impl.worldTargets, SceneTargets::create(impl.graphics->device(), size, "World"));
    } else {
        CORDITE_TRY(impl.worldTargets.resize(impl.graphics->device(), size));
    }

    const CameraView view = makeCameraView(impl.world.worldMatrix(camera), *impl.world.tryGet<CameraComponent>(camera),
                                           static_cast<float>(size.x) / static_cast<float>(size.y));
    // Whose view it is and what time it is are the engine's to say.
    SceneRenderSettings drawn = settings;
    drawn.viewEntity = impl.world.id(camera);
    drawn.deltaSeconds = static_cast<float>(impl.deltaSeconds);
    drawn.timeSeconds = impl.timeSeconds;
    impl.sceneRenderer->render(impl.commandList, impl.world, view, impl.worldTargets, drawn);
    impl.blit->draw(impl.commandList, impl.worldTargets.display(), impl.frameTarget.framebuffer());
    return {};
}

void Engine::updateWorld(WorldClock clock) {
    Impl& impl = *m_impl;
    // How much time passes is what a frame says; between frames there is none to pass.
    CORDITE_ASSERT(clock == WorldClock::Stopped || impl.frameOpen, "the clock of the world runs only during a frame");
    // Scripts first: what they decide in a frame - where a character is to walk - is
    // what the simulation does in the same frame. Then bodies: what the simulation moves
    // is where it is before anything is posed or placed against it - and what met and
    // parted on the way is told to the scripts at once, while it is true.
    if (clock == WorldClock::Running && impl.frameOpen) {
        const auto deltaSeconds = static_cast<float>(impl.deltaSeconds);
        const ScriptContext scripted = {.world = impl.world,
                                        .components = impl.components,
                                        .assets = *impl.assets,
                                        .deltaSeconds = impl.deltaSeconds,
                                        .controls = &impl.actions,
                                        .physics = impl.physics.get()};
        Stopwatch scriptClock;
        impl.scriptStatistics = impl.scripts->update(scripted);
        double scriptSeconds = scriptClock.elapsedSeconds();
        impl.physicsStatistics = impl.physics->update(impl.world, *impl.assets, deltaSeconds);
        scriptClock.restart();
        impl.scriptStatistics = impl.scripts->tellContacts(scripted, impl.physics->events());
        scriptSeconds += scriptClock.elapsedSeconds();
        impl.scriptMilliseconds = scriptSeconds * 1000.0;
        if (const std::optional<Uuid> scene = impl.scripts->takeRequestedScene()) {
            impl.pendingScene = scene;
        }
        impl.animationStatistics = impl.animation.update(impl.world, *impl.assets, deltaSeconds);
    } else {
        impl.physicsStatistics = impl.physics->synchronize(impl.world, *impl.assets);
        impl.animationStatistics = impl.animation.evaluate(impl.world, *impl.assets);
    }
    impl.world.updateWorldTransforms();
}

Result<bool> Engine::runFrame() {
    CORDITE_TRY_ASSIGN(const bool keepRunning, beginFrame());
    if (!keepRunning) {
        return false;
    }
    updateWorld(WorldClock::Running);
    // A frame that was begun is always ended, so that the engine stays usable after
    // a failure in between.
    const Status drawn = drawWorld(m_impl->renderSettings);
    CORDITE_TRY(endFrame());
    CORDITE_TRY(drawn);
    return true;
}

SceneRenderSettings& Engine::renderSettings() {
    return m_impl->renderSettings;
}

const SceneRenderSettings& Engine::renderSettings() const {
    return m_impl->renderSettings;
}

void Engine::requestExit() {
    m_impl->exitRequested = true;
}

Result<Image> Engine::captureFrame() {
    if (m_impl->frameIndex == 0) {
        return makeError(ErrorCode::InvalidState, "no frame has been rendered yet");
    }
    CORDITE_TRY_ASSIGN(TexturePixels pixels, readTexture(*m_impl->graphics, m_impl->frameTarget.color()));
    return Image::fromPixels(pixels.width, pixels.height, ImageFormat::Rgba8, std::move(pixels.data));
}

uint64_t Engine::frameIndex() const {
    return m_impl->frameIndex;
}

const FrameStatistics& Engine::statistics() const {
    return m_impl->statistics;
}

double Engine::timeSeconds() const {
    return m_impl->timeSeconds;
}

float Engine::deltaSeconds() const {
    return static_cast<float>(m_impl->deltaSeconds);
}

UVec2 Engine::renderSize() const {
    return m_impl->frameTarget.size();
}

const RenderTarget& Engine::frameTarget() const {
    return m_impl->frameTarget;
}

nvrhi::ICommandList* Engine::commandList() {
    return m_impl->commandList;
}

Window* Engine::window() {
    return m_impl->window.get();
}

Input& Engine::input() {
    return m_impl->window ? m_impl->window->input() : m_impl->headlessInput;
}

InputActions& Engine::actions() {
    return m_impl->actions;
}

World& Engine::world() {
    return m_impl->world;
}

ComponentRegistry& Engine::components() {
    return m_impl->components;
}

AssetManager& Engine::assets() {
    return *m_impl->assets;
}

PhysicsSystem& Engine::physics() {
    return *m_impl->physics;
}

ScriptSystem& Engine::scripts() {
    return *m_impl->scripts;
}

Status Engine::loadScene(const Uuid& scene) {
    Impl& impl = *m_impl;
    CORDITE_ASSERT(!impl.frameOpen, "a scene is loaded between frames, not in one");
    if (impl.frameOpen) {
        return makeError(ErrorCode::InvalidState, "a scene is loaded between two frames, not during one");
    }
    return changeScene({.world = impl.world,
                        .components = impl.components,
                        .assets = *impl.assets,
                        .scripts = *impl.scripts,
                        .physics = impl.physics.get(),
                        .animation = &impl.animation,
                        .controls = &impl.actions},
                       scene);
}

SceneRenderer& Engine::sceneRenderer() {
    return *m_impl->sceneRenderer;
}

GraphicsDevice& Engine::graphics() {
    return *m_impl->graphics;
}

ShaderLibrary& Engine::shaders() {
    return *m_impl->shaderLibrary;
}

ShaderManager& Engine::shaderManager() {
    return *m_impl->shaderManager;
}

UiSystem& Engine::ui() {
    return *m_impl->ui;
}

} // namespace cordite
