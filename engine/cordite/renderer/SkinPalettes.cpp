#include "cordite/renderer/SkinPalettes.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"

#include <algorithm>
#include <utility>

namespace cordite {
namespace {

/// Room for a few figures before the buffer has to grow.
constexpr size_t kInitialJointCapacity = 256;

} // namespace

Result<SkinPalettes> SkinPalettes::create(nvrhi::IDevice* device) {
    CORDITE_ASSERT(device != nullptr, "skin palettes need a device");

    SkinPalettes palettes;
    palettes.m_device = device;

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Vertex;
    layoutDesc.registerSpace = 2;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindings = {nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0)};
    palettes.m_layout = device->createBindingLayout(layoutDesc);
    if (!palettes.m_layout) {
        return makeError(ErrorCode::DeviceError, "cannot create the layout of the joint matrices");
    }
    // Something to bind from the start: a pipeline that skins needs its set, also in a
    // frame whose upload failed.
    if (!palettes.reserve(kInitialJointCapacity)) {
        return makeError(ErrorCode::DeviceError, "cannot create the buffer of the joint matrices");
    }
    return palettes;
}

void SkinPalettes::clear() {
    m_joints.clear();
}

uint32_t SkinPalettes::add(std::span<const Mat4> skinning) {
    if (skinning.empty()) {
        return kNoPalette;
    }
    if (skinning.size() > kMaxPaletteJoints - m_joints.size()) {
        if (!m_reportedFull) {
            CORDITE_LOG_WARN("Renderer",
                             "skinned meshes are drawn as they were modelled: a view is drawn with {} joint "
                             "matrices at the most",
                             kMaxPaletteJoints);
            m_reportedFull = true;
        }
        return kNoPalette;
    }

    const auto first = static_cast<uint32_t>(m_joints.size());
    static const Mat4 kInPlace(1.0f);
    for (const Mat4& joint : skinning) {
        // One matrix that is no number would make every vertex that follows it one,
        // and every pass that blurs would spread it.
        const Mat4& matrix = isFinite(joint) ? joint : kInPlace;
        // glm stores columns; the shader wants rows.
        m_joints.push_back({Vec4(matrix[0][0], matrix[1][0], matrix[2][0], matrix[3][0]),
                            Vec4(matrix[0][1], matrix[1][1], matrix[2][1], matrix[3][1]),
                            Vec4(matrix[0][2], matrix[1][2], matrix[2][2], matrix[3][2])});
    }
    return first;
}

bool SkinPalettes::reserve(size_t jointCount) {
    const size_t capacity = m_buffer ? static_cast<size_t>(m_buffer->getDesc().byteSize) / sizeof(GpuJoint) : 0;
    if (jointCount <= capacity) {
        return true;
    }
    // Half as much again as is needed, so that a crowd that grows by one figure at a
    // time does not get a new buffer for each.
    nvrhi::BufferDesc desc;
    desc.byteSize = std::max(jointCount + jointCount / 2, kInitialJointCapacity) * sizeof(GpuJoint);
    desc.structStride = sizeof(GpuJoint);
    desc.debugName = "Joint matrices";
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    // The previous buffer and its set stay alive until the draws that used them have
    // finished.
    nvrhi::BufferHandle buffer = m_device->createBuffer(desc);
    if (!buffer) {
        CORDITE_LOG_ERROR("Renderer", "cannot allocate {} bytes for the joint matrices", desc.byteSize);
        return false;
    }
    nvrhi::BindingSetDesc setDesc;
    setDesc.bindings = {nvrhi::BindingSetItem::StructuredBuffer_SRV(0, buffer)};
    nvrhi::BindingSetHandle bindings = m_device->createBindingSet(setDesc, m_layout);
    if (!bindings) {
        CORDITE_LOG_ERROR("Renderer", "cannot create the bindings of the joint matrices");
        return false;
    }
    m_buffer = std::move(buffer);
    m_bindings = std::move(bindings);
    return true;
}

bool SkinPalettes::upload(nvrhi::ICommandList* commandList) {
    CORDITE_ASSERT(commandList != nullptr, "upload needs a command list");
    if (commandList == nullptr) {
        return false;
    }
    if (m_joints.empty()) {
        return true;
    }
    if (!reserve(m_joints.size())) {
        return false;
    }
    commandList->writeBuffer(m_buffer, m_joints.data(), m_joints.size() * sizeof(GpuJoint));
    return true;
}

uint64_t SkinPalettes::bufferBytes() const {
    return m_buffer ? m_buffer->getDesc().byteSize : 0;
}

} // namespace cordite
