#pragma once

/// @file
/// The GPU device.
///
/// GraphicsDevice creates the device of a graphics API - Vulkan, or on Windows Direct3D 12
/// - and wraps it in an NVRHI device, which is what the renderer programs against. It can
/// be created without a window ("headless") to render offscreen, which is how the
/// automated rendering tests and the editor's command-line mode work.
///
/// @code
/// GraphicsDeviceDesc desc;
/// desc.validation = true;
/// desc.presentationExtensions = windowSystem->vulkanInstanceExtensions();
/// CORDITE_TRY_ASSIGN(auto graphics, GraphicsDevice::create(desc));
///
/// nvrhi::TextureHandle target = graphics->device()->createTexture(textureDesc);
/// ...
/// graphics->collectGarbage();   // once per frame
///
/// // Direct3D 12 instead, on WARP - its rasterizer that runs on the CPU:
/// CORDITE_TRY_ASSIGN(auto warp, GraphicsDevice::create({.backend = GraphicsBackend::Direct3D12,
///                                                       .preferredAdapter = "Basic Render"}));
/// @endcode
///
/// Only one GraphicsDevice can exist at a time, and it must be created and destroyed on
/// the main thread. A device of Direct3D 12 draws offscreen only, for now: it cannot make a
/// Swapchain.

#include "cordite/core/Base.h"
#include "cordite/core/Result.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cordite {

struct VulkanContext;

/// The Vulkan that a game exported as an application bundle of macOS brings with it, in the
/// bundle's Contents/Frameworks (GameExport.h): MoltenVK, which runs Vulkan on Metal and
/// which macOS does not have. A program in such a bundle loads it directly, without the
/// Vulkan loader - and so without validation layers, which a game does not use; every other
/// program uses the Vulkan of the system.
inline constexpr std::string_view kBundledVulkanLibrary = "libMoltenVK.dylib";

/// The graphics API a GraphicsDevice draws with.
enum class GraphicsBackend : uint8_t {
    /// The one the engine draws with when it is not told: Vulkan.
    Auto,
    Vulkan,
    /// Windows only; elsewhere a device of it fails with Unsupported.
    Direct3D12,
};

/// "Auto", "Vulkan" or "Direct3D 12".
std::string_view nameOf(GraphicsBackend backend);

enum class AdapterType : uint8_t {
    Discrete,
    Integrated,
    Virtual,
    /// A CPU rasterizer such as lavapipe or WARP.
    Software,
    Other,
};

std::string_view nameOf(AdapterType type);

/// Describes the GPU a GraphicsDevice runs on.
struct AdapterInfo {
    std::string name;
    AdapterType type = AdapterType::Other;
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    /// Memory in heaps local to the device, in bytes.
    uint64_t dedicatedMemoryBytes = 0;
    /// What the device supports of its graphics API: for Vulkan its version, e.g.
    /// "1.4.321"; for Direct3D 12 its highest feature level and shader model, e.g.
    /// "12_1, shader model 6.6".
    std::string apiVersion;
    /// The device can time its own work (see GpuTimer). Nearly every GPU can; software
    /// implementations sometimes cannot.
    bool supportsTimerQueries = false;
    /// The device can draw the edges of triangles instead of filling them, which the
    /// wireframe debug view needs.
    bool supportsWireframe = false;
};

struct GraphicsDeviceDesc {
    /// The graphics API to draw with.
    GraphicsBackend backend = GraphicsBackend::Auto;

    std::string applicationName = "Cordite";

    /// Enables NVRHI's validation layer and, when it is installed, the validation of the
    /// graphics API itself: the Khronos validation layer of Vulkan, the debug layer of
    /// Direct3D 12 (Windows' optional feature "Graphics Tools"). Every problem any of
    /// them reports is logged and recorded; see validationIssues(). Costs performance;
    /// meant for development and tests.
    bool validation = false;

    /// Vulkan instance extensions needed to present to a window, from
    /// WindowSystem::vulkanInstanceExtensions(). Leave empty for a headless device,
    /// which can render offscreen but cannot create a Swapchain.
    std::vector<std::string> presentationExtensions;

    /// Case-insensitive substring of the adapter name to prefer, e.g. "llvmpipe" or
    /// "RTX". Empty picks the most capable adapter (discrete before integrated). WARP, the
    /// software rasterizer of Direct3D 12, is called "Microsoft Basic Render Driver".
    std::string preferredAdapter;
};

class GraphicsDevice {
public:
    /// Fails with Unsupported when no adapter can draw for the engine - for Vulkan, none
    /// of version 1.3; for Direct3D 12, none of feature level 11_0 and shader model 6.0,
    /// and anywhere but Windows - with DeviceError when the graphics API itself reports an
    /// error, and with InvalidState when a GraphicsDevice already exists.
    [[nodiscard]] static Result<std::unique_ptr<GraphicsDevice>> create(const GraphicsDeviceDesc& desc);
    ~GraphicsDevice();
    CORDITE_NON_MOVABLE(GraphicsDevice);

    /// The NVRHI device. Never null; owned by the GraphicsDevice.
    [[nodiscard]] nvrhi::IDevice* device() const;

    /// The graphics API it draws with: Vulkan or Direct3D12, never Auto.
    [[nodiscard]] GraphicsBackend backend() const;

    [[nodiscard]] const AdapterInfo& adapter() const;

    /// True when the device can create a Swapchain.
    [[nodiscard]] bool canPresent() const;

    /// True when the validation of the graphics API itself is active - the Khronos layer
    /// of Vulkan, the debug layer of Direct3D 12. It can be requested but unavailable, in
    /// which case only NVRHI's own validation runs.
    [[nodiscard]] bool hasApiValidation() const;

    /// Warnings and errors reported by the validation layers since the last clear, in
    /// order. Tests assert that this stays empty.
    [[nodiscard]] std::vector<std::string> validationIssues() const;
    [[nodiscard]] size_t validationIssueCount() const;
    void clearValidationIssues();

    /// Releases GPU resources whose last use has finished executing. Call once per frame.
    void collectGarbage();

    /// Blocks until the GPU has finished all submitted work and the device has taken
    /// note of it: whatever is polled for right afterwards - a timer query, an event
    /// query - is there.
    void waitIdle();

    /// Raw Vulkan handles, for the rhi module's own implementation files only. The device
    /// must draw with Vulkan.
    [[nodiscard]] const VulkanContext& vulkan() const;

private:
    struct Impl;

    explicit GraphicsDevice(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
};

} // namespace cordite
