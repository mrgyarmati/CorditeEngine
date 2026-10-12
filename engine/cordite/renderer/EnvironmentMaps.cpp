#include "cordite/renderer/EnvironmentMaps.h"

#include "cordite/assets/Image.h"
#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"
#include "cordite/renderer/EnvironmentLight.h"

#include <nvrhi/utils.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <utility>

namespace cordite {
namespace {

/// Mirrors EnvironmentPrefilterConstants in shaders/passes/EnvironmentPrefilter.slang.
struct EnvironmentPrefilterConstants {
    uint32_t face;
    float roughness;
    float sourceLevel;
    float sourceDensity;
    uint32_t sampleCount;
    float padding0;
    float padding1;
    float padding2;
};

static_assert(sizeof(EnvironmentPrefilterConstants) == 32,
              "must match the cbuffer layout in EnvironmentPrefilter.slang");

constexpr uint32_t kFaceCount = 6;

/// How often the image is read for one texel of a level of the cube map. Each level has
/// a quarter of the texels of the one before, so it can read four times as often for
/// the same work - up to a number that leaves the wide blur of a rough surface with
/// next to none of the extra blur that few reads cost.
constexpr uint32_t sampleCountOf(uint32_t level) {
    if (level <= 1) {
        return 64;
    }
    return level == 2 ? 256 : 1024;
}

/// The constants are rewritten for every face of every level of a cube map that is made,
/// and several can be made in the frames that are on their way through the GPU.
constexpr uint32_t kMaxConstantVersions = kFaceCount * kEnvironmentLevels * 8;

nvrhi::TextureDesc cubeDesc(uint32_t faceSize, uint32_t levels, const std::string& name) {
    nvrhi::TextureDesc desc;
    desc.dimension = nvrhi::TextureDimension::TextureCube;
    desc.width = faceSize;
    desc.height = faceSize;
    desc.arraySize = kFaceCount;
    desc.mipLevels = levels;
    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.debugName = name;
    desc.isRenderTarget = true;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    return desc;
}

} // namespace

EnvironmentMaps::EnvironmentMaps(Parts parts)
    : m_device(parts.device),
      m_assets(parts.assets),
      m_constants(std::move(parts.constants)),
      m_sampler(std::move(parts.sampler)),
      m_layout(std::move(parts.layout)),
      m_prefilter(std::move(parts.prefilter)),
      m_none(std::move(parts.none)) {}

Result<EnvironmentMaps> EnvironmentMaps::create(nvrhi::IDevice* device, ShaderManager& shaders, AssetManager& assets) {
    CORDITE_ASSERT(device != nullptr, "environment maps need a device");
    if (device == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "environment maps need a device");
    }

    nvrhi::BufferHandle constants = device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(
        sizeof(EnvironmentPrefilterConstants), "EnvironmentPrefilterConstants", kMaxConstantVersions));
    // The image wraps around where its left and right edges meet, and ends at its poles.
    nvrhi::SamplerHandle sampler = device->createSampler(nvrhi::SamplerDesc()
                                                             .setAllFilters(true)
                                                             .setAddressU(nvrhi::SamplerAddressMode::Wrap)
                                                             .setAddressV(nvrhi::SamplerAddressMode::Clamp)
                                                             .setAddressW(nvrhi::SamplerAddressMode::Clamp));
    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::Pixel;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
                           nvrhi::BindingLayoutItem::Texture_SRV(0), nvrhi::BindingLayoutItem::Sampler(0)};
    nvrhi::BindingLayoutHandle layout = device->createBindingLayout(layoutDesc);

    // No environment: a cube of one black texel a face, and an image of one.
    GpuEnvironment none;
    none.radiance = device->createTexture(cubeDesc(1, 1, "No environment"));
    nvrhi::TextureDesc skyDesc;
    skyDesc.width = 1;
    skyDesc.height = 1;
    skyDesc.format = nvrhi::Format::RGBA16_FLOAT;
    skyDesc.debugName = "No sky";
    skyDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    skyDesc.keepInitialState = true;
    none.sky = device->createTexture(skyDesc);
    if (!constants || !sampler || !layout || !none.radiance || !none.sky) {
        return makeError(ErrorCode::DeviceError, "cannot create the resources of the environment maps");
    }
    // Black, by a command list of its own: this may be called while a frame is being
    // recorded, and only one command list that executes at once can be open at a time.
    // Written, not cleared: a command list keeps alive what it writes to, but not what
    // it only clears - and these may go with a renderer that is taken down again
    // before the GPU has got round to the list.
    const nvrhi::CommandListHandle commandList =
        device->createCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false));
    if (!commandList) {
        return makeError(ErrorCode::DeviceError, "cannot create a command list for the environment maps");
    }
    // One texel of four half floats: no light.
    constexpr std::array<uint16_t, 4> kBlack = {0, 0, 0, 0};
    commandList->open();
    for (uint32_t face = 0; face < kFaceCount; ++face) {
        commandList->writeTexture(none.radiance, face, 0, kBlack.data(), sizeof(kBlack));
    }
    commandList->writeTexture(none.sky, 0, 0, kBlack.data(), sizeof(kBlack));
    commandList->close();
    device->executeCommandList(commandList);

    FullscreenPassDesc passDesc;
    passDesc.debugName = "Environment prefilter";
    passDesc.pixelShader = {.path = "passes/EnvironmentPrefilter.slang",
                            .entryPoint = "environmentPrefilterPixel",
                            .stage = ShaderStage::Pixel};
    passDesc.bindingLayouts = {layout};
    CORDITE_TRY_ASSIGN(FullscreenPass prefilter, FullscreenPass::create(device, shaders, std::move(passDesc)));

    return EnvironmentMaps(Parts{.device = device,
                                 .assets = &assets,
                                 .constants = std::move(constants),
                                 .sampler = std::move(sampler),
                                 .layout = std::move(layout),
                                 .prefilter = std::move(prefilter),
                                 .none = std::move(none)});
}

uint32_t EnvironmentMaps::count() const {
    return static_cast<uint32_t>(std::ranges::count_if(m_entries, &Entry::usable));
}

bool EnvironmentMaps::bake(nvrhi::ICommandList* commandList, nvrhi::ITexture* source, nvrhi::ITexture* cube) {
    nvrhi::BindingSetDesc setDesc;
    setDesc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, m_constants),
                        nvrhi::BindingSetItem::Texture_SRV(0, source), nvrhi::BindingSetItem::Sampler(0, m_sampler)};
    const nvrhi::BindingSetHandle bindings = m_device->createBindingSet(setDesc, m_layout);
    if (!bindings) {
        return false;
    }

    const nvrhi::TextureDesc& sourceDesc = source->getDesc();
    const float sourcePixels = static_cast<float>(sourceDesc.width) * static_cast<float>(sourceDesc.height);
    for (uint32_t level = 0; level < kEnvironmentLevels; ++level) {
        const auto faceSize = static_cast<float>(kEnvironmentFaceSize >> level);
        const float faceTexels = static_cast<float>(kFaceCount) * faceSize * faceSize;
        for (uint32_t face = 0; face < kFaceCount; ++face) {
            const nvrhi::FramebufferHandle framebuffer =
                m_device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(
                    nvrhi::FramebufferAttachment().setTexture(cube).setArraySlice(face).setMipLevel(level)));
            if (!framebuffer) {
                return false;
            }
            EnvironmentPrefilterConstants constants{};
            constants.face = face;
            constants.roughness = static_cast<float>(level) / static_cast<float>(kEnvironmentLevels - 1);
            // How many pixels of the image fall into one texel of this level, on average.
            constants.sourceLevel = 0.5f * std::log2(std::max(sourcePixels / faceTexels, 1.0f));
            constants.sourceDensity = sourcePixels / (4.0f * kPi);
            constants.sampleCount = sampleCountOf(level);
            commandList->writeBuffer(m_constants, &constants, sizeof(constants));

            const std::array<nvrhi::IBindingSet*, 1> bindingSets = {bindings.Get()};
            m_prefilter.draw(commandList, framebuffer, bindingSets);
        }
    }
    ++m_bakeCount;
    return true;
}

bool EnvironmentMaps::make(nvrhi::ICommandList* commandList, const Uuid& id, nvrhi::ITexture* source,
                           GpuEnvironment& made) {
    const AssetInfo* asset = m_assets->database().find(id);
    const std::string name = asset != nullptr ? asset->reference() : id.toString();

    // The pixels once more, for what they light matte surfaces with.
    const Result<std::shared_ptr<const Image>> image = m_assets->loadTexture(id);
    if (!image) {
        CORDITE_LOG_ERROR("Renderer", "cannot use {} as an environment: {}", name, image.error().message);
        return false;
    }
    made.radiance = m_device->createTexture(cubeDesc(kEnvironmentFaceSize, kEnvironmentLevels, "Environment " + name));
    if (!made.radiance) {
        CORDITE_LOG_ERROR("Renderer", "cannot use {} as an environment: there is no memory for its cube map", name);
        return false;
    }
    if (!bake(commandList, source, made.radiance)) {
        CORDITE_LOG_ERROR("Renderer", "cannot use {} as an environment: its cube map cannot be drawn", name);
        return false;
    }
    made.sky = source;
    made.diffuse = projectEnvironment(**image).diffuseCoefficients();
    made.levelCount = kEnvironmentLevels;
    return true;
}

const GpuEnvironment* EnvironmentMaps::environment(nvrhi::ICommandList* commandList, const Uuid& id,
                                                   RenderResources& resources) {
    CORDITE_ASSERT(commandList != nullptr, "making an environment needs a command list");
    ++m_calls;
    // What nobody has asked for in a while gives its memory back: the cube map, and
    // the image, which may be a large one.
    std::erase_if(m_entries,
                  [&](const Entry& entry) { return entry.id != id && m_calls - entry.lastUsed > kKeepUnusedCalls; });
    if (id.isNil() || commandList == nullptr) {
        return nullptr;
    }
    // As colors: an image of 8-bit numbers is sRGB, one of floating-point numbers light.
    nvrhi::ITexture* source = resources.colorTexture(id);
    if (source == nullptr) {
        return nullptr;
    }

    auto entry = std::ranges::find_if(m_entries, [&](const Entry& candidate) { return candidate.id == id; });
    if (entry == m_entries.end()) {
        m_entries.push_back({.id = id});
        entry = m_entries.end() - 1;
    }
    entry->lastUsed = m_calls;
    if (entry->source.Get() != source) {
        // New, or made of an image that has since been loaded anew.
        entry->source = source;
        entry->environment = {};
        entry->usable = make(commandList, id, source, entry->environment);
        if (!entry->usable) {
            // Nothing is kept of what could not be finished.
            entry->environment = {};
        }
    }
    return entry->usable ? &entry->environment : nullptr;
}

} // namespace cordite
