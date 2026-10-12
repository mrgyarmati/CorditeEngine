#include "cordite/renderer/PostProcessPass.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"
#include "cordite/math/Math.h"
#include "cordite/renderer/ViewConstants.h"

#include <nvrhi/utils.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace cordite {
namespace {

/// Mirrors PostProcessConstants in shaders/common/PostProcess.slang.
struct PostProcessConstants {
    float exposureScale;
    float autoExposure;
    float bloomIntensity;
    uint32_t toneMapper;

    Vec3 whiteBalance;
    float contrast;

    Vec3 lift;
    float saturation;

    Vec3 inverseGamma;
    float vignette;

    Vec3 gain;
    float chromaticAberration;

    float filmGrain;
    uint32_t grainSeed;
    float colorLutIntensity;
    float colorLutSize;

    float bloomThreshold;
    float bloomScatter;
    float dither;
    float instantAdaptation;

    float exposureMinimum;
    float exposureMaximum;
    float adaptationUp;
    float adaptationDown;

    float antiAliased;
    float antiAliasingEdgeThreshold;
    float antiAliasingEdgeMinimum;
    uint32_t antiAliasingSteps;
};

static_assert(sizeof(PostProcessConstants) == 144, "must match the cbuffer layout in PostProcess.slang");

// The shader tells the tone mappers apart by these numbers.
static_assert(static_cast<uint32_t>(ToneMapper::Filmic) == 0 && static_cast<uint32_t>(ToneMapper::Aces) == 1 &&
                  static_cast<uint32_t>(ToneMapper::Linear) == 2,
              "must match the kToneMapper constants in ToneMapping.slang");

/// What the settings for chromatic aberration and film grain are a share of: how far
/// red and blue part in the corners, as a share of the picture's size, and how much
/// grain changes a color.
constexpr float kMaxAberration = 0.01f;
constexpr float kMaxGrain = 0.1f;

/// Film grain changes this often, whatever the frame rate: as often as film does.
constexpr double kGrainFramesPerSecond = 24.0;

/// Binding sets of targets that were not drawn for this many calls are dropped: the
/// targets are gone, or were replaced when their view changed its size.
constexpr uint64_t kUnusedCalls = 64;

FullscreenPassDesc passDesc(const char* name, const char* path, const char* entryPoint,
                            const nvrhi::BindingLayoutHandle& layout) {
    FullscreenPassDesc desc;
    desc.debugName = name;
    desc.pixelShader = {.path = path, .entryPoint = entryPoint, .stage = ShaderStage::Pixel};
    desc.bindingLayouts = {layout};
    return desc;
}

/// How many slices a lookup table has, or 0 when the texture is not one: N slices of N
/// by N texels side by side.
uint32_t lutSizeOf(nvrhi::ITexture* texture) {
    if (texture == nullptr) {
        return 0;
    }
    const nvrhi::TextureDesc& desc = texture->getDesc();
    const uint64_t size = desc.height;
    return size >= 2 && size * size == desc.width ? desc.height : 0;
}

/// Draws one pass with one binding set. Returns 1 when it was drawn and 0 when there was
/// nothing to draw it with: a binding set that cannot be made is the device's to
/// report, and the pass is left out.
uint32_t drawPass(nvrhi::ICommandList* commandList, FullscreenPass& pass, nvrhi::IFramebuffer* framebuffer,
                  nvrhi::IBindingSet* bindings) {
    if (bindings == nullptr || framebuffer == nullptr) {
        return 0;
    }
    const std::array<nvrhi::IBindingSet*, 1> bindingSets = {bindings};
    pass.draw(commandList, framebuffer, bindingSets);
    return 1;
}

/// What the grain is seeded with at a time: a number that changes as often as film
/// does, and wraps around after years, which the grain does not mind. A time that is
/// none is the beginning.
uint32_t grainSeedAt(double timeSeconds) {
    constexpr double kMostTicks = 4.0e18;
    const double ticks =
        std::isfinite(timeSeconds) ? std::clamp(timeSeconds * kGrainFramesPerSecond, 0.0, kMostTicks) : 0.0;
    return static_cast<uint32_t>(static_cast<uint64_t>(ticks) & 0xFFFFFFFFu);
}

/// The share of the way to a new exposure that is covered in `seconds` at `speed`.
float adaptationShare(float seconds, float speed) {
    return 1.0f - std::exp(-std::max(seconds, 0.0f) * speed);
}

} // namespace

PostProcessPass::PostProcessPass(Parts parts)
    : m_device(parts.device),
      m_constants(std::move(parts.constants)),
      m_sampler(std::move(parts.sampler)),
      m_noLut(std::move(parts.noLut)),
      m_sampleLayout(std::move(parts.sampleLayout)),
      m_exposureLayout(std::move(parts.exposureLayout)),
      m_finalLayout(std::move(parts.finalLayout)),
      m_prefilter(std::move(parts.prefilter)),
      m_downsample(std::move(parts.downsample)),
      m_upsample(std::move(parts.upsample)),
      m_exposure(std::move(parts.exposure)),
      m_final(std::move(parts.final)),
      m_antiAliasing(std::move(parts.antiAliasing)) {}

Result<PostProcessPass> PostProcessPass::create(nvrhi::IDevice* device, ShaderManager& shaders) {
    CORDITE_ASSERT(device != nullptr, "the post-processing pass needs a device");

    nvrhi::BufferHandle constants = device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(
        sizeof(PostProcessConstants), "PostProcessConstants", kMaxViewsInFlight));
    nvrhi::SamplerHandle sampler =
        device->createSampler(nvrhi::SamplerDesc().setAllFilters(true).setMipFilter(false).setAllAddressModes(
            nvrhi::SamplerAddressMode::Clamp));

    nvrhi::TextureDesc noLutDesc;
    noLutDesc.width = 1;
    noLutDesc.height = 1;
    noLutDesc.format = nvrhi::Format::RGBA8_UNORM;
    noLutDesc.debugName = "No color lookup table";
    noLutDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    noLutDesc.keepInitialState = true;
    nvrhi::TextureHandle noLut = device->createTexture(noLutDesc);

    // One texture and the sampler: the bloom passes, and the anti-aliasing.
    nvrhi::BindingLayoutDesc sampleDesc;
    sampleDesc.visibility = nvrhi::ShaderType::Pixel;
    sampleDesc.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
                           nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Sampler(0)};
    nvrhi::BindingLayoutHandle sampleLayout = device->createBindingLayout(sampleDesc);

    nvrhi::BindingLayoutDesc exposureDesc;
    exposureDesc.visibility = nvrhi::ShaderType::Pixel;
    exposureDesc.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
                             nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Texture_SRV(1)};
    nvrhi::BindingLayoutHandle exposureLayout = device->createBindingLayout(exposureDesc);

    nvrhi::BindingLayoutDesc finalDesc;
    finalDesc.visibility = nvrhi::ShaderType::Pixel;
    finalDesc.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
                          nvrhi::BindingLayoutItem::Texture_SRV(0),
                          nvrhi::BindingLayoutItem::Texture_SRV(1),
                          nvrhi::BindingLayoutItem::Texture_SRV(2),
                          nvrhi::BindingLayoutItem::Texture_SRV(3),
                          nvrhi::BindingLayoutItem::Sampler(0)};
    nvrhi::BindingLayoutHandle finalLayout = device->createBindingLayout(finalDesc);

    if (!constants || !sampler || !noLut || !sampleLayout || !exposureLayout || !finalLayout) {
        return makeError(ErrorCode::DeviceError, "cannot create the resources of the post-processing pass");
    }

    CORDITE_TRY_ASSIGN(
        FullscreenPass prefilter,
        FullscreenPass::create(device, shaders,
                               passDesc("Bloom prefilter", "passes/Bloom.slang", "bloomPrefilterPixel", sampleLayout)));
    CORDITE_TRY_ASSIGN(
        FullscreenPass downsample,
        FullscreenPass::create(
            device, shaders, passDesc("Bloom downsample", "passes/Bloom.slang", "bloomDownsamplePixel", sampleLayout)));
    // What the upsampling writes is blended with what its target holds, by the alpha
    // the shader gives it; the target's own alpha stays.
    FullscreenPassDesc upsampleDesc =
        passDesc("Bloom upsample", "passes/Bloom.slang", "bloomUpsamplePixel", sampleLayout);
    upsampleDesc.blend.blendEnable = true;
    upsampleDesc.blend.srcBlend = nvrhi::BlendFactor::SrcAlpha;
    upsampleDesc.blend.destBlend = nvrhi::BlendFactor::InvSrcAlpha;
    upsampleDesc.blend.colorWriteMask = nvrhi::ColorMask::Red | nvrhi::ColorMask::Green | nvrhi::ColorMask::Blue;
    CORDITE_TRY_ASSIGN(FullscreenPass upsample, FullscreenPass::create(device, shaders, std::move(upsampleDesc)));
    CORDITE_TRY_ASSIGN(FullscreenPass exposure, FullscreenPass::create(device, shaders,
                                                                       passDesc("Exposure", "passes/Exposure.slang",
                                                                                "exposureMeterPixel", exposureLayout)));
    CORDITE_TRY_ASSIGN(
        FullscreenPass final,
        FullscreenPass::create(
            device, shaders, passDesc("Post-processing", "passes/PostProcess.slang", "postProcessPixel", finalLayout)));
    CORDITE_TRY_ASSIGN(FullscreenPass antiAliasing,
                       FullscreenPass::create(
                           device, shaders,
                           passDesc("Anti-aliasing", "passes/AntiAliasing.slang", "antiAliasingPixel", sampleLayout)));

    return PostProcessPass(Parts{.device = device,
                                 .constants = std::move(constants),
                                 .sampler = std::move(sampler),
                                 .noLut = std::move(noLut),
                                 .sampleLayout = std::move(sampleLayout),
                                 .exposureLayout = std::move(exposureLayout),
                                 .finalLayout = std::move(finalLayout),
                                 .prefilter = std::move(prefilter),
                                 .downsample = std::move(downsample),
                                 .upsample = std::move(upsample),
                                 .exposure = std::move(exposure),
                                 .final = std::move(final),
                                 .antiAliasing = std::move(antiAliasing)});
}

PostProcessPass::ViewCache& PostProcessPass::cacheFor(const SceneTargets& targets) {
    ++m_drawCount;
    std::erase_if(m_views, [&](const ViewCache& view) {
        return view.serial != targets.serial() && m_drawCount - view.lastUsed > kUnusedCalls;
    });
    auto found = std::ranges::find_if(m_views, [&](const ViewCache& view) { return view.serial == targets.serial(); });
    if (found == m_views.end()) {
        m_views.emplace_back();
        found = m_views.end() - 1;
        found->serial = targets.serial();
    }
    found->lastUsed = m_drawCount;
    return *found;
}

nvrhi::IBindingSet* PostProcessPass::sampleBindings(nvrhi::BindingSetHandle& set, nvrhi::ITexture* texture) {
    if (!set) {
        nvrhi::BindingSetDesc desc;
        desc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, m_constants),
                         nvrhi::BindingSetItem::Texture_SRV(0, texture), nvrhi::BindingSetItem::Sampler(0, m_sampler)};
        set = m_device->createBindingSet(desc, m_sampleLayout);
    }
    return set;
}

nvrhi::IBindingSet* PostProcessPass::antiAliasingBindings(ViewCache& view, SceneTargets& targets) {
    Status prepared = targets.prepareAliased(m_device);
    if (prepared && (!view.antiAliasing || view.aliased != targets.aliased())) {
        nvrhi::BindingSetDesc desc;
        desc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, m_constants),
                         nvrhi::BindingSetItem::Texture_SRV(0, targets.aliased()),
                         nvrhi::BindingSetItem::Sampler(0, m_sampler)};
        view.antiAliasing = m_device->createBindingSet(desc, m_sampleLayout);
        view.aliased = targets.aliased();
        if (!view.antiAliasing) {
            prepared = makeError(ErrorCode::DeviceError, "cannot bind what the anti-aliasing reads");
        }
    }
    if (!prepared) {
        if (!m_reportedAntiAliasing) {
            CORDITE_LOG_WARN("Renderer", "the picture is finished without anti-aliasing: {}", prepared.error().message);
            m_reportedAntiAliasing = true;
        }
        return nullptr;
    }
    return view.antiAliasing;
}

uint32_t PostProcessPass::draw(nvrhi::ICommandList* commandList, SceneTargets& targets,
                               const PostProcessSettings& settings, const PostProcessFrame& frame) {
    CORDITE_ASSERT(commandList != nullptr && targets.isValid(),
                   "post-processing needs a command list and valid targets");
    if (commandList == nullptr || !targets.isValid()) {
        return 0;
    }
    uint32_t passes = 0;

    const PostProcessSettings look = sanitized(settings);
    const PostProcessQualitySettings quality = postProcessSettingsFor(frame.quality);
    // The levels of the bloom chain that this quality fills, as far as the view has them.
    const uint32_t firstLevel = std::min(quality.bloomFirstLevel, targets.bloomLevelCount() - 1);
    const uint32_t lastLevel = std::min(firstLevel + std::max(quality.bloomLevels, 1u), targets.bloomLevelCount()) - 1;
    const bool automatic = look.exposureMode == ExposureMode::Automatic;
    const bool blooms = look.bloomIntensity > 0.0f;

    const uint32_t lutSize = lutSizeOf(frame.colorLut);
    if (frame.colorLut != nullptr && lutSize == 0 && !m_reportedLut) {
        const nvrhi::TextureDesc& desc = frame.colorLut->getDesc();
        CORDITE_LOG_WARN("Renderer",
                         "a color lookup table of {}x{} pixels is left out: it must be N * N pixels wide and N "
                         "high, such as 256x16",
                         desc.width, desc.height);
        m_reportedLut = true;
    }
    nvrhi::ITexture* lut = lutSize > 0 ? frame.colorLut : m_noLut.Get();

    // A picture whose edges are smoothed is finished into an image of its own first.
    // A view that is not to have that, or cannot, gives the image back.
    ViewCache& view = cacheFor(targets);
    nvrhi::IBindingSet* smoothing =
        frame.antiAliasing == AntiAliasing::Fxaa ? antiAliasingBindings(view, targets) : nullptr;
    if (smoothing == nullptr) {
        view.antiAliasing = nullptr;
        view.aliased = nullptr;
        targets.releaseAliased();
    }

    PostProcessConstants constants{};
    constants.exposureScale = exposureScale(look.exposure);
    constants.autoExposure = automatic ? 1.0f : 0.0f;
    constants.bloomIntensity = look.bloomIntensity;
    constants.toneMapper = static_cast<uint32_t>(look.toneMapper);
    constants.whiteBalance = whiteBalanceScale(look.temperature, look.tint);
    constants.contrast = look.contrast;
    constants.lift = look.lift;
    constants.saturation = look.saturation;
    constants.inverseGamma = Vec3(1.0f) / look.gamma;
    constants.vignette = look.vignette;
    constants.gain = look.gain;
    constants.chromaticAberration = quality.lensEffects ? look.chromaticAberration * kMaxAberration : 0.0f;
    constants.filmGrain = quality.lensEffects ? look.filmGrain * kMaxGrain : 0.0f;
    constants.grainSeed = grainSeedAt(frame.timeSeconds);
    constants.colorLutIntensity = lutSize > 0 ? look.colorLutIntensity : 0.0f;
    constants.colorLutSize = static_cast<float>(lutSize);
    constants.bloomThreshold = look.bloomThreshold;
    constants.bloomScatter = look.bloomScatter;
    constants.dither = 1.0f;
    constants.instantAdaptation = targets.hasExposure() ? 0.0f : 1.0f;
    constants.exposureMinimum = look.autoExposureMinimum;
    constants.exposureMaximum = look.autoExposureMaximum;
    const float seconds = std::isfinite(frame.deltaSeconds) ? frame.deltaSeconds : 0.0f;
    constants.adaptationUp = adaptationShare(seconds, look.adaptationSpeedUp);
    constants.adaptationDown = adaptationShare(seconds, look.adaptationSpeedDown);
    constants.antiAliased = smoothing != nullptr ? 1.0f : 0.0f;
    constants.antiAliasingEdgeThreshold = quality.antiAliasingEdgeThreshold;
    constants.antiAliasingEdgeMinimum = quality.antiAliasingEdgeMinimum;
    constants.antiAliasingSteps = quality.antiAliasingSteps;
    commandList->writeBuffer(m_constants, &constants, sizeof(constants));

    // The bloom chain, down: needed for the bloom, and for measuring how bright the
    // picture is.
    if (blooms || automatic) {
        passes += drawPass(commandList, m_prefilter, targets.bloomFramebuffer(firstLevel),
                           sampleBindings(view.sceneColor, targets.sceneColor()));
        for (uint32_t level = firstLevel; level < lastLevel; ++level) {
            passes += drawPass(commandList, m_downsample, targets.bloomFramebuffer(level + 1),
                               sampleBindings(view.bloomLevels[level], targets.bloomLevel(level)));
        }
    }

    if (automatic) {
        // From the smallest level and the exposure of the frame before to this frame's.
        nvrhi::BindingSetHandle& set = view.exposure[lastLevel][targets.exposureIndex()];
        if (!set) {
            nvrhi::BindingSetDesc desc;
            desc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, m_constants),
                             nvrhi::BindingSetItem::Texture_SRV(0, targets.bloomLevel(lastLevel)),
                             nvrhi::BindingSetItem::Texture_SRV(1, targets.exposure())};
            set = m_device->createBindingSet(desc, m_exposureLayout);
        }
        passes += drawPass(commandList, m_exposure, targets.nextExposureFramebuffer(), set);
        targets.advanceExposure();
    } else {
        // A view that sets its exposure by hand has none to get used to; when it
        // measures again, it starts afresh.
        targets.forgetExposure();
    }

    // The bloom chain, up again: each level into the one above it.
    if (blooms) {
        for (uint32_t level = lastLevel; level > firstLevel; --level) {
            passes += drawPass(commandList, m_upsample, targets.bloomFramebuffer(level - 1),
                               sampleBindings(view.bloomLevels[level], targets.bloomLevel(level)));
        }
    }

    const uint32_t current = targets.exposureIndex();
    nvrhi::BindingSetHandle& finalSet = view.final[firstLevel][current];
    if (!finalSet || view.finalLut[firstLevel][current] != lut) {
        nvrhi::BindingSetDesc desc;
        desc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, m_constants),
                         nvrhi::BindingSetItem::Texture_SRV(0, targets.sceneColor()),
                         nvrhi::BindingSetItem::Texture_SRV(1, targets.bloomLevel(firstLevel)),
                         nvrhi::BindingSetItem::Texture_SRV(2, targets.exposure()),
                         nvrhi::BindingSetItem::Texture_SRV(3, lut),
                         nvrhi::BindingSetItem::Sampler(0, m_sampler)};
        finalSet = m_device->createBindingSet(desc, m_finalLayout);
        view.finalLut[firstLevel][current] = lut;
    }
    if (!finalSet) {
        CORDITE_LOG_ERROR("Renderer", "cannot bind what the post-processing reads; the image is not updated");
        return passes;
    }
    if (smoothing == nullptr) {
        return passes + drawPass(commandList, m_final, targets.displayFramebuffer(), finalSet);
    }
    passes += drawPass(commandList, m_final, targets.aliasedFramebuffer(), finalSet);
    return passes + drawPass(commandList, m_antiAliasing, targets.displayFramebuffer(), smoothing);
}

} // namespace cordite
