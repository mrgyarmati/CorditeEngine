#include "cordite/renderer/MotionBlurPass.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"
#include "cordite/renderer/MotionBlur.h"
#include "cordite/renderer/ViewConstants.h"

#include <nvrhi/utils.h>

#include <array>
#include <string_view>
#include <utility>

namespace cordite {
namespace {

/// Mirrors MotionBlurConstants in shaders/passes/MotionBlur.slang.
struct MotionBlurConstants {
    float amount;
    uint32_t radius;
    uint32_t sampleCount;
    float padding0;
};

static_assert(sizeof(MotionBlurConstants) == 16, "must match the cbuffer layout in MotionBlur.slang");

/// The register space of the set: the view's bindings are in space 0.
constexpr uint32_t kRegisterSpace = 1;

} // namespace

MotionBlurPass::MotionBlurPass(Parts parts)
    : m_device(parts.device),
      m_constants(std::move(parts.constants)),
      m_layout(std::move(parts.layout)),
      m_tiles(std::move(parts.tiles)),
      m_neighbours(std::move(parts.neighbours)),
      m_blur(std::move(parts.blur)) {}

Result<MotionBlurPass> MotionBlurPass::create(nvrhi::IDevice* device, ShaderManager& shaders,
                                              nvrhi::IBindingLayout* viewLayout) {
    CORDITE_ASSERT(device != nullptr && viewLayout != nullptr, "motion blur needs a device and the view layout");
    if (device == nullptr || viewLayout == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "motion blur needs a device and the view layout");
    }

    nvrhi::BufferHandle constants = device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(
        sizeof(MotionBlurConstants), "MotionBlurConstants", kMaxViewsInFlight));
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Pixel;
    layoutDesc.registerSpace = kRegisterSpace;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
                           nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Texture_SRV(1),
                           nvrhi::BindingLayoutItem::Texture_SRV(2), nvrhi::BindingLayoutItem::Texture_SRV(3)};
    nvrhi::BindingLayoutHandle layout = device->createBindingLayout(layoutDesc);
    if (!constants || !layout) {
        return makeError(ErrorCode::DeviceError, "cannot create the resources of the motion blur");
    }

    const auto pass = [&](const char* name, const char* entryPoint) {
        FullscreenPassDesc desc;
        desc.debugName = name;
        desc.pixelShader = {.path = "passes/MotionBlur.slang", .entryPoint = entryPoint, .stage = ShaderStage::Pixel};
        desc.bindingLayouts = {nvrhi::BindingLayoutHandle(viewLayout), layout};
        return FullscreenPass::create(device, shaders, std::move(desc));
    };
    CORDITE_TRY_ASSIGN(FullscreenPass tiles, pass("Motion blur tiles", "motionBlurTilesPixel"));
    CORDITE_TRY_ASSIGN(FullscreenPass neighbours, pass("Motion blur neighbours", "motionBlurNeighboursPixel"));
    CORDITE_TRY_ASSIGN(FullscreenPass blur, pass("Motion blur", "motionBlurPixel"));

    return MotionBlurPass(Parts{.device = device,
                                .constants = std::move(constants),
                                .layout = std::move(layout),
                                .tiles = std::move(tiles),
                                .neighbours = std::move(neighbours),
                                .blur = std::move(blur)});
}

nvrhi::BindingSetHandle MotionBlurPass::bindingsFor(nvrhi::ITexture* tiles, const SceneTargets& targets) const {
    nvrhi::BindingSetDesc desc;
    desc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, m_constants),
                     nvrhi::BindingSetItem::Texture_SRV(0, tiles),
                     nvrhi::BindingSetItem::Texture_SRV(1, targets.motionBlurSource()),
                     nvrhi::BindingSetItem::Texture_SRV(2, targets.depth()),
                     nvrhi::BindingSetItem::Texture_SRV(3, targets.motion())};
    return m_device->createBindingSet(desc, m_layout);
}

uint32_t MotionBlurPass::draw(nvrhi::ICommandList* commandList, SceneTargets& targets, nvrhi::IBindingSet* viewBindings,
                              const PostProcessSettings& settings, const MotionBlurFrame& frame) {
    CORDITE_ASSERT(commandList != nullptr && targets.isValid() && viewBindings != nullptr,
                   "motion blur needs a command list, valid targets and the view");
    if (commandList == nullptr || !targets.isValid() || viewBindings == nullptr) {
        return 0;
    }
    const auto fail = [this](std::string_view why) {
        if (!m_reportedFailure) {
            CORDITE_LOG_ERROR("Renderer", "no motion blur is drawn: {}", why);
            m_reportedFailure = true;
        }
        return 0u;
    };
    if (const Status prepared = targets.prepareMotionBlur(m_device); !prepared) {
        return fail(prepared.error().describe());
    }
    // Binding sets are cheap, and the command list keeps them alive until it has run. A
    // pass reads none of the image it draws into: the first reads the tiles of the second
    // - and does nothing with them - the second those of the first, the third those of
    // the second.
    const nvrhi::BindingSetHandle tileBindings = bindingsFor(targets.motionBlurTiles(1), targets);
    const nvrhi::BindingSetHandle neighbourBindings = bindingsFor(targets.motionBlurTiles(0), targets);
    const nvrhi::BindingSetHandle blurBindings = bindingsFor(targets.motionBlurTiles(1), targets);
    if (!tileBindings || !neighbourBindings || !blurBindings) {
        return fail("its textures cannot be bound");
    }

    const MotionBlurConstants constants{
        .amount = sanitized(settings).motionBlur,
        .radius = motionBlurRadius(targets.size().y),
        .sampleCount = postProcessSettingsFor(frame.quality).motionBlurSamples,
        .padding0 = 0.0f,
    };
    commandList->writeBuffer(m_constants, &constants, sizeof(constants));

    // The picture as it is, for the last pass to read while it draws over it.
    commandList->copyTexture(targets.motionBlurSource(), nvrhi::TextureSlice(), targets.sceneColor(),
                             nvrhi::TextureSlice());
    const std::array<nvrhi::IBindingSet*, 2> tileSets = {viewBindings, tileBindings.Get()};
    m_tiles.draw(commandList, targets.motionBlurTileFramebuffer(0), tileSets);
    const std::array<nvrhi::IBindingSet*, 2> neighbourSets = {viewBindings, neighbourBindings.Get()};
    m_neighbours.draw(commandList, targets.motionBlurTileFramebuffer(1), neighbourSets);
    const std::array<nvrhi::IBindingSet*, 2> blurSets = {viewBindings, blurBindings.Get()};
    m_blur.draw(commandList, targets.lightingFramebuffer(), blurSets);
    return 3;
}

} // namespace cordite
