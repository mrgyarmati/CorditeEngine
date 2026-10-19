#include "cordite/core/FileSystem.h"
#include "cordite/core/Log.h"
#include "cordite/core/Platform.h"
#include "cordite/core/Version.h"
#include "cordite/core/memory/Memory.h"
#include "cordite/rhi/DeviceBackend.h"
#include "cordite/rhi/vulkan/VulkanContext.h"

#include <nvrhi/vulkan.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

// Storage for the Vulkan-Hpp dispatcher shared with NVRHI. It must be defined in exactly
// one translation unit of the program.
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE

namespace cordite {
namespace {

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
    candidate.score = adapterScore(adapterTypeOf(properties.deviceType), name, preferredAdapter);
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

/// The Vulkan library: on macOS MoltenVK, where the application bundle that the program is
/// in brings it (kBundledVulkanLibrary), and otherwise the system's. Throws
/// std::runtime_error when the library cannot be loaded.
std::unique_ptr<vk::detail::DynamicLoader> loadVulkan() {
#if CORDITE_PLATFORM_MACOS
    if (const std::optional<std::filesystem::path> bundled = bundledLibrary(executablePath(), kBundledVulkanLibrary)) {
        CORDITE_LOG_INFO("RHI", "drawing with the Vulkan of the bundle, {}", pathToUtf8(*bundled));
        return std::make_unique<vk::detail::DynamicLoader>(pathToUtf8(*bundled));
    }
#endif
    return std::make_unique<vk::detail::DynamicLoader>();
}

/// The objects of Vulkan behind a GraphicsDevice. NVRHI and this module call Vulkan through
/// one global dispatcher that holds the entry points of a single instance and device -
/// which is one reason there can only be one device at a time.
struct VulkanBackend final : public DeviceBackend {
    // Declared first so the Vulkan library stays loaded until everything else is gone.
    std::unique_ptr<vk::detail::DynamicLoader> loader;
    VulkanContext context;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    bool validationLayer = false;
    /// Told what the validation layer reports; outlives the backend.
    ValidationSink* issues = nullptr;

    explicit VulkanBackend(ValidationSink& sink) : issues(&sink) {}

    CORDITE_NON_MOVABLE(VulkanBackend);

    ~VulkanBackend() override {
        if (context.device) {
            context.device.destroy();
        }
        if (debugMessenger != VK_NULL_HANDLE) {
            VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyDebugUtilsMessengerEXT(context.instance, debugMessenger, nullptr);
        }
        if (context.instance) {
            context.instance.destroy();
        }
    }

    [[nodiscard]] const VulkanContext* vulkan() const override { return &context; }

    /// Messages from the Vulkan loader, drivers and the Khronos validation layer.
    static VKAPI_ATTR VkBool32 VKAPI_CALL onVulkanMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                          VkDebugUtilsMessageTypeFlagsEXT types,
                                                          const VkDebugUtilsMessengerCallbackDataEXT* data,
                                                          void* userData) {
        auto* sink = static_cast<ValidationSink*>(userData);
        const char* text = data != nullptr && data->pMessage != nullptr ? data->pMessage : "";

        // Only validation findings count as issues. General messages are loader and
        // driver chatter (for example about other vendors' layers on the machine).
        if ((types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) == 0) {
            CORDITE_LOG_DEBUG("Vulkan", "{}", text);
            return VK_FALSE;
        }

        if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0) {
            CORDITE_LOG_ERROR("Vulkan", "{}", text);
            sink->recordIssue(fmt::format("Vulkan validation error: {}", text));
        } else {
            CORDITE_LOG_WARN("Vulkan", "{}", text);
            sink->recordIssue(fmt::format("Vulkan validation warning: {}", text));
        }
        return VK_FALSE;
    }

    Status createInstance(const GraphicsDeviceDesc& desc, std::vector<const char*>& enabledExtensions);
    [[nodiscard]] Result<AdapterCandidate> selectAdapter(const GraphicsDeviceDesc& desc) const;
    void createDevice(const AdapterCandidate& candidate, bool needsPresentation,
                      std::vector<const char*>& enabledExtensions, bool& bufferDeviceAddress);
};

Status VulkanBackend::createInstance(const GraphicsDeviceDesc& desc, std::vector<const char*>& enabledExtensions) {
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
            validationLayer = true;
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
    context.instance = vk::createInstance(instanceInfo);

    VULKAN_HPP_DEFAULT_DISPATCHER.init(context.instance);

    if (debugUtils) {
        VkDebugUtilsMessengerCreateInfoEXT messengerInfo{};
        messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messengerInfo.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messengerInfo.messageType =
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        messengerInfo.pfnUserCallback = &VulkanBackend::onVulkanMessage;
        messengerInfo.pUserData = issues;

        const VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateDebugUtilsMessengerEXT(
            context.instance, &messengerInfo, nullptr, &debugMessenger);
        if (result != VK_SUCCESS) {
            CORDITE_LOG_WARN("RHI", "cannot install the Vulkan debug messenger: {}",
                             nvrhi::vulkan::resultToString(result));
            debugMessenger = VK_NULL_HANDLE;
        }
    }
    return {};
}

Result<AdapterCandidate> VulkanBackend::selectAdapter(const GraphicsDeviceDesc& desc) const {
    const bool needsPresentation = !desc.presentationExtensions.empty();

    std::optional<AdapterCandidate> best;
    std::string rejections;
    for (const vk::PhysicalDevice physicalDevice : context.instance.enumeratePhysicalDevices()) {
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

void VulkanBackend::createDevice(const AdapterCandidate& candidate, bool needsPresentation,
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

    context.physicalDevice = candidate.device;
    context.device = candidate.device.createDevice(deviceInfo);
    // Resolve device-level functions straight to the driver, skipping the loader's
    // dispatch on every call. This is why only one GraphicsDevice may exist at a time:
    // NVRHI and this module share one global dispatcher.
    VULKAN_HPP_DEFAULT_DISPATCHER.init(context.device);
    context.graphicsQueueFamily = candidate.queueFamily;
    context.graphicsQueue = context.device.getQueue(candidate.queueFamily, 0);
    context.canPresent = needsPresentation;
}

/// What the device is, as Vulkan says.
AdapterInfo describeAdapter(const VulkanContext& context) {
    const vk::PhysicalDeviceProperties properties = context.physicalDevice.getProperties();
    AdapterInfo adapter;
    adapter.name = properties.deviceName.data();
    adapter.type = adapterTypeOf(properties.deviceType);
    adapter.vendorId = properties.vendorID;
    adapter.deviceId = properties.deviceID;
    adapter.apiVersion = formatVersion(properties.apiVersion);
    adapter.supportsWireframe = context.physicalDevice.getFeatures().fillModeNonSolid != VK_FALSE;
    // Timestamps have to be supported by the device and by the queue that is used.
    const std::vector<vk::QueueFamilyProperties> families = context.physicalDevice.getQueueFamilyProperties();
    adapter.supportsTimerQueries = properties.limits.timestampComputeAndGraphics != VK_FALSE &&
                                   context.graphicsQueueFamily < families.size() &&
                                   families[context.graphicsQueueFamily].timestampValidBits > 0;
    const vk::PhysicalDeviceMemoryProperties memory = context.physicalDevice.getMemoryProperties();
    for (uint32_t heap = 0; heap < memory.memoryHeapCount; ++heap) {
        if (memory.memoryHeaps[heap].flags & vk::MemoryHeapFlagBits::eDeviceLocal) {
            adapter.dedicatedMemoryBytes += memory.memoryHeaps[heap].size;
        }
    }
    return adapter;
}

} // namespace

Result<BackendDevice> createVulkanDevice(const GraphicsDeviceDesc& desc, nvrhi::IMessageCallback& messages,
                                         ValidationSink& issues) {
    auto backend = std::make_unique<VulkanBackend>(issues);
    try {
        backend->loader = loadVulkan();
    } catch (const std::runtime_error&) {
        return makeError(ErrorCode::Unsupported,
                         "the Vulkan runtime library was not found; install or update the graphics drivers");
    }

    BackendDevice made;
    try {
        VULKAN_HPP_DEFAULT_DISPATCHER.init(
            backend->loader->getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr"));

        std::vector<const char*> instanceExtensions;
        CORDITE_TRY(backend->createInstance(desc, instanceExtensions));
        CORDITE_TRY_ASSIGN(const AdapterCandidate candidate, backend->selectAdapter(desc));

        std::vector<const char*> deviceExtensions;
        bool bufferDeviceAddress = false;
        backend->createDevice(candidate, !desc.presentationExtensions.empty(), deviceExtensions, bufferDeviceAddress);
        made.adapter = describeAdapter(backend->context);

        const VulkanContext& context = backend->context;
        nvrhi::vulkan::DeviceDesc deviceDesc;
        deviceDesc.errorCB = &messages;
        deviceDesc.instance = context.instance;
        deviceDesc.physicalDevice = context.physicalDevice;
        deviceDesc.device = context.device;
        deviceDesc.graphicsQueue = context.graphicsQueue;
        deviceDesc.graphicsQueueIndex = static_cast<int>(context.graphicsQueueFamily);
        deviceDesc.transferQueue = VK_NULL_HANDLE;
        deviceDesc.computeQueue = VK_NULL_HANDLE;
        deviceDesc.instanceExtensions = instanceExtensions.data();
        deviceDesc.numInstanceExtensions = instanceExtensions.size();
        deviceDesc.deviceExtensions = deviceExtensions.data();
        deviceDesc.numDeviceExtensions = deviceExtensions.size();
        deviceDesc.bufferDeviceAddressSupported = bufferDeviceAddress;

        const nvrhi::vulkan::DeviceHandle vulkanDevice = nvrhi::vulkan::createDevice(deviceDesc);
        if (!vulkanDevice) {
            return makeError(ErrorCode::DeviceError, "NVRHI could not create its Vulkan device");
        }
        backend->context.nvrhiDevice = vulkanDevice.Get();
        made.device = vulkanDevice;
    } catch (const vk::SystemError& error) {
        return makeError(ErrorCode::DeviceError, "Vulkan device creation failed: {}", error.what());
    }

    CORDITE_LOG_INFO("RHI", "Using {} ({}, Vulkan {}, {} MiB){}", made.adapter.name, nameOf(made.adapter.type),
                     made.adapter.apiVersion, made.adapter.dedicatedMemoryBytes / kMebibyte,
                     backend->validationLayer ? ", validation on" : "");
    made.apiValidation = backend->validationLayer;
    made.canPresent = backend->context.canPresent;
    made.backend = std::move(backend);
    return made;
}

} // namespace cordite
