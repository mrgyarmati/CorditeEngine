#include "cordite/engine/Engine.h"

#include "cordite/core/FileSystem.h"
#include "cordite/core/Log.h"
#include "cordite/core/Platform.h"
#include "cordite/core/Time.h"
#include "cordite/renderer/BlitPass.h"
#include "cordite/renderer/ShaderManager.h"
#include "cordite/renderer/TestPatternPass.h"
#include "cordite/rhi/Swapchain.h"
#include "cordite/rhi/TextureReadback.h"
#include "cordite/shader_compiler/SlangShaderCompiler.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

namespace cordite {
namespace {

/// Largest frame time fed into the simulation. A longer pause (a breakpoint, a window
/// drag) should not make everything jump ahead by seconds.
constexpr double kMaxFrameSeconds = 0.25;

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
    // resources go before the device and the window before the window system.
    std::unique_ptr<WindowSystem> windowSystem;
    std::unique_ptr<Window> window;
    std::unique_ptr<GraphicsDevice> graphics;
    std::unique_ptr<Swapchain> swapchain;
    std::unique_ptr<SlangShaderCompiler> shaderCompiler;
    std::unique_ptr<ShaderLibrary> shaderLibrary;
    std::unique_ptr<ShaderManager> shaderManager;

    nvrhi::CommandListHandle commandList;
    std::unique_ptr<TestPatternPass> testPattern;
    std::unique_ptr<BlitPass> blit;

    // The frame is rendered here first; the swapchain only receives a copy. This keeps
    // windowed and headless rendering identical and gives captureFrame() a stable source.
    nvrhi::TextureHandle frameColor;
    nvrhi::FramebufferHandle frameFramebuffer;
    UVec2 frameSize{0};

    Stopwatch frameClock;
    double timeSeconds = 0.0;
    uint64_t frameIndex = 0;
    bool exitRequested = false;

    Status initialize();
    Status resizeFrameTarget(UVec2 size);
};

Status Engine::Impl::resizeFrameTarget(UVec2 size) {
    if (size == frameSize && frameColor) {
        return {};
    }

    nvrhi::TextureDesc textureDesc;
    textureDesc.width = size.x;
    textureDesc.height = size.y;
    textureDesc.format = nvrhi::Format::RGBA8_UNORM;
    textureDesc.debugName = "Frame color";
    textureDesc.isRenderTarget = true;
    textureDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    textureDesc.keepInitialState = true;
    textureDesc.clearValue = nvrhi::Color(0.0f, 0.0f, 0.0f, 1.0f);
    textureDesc.useClearValue = true;

    nvrhi::IDevice* device = graphics->device();
    nvrhi::TextureHandle texture = device->createTexture(textureDesc);
    if (!texture) {
        return makeError(ErrorCode::DeviceError, "cannot create a {}x{} frame target", size.x, size.y);
    }
    frameFramebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(texture));
    frameColor = std::move(texture);
    frameSize = size;
    return {};
}

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

    commandList = graphics->device()->createCommandList();
    CORDITE_TRY_ASSIGN(TestPatternPass createdTestPattern, TestPatternPass::create(graphics->device(), *shaderManager));
    testPattern = std::make_unique<TestPatternPass>(std::move(createdTestPattern));
    CORDITE_TRY_ASSIGN(BlitPass createdBlit, BlitPass::create(graphics->device(), *shaderManager));
    blit = std::make_unique<BlitPass>(std::move(createdBlit));

    CORDITE_TRY(resizeFrameTarget(swapchain ? swapchain->size() : desc.renderSize));
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
    // Finish everything in flight before the members that own GPU resources go away.
    m_impl->graphics->waitIdle();
}

Result<bool> Engine::runFrame() {
    Impl& impl = *m_impl;

    impl.windowSystem->pollEvents();
    if (impl.exitRequested || (impl.window && impl.window->shouldClose())) {
        return false;
    }

    const double frameSeconds = impl.desc.fixedTimeStep > 0.0
                                    ? impl.desc.fixedTimeStep
                                    : std::min(impl.frameClock.elapsedSeconds(), kMaxFrameSeconds);
    impl.frameClock.restart();

    SwapchainFrame backBuffer;
    if (impl.swapchain) {
        CORDITE_TRY_ASSIGN(backBuffer, impl.swapchain->beginFrame());
        if (backBuffer.framebuffer == nullptr) {
            // Minimized. Keep time moving, but there is nothing to draw on; sleep so the
            // loop does not spin at full speed until the window comes back.
            impl.timeSeconds += frameSeconds;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            return true;
        }
        CORDITE_TRY(impl.resizeFrameTarget(backBuffer.size));
    }

    impl.commandList->open();
    impl.testPattern->draw(impl.commandList, impl.frameFramebuffer,
                           {.timeSeconds = static_cast<float>(impl.timeSeconds)});
    if (backBuffer.framebuffer != nullptr) {
        impl.blit->draw(impl.commandList, impl.frameColor, backBuffer.framebuffer);
    }
    impl.commandList->close();

    impl.graphics->device()->executeCommandList(impl.commandList);

    if (impl.swapchain) {
        CORDITE_TRY(impl.swapchain->present());
    }
    impl.graphics->collectGarbage();

    impl.timeSeconds += frameSeconds;
    ++impl.frameIndex;
    return true;
}

void Engine::requestExit() {
    m_impl->exitRequested = true;
}

Result<Image> Engine::captureFrame() {
    if (m_impl->frameIndex == 0) {
        return makeError(ErrorCode::InvalidState, "no frame has been rendered yet");
    }
    CORDITE_TRY_ASSIGN(TexturePixels pixels, readTexture(*m_impl->graphics, m_impl->frameColor));
    return Image::fromPixels(pixels.width, pixels.height, ImageFormat::Rgba8, std::move(pixels.data));
}

uint64_t Engine::frameIndex() const {
    return m_impl->frameIndex;
}

double Engine::timeSeconds() const {
    return m_impl->timeSeconds;
}

UVec2 Engine::renderSize() const {
    return m_impl->frameSize;
}

Window* Engine::window() {
    return m_impl->window.get();
}

GraphicsDevice& Engine::graphics() {
    return *m_impl->graphics;
}

ShaderLibrary& Engine::shaders() {
    return *m_impl->shaderLibrary;
}

} // namespace cordite
