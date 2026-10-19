#include "cordite/rhi/Swapchain.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"
#include "cordite/rhi/vulkan/VulkanContext.h"

#include <nvrhi/vulkan.h>

#include <algorithm>
#include <array>
#include <bit>
#include <deque>
#include <vector>

namespace cordite {
namespace {

/// Frames the CPU may queue before present() waits for the GPU. Keeps input latency
/// bounded when the CPU is faster than the GPU.
constexpr size_t kMaxFramesInFlight = 2;

struct SurfaceFormatChoice {
    vk::SurfaceFormatKHR surface;
    nvrhi::Format format = nvrhi::Format::UNKNOWN;
};

/// Picks an 8-bit UNORM format. The tonemapping pass encodes to sRGB itself, and UI is
/// blended in display space, so an sRGB swapchain format would encode twice.
Result<SurfaceFormatChoice> chooseSurfaceFormat(const std::vector<vk::SurfaceFormatKHR>& available) {
    const std::array<std::pair<vk::Format, nvrhi::Format>, 2> preferred = {{
        {vk::Format::eB8G8R8A8Unorm, nvrhi::Format::BGRA8_UNORM},
        {vk::Format::eR8G8B8A8Unorm, nvrhi::Format::RGBA8_UNORM},
    }};
    for (const auto& [vulkanFormat, format] : preferred) {
        for (const vk::SurfaceFormatKHR& candidate : available) {
            if (candidate.format == vulkanFormat && candidate.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
                return SurfaceFormatChoice{candidate, format};
            }
        }
    }
    return makeError(ErrorCode::Unsupported, "the display offers no 8-bit BGRA or RGBA surface format");
}

vk::PresentModeKHR choosePresentMode(const std::vector<vk::PresentModeKHR>& available, bool vsync) {
    if (!vsync) {
        // Mailbox avoids tearing without waiting; immediate is the fallback.
        for (const vk::PresentModeKHR mode : {vk::PresentModeKHR::eMailbox, vk::PresentModeKHR::eImmediate}) {
            if (std::ranges::find(available, mode) != available.end()) {
                return mode;
            }
        }
    }
    // FIFO is the only mode every implementation must support.
    return vk::PresentModeKHR::eFifo;
}

} // namespace

struct Swapchain::Impl {
    GraphicsDevice& graphics;
    const Window& window;
    nvrhi::vulkan::IDevice* vulkanDevice = nullptr;

    vk::SurfaceKHR surface;
    vk::SwapchainKHR swapchain;
    nvrhi::Format format = nvrhi::Format::UNKNOWN;
    UVec2 size{0};
    bool vsync = true;
    bool needsRecreation = false;
    uint32_t recreationCount = 0;

    std::vector<nvrhi::TextureHandle> textures;
    std::vector<nvrhi::FramebufferHandle> framebuffers;

    // One semaphore per back buffer signals that rendering finished and the image may be
    // presented. Acquire semaphores are cycled independently, because the image index is
    // only known after the acquire call that needs the semaphore.
    std::vector<vk::Semaphore> presentSemaphores;
    std::vector<vk::Semaphore> acquireSemaphores;
    size_t nextAcquireSemaphore = 0;

    uint32_t imageIndex = 0;
    bool frameAcquired = false;

    nvrhi::CommandListHandle commandList;
    std::deque<nvrhi::EventQueryHandle> framesInFlight;
    std::vector<nvrhi::EventQueryHandle> freeQueries;

    Impl(GraphicsDevice& device, const Window& targetWindow) : graphics(device), window(targetWindow) {}

    CORDITE_NON_MOVABLE(Impl);

    ~Impl() {
        // Nothing may still be rendering into images that are about to be destroyed.
        graphics.waitIdle();
        destroySwapchainResources();
        const vk::Device device = graphics.vulkan().device;
        if (swapchain) {
            device.destroySwapchainKHR(swapchain);
        }
        if (surface) {
            graphics.vulkan().instance.destroySurfaceKHR(surface);
        }
    }

    /// Releases everything that refers to the current swapchain images.
    void destroySwapchainResources() {
        framesInFlight.clear();
        freeQueries.clear();
        framebuffers.clear();
        textures.clear();
        graphics.collectGarbage();

        const vk::Device device = graphics.vulkan().device;
        for (const vk::Semaphore semaphore : presentSemaphores) {
            device.destroySemaphore(semaphore);
        }
        for (const vk::Semaphore semaphore : acquireSemaphores) {
            device.destroySemaphore(semaphore);
        }
        presentSemaphores.clear();
        acquireSemaphores.clear();
        nextAcquireSemaphore = 0;
    }

    /// Both return false, without an error, when the surface currently has no area (the
    /// window was minimized at that moment); the caller retries on a later frame.
    Result<bool> createSwapchain();
    Result<bool> recreate();
};

Result<bool> Swapchain::Impl::createSwapchain() {
    const VulkanContext& vulkan = graphics.vulkan();
    try {
        if (vulkan.physicalDevice.getSurfaceSupportKHR(vulkan.graphicsQueueFamily, surface) == VK_FALSE) {
            return makeError(ErrorCode::Unsupported, "{} cannot present to this window", graphics.adapter().name);
        }

        const vk::SurfaceCapabilitiesKHR capabilities = vulkan.physicalDevice.getSurfaceCapabilitiesKHR(surface);
        CORDITE_TRY_ASSIGN(const SurfaceFormatChoice formatChoice,
                           chooseSurfaceFormat(vulkan.physicalDevice.getSurfaceFormatsKHR(surface)));
        const vk::PresentModeKHR presentMode =
            choosePresentMode(vulkan.physicalDevice.getSurfacePresentModesKHR(surface), vsync);

        // Some platforms fix the extent; others (currentExtent == UINT32_MAX) let us choose.
        vk::Extent2D extent = capabilities.currentExtent;
        if (extent.width == UINT32_MAX) {
            const UVec2 framebufferSize = window.framebufferSize();
            extent.width =
                std::clamp(framebufferSize.x, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            extent.height =
                std::clamp(framebufferSize.y, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
        }
        if (extent.width == 0 || extent.height == 0) {
            return false;
        }

        // One image more than the minimum avoids waiting on the driver to release one.
        uint32_t imageCount = capabilities.minImageCount + 1;
        if (capabilities.maxImageCount > 0) {
            imageCount = std::min(imageCount, capabilities.maxImageCount);
        }

        // Transfer source lets frames be read back for screenshots; transfer destination
        // lets them be cleared and copied into directly.
        vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eColorAttachment;
        for (const vk::ImageUsageFlagBits optional :
             {vk::ImageUsageFlagBits::eTransferSrc, vk::ImageUsageFlagBits::eTransferDst}) {
            if (capabilities.supportedUsageFlags & optional) {
                usage |= optional;
            }
        }

        const vk::CompositeAlphaFlagBitsKHR compositeAlpha =
            (capabilities.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eOpaque)
                ? vk::CompositeAlphaFlagBitsKHR::eOpaque
                : vk::CompositeAlphaFlagBitsKHR::eInherit;

        const vk::SwapchainKHR previous = swapchain;
        vk::SwapchainCreateInfoKHR createInfo;
        createInfo.setSurface(surface)
            .setMinImageCount(imageCount)
            .setImageFormat(formatChoice.surface.format)
            .setImageColorSpace(formatChoice.surface.colorSpace)
            .setImageExtent(extent)
            .setImageArrayLayers(1)
            .setImageUsage(usage)
            .setImageSharingMode(vk::SharingMode::eExclusive)
            .setPreTransform(capabilities.currentTransform)
            .setCompositeAlpha(compositeAlpha)
            .setPresentMode(presentMode)
            .setClipped(VK_TRUE)
            .setOldSwapchain(previous);
        swapchain = vulkan.device.createSwapchainKHR(createInfo);
        if (previous) {
            vulkan.device.destroySwapchainKHR(previous);
        }

        format = formatChoice.format;
        size = UVec2(extent.width, extent.height);

        const std::vector<vk::Image> images = vulkan.device.getSwapchainImagesKHR(swapchain);
        nvrhi::IDevice* device = graphics.device();
        for (size_t index = 0; index < images.size(); ++index) {
            const std::string name = fmt::format("Swapchain image {}", index);
            nvrhi::TextureDesc textureDesc;
            textureDesc.width = extent.width;
            textureDesc.height = extent.height;
            textureDesc.format = format;
            textureDesc.debugName = name;
            textureDesc.isRenderTarget = true;
            // NVRHI returns the image to this state at the end of every command list,
            // which is the layout presentation requires. It also knows that the image
            // starts out with an undefined layout and handles the first use accordingly.
            textureDesc.initialState = nvrhi::ResourceStates::Present;
            textureDesc.keepInitialState = true;

            VkImage image = images[index];
            nvrhi::TextureHandle texture =
                device->createHandleForNativeTexture(nvrhi::ObjectTypes::VK_Image, nvrhi::Object(image), textureDesc);
            if (!texture) {
                return makeError(ErrorCode::DeviceError, "cannot wrap swapchain image {}", index);
            }
            framebuffers.push_back(device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(texture)));
            textures.push_back(std::move(texture));

            presentSemaphores.push_back(vulkan.device.createSemaphore(vk::SemaphoreCreateInfo()));
        }
        // One more acquire semaphore than images: at most that many acquisitions can be
        // outstanding, so the next one in the cycle is always free.
        for (size_t index = 0; index < images.size() + 1; ++index) {
            acquireSemaphores.push_back(vulkan.device.createSemaphore(vk::SemaphoreCreateInfo()));
        }
    } catch (const vk::SystemError& error) {
        return makeError(ErrorCode::DeviceError, "swapchain creation failed: {}", error.what());
    }

    return true;
}

Result<bool> Swapchain::Impl::recreate() {
    graphics.waitIdle();
    destroySwapchainResources();
    CORDITE_TRY_ASSIGN(const bool created, createSwapchain());
    if (!created) {
        needsRecreation = true;
        return false;
    }
    needsRecreation = false;
    ++recreationCount;
    CORDITE_LOG_DEBUG("RHI", "Swapchain recreated at {}x{}", size.x, size.y);
    return true;
}

Result<std::unique_ptr<Swapchain>> Swapchain::create(GraphicsDevice& device, const Window& window,
                                                     const SwapchainDesc& desc) {
    if (device.backend() != GraphicsBackend::Vulkan) {
        return makeError(ErrorCode::Unsupported, "a window is drawn into with Vulkan alone, not with {} yet",
                         nameOf(device.backend()));
    }
    if (!device.canPresent()) {
        return makeError(ErrorCode::InvalidState,
                         "the graphics device was created without presentation support (headless)");
    }

    auto impl = std::make_unique<Impl>(device, window);
    impl->vsync = desc.vsync;
    impl->vulkanDevice = device.vulkan().nvrhiDevice;
    impl->commandList = device.device()->createCommandList();

    CORDITE_TRY_ASSIGN(const uint64_t surfaceHandle, window.createVulkanSurface(device.vulkan().instance));
    impl->surface = std::bit_cast<VkSurfaceKHR>(surfaceHandle);

    CORDITE_TRY_ASSIGN(const bool created, impl->createSwapchain());
    if (!created) {
        return makeError(ErrorCode::InvalidState, "cannot create a swapchain for a window without area");
    }
    return std::unique_ptr<Swapchain>(new Swapchain(std::move(impl)));
}

Swapchain::Swapchain(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

Swapchain::~Swapchain() = default;

Result<SwapchainFrame> Swapchain::beginFrame() {
    Impl& impl = *m_impl;
    CORDITE_ASSERT(!impl.frameAcquired, "beginFrame called twice without present");

    const UVec2 framebufferSize = impl.window.framebufferSize();
    if (framebufferSize.x == 0 || framebufferSize.y == 0) {
        // Minimized: there is no surface to render to until the window is restored.
        return SwapchainFrame{};
    }

    const VulkanContext& vulkan = impl.graphics.vulkan();

    // An out-of-date swapchain is recreated and the acquire retried once. A second
    // failure means the window is changing faster than we can follow; skip the frame.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (impl.needsRecreation || framebufferSize != impl.size) {
            CORDITE_TRY_ASSIGN(const bool recreated, impl.recreate());
            if (!recreated) {
                return SwapchainFrame{};
            }
        }

        const vk::Semaphore semaphore = impl.acquireSemaphores[impl.nextAcquireSemaphore];
        // The C entry point is used because Vulkan-Hpp would throw for VK_ERROR_OUT_OF_DATE_KHR,
        // which is an ordinary event here.
        const VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkAcquireNextImageKHR(
            vulkan.device, impl.swapchain, UINT64_MAX, semaphore, VK_NULL_HANDLE, &impl.imageIndex);

        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            impl.needsRecreation = true;
            continue;
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            return makeError(ErrorCode::DeviceError, "cannot acquire a swapchain image: {}",
                             nvrhi::vulkan::resultToString(result));
        }
        if (result == VK_SUBOPTIMAL_KHR) {
            // Still presentable; recreate before the next frame.
            impl.needsRecreation = true;
        }

        impl.nextAcquireSemaphore = (impl.nextAcquireSemaphore + 1) % impl.acquireSemaphores.size();
        // The first command list of the frame must not start before the image is ours.
        impl.vulkanDevice->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, semaphore, 0);
        impl.frameAcquired = true;

        SwapchainFrame frame;
        frame.texture = impl.textures[impl.imageIndex];
        frame.framebuffer = impl.framebuffers[impl.imageIndex];
        frame.size = impl.size;
        return frame;
    }
    return SwapchainFrame{};
}

Status Swapchain::present() {
    Impl& impl = *m_impl;
    CORDITE_ASSERT(impl.frameAcquired, "present called without a frame from beginFrame");
    if (!impl.frameAcquired) {
        return makeError(ErrorCode::InvalidState, "present called without an acquired frame");
    }
    impl.frameAcquired = false;

    const VulkanContext& vulkan = impl.graphics.vulkan();
    nvrhi::IDevice* device = impl.graphics.device();
    const vk::Semaphore presentSemaphore = impl.presentSemaphores[impl.imageIndex];

    // NVRHI attaches the signal to the next submission, so submit a command list to carry
    // it. Requesting the Present state there costs nothing when the frame rendered into
    // the image, and gives a frame that drew nothing a presentable layout.
    impl.vulkanDevice->queueSignalSemaphore(nvrhi::CommandQueue::Graphics, presentSemaphore, 0);
    impl.commandList->open();
    impl.commandList->setTextureState(impl.textures[impl.imageIndex], nvrhi::AllSubresources,
                                      nvrhi::ResourceStates::Present);
    impl.commandList->commitBarriers();
    impl.commandList->close();
    device->executeCommandList(impl.commandList);

    VkSemaphore waitSemaphore = presentSemaphore;
    VkSwapchainKHR swapchain = impl.swapchain;
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &waitSemaphore;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchain;
    presentInfo.pImageIndices = &impl.imageIndex;

    const VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkQueuePresentKHR(vulkan.graphicsQueue, &presentInfo);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        impl.needsRecreation = true;
    } else if (result != VK_SUCCESS) {
        return makeError(ErrorCode::DeviceError, "cannot present: {}", nvrhi::vulkan::resultToString(result));
    }

    // Frame pacing: block until the GPU has finished the oldest queued frame.
    while (impl.framesInFlight.size() >= kMaxFramesInFlight) {
        nvrhi::EventQueryHandle oldest = impl.framesInFlight.front();
        impl.framesInFlight.pop_front();
        device->waitEventQuery(oldest);
        impl.freeQueries.push_back(std::move(oldest));
    }

    nvrhi::EventQueryHandle query;
    if (impl.freeQueries.empty()) {
        query = device->createEventQuery();
    } else {
        query = std::move(impl.freeQueries.back());
        impl.freeQueries.pop_back();
    }
    device->resetEventQuery(query);
    device->setEventQuery(query, nvrhi::CommandQueue::Graphics);
    impl.framesInFlight.push_back(std::move(query));
    return {};
}

UVec2 Swapchain::size() const {
    return m_impl->size;
}

nvrhi::Format Swapchain::format() const {
    return m_impl->format;
}

bool Swapchain::vsync() const {
    return m_impl->vsync;
}

void Swapchain::setVsync(bool enabled) {
    if (enabled != m_impl->vsync) {
        m_impl->vsync = enabled;
        m_impl->needsRecreation = true;
    }
}

uint32_t Swapchain::recreationCount() const {
    return m_impl->recreationCount;
}

} // namespace cordite
