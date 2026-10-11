#include "cordite/renderer/SceneTargets.h"

#include "cordite/core/Assert.h"

#include <utility>

namespace cordite {
namespace {

/// The largest texture dimension every supported GPU can create.
constexpr uint32_t kMaxDimension = 16384;

nvrhi::TextureHandle createTexture(nvrhi::IDevice* device, UVec2 size, nvrhi::Format format, std::string name,
                                   nvrhi::ResourceStates state) {
    nvrhi::TextureDesc desc;
    desc.width = size.x;
    desc.height = size.y;
    desc.format = format;
    desc.debugName = std::move(name);
    desc.isRenderTarget = true;
    desc.initialState = state;
    desc.keepInitialState = true;
    desc.clearValue = nvrhi::Color(0.0f);
    desc.useClearValue = true;
    return device->createTexture(desc);
}

} // namespace

Result<SceneTargets> SceneTargets::create(nvrhi::IDevice* device, UVec2 size, std::string debugName) {
    if (size.x == 0 || size.y == 0 || size.x > kMaxDimension || size.y > kMaxDimension) {
        return makeError(ErrorCode::InvalidArgument, "scene targets must be between 1 and {} pixels a side, got {}x{}",
                         kMaxDimension, size.x, size.y);
    }

    SceneTargets targets;
    targets.m_size = size;
    targets.m_debugName = std::move(debugName);
    const std::string& name = targets.m_debugName;

    targets.m_sceneColor = createTexture(device, size, nvrhi::Format::RGBA16_FLOAT, name + " scene color",
                                         nvrhi::ResourceStates::ShaderResource);
    targets.m_depth =
        createTexture(device, size, nvrhi::Format::D32, name + " depth", nvrhi::ResourceStates::DepthWrite);
    targets.m_display = createTexture(device, size, nvrhi::Format::RGBA8_UNORM, name + " display",
                                      nvrhi::ResourceStates::ShaderResource);
    if (!targets.m_sceneColor || !targets.m_depth || !targets.m_display) {
        return makeError(ErrorCode::DeviceError, "cannot create the {}x{} textures of '{}'", size.x, size.y, name);
    }

    targets.m_sceneFramebuffer = device->createFramebuffer(
        nvrhi::FramebufferDesc().addColorAttachment(targets.m_sceneColor).setDepthAttachment(targets.m_depth));
    targets.m_displayFramebuffer =
        device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(targets.m_display));
    targets.m_overlayFramebuffer = device->createFramebuffer(
        nvrhi::FramebufferDesc().addColorAttachment(targets.m_display).setDepthAttachment(targets.m_depth));
    if (!targets.m_sceneFramebuffer || !targets.m_displayFramebuffer || !targets.m_overlayFramebuffer) {
        return makeError(ErrorCode::DeviceError, "cannot create the framebuffers of '{}'", name);
    }
    return targets;
}

void SceneTargets::clear(nvrhi::ICommandList* commandList, const Vec3& background) const {
    CORDITE_ASSERT(isValid() && commandList != nullptr, "clear needs valid targets and a command list");
    if (!isValid() || commandList == nullptr) {
        return;
    }
    commandList->clearTextureFloat(m_sceneColor, nvrhi::AllSubresources,
                                   nvrhi::Color(background.r, background.g, background.b, 1.0f));
    // Reversed-Z: 0 is the far plane.
    const float farDepth = 0.0f;
    commandList->clearDepthStencilTexture(m_depth, nvrhi::AllSubresources, true, farDepth, false, 0);
}

Status SceneTargets::resize(nvrhi::IDevice* device, UVec2 size) {
    if (isValid() && size == m_size) {
        return {};
    }
    CORDITE_TRY_ASSIGN(SceneTargets replacement, create(device, size, m_debugName.empty() ? "Scene" : m_debugName));
    *this = std::move(replacement);
    return {};
}

} // namespace cordite
