#pragma once

/// @file
/// The GPU device.
///
/// GraphicsDevice creates the Vulkan instance and device and wraps them in an NVRHI
/// device, which is what the renderer programs against. It can be created without a
/// window ("headless") to render offscreen, which is how the automated rendering tests
/// and the editor's command-line mode work.
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
/// @endcode
///
/// Only one GraphicsDevice can exist at a time, and it must be created and destroyed on
/// the main thread.

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

enum class AdapterType : uint8_t {
    Discrete,
    Integrated,
    Virtual,
    /// A CPU rasterizer such as lavapipe.
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
    /// Vulkan version the device supports, e.g. "1.4.321".
    std::string apiVersion;
};

struct GraphicsDeviceDesc {
    std::string applicationName = "Cordite";

    /// Enables NVRHI's validation layer and, when it is installed, the Khronos Vulkan
    /// validation layer. Every problem either of them reports is logged and recorded;
    /// see validationIssues(). Costs performance; meant for development and tests.
    bool validation = false;

    /// Vulkan instance extensions needed to present to a window, from
    /// WindowSystem::vulkanInstanceExtensions(). Leave empty for a headless device,
    /// which can render offscreen but cannot create a Swapchain.
    std::vector<std::string> presentationExtensions;

    /// Case-insensitive substring of the adapter name to prefer, e.g. "llvmpipe" or
    /// "RTX". Empty picks the most capable adapter (discrete before integrated).
    std::string preferredAdapter;
};

class GraphicsDevice {
public:
    /// Fails with Unsupported when no Vulkan 1.3 capable adapter is available, with
    /// DeviceError when Vulkan itself reports an error, and with InvalidState when a
    /// GraphicsDevice already exists.
    [[nodiscard]] static Result<std::unique_ptr<GraphicsDevice>> create(const GraphicsDeviceDesc& desc);
    ~GraphicsDevice();
    CORDITE_NON_MOVABLE(GraphicsDevice);

    /// The NVRHI device. Never null; owned by the GraphicsDevice.
    [[nodiscard]] nvrhi::IDevice* device() const;

    [[nodiscard]] const AdapterInfo& adapter() const;

    /// True when the device can create a Swapchain.
    [[nodiscard]] bool canPresent() const;

    /// True when the Khronos validation layer is active. It can be requested but
    /// unavailable, in which case only NVRHI's own validation runs.
    [[nodiscard]] bool hasVulkanValidation() const;

    /// Warnings and errors reported by the validation layers since the last clear, in
    /// order. Tests assert that this stays empty.
    [[nodiscard]] std::vector<std::string> validationIssues() const;
    [[nodiscard]] size_t validationIssueCount() const;
    void clearValidationIssues();

    /// Releases GPU resources whose last use has finished executing. Call once per frame.
    void collectGarbage();

    /// Blocks until the GPU has finished all submitted work.
    void waitIdle();

    /// Raw Vulkan handles, for the rhi module's own implementation files only.
    [[nodiscard]] const VulkanContext& vulkan() const;

private:
    struct Impl;

    explicit GraphicsDevice(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> m_impl;
};

} // namespace cordite
