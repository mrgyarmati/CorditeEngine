#include "cordite/renderer/SceneTargets.h"

#include "cordite/core/Assert.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <utility>

namespace cordite {
namespace {

/// The largest texture dimension every supported GPU can create.
constexpr uint32_t kMaxDimension = 16384;

constexpr std::array<GBufferTarget, kGBufferTargetCount> kGBufferTargets = {
    GBufferTarget::BaseColor, GBufferTarget::Normal, GBufferTarget::Parameters, GBufferTarget::Emissive};

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

std::string_view nameOf(GBufferTarget target) {
    switch (target) {
        case GBufferTarget::BaseColor:
            return "base color";
        case GBufferTarget::Normal:
            return "normal";
        case GBufferTarget::Parameters:
            return "parameters";
        case GBufferTarget::Emissive:
            return "emissive";
    }
    return "unknown";
}

nvrhi::Format formatOf(GBufferTarget target) {
    switch (target) {
        case GBufferTarget::BaseColor:
            return nvrhi::Format::SRGBA8_UNORM;
        case GBufferTarget::Normal:
            return nvrhi::Format::RG16_UNORM;
        case GBufferTarget::Parameters:
            return nvrhi::Format::RGBA8_UNORM;
        case GBufferTarget::Emissive:
            return nvrhi::Format::R11G11B10_FLOAT;
    }
    return nvrhi::Format::UNKNOWN;
}

Result<SceneTargets> SceneTargets::create(nvrhi::IDevice* device, UVec2 size, std::string debugName) {
    if (size.x == 0 || size.y == 0 || size.x > kMaxDimension || size.y > kMaxDimension) {
        return makeError(ErrorCode::InvalidArgument, "scene targets must be between 1 and {} pixels a side, got {}x{}",
                         kMaxDimension, size.x, size.y);
    }

    SceneTargets targets;
    targets.m_size = size;
    targets.m_debugName = std::move(debugName);
    const std::string& name = targets.m_debugName;

    bool created = true;
    for (const GBufferTarget target : kGBufferTargets) {
        nvrhi::TextureHandle& texture = targets.m_gbuffer[static_cast<size_t>(target)];
        texture = createTexture(device, size, formatOf(target), fmt::format("{} G-buffer {}", name, nameOf(target)),
                                nvrhi::ResourceStates::ShaderResource);
        created = created && texture != nullptr;
    }
    targets.m_sceneColor = createTexture(device, size, nvrhi::Format::RGBA16_FLOAT, name + " scene color",
                                         nvrhi::ResourceStates::ShaderResource);
    targets.m_depth =
        createTexture(device, size, nvrhi::Format::D32, name + " depth", nvrhi::ResourceStates::DepthWrite);
    targets.m_display = createTexture(device, size, nvrhi::Format::RGBA8_UNORM, name + " display",
                                      nvrhi::ResourceStates::ShaderResource);
    targets.m_ambientOcclusion = createTexture(device, size, nvrhi::Format::R8_UNORM, name + " ambient occlusion",
                                               nvrhi::ResourceStates::ShaderResource);
    if (!created || !targets.m_sceneColor || !targets.m_depth || !targets.m_display || !targets.m_ambientOcclusion) {
        return makeError(ErrorCode::DeviceError, "cannot create the {}x{} textures of '{}'", size.x, size.y, name);
    }

    nvrhi::FramebufferDesc gbufferDesc;
    for (const nvrhi::TextureHandle& texture : targets.m_gbuffer) {
        gbufferDesc.addColorAttachment(texture);
    }
    gbufferDesc.setDepthAttachment(targets.m_depth);

    targets.m_depthFramebuffer =
        device->createFramebuffer(nvrhi::FramebufferDesc().setDepthAttachment(targets.m_depth));
    targets.m_gbufferFramebuffer = device->createFramebuffer(gbufferDesc);
    targets.m_ambientOcclusionFramebuffer =
        device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(targets.m_ambientOcclusion));
    targets.m_lightingFramebuffer =
        device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(targets.m_sceneColor));
    targets.m_sceneFramebuffer = device->createFramebuffer(
        nvrhi::FramebufferDesc().addColorAttachment(targets.m_sceneColor).setDepthAttachment(targets.m_depth));
    targets.m_displayFramebuffer =
        device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(targets.m_display));
    targets.m_overlayFramebuffer = device->createFramebuffer(
        nvrhi::FramebufferDesc().addColorAttachment(targets.m_display).setDepthAttachment(targets.m_depth));
    if (!targets.m_depthFramebuffer || !targets.m_gbufferFramebuffer || !targets.m_ambientOcclusionFramebuffer ||
        !targets.m_lightingFramebuffer || !targets.m_sceneFramebuffer || !targets.m_displayFramebuffer ||
        !targets.m_overlayFramebuffer) {
        return makeError(ErrorCode::DeviceError, "cannot create the framebuffers of '{}'", name);
    }

    // The bloom chain: halves, down to one texel.
    UVec2 levelSize = size;
    do {
        levelSize = glm::max(levelSize / 2u, UVec2(1));
        const uint32_t level = targets.m_bloomLevelCount++;
        targets.m_bloom[level] =
            createTexture(device, levelSize, nvrhi::Format::RGBA16_FLOAT, fmt::format("{} bloom {}", name, level),
                          nvrhi::ResourceStates::ShaderResource);
        if (targets.m_bloom[level]) {
            targets.m_bloomFramebuffers[level] =
                device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(targets.m_bloom[level]));
        }
        if (!targets.m_bloomFramebuffers[level]) {
            return makeError(ErrorCode::DeviceError, "cannot create the bloom levels of '{}'", name);
        }
    } while ((levelSize.x > 1 || levelSize.y > 1) && targets.m_bloomLevelCount < kMaxBloomLevels);

    for (size_t index = 0; index < targets.m_exposure.size(); ++index) {
        targets.m_exposure[index] =
            createTexture(device, UVec2(1), nvrhi::Format::R16_FLOAT, fmt::format("{} exposure {}", name, index),
                          nvrhi::ResourceStates::ShaderResource);
        if (targets.m_exposure[index]) {
            targets.m_exposureFramebuffers[index] =
                device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(targets.m_exposure[index]));
        }
        if (!targets.m_exposureFramebuffers[index]) {
            return makeError(ErrorCode::DeviceError, "cannot create the exposure of '{}'", name);
        }
    }

    static std::atomic<uint64_t> nextSerial{1};
    targets.m_serial = nextSerial.fetch_add(1);
    return targets;
}

Status SceneTargets::prepareTracedOcclusion(nvrhi::IDevice* device, uint32_t divisor) {
    CORDITE_ASSERT(isValid() && device != nullptr && divisor >= 1 && divisor <= 4,
                   "the traced occlusion needs valid targets, a device and a divisor from 1 to 4");
    if (!isValid() || device == nullptr || divisor < 1 || divisor > 4) {
        return makeError(ErrorCode::InvalidArgument,
                         "the traced occlusion needs valid targets, a device and a divisor from 1 to 4");
    }
    if (m_tracedOcclusion && m_tracedOcclusionDivisor == divisor) {
        return {};
    }
    releaseTracedOcclusion();

    // A texel for every block of pixels, the last of which may hang over the edge.
    const UVec2 size = (m_size + UVec2(divisor - 1)) / divisor;
    nvrhi::TextureHandle traced =
        createTexture(device, size, nvrhi::Format::RG16_FLOAT, m_debugName + " traced occlusion",
                      nvrhi::ResourceStates::ShaderResource);
    nvrhi::FramebufferHandle framebuffer;
    if (traced) {
        framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(traced));
    }
    if (!framebuffer) {
        return makeError(ErrorCode::DeviceError,
                         "cannot create the {}x{} image the ambient occlusion of '{}' is "
                         "worked out in",
                         size.x, size.y, m_debugName);
    }
    m_tracedOcclusion = std::move(traced);
    m_tracedOcclusionFramebuffer = std::move(framebuffer);
    m_tracedOcclusionDivisor = divisor;
    return {};
}

void SceneTargets::releaseTracedOcclusion() {
    // What a command list on its way still draws into is kept alive by that list.
    m_tracedOcclusionFramebuffer = nullptr;
    m_tracedOcclusion = nullptr;
    m_tracedOcclusionDivisor = 0;
}

uint64_t SceneTargets::tracedOcclusionBytes() const {
    if (!m_tracedOcclusion) {
        return 0;
    }
    // Two half floats a texel.
    const nvrhi::TextureDesc& desc = m_tracedOcclusion->getDesc();
    return uint64_t{desc.width} * desc.height * 4;
}

Status SceneTargets::prepareAliased(nvrhi::IDevice* device) {
    CORDITE_ASSERT(isValid() && device != nullptr, "the picture before anti-aliasing needs valid targets and a device");
    if (!isValid() || device == nullptr) {
        return makeError(ErrorCode::InvalidArgument,
                         "the picture before anti-aliasing needs valid targets and a device");
    }
    if (m_aliased) {
        return {};
    }
    nvrhi::TextureHandle aliased =
        createTexture(device, m_size, nvrhi::Format::RGBA8_UNORM, m_debugName + " before anti-aliasing",
                      nvrhi::ResourceStates::ShaderResource);
    nvrhi::FramebufferHandle framebuffer;
    if (aliased) {
        framebuffer = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(aliased));
    }
    if (!framebuffer) {
        return makeError(ErrorCode::DeviceError,
                         "cannot create the {}x{} image the picture of '{}' is in before its edges are smoothed",
                         m_size.x, m_size.y, m_debugName);
    }
    m_aliased = std::move(aliased);
    m_aliasedFramebuffer = std::move(framebuffer);
    return {};
}

void SceneTargets::releaseAliased() {
    // What a command list on its way still draws into is kept alive by that list.
    m_aliasedFramebuffer = nullptr;
    m_aliased = nullptr;
}

uint64_t SceneTargets::aliasedBytes() const {
    // Four bytes a pixel.
    return m_aliased ? uint64_t{m_size.x} * m_size.y * 4 : 0;
}

Status SceneTargets::prepareDepthOfField(nvrhi::IDevice* device) {
    CORDITE_ASSERT(isValid() && device != nullptr, "depth of field needs valid targets and a device");
    if (!isValid() || device == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "depth of field needs valid targets and a device");
    }
    if (m_depthOfFieldSource && m_depthOfFieldBlur) {
        return {};
    }
    releaseDepthOfField();

    // A texel for every block of four pixels, the last of which may hang over the edge.
    const UVec2 size = (m_size + UVec2(1)) / 2u;
    std::array<nvrhi::TextureHandle, 2> images;
    std::array<nvrhi::FramebufferHandle, 2> framebuffers;
    for (size_t index = 0; index < images.size(); ++index) {
        images[index] = createTexture(device, size, nvrhi::Format::RGBA16_FLOAT,
                                      m_debugName + (index == 0 ? " depth of field source" : " depth of field blur"),
                                      nvrhi::ResourceStates::ShaderResource);
        if (images[index]) {
            framebuffers[index] = device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(images[index]));
        }
        if (!framebuffers[index]) {
            return makeError(ErrorCode::DeviceError,
                             "cannot create the {}x{} images the depth of field of '{}' is "
                             "worked out in",
                             size.x, size.y, m_debugName);
        }
    }
    m_depthOfFieldSource = std::move(images[0]);
    m_depthOfFieldBlur = std::move(images[1]);
    m_depthOfFieldSourceFramebuffer = std::move(framebuffers[0]);
    m_depthOfFieldBlurFramebuffer = std::move(framebuffers[1]);
    return {};
}

void SceneTargets::releaseDepthOfField() {
    // What a command list on its way still draws into is kept alive by that list.
    m_depthOfFieldSourceFramebuffer = nullptr;
    m_depthOfFieldBlurFramebuffer = nullptr;
    m_depthOfFieldSource = nullptr;
    m_depthOfFieldBlur = nullptr;
}

uint64_t SceneTargets::depthOfFieldBytes() const {
    if (!m_depthOfFieldSource || !m_depthOfFieldBlur) {
        return 0;
    }
    // Two images of four half floats a texel.
    const nvrhi::TextureDesc& desc = m_depthOfFieldSource->getDesc();
    return uint64_t{desc.width} * desc.height * 8 * 2;
}

nvrhi::ITexture* SceneTargets::bloomLevel(uint32_t level) const {
    CORDITE_ASSERT(level < m_bloomLevelCount, "there is no bloom level {}", level);
    return level < m_bloomLevelCount ? m_bloom[level].Get() : nullptr;
}

nvrhi::IFramebuffer* SceneTargets::bloomFramebuffer(uint32_t level) const {
    CORDITE_ASSERT(level < m_bloomLevelCount, "there is no bloom level {}", level);
    return level < m_bloomLevelCount ? m_bloomFramebuffers[level].Get() : nullptr;
}

void SceneTargets::clear(nvrhi::ICommandList* commandList, const Vec3& background) const {
    CORDITE_ASSERT(isValid() && commandList != nullptr, "clear needs valid targets and a command list");
    if (!isValid() || commandList == nullptr) {
        return;
    }
    // The G-buffer is only ever read where something was drawn, but a pixel that was
    // not written although it should have been then shows as black rather than as
    // whatever the last frame left there.
    for (const nvrhi::TextureHandle& texture : m_gbuffer) {
        commandList->clearTextureFloat(texture, nvrhi::AllSubresources, nvrhi::Color(0.0f));
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
