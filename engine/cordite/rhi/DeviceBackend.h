#pragma once

/// @file
/// Internal to the rhi module: what a GraphicsDevice is made of on one graphics API.
/// Nothing outside this module includes this header.
///
/// Each API has a function that makes a device of it - createVulkanDevice in
/// vulkan/VulkanDevice.cpp, createD3D12Device in d3d12/D3D12Device.cpp - and hands back the
/// NVRHI device, what the adapter is, and the objects of the API behind it. GraphicsDevice
/// keeps those for as long as the NVRHI device lives, and lets go of them after it.
///
/// @code
/// CORDITE_TRY_ASSIGN(BackendDevice made, createVulkanDevice(desc, messages, issues));
/// nvrhi::DeviceHandle device = std::move(made.device);   // released first
/// std::unique_ptr<DeviceBackend> backend = std::move(made.backend);   // then this
/// @endcode

#include "cordite/core/Base.h"
#include "cordite/core/Result.h"
#include "cordite/rhi/GraphicsDevice.h"

#include <nvrhi/nvrhi.h>

#include <memory>
#include <string>
#include <string_view>

namespace cordite {

struct VulkanContext;

/// Where what the validation of a graphics API says is recorded, as an issue of the device
/// (GraphicsDevice::validationIssues). Thread-safe: drivers call back on threads of their own.
class ValidationSink {
public:
    virtual void recordIssue(std::string text) = 0;

protected:
    ValidationSink() = default;
    ~ValidationSink() = default;
    ValidationSink(const ValidationSink&) = default;
    ValidationSink& operator=(const ValidationSink&) = default;
    ValidationSink(ValidationSink&&) = default;
    ValidationSink& operator=(ValidationSink&&) = default;
};

/// The objects of one graphics API behind a GraphicsDevice. Destroyed after the NVRHI
/// device that was made of them.
class DeviceBackend {
public:
    DeviceBackend() = default;
    virtual ~DeviceBackend() = default;
    CORDITE_NON_MOVABLE(DeviceBackend);

    /// Records what the validation of the API has said since it was last asked, for an
    /// API that keeps its messages instead of calling back with them.
    virtual void collectMessages() {}

    /// The raw Vulkan objects of a device of Vulkan; null for any other.
    [[nodiscard]] virtual const VulkanContext* vulkan() const { return nullptr; }
};

/// What a function that makes a device hands back.
struct BackendDevice {
    std::unique_ptr<DeviceBackend> backend;
    /// The device as NVRHI has it for the API, not yet wrapped in NVRHI's validation.
    nvrhi::DeviceHandle device;
    AdapterInfo adapter;
    /// The validation of the API itself is active (GraphicsDevice::hasApiValidation).
    bool apiValidation = false;
    /// The device can make a Swapchain.
    bool canPresent = false;
};

/// How much an adapter is wanted: by its kind - discrete before integrated before virtual,
/// software and other - and before all of those when its name has `preferred` in it,
/// whatever the case (GraphicsDeviceDesc::preferredAdapter).
[[nodiscard]] int adapterScore(AdapterType type, std::string_view name, std::string_view preferred);

/// Makes a device of Vulkan. `messages` is told what NVRHI says, `issues` what the
/// validation layer of Vulkan reports; both outlive the device. Fails as
/// GraphicsDevice::create says.
[[nodiscard]] Result<BackendDevice> createVulkanDevice(const GraphicsDeviceDesc& desc,
                                                       nvrhi::IMessageCallback& messages, ValidationSink& issues);

/// Makes a device of Direct3D 12, as createVulkanDevice does one of Vulkan. Defined on
/// Windows alone, and called only there.
[[nodiscard]] Result<BackendDevice> createD3D12Device(const GraphicsDeviceDesc& desc, nvrhi::IMessageCallback& messages,
                                                      ValidationSink& issues);

} // namespace cordite
