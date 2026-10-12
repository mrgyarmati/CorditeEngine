#pragma once

/// @file
/// The engine instance: owns every subsystem and runs the frame loop.
///
/// The editor and the game runtime are both thin programs around an Engine. A frame is
/// bracketed by beginFrame() and endFrame(); in between, the program updates the world,
/// renders into the frame target and describes its user interface:
///
/// @code
/// EngineDesc desc;
/// desc.applicationName = "My Game";
/// CORDITE_TRY_ASSIGN(auto engine, Engine::create(desc));
///
/// while (true) {
///     CORDITE_TRY_ASSIGN(const bool keepRunning, engine->beginFrame());
///     if (!keepRunning) {
///         break;
///     }
///     updateGame(engine->world(), engine->deltaSeconds());
///     engine->updateWorld();                            // animations play, transforms follow
///     CORDITE_TRY(engine->drawWorld());                 // or any passes on engine->commandList()
///     ImGui::Text("frame %llu", engine->frameIndex());  // the interface is drawn on top
///     CORDITE_TRY(engine->endFrame());
/// }
/// @endcode
///
/// A program with nothing of its own to draw calls runFrame(), which does all of that.
///
/// Only one Engine can exist in a process at a time, because it owns the window system.
///
/// A headless engine has no window and no swapchain. It renders to the same offscreen
/// frame target and takes its input from what is injected into input(), so it behaves
/// identically otherwise; the rendering tests and the editor's automation mode rely on it.

#include "cordite/animation/AnimationSystem.h"
#include "cordite/assets/AssetManager.h"
#include "cordite/assets/Image.h"
#include "cordite/core/Base.h"
#include "cordite/core/Result.h"
#include "cordite/math/Math.h"
#include "cordite/physics/PhysicsSystem.h"
#include "cordite/platform/Input.h"
#include "cordite/platform/InputMap.h"
#include "cordite/platform/Window.h"
#include "cordite/renderer/RenderResources.h"
#include "cordite/renderer/RenderTarget.h"
#include "cordite/renderer/SceneRenderer.h"
#include "cordite/renderer/ShaderManager.h"
#include "cordite/rhi/GraphicsDevice.h"
#include "cordite/shaders/ShaderLibrary.h"
#include "cordite/ui/UiSystem.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/World.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace cordite {

/// What a frame cost. See Engine::statistics().
struct FrameStatistics {
    /// The frame the numbers describe: the value frameIndex() had while it ran.
    uint64_t frameIndex = 0;
    /// The time from beginFrame() until the frame was handed to the GPU, in
    /// milliseconds: the program's own work on the frame. Waiting for the display
    /// afterwards is not counted.
    double cpuMilliseconds = 0.0;
    /// How long the GPU worked on the commands of a recent frame, in milliseconds. The
    /// measurement is a few frames old when it arrives; there is none before the first
    /// has arrived, and never one on an adapter that cannot time its work.
    std::optional<double> gpuMilliseconds;
    /// The steps the simulation took in the frame, and the bodies it holds: what the
    /// last updateWorld() of the frame did. All zero for a frame without one.
    PhysicsFrameStatistics physics;
    /// The animators that were posed in the frame, and their joints: what the last
    /// updateWorld() of the frame did. All zero for a frame without one.
    AnimationStatistics animation;
    /// What the scene renderer drew during the frame, all views together.
    SceneRenderStatistics scene;
    /// What the scene renderer holds on the GPU.
    RenderResourceStatistics resources;
};

struct EngineDesc {
    std::string applicationName = "Cordite";

    /// Run without a window, rendering offscreen at `renderSize`.
    bool headless = false;
    WindowDesc window;

    /// Size of the frame target in headless mode. With a window, the frame target
    /// follows the window's framebuffer size instead.
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

    /// Directory holding the assets of the game: models, textures, materials, scenes.
    /// Empty leaves only the engine's built-in assets. See AssetDatabaseDesc.
    std::filesystem::path assetDirectory;

    /// Give every asset file that has none a `.meta` file with its id. Editors do; a
    /// running game only reads. See AssetDatabaseDesc::writeMetaFiles.
    bool writeAssetMetaFiles = false;

    /// File in which the user interface remembers its layout. See
    /// UiSystemDesc::settingsFile.
    std::filesystem::path uiSettingsFile;

    /// Advance time by exactly this many seconds per frame instead of measuring it.
    /// Zero uses the real clock. Tests and frame captures set it so that every run
    /// produces the same frames.
    double fixedTimeStep = 0.0;

    /// How the bodies of the world are simulated: the pull of gravity, how large the
    /// simulation may become, and how long its steps are. See PhysicsSystemDesc.
    PhysicsSystemDesc physics;
};

/// Whether time passes for a world. See Engine::updateWorld().
enum class WorldClock : uint8_t {
    /// It does: what plays moves on by the time the frame takes. A game.
    Running,
    /// It does not: everything is shown as it is at this moment, and nothing in the
    /// world is changed. An editor, with a scene that is being worked on.
    Stopped,
};

class Engine {
public:
    [[nodiscard]] static Result<std::unique_ptr<Engine>> create(const EngineDesc& desc);
    ~Engine();
    CORDITE_NON_MOVABLE(Engine);

    /// Starts a frame: delivers window events, advances the clock, clears the frame
    /// target and begins the interface frame. Returns false once the application should
    /// exit (the window was closed or requestExit() was called); the frame has not been
    /// started then, and endFrame() must not be called.
    [[nodiscard]] Result<bool> beginFrame();

    /// Finishes the frame: draws the interface over the frame target, shows the result
    /// in the window if there is one, and advances frameIndex() and timeSeconds().
    [[nodiscard]] Status endFrame();

    /// True between a beginFrame() that returned true and the matching endFrame().
    [[nodiscard]] bool isFrameOpen() const;

    /// Fills the frame target with the diagnostic test pattern. Only during a frame. It
    /// is what the engine shows while it has nothing else to draw.
    void drawTestPattern();

    /// Brings the world up to date for drawing, after the program has changed what it
    /// wanted to change: the simulation of its bodies moves on by deltaSeconds(), in
    /// steps of a fixed length, and the entities follow their bodies (PhysicsSystem);
    /// the animators play on by as much and every skeleton is posed (AnimationSystem);
    /// then the world transforms are worked out. With the clock stopped nothing is
    /// simulated and nothing plays: bodies are where their entities are, and the poses
    /// are those of the times the animators hold.
    ///
    /// Call it once per frame with a running clock, between beginFrame() and drawing:
    /// each call lets the time of a frame pass. A running clock outside a frame is a
    /// mistake, and is taken as a stopped one.
    void updateWorld(WorldClock clock = WorldClock::Running);

    /// Draws the world into the frame target as its primary camera sees it (the first
    /// enabled camera with `primary` set; see findPrimaryCamera). A world without one
    /// shows the test pattern. Only during a frame, with the world up to date
    /// (updateWorld()).
    /// Whose view it is and the time are filled in here, whatever `settings` say for
    /// viewEntity, deltaSeconds and timeSeconds.
    [[nodiscard]] Status drawWorld(const SceneRenderSettings& settings = {});

    /// Runs a whole frame for a program that draws nothing itself: beginFrame(),
    /// updateWorld() with a running clock, drawWorld(renderSettings()), endFrame().
    /// Returns what beginFrame() returned.
    [[nodiscard]] Result<bool> runFrame();

    /// What runFrame() draws the world with: the background, the quality of the
    /// shadows and of the finishing of the picture. A game changes them when its player
    /// changes the graphics options (applyRenderQuality() sets them by one word).
    [[nodiscard]] SceneRenderSettings& renderSettings();
    [[nodiscard]] const SceneRenderSettings& renderSettings() const;

    /// Makes the next beginFrame() return false.
    void requestExit();

    /// Copies the most recently finished frame to the CPU as an Rgba8 image.
    [[nodiscard]] Result<Image> captureFrame();

    /// Number of frames finished so far.
    [[nodiscard]] uint64_t frameIndex() const;

    /// Seconds of engine time elapsed up to the start of the current frame.
    [[nodiscard]] double timeSeconds() const;

    /// Seconds the current frame advances time by: `fixedTimeStep` if set, otherwise the
    /// measured duration of the previous frame, capped so that a pause does not turn
    /// into a jump.
    [[nodiscard]] float deltaSeconds() const;

    /// What the last frame that was ended cost. All zero before the first.
    [[nodiscard]] const FrameStatistics& statistics() const;

    /// Size in pixels of the frame target.
    [[nodiscard]] UVec2 renderSize() const;

    /// Where the frame is rendered before it is shown: an 8-bit target that holds
    /// display-ready colors. The interface is drawn onto it in endFrame().
    [[nodiscard]] const RenderTarget& frameTarget() const;

    /// The command list every pass of the frame records into. Open only during a frame.
    [[nodiscard]] nvrhi::ICommandList* commandList();

    /// Null in headless mode.
    [[nodiscard]] Window* window();

    /// The keyboard, pointer and gamepads: the window's, or in headless mode a stand-in
    /// that only ever reports what is injected into it. Inject between frames; the next
    /// frame sees it.
    [[nodiscard]] Input& input();

    /// What a game calls its controls - "jump", "moveX" - and what they are doing in
    /// this frame: worked out from input() by every beginFrame(). Empty until it is
    /// given a map (InputActions::setMap()), such as the one a project names.
    [[nodiscard]] InputActions& actions();

    /// The scene, and the component types that scene files and tools know by name.
    [[nodiscard]] World& world();
    [[nodiscard]] ComponentRegistry& components();

    /// The assets of the game, and of the engine itself.
    [[nodiscard]] AssetManager& assets();

    /// The simulation of the world's bodies, for what is asked of it directly: what a
    /// ray hits (physics().simulation()), which entity a body belongs to.
    [[nodiscard]] PhysicsSystem& physics();

    /// Draws worlds. The editor uses it for its viewports as the game does for its view.
    [[nodiscard]] SceneRenderer& sceneRenderer();

    [[nodiscard]] GraphicsDevice& graphics();
    [[nodiscard]] ShaderLibrary& shaders();
    [[nodiscard]] ShaderManager& shaderManager();
    [[nodiscard]] UiSystem& ui();

private:
    struct Impl;

    explicit Engine(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
};

} // namespace cordite
