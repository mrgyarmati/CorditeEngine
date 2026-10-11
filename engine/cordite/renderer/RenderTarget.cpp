#include "cordite/renderer/RenderTarget.h"

#include "cordite/core/Assert.h"

#include <utility>

namespace cordite {
namespace {

/// The largest texture dimension every supported GPU can create.
constexpr uint32_t kMaxDimension = 16384;

} // namespace

Result<RenderTarget> RenderTarget::create(nvrhi::IDevice* device, const RenderTargetDesc& desc) {
    if (desc.size.x == 0 || desc.size.y == 0 || desc.size.x > kMaxDimension || desc.size.y > kMaxDimension) {
        return makeError(ErrorCode::InvalidArgument,
                         "a render target must be between 1 and {} pixels a side, got {}x{}", kMaxDimension,
                         desc.size.x, desc.size.y);
    }

    RenderTarget target;
    target.m_desc = desc;

    nvrhi::TextureDesc colorDesc;
    colorDesc.width = desc.size.x;
    colorDesc.height = desc.size.y;
    colorDesc.format = desc.colorFormat;
    colorDesc.debugName = desc.debugName + " color";
    colorDesc.isRenderTarget = true;
    colorDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    colorDesc.keepInitialState = true;
    colorDesc.clearValue = nvrhi::Color(desc.clearColor.r, desc.clearColor.g, desc.clearColor.b, desc.clearColor.a);
    colorDesc.useClearValue = true;
    target.m_color = device->createTexture(colorDesc);
    if (!target.m_color) {
        return makeError(ErrorCode::DeviceError, "cannot create the {}x{} color texture of '{}'", desc.size.x,
                         desc.size.y, desc.debugName);
    }

    nvrhi::FramebufferDesc framebufferDesc;
    framebufferDesc.addColorAttachment(target.m_color);

    if (desc.withDepth) {
        nvrhi::TextureDesc depthDesc;
        depthDesc.width = desc.size.x;
        depthDesc.height = desc.size.y;
        depthDesc.format = nvrhi::Format::D32;
        depthDesc.debugName = desc.debugName + " depth";
        depthDesc.isRenderTarget = true;
        depthDesc.initialState = nvrhi::ResourceStates::DepthWrite;
        depthDesc.keepInitialState = true;
        depthDesc.clearValue = nvrhi::Color(0.0f);
        depthDesc.useClearValue = true;
        target.m_depth = device->createTexture(depthDesc);
        if (!target.m_depth) {
            return makeError(ErrorCode::DeviceError, "cannot create the {}x{} depth buffer of '{}'", desc.size.x,
                             desc.size.y, desc.debugName);
        }
        framebufferDesc.setDepthAttachment(target.m_depth);
    }

    target.m_framebuffer = device->createFramebuffer(framebufferDesc);
    if (!target.m_framebuffer) {
        return makeError(ErrorCode::DeviceError, "cannot create the framebuffer of '{}'", desc.debugName);
    }
    return target;
}

void RenderTarget::clear(nvrhi::ICommandList* commandList) const {
    CORDITE_ASSERT(isValid() && commandList != nullptr, "clear needs a valid target and a command list");
    if (!isValid() || commandList == nullptr) {
        return;
    }

    const Vec4& color = m_desc.clearColor;
    commandList->clearTextureFloat(m_color, nvrhi::AllSubresources, nvrhi::Color(color.r, color.g, color.b, color.a));
    if (m_depth) {
        // Reversed-Z: 0 is the far plane.
        const float farDepth = 0.0f;
        commandList->clearDepthStencilTexture(m_depth, nvrhi::AllSubresources, true, farDepth, false, 0);
    }
}

Status RenderTarget::resize(nvrhi::IDevice* device, UVec2 size) {
    if (isValid() && size == m_desc.size) {
        return {};
    }

    RenderTargetDesc desc = m_desc;
    desc.size = size;
    CORDITE_TRY_ASSIGN(RenderTarget replacement, create(device, desc));
    *this = std::move(replacement);
    return {};
}

} // namespace cordite
