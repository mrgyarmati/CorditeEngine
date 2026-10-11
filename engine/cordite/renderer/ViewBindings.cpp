#include "cordite/renderer/ViewBindings.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"

#include <nvrhi/utils.h>

#include <algorithm>
#include <span>
#include <utility>
#include <vector>

namespace cordite {
namespace {

/// Mirrors ViewConstants in shaders/common/View.slang.
struct ViewConstants {
    Mat4 viewProjection;
    Mat4 inverseViewProjection;
    Vec3 cameraPosition;
    /// 1 for a camera without perspective, 0 for one with.
    float orthographic;
    Vec3 cameraForward;
    uint32_t directionalLightCount;
    Vec3 skyColor;
    float clusterDepthScale;
    Vec3 groundColor;
    float clusterDepthBias;
    Vec2 targetSize;
    Vec2 inverseTargetSize;
};

static_assert(sizeof(ViewConstants) == 208, "must match the cbuffer layout in View.slang");
static_assert(sizeof(ClusterRange) == 8, "must match ClusterRange in Lights.slang");

/// The constants are rewritten for every view of every frame, so they need as many
/// versions as there can be views in flight: a few views, and a few frames.
constexpr uint32_t kMaxVersions = 64;

/// Room for the lights of an ordinary scene before the buffers have to grow.
constexpr size_t kInitialLightCapacity = 64;
constexpr size_t kInitialIndexCapacity = 4096;

/// The bytes of a list, or of one element of nothing when the list is empty: a buffer
/// cannot be empty, and a shader that finds a count of zero reads none of it.
template <typename T>
std::span<const T> orPlaceholder(const std::vector<T>& values, const T& placeholder) {
    return values.empty() ? std::span<const T>(&placeholder, 1) : std::span<const T>(values);
}

} // namespace

Result<ViewBindings> ViewBindings::create(nvrhi::IDevice* device) {
    CORDITE_ASSERT(device != nullptr, "view bindings need a device");

    ViewBindings view;
    view.m_device = device;
    view.m_constants = device->createBuffer(
        nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(ViewConstants), "ViewConstants", kMaxVersions));

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2),
    };
    view.m_layout = device->createBindingLayout(layoutDesc);
    if (!view.m_constants || !view.m_layout) {
        return makeError(ErrorCode::DeviceError, "cannot create the view constants");
    }

    const bool reserved =
        view.reserve(view.m_lights, kInitialLightCapacity * sizeof(GpuLight), sizeof(GpuLight), "Lights") &&
        view.reserve(view.m_clusterGrid, kClusterCount * sizeof(ClusterRange), sizeof(ClusterRange), "Cluster grid") &&
        view.reserve(view.m_lightIndices, kInitialIndexCapacity * sizeof(uint32_t), sizeof(uint32_t), "Light lists");
    if (!reserved) {
        return makeError(ErrorCode::DeviceError, "cannot create the buffers of the lights");
    }
    return view;
}

bool ViewBindings::reserve(nvrhi::BufferHandle& buffer, size_t byteSize, uint32_t stride, const char* name) {
    if (buffer && buffer->getDesc().byteSize >= byteSize) {
        return true;
    }
    // Half as much again as is needed, so that a scene that gains lights one by one
    // does not get a new buffer for each.
    nvrhi::BufferDesc desc;
    desc.byteSize = (byteSize + byteSize / 2 + stride - 1) / stride * stride;
    desc.structStride = stride;
    desc.debugName = name;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    // The previous buffer stays alive until the frames that read it have finished.
    nvrhi::BufferHandle replacement = m_device->createBuffer(desc);
    if (!replacement) {
        CORDITE_LOG_ERROR("Renderer", "cannot allocate {} bytes for the buffer '{}'", desc.byteSize, name);
        return false;
    }
    buffer = std::move(replacement);
    m_bindingsOutdated = true;
    return true;
}

void ViewBindings::update(nvrhi::ICommandList* commandList, const SceneView& view, const LightClusters& lights) {
    CORDITE_ASSERT(commandList != nullptr && m_constants != nullptr,
                   "update needs a command list and created bindings");
    if (commandList == nullptr || m_constants == nullptr) {
        return;
    }

    // The lights, where they fit. If a buffer cannot grow, the view is drawn without
    // its point and spot lights rather than with lists that point past the end.
    static const GpuLight kNoLight;
    static const uint32_t kNoIndex = 0;
    const std::span<const GpuLight> lightData = orPlaceholder(lights.lights, kNoLight);
    const std::span<const uint32_t> indexData = orPlaceholder(lights.indices, kNoIndex);
    const bool gridIsComplete = lights.grid.size() == kClusterCount;
    const bool fits = gridIsComplete && reserve(m_lights, lightData.size_bytes(), sizeof(GpuLight), "Lights") &&
                      reserve(m_lightIndices, indexData.size_bytes(), sizeof(uint32_t), "Light lists");
    if (fits) {
        commandList->writeBuffer(m_lights, lightData.data(), lightData.size_bytes());
        commandList->writeBuffer(m_lightIndices, indexData.data(), indexData.size_bytes());
        commandList->writeBuffer(m_clusterGrid, lights.grid.data(), lights.grid.size() * sizeof(ClusterRange));
    } else {
        const std::vector<ClusterRange> empty(kClusterCount);
        commandList->writeBuffer(m_clusterGrid, empty.data(), empty.size() * sizeof(ClusterRange));
    }

    const Vec2 size(std::max(view.targetSize.x, 1u), std::max(view.targetSize.y, 1u));
    ViewConstants constants{};
    constants.viewProjection = view.camera.viewProjection;
    constants.inverseViewProjection = view.camera.inverseViewProjection;
    constants.cameraPosition = view.camera.position;
    constants.orthographic = view.camera.orthographic ? 1.0f : 0.0f;
    constants.cameraForward = normalizeOr(view.camera.forward, kWorldForward);
    constants.directionalLightCount = fits ? lights.directionalCount : 0;
    constants.skyColor = view.skyColor;
    constants.clusterDepthScale = lights.depthMapping.scale;
    constants.groundColor = view.groundColor;
    constants.clusterDepthBias = lights.depthMapping.bias;
    constants.targetSize = size;
    constants.inverseTargetSize = 1.0f / size;
    commandList->writeBuffer(m_constants, &constants, sizeof(constants));

    if (m_bindingsOutdated || !m_bindings) {
        nvrhi::BindingSetDesc setDesc;
        setDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_constants),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_lights),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_clusterGrid),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(2, m_lightIndices),
        };
        m_bindings = m_device->createBindingSet(setDesc, m_layout);
        CORDITE_VERIFY(m_bindings, "cannot create the view bindings");
        m_bindingsOutdated = false;
    }
}

} // namespace cordite
