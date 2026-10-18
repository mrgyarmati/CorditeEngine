#include "cordite/rhi/GraphicsDevice.h"

#include "cordite/core/Log.h"
#include "cordite/core/StringUtils.h"
#include "cordite/core/Version.h"
#include "cordite/core/memory/Memory.h"
#include "cordite/rhi/vulkan/VulkanContext.h"

#include <nvrhi/validation.h>
#include <nvrhi/vulkan.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <stdexcept>

// Storage for the Vulkan-Hpp dispatcher shared with NVRHI. It must be defined in exactly
// one translation unit of the program.
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace cordite {
namespace {

/// NVRHI and this module call Vulkan through one global dispatcher that holds the entry
/// points of a single instance and device, so there can only be one device at a time.
std::atomic<bool> g_graphicsDeviceExists{false};

constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";
// Declared by the Vulkan headers only when beta extensions are enabled.
constexpr const char* kPortabilitySubsetExtension = "VK_KHR_portability_subset";

using FeatureChain = vk::StructureChain<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features,
                                        vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan13Features>;

bool hasExtension(const std::vector<vk::ExtensionProperties>& available, std::string_view name) {
    return std::ranges::any_of(available, [name](const vk::ExtensionProperties& properties) {
        return name == std::string_view(properties.extensionName.data());
    });
}

bool hasLayer(const std::vector<vk::LayerProperties>& available, std::string_view name) {
    return std::ranges::any_of(available, [name](const vk::LayerProperties& properties) {
        return name == std::string_view(properties.layerName.data());
    });
}

std::string formatVersion(uint32_t version) {
    return fmt::format("{}.{}.{}", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
                       VK_API_VERSION_PATCH(version));
}

AdapterType adapterTypeOf(vk::PhysicalDeviceType type) {
    switch (type) {
        case vk::PhysicalDeviceType::eDiscreteGpu:
            return AdapterType::Discrete;
        case vk::PhysicalDeviceType::eIntegratedGpu:
            return AdapterType::Integrated;
        case vk::PhysicalDeviceType::eVirtualGpu:
            return AdapterType::Virtual;
        case vk::PhysicalDeviceType::eCpu:
            return AdapterType::Software;
        default:
            return AdapterType::Other;
    }
}

int adapterTypeScore(AdapterType type) {
    switch (type) {
        case AdapterType::Discrete:
            return 400;
        case AdapterType::Integrated:
            return 300;
        case AdapterType::Virtual:
            return 200;
        case AdapterType::Software:
            return 100;
        case AdapterType::Other:
            return 50;
    }
    return 0;
}

/// A physical device that passed every requirement, and how desirable it is.
struct AdapterCandidate {
    vk::PhysicalDevice device;
    uint32_t queueFamily = 0;
    int score = 0;
    bool hasPortabilitySubset = false;
};

/// Checks one physical device against the engine's requirements. Returns the candidate,
/// or the reason it cannot be used.
Result<AdapterCandidate> evaluateAdapter(vk::PhysicalDevice device, bool needsPresentation,
                                         std::string_view preferredAdapter) {
    const vk::PhysicalDeviceProperties properties = device.getProperties();
    const std::string_view name(properties.deviceName.data());

    // NVRHI's Vulkan backend relies on dynamic rendering and synchronization2.
    if (properties.apiVersion < VK_API_VERSION_1_3) {
        return makeError(ErrorCode::Unsupported, "supports only Vulkan {}, 1.3 is required",
                         formatVersion(properties.apiVersion));
    }

    const auto supported =
        device.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features,
                            vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan13Features>();
    if (supported.get<vk::PhysicalDeviceVulkan12Features>().timelineSemaphore == VK_FALSE ||
        supported.get<vk::PhysicalDeviceVulkan13Features>().dynamicRendering == VK_FALSE ||
        supported.get<vk::PhysicalDeviceVulkan13Features>().synchronization2 == VK_FALSE) {
        return makeError(ErrorCode::Unsupported, "lacks timeline semaphores, dynamic rendering or synchronization2");
    }
    // The renderer reads the pictures of environment probes as one array of cube maps, in
    // the shaders that light every surface: without it no lighting pipeline can be made.
    if (supported.get<vk::PhysicalDeviceFeatures2>().features.imageCubeArray == VK_FALSE) {
        return makeError(ErrorCode::Unsupported, "lacks arrays of cube maps (imageCubeArray)");
    }

    std::optional<uint32_t> queueFamily;
    const std::vector<vk::QueueFamilyProperties> families = device.getQueueFamilyProperties();
    for (uint32_t index = 0; index < families.size(); ++index) {
        const vk::QueueFlags required = vk::QueueFlagBits::eGraphics | vk::QueueFlagBits::eCompute;
        if (families[index].queueCount > 0 && (families[index].queueFlags & required) == required) {
            queueFamily = index;
            break;
        }
    }
    if (!queueFamily) {
        return makeError(ErrorCode::Unsupported, "has no queue family supporting graphics and compute");
    }

    const std::vector<vk::ExtensionProperties> extensions = device.enumerateDeviceExtensionProperties();
    if (needsPresentation && !hasExtension(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
        return makeError(ErrorCode::Unsupported, "cannot present to a window (no VK_KHR_swapchain)");
    }

    AdapterCandidate candidate;
    candidate.device = device;
    candidate.queueFamily = *queueFamily;
    candidate.hasPortabilitySubset = hasExtension(extensions, kPortabilitySubsetExtension);
    candidate.score = adapterTypeScore(adapterTypeOf(properties.deviceType));
    if (!preferredAdapter.empty() && toLowerAscii(name).find(toLowerAscii(preferredAdapter)) != std::string::npos) {
        candidate.score += 1000;
    }
    return candidate;
}

/// Turns on every optional feature the renderer can use, where the device supports it.
/// The features NVRHI and the renderer need are required, and were checked by
/// evaluateAdapter.
void selectFeatures(const FeatureChain& supported, FeatureChain& enabled) {
    const vk::PhysicalDeviceFeatures& supportedCore = supported.get<vk::PhysicalDeviceFeatures2>().features;
    vk::PhysicalDeviceFeatures& core = enabled.get<vk::PhysicalDeviceFeatures2>().features;
    core.samplerAnisotropy = supportedCore.samplerAnisotropy;
    core.textureCompressionBC = supportedCore.textureCompressionBC;
    core.depthClamp = supportedCore.depthClamp;
    core.depthBiasClamp = supportedCore.depthBiasClamp;
    core.fillModeNonSolid = supportedCore.fillModeNonSolid;
    core.independentBlend = supportedCore.independentBlend;
    core.imageCubeArray = VK_TRUE;
    core.multiDrawIndirect = supportedCore.multiDrawIndirect;
    core.drawIndirectFirstInstance = supportedCore.drawIndirectFirstInstance;
    core.fragmentStoresAndAtomics = supportedCore.fragmentStoresAndAtomics;
    core.shaderImageGatherExtended = supportedCore.shaderImageGatherExtended;
    core.dualSrcBlend = supportedCore.dualSrcBlend;

    const auto& supported11 = supported.get<vk::PhysicalDeviceVulkan11Features>();
    auto& vulkan11 = enabled.get<vk::PhysicalDeviceVulkan11Features>();
    vulkan11.shaderDrawParameters = supported11.shaderDrawParameters;

    const auto& supported12 = supported.get<vk::PhysicalDeviceVulkan12Features>();
    auto& vulkan12 = enabled.get<vk::PhysicalDeviceVulkan12Features>();
    vulkan12.timelineSemaphore = VK_TRUE;
    vulkan12.bufferDeviceAddress = supported12.bufferDeviceAddress;
    vulkan12.scalarBlockLayout = supported12.scalarBlockLayout;
    vulkan12.hostQueryReset = supported12.hostQueryReset;

    const auto& supported13 = supported.get<vk::PhysicalDeviceVulkan13Features>();
    auto& vulkan13 = enabled.get<vk::PhysicalDeviceVulkan13Features>();
    vulkan13.dynamicRendering = VK_TRUE;
    vulkan13.synchronization2 = VK_TRUE;
    vulkan13.maintenance4 = supported13.maintenance4;
}

} // namespace

std::string_view nameOf(AdapterType type) {
    switch (type) {
        case AdapterType::Discrete:
            return "Discrete";
        case AdapterType::Integrated:
            return "Integrated";
        case AdapterType::Virtual:
            return "Virtual";
        case AdapterType::Software:
            return "Software";
        case AdapterType::Other:
            return "Other";
    }
    return "Other";
}

struct GraphicsDevice::Impl final : public nvrhi::IMessageCallback {
    // Declared first so the Vulkan library stays loaded until everything else is gone.
    std::unique_ptr<vk::detail::DynamicLoader> loader;
    VulkanContext vulkan;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    nvrhi::vulkan::DeviceHandle vulkanDevice;
    nvrhi::DeviceHandle device;
    /// Used by waitIdle() to wait for the last submission by name.
    nvrhi::EventQueryHandle idleQuery;
    AdapterInfo adapter;
    bool vulkanValidation = false;

    mutable std::mutex issuesMutex;
    std::vector<std::string> issues;

    Impl() = default;
    CORDITE_NON_MOVABLE(Impl);

    ~Impl() override {
        if (device) {
            device->waitForIdle();
            device->runGarbageCollection();
        }
        idleQuery = nullptr;
        device = nullptr;
        vulkanDevice = nullptr;

        if (vulkan.device) {
            vulkan.device.destroy();
        }
        if (debugMessenger != VK_NULL_HANDLE) {
            VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyDebugUtilsMessengerEXT(vulkan.instance, debugMessenger, nullptr);
        }
        if (vulkan.instance) {
            vulkan.instance.destroy();
        }
    }

    void recordIssue(std::string text) {
        const std::lock_guard lock(issuesMutex);
        issues.push_back(std::move(text));
    }

    /// Messages from NVRHI itself and from its validation layer.
    void message(nvrhi::MessageSeverity severity, const char* messageText) override {
        switch (severity) {
            case nvrhi::MessageSeverity::Info:
                CORDITE_LOG_DEBUG("RHI", "{}", messageText);
                break;
            case nvrhi::MessageSeverity::Warning:
                CORDITE_LOG_WARN("RHI", "{}", messageText);
                recordIssue(fmt::format("NVRHI warning: {}", messageText));
                break;
            case nvrhi::MessageSeverity::Error:
            case nvrhi::MessageSeverity::Fatal:
                CORDITE_LOG_ERROR("RHI", "{}", messageText);
                recordIssue(fmt::format("NVRHI error: {}", messageText));
                break;
        }
    }

    /// Messages from the Vulkan loader, drivers and the Khronos validation layer.
    static VKAPI_ATTR VkBool32 VKAPI_CALL onVulkanMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                          VkDebugUtilsMessageTypeFlagsEXT types,
                                                          const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                          void* userData) {
        auto* self = static_cast<Impl*>(userData);
        const char* text = data != nullptr && data->pMessage != nullptr ? data->pMessage : "";

        // Only validation findings count as issues. General messages are loader and
        // driver chatter (for example about other vendors' layers on the machine).
        if ((types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) == 0) {
            CORDITE_LOG_DEBUG("Vulkan", "{}", text);
            return VK_FALSE;
        }

        if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
            CORDITE_LOG_ERROR("Vulkan", "{}", text);
            self->recordIssue(fmt::format("Vulkan validation error: {}", text));
        } else {
            CORDITE_LOG_WARN("Vulkan", "{}", text);
            self->recordIssue(fmt::format("Vulkan validation warning: {}", text));
        }
        return VK_FALSE;
    }

    Status initialize(const GraphicsDeviceDesc& desc);
    Status createInstance(const GraphicsDeviceDesc& desc, std::vector<const char*>& enabledExtensions);
    Result<AdapterCandidate> selectAdapter(const GraphicsDeviceDesc& desc) const;
    void createDevice(const AdapterCandidate& candidate, bool needsPresentation,
                      std::vector<const char*>& enabledExtensions, bool& bufferDeviceAddress);
};

Status GraphicsDevice::Impl::createInstance(const GraphicsDeviceDesc& desc,
                                            std::vector<const char*>& enabledExtensions) {
    if (vk::enumerateInstanceVersion() < VK_API_VERSION_1_3) {
        return makeError(ErrorCode::Unsupported,
                         "the installed Vulkan runtime is older than 1.3; update the graphics drivers");
    }

    const std::vector<vk::ExtensionProperties> availableExtensions = vk::enumerateInstanceExtensionProperties();
    const std::vector<vk::LayerProperties> availableLayers = vk::enumerateInstanceLayerProperties();

    for (const std::string& extension : desc.presentationExtensions) {
        if (!hasExtension(availableExtensions, extension)) {
            return makeError(ErrorCode::Unsupported, "the Vulkan runtime lacks {}, which is needed to present",
                             extension);
        }
        enabledExtensions.push_back(extension.c_str());
    }

    std::vector<const char*> layers;
    bool debugUtils = false;
    if (desc.validation) {
        if (hasLayer(availableLayers, kValidationLayerName)) {
            layers.push_back(kValidationLayerName);
            vulkanValidation = true;
        } else {
            CORDITE_LOG_WARN("RHI",
                             "{} is not installed; only NVRHI validation is active. Install the Vulkan SDK "
                             "(scripts/bootstrap.ps1 -WithVulkanSdk) for full validation.",
                             kValidationLayerName);
        }
        if (hasExtension(availableExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            enabledExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            debugUtils = true;
        }
    }

    // MoltenVK is a "portability" implementation, which the loader hides unless asked.
    vk::InstanceCreateFlags flags;
    if (hasExtension(availableExtensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        enabledExtensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        flags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
    }

    const uint32_t engineVersion =
        VK_MAKE_API_VERSION(0, CORDITE_VERSION_MAJOR, CORDITE_VERSION_MINOR, CORDITE_VERSION_PATCH);
    const vk::ApplicationInfo applicationInfo(desc.applicationName.c_str(), engineVersion, kEngineName, engineVersion,
                                              VK_API_VERSION_1_3);

    vk::InstanceCreateInfo instanceInfo;
    instanceInfo.setFlags(flags)
        .setPApplicationInfo(&applicationInfo)
        .setPEnabledLayerNames(layers)
        .setPEnabledExtensionNames(enabledExtensions);
    vulkan.instance = vk::createInstance(instanceInfo);

    VULKAN_HPP_DEFAULT_DISPATCHER.init(vulkan.instance);

    if (debugUtils) {
        VkDebugUtilsMessengerCreateInfoEXT messengerInfo{};
        messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messengerInfo.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messengerInfo.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        messengerInfo.pfnUserCallback = &Impl::onVulkanMessage;
        messengerInfo.pUserData = this;

        const VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateDebugUtilsMessengerEXT(
            vulkan.instance, &messengerInfo, nullptr, &debugMessenger);
        if (result != VK_SUCCESS) {
            CORDITE_LOG_WARN("RHI", "cannot install the Vulkan debug messenger: {}",
                             nvrhi::vulkan::resultToString(result));
            debugMessenger = VK_NULL_HANDLE;
        }
    }
    return {};
}

Result<AdapterCandidate> GraphicsDevice::Impl::selectAdapter(const GraphicsDeviceDesc& desc) const {
    const bool needsPresentation = !desc.presentationExtensions.empty();

    std::optional<AdapterCandidate> best;
    std::string rejections;
    for (const vk::PhysicalDevice physicalDevice : vulkan.instance.enumeratePhysicalDevices()) {
        Result<AdapterCandidate> candidate = evaluateAdapter(physicalDevice, needsPresentation, desc.preferredAdapter);
        if (!candidate) {
            rejections += fmt::format("{}{}: {}", rejections.empty() ? "" : "; ",
                                      std::string_view(physicalDevice.getProperties().deviceName.data()),
                                      candidate.error().message);
            continue;
        }
        if (!best || candidate->score > best->score) {
            best = *candidate;
        }
    }

    if (!best) {
        return makeError(ErrorCode::Unsupported, "no usable graphics adapter was found ({})",
                         rejections.empty() ? "the system reports no Vulkan devices" : rejections);
    }
    return *best;
}

void GraphicsDevice::Impl::createDevice(const AdapterCandidate& candidate, bool needsPresentation,
                                        std::vector<const char*>& enabledExtensions, bool& bufferDeviceAddress) {
    if (needsPresentation) {
        enabledExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    }
    // The specification requires this extension to be enabled whenever a device offers it.
    if (candidate.hasPortabilitySubset) {
        enabledExtensions.push_back(kPortabilitySubsetExtension);
    }

    const FeatureChain supported =
        candidate.device.getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features,
                                      vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan13Features>();
    FeatureChain enabled;
    selectFeatures(supported, enabled);
    bufferDeviceAddress = enabled.get<vk::PhysicalDeviceVulkan12Features>().bufferDeviceAddress == VK_TRUE;

    const float queuePriority = 1.0f;
    const vk::DeviceQueueCreateInfo queueInfo({}, candidate.queueFamily, 1, &queuePriority);

    vk::DeviceCreateInfo deviceInfo;
    deviceInfo.setQueueCreateInfos(queueInfo)
        .setPEnabledExtensionNames(enabledExtensions)
        .setPNext(&enabled.get<vk::PhysicalDeviceFeatures2>());

    vulkan.physicalDevice = candidate.device;
    vulkan.device = candidate.device.createDevice(deviceInfo);
    // Resolve device-level functions straight to the driver, skipping the loader's
    // dispatch on every call. This is why only one GraphicsDevice may exist at a time:
    // NVRHI and this module share one global dispatcher.
    VULKAN_HPP_DEFAULT_DISPATCHER.init(vulkan.device);
    vulkan.graphicsQueueFamily = candidate.queueFamily;
    vulkan.graphicsQueue = vulkan.device.getQueue(candidate.queueFamily, 0);
    vulkan.canPresent = needsPresentation;
}

Status GraphicsDevice::Impl::initialize(const GraphicsDeviceDesc& desc) {
    try {
        loader = std::make_unique<vk::detail::DynamicLoader>();
    } catch (const std::runtime_error&) {
        return makeError(ErrorCode::Unsupported,
                         "the Vulkan runtime library was not found; install or update the graphics drivers");
    }

    try {
        VULKAN_HPP_DEFAULT_DISPATCHER.init(loader->getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));

        std::vector<const char*> instanceExtensions;
        CORDITE_TRY(createInstance(desc, instanceExtensions));
        CORDITE_TRY_ASSIGN(const AdapterCandidate candidate, selectAdapter(desc));

        std::vector<const char*> deviceExtensions;
        bool bufferDeviceAddress = false;
        createDevice(candidate, !desc.presentationExtensions.empty(), deviceExtensions, bufferDeviceAddress);

        const vk::PhysicalDeviceProperties properties = vulkan.physicalDevice.getProperties();
        adapter.name = properties.deviceName.data();
        adapter.type = adapterTypeOf(properties.deviceType);
        adapter.vendorId = properties.vendorID;
        adapter.deviceId = properties.deviceID;
        adapter.apiVersion = formatVersion(properties.apiVersion);
        adapter.supportsWireframe = vulkan.physicalDevice.getFeatures().fillModeNonSolid != VK_FALSE;
        // Timestamps have to be supported by the device and by the queue that is used.
        const std::vector<vk::QueueFamilyProperties> families = vulkan.physicalDevice.getQueueFamilyProperties();
        adapter.supportsTimerQueries = properties.limits.timestampComputeAndGraphics != VK_FALSE &&
                                       vulkan.graphicsQueueFamily < families.size() &&
                                       families[vulkan.graphicsQueueFamily].timestampValidBits > 0;
        const vk::PhysicalDeviceMemoryProperties memory = vulkan.physicalDevice.getMemoryProperties();
        for (uint32_t heap = 0; heap < memory.memoryHeapCount; ++heap) {
            if (memory.memoryHeaps[heap].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
                adapter.dedicatedMemoryBytes += memory.memoryHeaps[heap].size;
            }
        }

        nvrhi::vulkan::DeviceDesc deviceDesc;
        deviceDesc.errorCB = this;
        deviceDesc.instance = vulkan.instance;
        deviceDesc.physicalDevice = vulkan.physicalDevice;
        deviceDesc.device = vulkan.device;
        deviceDesc.graphicsQueue = vulkan.graphicsQueue;
        deviceDesc.graphicsQueueIndex = static_cast<int>(vulkan.graphicsQueueFamily);
        deviceDesc.transferQueue = VK_NULL_HANDLE;
        deviceDesc.computeQueue = VK_NULL_HANDLE;
        deviceDesc.instanceExtensions = instanceExtensions.data();
        deviceDesc.numInstanceExtensions = instanceExtensions.size();
        deviceDesc.deviceExtensions = deviceExtensions.data();
        deviceDesc.numDeviceExtensions = deviceExtensions.size();
        deviceDesc.bufferDeviceAddressSupported = bufferDeviceAddress;

        vulkanDevice = nvrhi::vulkan::createDevice(deviceDesc);
        if (!vulkanDevice) {
            return makeError(ErrorCode::DeviceError, "NVRHI could not create its Vulkan device");
        }
        vulkan.nvrhiDevice = vulkanDevice.Get();
        device = desc.validation ? nvrhi::validation::createValidationLayer(vulkanDevice)
                                 : nvrhi::DeviceHandle(vulkanDevice);
    } catch (const vk::SystemError& error) {
        return makeError(ErrorCode::DeviceError, "Vulkan device creation failed: {}", error.what());
    }

    CORDITE_LOG_INFO("RHI", "Using {} ({}, Vulkan {}, {} MiB){}", adapter.name, nameOf(adapter.type),
                     adapter.apiVersion, adapter.dedicatedMemoryBytes / kMebibyte,
                     vulkanValidation ? ", validation on" : "");
    return {};
}

Result<std::unique_ptr<GraphicsDevice>> GraphicsDevice::create(const GraphicsDeviceDesc& desc) {
    if (g_graphicsDeviceExists.exchange(true)) {
        return makeError(ErrorCode::InvalidState,
                         "a GraphicsDevice already exists; destroy it before creating another");
    }

    auto impl = std::make_unique<Impl>();
    if (Status status = impl->initialize(desc); !status) {
        impl.reset();
        g_graphicsDeviceExists = false;
        return tl::unexpected(std::move(status).error());
    }
    return std::unique_ptr<GraphicsDevice>(new GraphicsDevice(std::move(impl)));
}

GraphicsDevice::GraphicsDevice(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

GraphicsDevice::~GraphicsDevice() {
    m_impl.reset();
    g_graphicsDeviceExists = false;
}

nvrhi::IDevice* GraphicsDevice::device() const {
    return m_impl->device.Get();
}

const AdapterInfo& GraphicsDevice::adapter() const {
    return m_impl->adapter;
}

bool GraphicsDevice::canPresent() const {
    return m_impl->vulkan.canPresent;
}

bool GraphicsDevice::hasVulkanValidation() const {
    return m_impl->vulkanValidation;
}

std::vector<std::string> GraphicsDevice::validationIssues() const {
    const std::lock_guard lock(m_impl->issuesMutex);
    return m_impl->issues;
}

size_t GraphicsDevice::validationIssueCount() const {
    const std::lock_guard lock(m_impl->issuesMutex);
    return m_impl->issues.size();
}

void GraphicsDevice::clearValidationIssues() {
    const std::lock_guard lock(m_impl->issuesMutex);
    m_impl->issues.clear();
}

void GraphicsDevice::collectGarbage() {
    m_impl->device->runGarbageCollection();
}

void GraphicsDevice::waitIdle() {
    nvrhi::IDevice* device = m_impl->device;
    device->waitForIdle();

    // "Nothing is running" and "the last piece of work is recorded as finished" are two
    // things, and a driver may be a moment late with the second. Whoever polls for a
    // result right after this call would then be told, now and then, that it is not
    // there yet. Waiting for the last submission by name closes the gap.
    if (!m_impl->idleQuery) {
        m_impl->idleQuery = device->createEventQuery();
    }
    if (m_impl->idleQuery) {
        device->resetEventQuery(m_impl->idleQuery);
        device->setEventQuery(m_impl->idleQuery, nvrhi::CommandQueue::Graphics);
        device->waitEventQuery(m_impl->idleQuery);
    }
}

const VulkanContext& GraphicsDevice::vulkan() const {
    return m_impl->vulkan;
}

} // namespace cordite
