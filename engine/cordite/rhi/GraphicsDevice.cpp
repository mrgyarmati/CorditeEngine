#include "cordite/rhi/GraphicsDevice.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"
#include "cordite/core/StringUtils.h"
#include "cordite/rhi/DeviceBackend.h"
#include "cordite/rhi/vulkan/VulkanContext.h"

#include <nvrhi/validation.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace cordite {
namespace {

/// The Vulkan of NVRHI and of this module goes through one global dispatcher that holds the
/// entry points of a single instance and device, so there can only be one device at a time
/// - of any graphics API, so that what holds for one holds for all.
std::atomic<bool> g_graphicsDeviceExists{false};

/// The device of the graphics API the description asks for. Fails as GraphicsDevice::create
/// says.
Result<BackendDevice> makeDevice(const GraphicsDeviceDesc& desc, nvrhi::IMessageCallback& messages,
                                 ValidationSink& issues) {
    switch (desc.backend) {
        case GraphicsBackend::Auto:
        case GraphicsBackend::Vulkan:
            return createVulkanDevice(desc, messages, issues);
        case GraphicsBackend::Direct3D12:
#if CORDITE_PLATFORM_WINDOWS
            return createD3D12Device(desc, messages, issues);
#else
            return makeError(ErrorCode::Unsupported, "Direct3D 12 is a graphics API of Windows alone");
#endif
    }
    return makeError(ErrorCode::InvalidArgument, "there is no graphics API number {}", static_cast<int>(desc.backend));
}

} // namespace

std::string_view nameOf(GraphicsBackend backend) {
    switch (backend) {
        case GraphicsBackend::Auto:
            return "Auto";
        case GraphicsBackend::Vulkan:
            return "Vulkan";
        case GraphicsBackend::Direct3D12:
            return "Direct3D 12";
    }
    return "Auto";
}

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

int adapterScore(AdapterType type, std::string_view name, std::string_view preferred) {
    int score = 0;
    switch (type) {
        case AdapterType::Discrete:
            score = 400;
            break;
        case AdapterType::Integrated:
            score = 300;
            break;
        case AdapterType::Virtual:
            score = 200;
            break;
        case AdapterType::Software:
            score = 100;
            break;
        case AdapterType::Other:
            score = 50;
            break;
    }
    if (!preferred.empty() && toLowerAscii(name).find(toLowerAscii(preferred)) != std::string::npos) {
        score += 1000;
    }
    return score;
}

struct GraphicsDevice::Impl final : public nvrhi::IMessageCallback, public ValidationSink {
    // Declared first: the objects of the graphics API outlive the NVRHI devices made of
    // them, which the destructor lets go of before it.
    std::unique_ptr<DeviceBackend> backend;
    /// The device of the API, as NVRHI has it.
    nvrhi::DeviceHandle apiDevice;
    /// That device, or NVRHI's validation layer around it.
    nvrhi::DeviceHandle device;
    /// Used by waitIdle() to wait for the last submission by name.
    nvrhi::EventQueryHandle idleQuery;
    GraphicsBackend api = GraphicsBackend::Vulkan;
    AdapterInfo adapter;
    bool apiValidation = false;
    bool canPresent = false;
    /// Why the device was lost, once it was (reportLost, checkLost).
    std::optional<std::string> lostReason;

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
        apiDevice = nullptr;
        // What the validation of the API said on the way out is still heard.
        if (backend) {
            backend->collectMessages();
        }
        backend.reset();
    }

    void recordIssue(std::string text) override {
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
};

Result<std::unique_ptr<GraphicsDevice>> GraphicsDevice::create(const GraphicsDeviceDesc& desc) {
    if (g_graphicsDeviceExists.exchange(true)) {
        return makeError(ErrorCode::InvalidState,
                         "a GraphicsDevice already exists; destroy it before creating another");
    }

    auto impl = std::make_unique<Impl>();
    Result<BackendDevice> made = makeDevice(desc, *impl, *impl);
    if (!made) {
        impl.reset();
        g_graphicsDeviceExists = false;
        return tl::unexpected(std::move(made).error());
    }
    impl->api = desc.backend == GraphicsBackend::Direct3D12 ? GraphicsBackend::Direct3D12 : GraphicsBackend::Vulkan;
    impl->backend = std::move(made->backend);
    impl->apiDevice = std::move(made->device);
    impl->adapter = std::move(made->adapter);
    impl->apiValidation = made->apiValidation;
    impl->canPresent = made->canPresent;
    impl->device = desc.validation ? nvrhi::validation::createValidationLayer(impl->apiDevice) : impl->apiDevice;
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

GraphicsBackend GraphicsDevice::backend() const {
    return m_impl->api;
}

const AdapterInfo& GraphicsDevice::adapter() const {
    return m_impl->adapter;
}

bool GraphicsDevice::canPresent() const {
    return m_impl->canPresent;
}

bool GraphicsDevice::hasApiValidation() const {
    return m_impl->apiValidation;
}

std::vector<std::string> GraphicsDevice::validationIssues() const {
    m_impl->backend->collectMessages();
    const std::lock_guard lock(m_impl->issuesMutex);
    return m_impl->issues;
}

size_t GraphicsDevice::validationIssueCount() const {
    m_impl->backend->collectMessages();
    const std::lock_guard lock(m_impl->issuesMutex);
    return m_impl->issues.size();
}

void GraphicsDevice::clearValidationIssues() {
    // What was said before the clear is cleared with it, not heard later.
    m_impl->backend->collectMessages();
    const std::lock_guard lock(m_impl->issuesMutex);
    m_impl->issues.clear();
}

void GraphicsDevice::collectGarbage() {
    m_impl->device->runGarbageCollection();
    m_impl->backend->collectMessages();
}

void GraphicsDevice::reportLost(std::string reason) {
    if (!m_impl->lostReason) {
        CORDITE_LOG_ERROR("RHI", "the GPU {} was lost: {}", m_impl->adapter.name, reason);
        m_impl->lostReason = std::move(reason);
    }
}

Status GraphicsDevice::checkLost() {
    if (!m_impl->lostReason) {
        if (std::optional<std::string> reason = m_impl->backend->lostReason()) {
            reportLost(std::move(*reason));
        }
    }
    if (m_impl->lostReason) {
        return makeError(ErrorCode::DeviceLost, "the GPU {} was lost: {}", m_impl->adapter.name, *m_impl->lostReason);
    }
    return {};
}

void GraphicsDevice::waitIdle() {
    nvrhi::IDevice* device = m_impl->device;
    if (!device->waitForIdle()) {
        // Vulkan says it in the result of the wait, and nowhere else.
        reportLost("waiting for the GPU to finish failed");
        return;
    }

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
    const VulkanContext* context = m_impl->backend->vulkan();
    CORDITE_ASSERT(context != nullptr, "the device draws with {}, not with Vulkan", nameOf(m_impl->api));
    // Without assertions, handles of nothing: what is done with them fails as Vulkan fails.
    static const VulkanContext kNoVulkan;
    return context != nullptr ? *context : kNoVulkan;
}

} // namespace cordite
