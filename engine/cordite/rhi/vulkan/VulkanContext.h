#pragma once

/// @file
/// Internal to the rhi module: the raw Vulkan objects behind a GraphicsDevice. Nothing
/// outside this module includes this header; the rest of the engine talks to NVRHI.

#include <nvrhi/vulkan.h>
#include <vulkan/vulkan.hpp>

#include <cstdint>

namespace cordite {

struct VulkanContext {
    vk::Instance instance;
    vk::PhysicalDevice physicalDevice;
    vk::Device device;
    vk::Queue graphicsQueue;
    uint32_t graphicsQueueFamily = 0;
    /// True when the device was created with VK_KHR_swapchain.
    bool canPresent = false;
    /// The NVRHI Vulkan device, without the validation wrapper, for its Vulkan-specific
    /// queue and semaphore functions.
    nvrhi::vulkan::IDevice* nvrhiDevice = nullptr;
};

} // namespace cordite
