#include "cordite/renderer/ViewBindings.h"

#include "cordite/core/Assert.h"

#include <nvrhi/utils.h>

#include <algorithm>

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
    float padding0;
    Vec3 keyLightDirection;
    float padding1;
    Vec3 keyLightColor;
    float padding2;
    Vec3 skyColor;
    float padding3;
    Vec3 groundColor;
    float padding4;
    Vec2 targetSize;
    Vec2 inverseTargetSize;
};

static_assert(sizeof(ViewConstants) == 240, "must match the cbuffer layout in View.slang");

/// The buffer is rewritten for every view of every frame, so it needs as many versions
/// as there can be views in flight: a few views, and a few frames.
constexpr uint32_t kMaxVersions = 64;

} // namespace

Result<ViewBindings> ViewBindings::create(nvrhi::IDevice* device) {
    CORDITE_ASSERT(device != nullptr, "view bindings need a device");

    ViewBindings view;
    view.m_constants = device->createBuffer(
        nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(ViewConstants), "ViewConstants", kMaxVersions));

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0)};
    view.m_layout = device->createBindingLayout(layoutDesc);
    if (!view.m_constants || !view.m_layout) {
        return makeError(ErrorCode::DeviceError, "cannot create the view constants");
    }

    nvrhi::BindingSetDesc setDesc;
    setDesc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, view.m_constants)};
    view.m_bindings = device->createBindingSet(setDesc, view.m_layout);
    if (!view.m_bindings) {
        return makeError(ErrorCode::DeviceError, "cannot create the view bindings");
    }
    return view;
}

void ViewBindings::update(nvrhi::ICommandList* commandList, const SceneView& view) {
    CORDITE_ASSERT(commandList != nullptr && m_constants != nullptr,
                   "update needs a command list and created bindings");
    if (commandList == nullptr || m_constants == nullptr) {
        return;
    }
    const Vec2 size(std::max(view.targetSize.x, 1u), std::max(view.targetSize.y, 1u));

    ViewConstants constants{};
    constants.viewProjection = view.camera.viewProjection;
    constants.inverseViewProjection = view.camera.inverseViewProjection;
    constants.cameraPosition = view.camera.position;
    constants.orthographic = view.camera.orthographic ? 1.0f : 0.0f;
    constants.cameraForward = normalizeOr(view.camera.forward, kWorldForward);
    constants.keyLightDirection = normalizeOr(view.keyLightDirection, kWorldUp);
    constants.keyLightColor = view.keyLightColor;
    constants.skyColor = view.skyColor;
    constants.groundColor = view.groundColor;
    constants.targetSize = size;
    constants.inverseTargetSize = 1.0f / size;
    commandList->writeBuffer(m_constants, &constants, sizeof(constants));
}

} // namespace cordite
