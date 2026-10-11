#pragma once

/// @file
/// The engine instance: owns every subsystem and runs the frame loop.
///
/// The editor and the game runtime are both thin programs around an Engine.
///
/// @code
/// EngineDesc desc;
/// desc.applicationName = "My Game";
/// CORDITE_TRY_ASSIGN(auto engine, Engine::create(desc));
/// while (true) {
///     CORDITE_TRY_ASSIGN(const bool keepRunning, engine->runFrame());
///     if (!keepRunning) {
///         break;
///     }
/// }
/// @endcode
///
/// Only one Engine can exist in a process at a time, because it owns the window system.
///
/// A headless engine has no window and no swapchain and renders to an offscreen target.
/// It behaves identically otherwise, which is what the rendering tests and the editor's
/// automation mode rely on.

#include "cordite/assets/Image.h"
#include "cordite/core/Base.h"
#include "cordite/core/Result.h"
#include "cordite/math/Math.h"
#include "cordite/platform/Window.h"
#include "cordite/rhi/GraphicsDevice.h"
#include "cordite/shaders/ShaderLibrary.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace cordite {

struct EngineDesc {
    std::string applicationName = "Cordite";

    /// Run without a window, rendering offscreen at `renderSize`.
    bool headless = false;
    WindowDesc window;

    /// Size of the offscreen render target in headless mode. With a window, the render
    /// target follows the window's framebuffer size instead.
    UVec2 renderSize{1280, 720};

    bool vsync = true;

    /// Enable GPU validation layers. See GraphicsDeviceDesc::validation.
    bool validation = false;

    /// GPU to prefer, by name substring. See GraphicsDeviceDesc::preferredAdapter.
    std::string preferredAdapter;

    /// Directory holding the engine's shader sources. Empty uses the default: a
    /// `shaders` directory beside the executable, or the repository's in development builds.
    std::filesystem::path shaderDirectory;

    /// Directory for derived data such as the shader cache. Empty keeps it in memory.
    std::filesystem::path cacheDirectory;

    /// Advance time by exactly this many seconds per frame instead of measuring it.
    /// Zero uses the real clock. Tests and frame captures set it so that every run
    /// produces the same frames.
    double fixedTimeStep = 0.0;
};

class Engine {
public:
    [[nodiscard]] static Result<std::unique_ptr<Engine>> create(const EngineDesc& desc);
    ~Engine();
    CORDITE_NON_MOVABLE(Engine);

    /// Runs one frame: processes input, renders and presents. Returns false once the
    /// application should exit (the window was closed or requestExit() was called).
    [[nodiscard]] Result<bool> runFrame();

    /// Makes the next runFrame() return false.
    void requestExit();

    /// Copies the most recently rendered frame to the CPU as an Rgba8 image.
    [[nodiscard]] Result<Image> captureFrame();

    /// Number of frames rendered so far.
    [[nodiscard]] uint64_t frameIndex() const;

    /// Seconds of engine time elapsed, as advanced by runFrame().
    [[nodiscard]] double timeSeconds() const;

    /// Size in pixels of the frame being rendered.
    [[nodiscard]] UVec2 renderSize() const;

    /// Null in headless mode.
    [[nodiscard]] Window* window();

    [[nodiscard]] GraphicsDevice& graphics();
    [[nodiscard]] ShaderLibrary& shaders();

private:
    struct Impl;

    explicit Engine(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
};

} // namespace cordite
