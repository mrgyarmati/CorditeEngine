#include "cordite/renderer/ViewBindings.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"
#include "cordite/renderer/RectLights.h"
#include "cordite/renderer/ViewConstants.h"

#include <glm/gtc/packing.hpp>
#include <nvrhi/utils.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace cordite {
namespace {

static_assert(sizeof(ClusterRange) == 8, "must match ClusterRange in Lights.slang");

/// A shadow map as the shaders read it: `ShadowView` in shaders/common/Shadows.slang.
struct GpuShadowView {
    /// World space to the atlas: x and y from (0, 0) at its top left to (1, 1), z the
    /// depth as the light sees it, all three to be divided by w.
    Mat4 worldToAtlas;
    /// The part of the atlas the filter may read for this map: its tile, half a texel
    /// in from the edges, as the smallest and the largest x and y.
    Vec4 bounds;
    /// The directions in the world in which the map's x and y grow.
    Vec3 right;
    /// The size of a texel in the world: in metres for a directional light, in metres
    /// one metre from the light for the others.
    float texelSize;
    Vec3 up;
    /// 1 for the maps of point, spot and rect lights, whose texels grow with the distance
    /// and which may see less than their light reaches.
    float perspective;
};

static_assert(sizeof(GpuShadowView) == 112, "must match ShadowView in Shadows.slang");

/// Room for the lights of an ordinary scene before the buffers have to grow.
constexpr size_t kInitialLightCapacity = 64;
constexpr size_t kInitialIndexCapacity = 4096;
constexpr size_t kInitialShadowViewCapacity = 16;

/// A table of the fit of rect lights (RectLights.h), kLtcTableSize texels a side of four
/// half floats: every GPU can filter those, which is not so for 32-bit floats, and they
/// hold the fit, whose numbers are less than two, to a part in two thousand.
nvrhi::TextureHandle createRectLightTable(nvrhi::IDevice* device, const char* name) {
    nvrhi::TextureDesc desc;
    desc.width = kLtcTableSize;
    desc.height = kLtcTableSize;
    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.debugName = name;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    return device->createTexture(desc);
}

/// Writes `texels` into a table made by createRectLightTable().
void writeRectLightTable(nvrhi::ICommandList* commandList, nvrhi::ITexture* table, std::span<const Vec4> texels) {
    CORDITE_ASSERT(texels.size() == size_t{kLtcTableSize} * kLtcTableSize, "a table of the fit has 64 x 64 texels");
    if (texels.size() != size_t{kLtcTableSize} * kLtcTableSize) {
        return;
    }
    std::vector<uint16_t> halves(texels.size() * 4);
    for (size_t index = 0; index < texels.size(); ++index) {
        for (size_t channel = 0; channel < 4; ++channel) {
            halves[index * 4 + channel] = glm::packHalf1x16(texels[index][static_cast<glm::length_t>(channel)]);
        }
    }
    commandList->writeTexture(table, 0, 0, halves.data(), size_t{kLtcTableSize} * 4 * sizeof(uint16_t));
}

/// The bytes of a list, or of one element of nothing when the list is empty: a buffer
/// cannot be empty, and a shader that finds a count of zero reads none of it.
template <typename T>
std::span<const T> orPlaceholder(const std::vector<T>& values, const T& placeholder) {
    return values.empty() ? std::span<const T>(&placeholder, 1) : std::span<const T>(values);
}

/// A shadow map of the plan as the shaders read it, in an atlas `atlasSize` texels a side.
GpuShadowView toGpuShadowView(const ShadowView& shadow, float atlasSize) {
    // From the clip space of the map - x and y from -1 to 1, +Y up - to its tile in the
    // atlas, where y runs downwards.
    const float scale = static_cast<float>(shadow.tileSize) / atlasSize;
    const Vec2 origin = Vec2(shadow.tileOrigin) / atlasSize;
    Mat4 toAtlas(1.0f);
    toAtlas[0][0] = 0.5f * scale;
    toAtlas[1][1] = -0.5f * scale;
    toAtlas[3][0] = origin.x + 0.5f * scale;
    toAtlas[3][1] = origin.y + 0.5f * scale;

    const float halfTexel = 0.5f / atlasSize;
    GpuShadowView gpu{};
    gpu.worldToAtlas = toAtlas * shadow.viewProjection;
    gpu.bounds =
        Vec4(origin.x + halfTexel, origin.y + halfTexel, origin.x + scale - halfTexel, origin.y + scale - halfTexel);
    gpu.right = shadow.right;
    gpu.texelSize = shadow.texelSize;
    gpu.up = shadow.up;
    gpu.perspective = shadow.perspective ? 1.0f : 0.0f;
    return gpu;
}

} // namespace

Result<ViewBindings> ViewBindings::create(nvrhi::IDevice* device) {
    CORDITE_ASSERT(device != nullptr, "view bindings need a device");

    ViewBindings view;
    view.m_device = device;
    view.m_constants = device->createBuffer(
        nvrhi::utils::CreateVolatileConstantBufferDesc(sizeof(ViewConstants), "ViewConstants", kMaxViewsInFlight));

    nvrhi::BindingLayoutDesc layoutDesc;
    layoutDesc.visibility = nvrhi::ShaderType::All;
    layoutDesc.registerSpace = 0;
    layoutDesc.registerSpaceIsDescriptorSet = true;
    layoutDesc.bindings = {
        nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(0),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(1),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(2),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(3),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(9),
        nvrhi::BindingLayoutItem::StructuredBuffer_SRV(10),
        nvrhi::BindingLayoutItem::Texture_SRV(4),
        nvrhi::BindingLayoutItem::Texture_SRV(5),
        nvrhi::BindingLayoutItem::Texture_SRV(6),
        nvrhi::BindingLayoutItem::Texture_SRV(7),
        nvrhi::BindingLayoutItem::Texture_SRV(8),
        nvrhi::BindingLayoutItem::Texture_SRV(11),
        nvrhi::BindingLayoutItem::Texture_SRV(12),
        nvrhi::BindingLayoutItem::Texture_SRV(13),
        nvrhi::BindingLayoutItem::Sampler(0),
        nvrhi::BindingLayoutItem::Sampler(1),
        nvrhi::BindingLayoutItem::Sampler(2),
        nvrhi::BindingLayoutItem::Sampler(3),
        nvrhi::BindingLayoutItem::Sampler(4),
    };
    view.m_layout = device->createBindingLayout(layoutDesc);

    // Compares the depth of a lit point with four texels of a shadow map at once and
    // blends the four answers, where the GPU can do that for a depth texture; where it
    // cannot, it compares with the nearest texel alone.
    const bool canFilter = (device->queryFormatSupport(nvrhi::Format::D32) & nvrhi::FormatSupport::ShaderSample) !=
                           nvrhi::FormatSupport::None;
    view.m_shadowSampler = device->createSampler(nvrhi::SamplerDesc()
                                                     .setMinFilter(canFilter)
                                                     .setMagFilter(canFilter)
                                                     .setMipFilter(false)
                                                     .setAllAddressModes(nvrhi::SamplerAddressMode::Clamp)
                                                     .setReductionType(nvrhi::SamplerReductionType::Comparison));
    // The cube map of an environment is read between its levels, by roughness; its
    // image wraps around where the left and right edges meet, and ends at the poles.
    view.m_environmentSampler = device->createSampler(
        nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
    view.m_skySampler = device->createSampler(nvrhi::SamplerDesc()
                                                  .setAllFilters(true)
                                                  .setAddressU(nvrhi::SamplerAddressMode::Wrap)
                                                  .setAddressV(nvrhi::SamplerAddressMode::Clamp)
                                                  .setAddressW(nvrhi::SamplerAddressMode::Clamp));
    // A cookie is a picture laid onto what is lit, smaller where it is seen from afar: read
    // between its levels. A directional light's repeats; a spot light's ends where its
    // light does.
    view.m_cookieSampler = device->createSampler(
        nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Wrap));
    // The light of the fog is read between the middles of its boxes, and its grid covers
    // exactly the picture and the depth it is worked out to.
    view.m_fogVolumeSampler = device->createSampler(
        nvrhi::SamplerDesc().setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp));
    nvrhi::TextureDesc noFogDesc;
    noFogDesc.dimension = nvrhi::TextureDimension::Texture3D;
    noFogDesc.format = nvrhi::Format::RGBA16_FLOAT;
    noFogDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    noFogDesc.keepInitialState = true;
    noFogDesc.debugName = "No fog light";
    view.m_noFogVolume = device->createTexture(noFogDesc);
    nvrhi::BufferDesc noHarmonicsDesc;
    noHarmonicsDesc.byteSize = sizeof(Vec4) * kSphericalHarmonicsCount;
    noHarmonicsDesc.structStride = sizeof(Vec4);
    noHarmonicsDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    noHarmonicsDesc.keepInitialState = true;
    noHarmonicsDesc.debugName = "No probe harmonics";
    view.m_noProbeHarmonics = device->createBuffer(noHarmonicsDesc);
    nvrhi::TextureDesc noRadianceDesc;
    noRadianceDesc.dimension = nvrhi::TextureDimension::TextureCubeArray;
    noRadianceDesc.arraySize = kCubeFaceCount;
    noRadianceDesc.format = nvrhi::Format::RGBA16_FLOAT;
    noRadianceDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    noRadianceDesc.keepInitialState = true;
    noRadianceDesc.debugName = "No probe reflections";
    view.m_noProbeRadiance = device->createTexture(noRadianceDesc);
    view.m_rectLightMatrices = createRectLightTable(device, "Rect light matrices");
    view.m_rectLightMagnitudes = createRectLightTable(device, "Rect light magnitudes");
    if (!view.m_constants || !view.m_layout || !view.m_shadowSampler || !view.m_environmentSampler ||
        !view.m_skySampler || !view.m_cookieSampler || !view.m_fogVolumeSampler || !view.m_noFogVolume ||
        !view.m_noProbeHarmonics || !view.m_noProbeRadiance || !view.m_rectLightMatrices ||
        !view.m_rectLightMagnitudes) {
        return makeError(ErrorCode::DeviceError, "cannot create the view constants");
    }

    const bool reserved =
        view.reserve(view.m_lights, kInitialLightCapacity * sizeof(GpuLight), sizeof(GpuLight), "Lights") &&
        view.reserve(view.m_clusterGrid, kClusterCount * sizeof(ClusterRange), sizeof(ClusterRange), "Cluster grid") &&
        view.reserve(view.m_lightIndices, kInitialIndexCapacity * sizeof(uint32_t), sizeof(uint32_t), "Light lists") &&
        view.reserve(view.m_shadowViews, kInitialShadowViewCapacity * sizeof(GpuShadowView), sizeof(GpuShadowView),
                     "Shadow views") &&
        view.reserve(view.m_probes, kMaxViewProbes * sizeof(GpuEnvironmentProbe), sizeof(GpuEnvironmentProbe),
                     "Environment probes");
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

uint32_t ViewBindings::writeProbes(nvrhi::ICommandList* commandList, const ViewEnvironment& environment) {
    if (!m_noProbeRadianceWritten) {
        // Written, not cleared, as the texel of no fog light.
        constexpr std::array<uint16_t, 4> kBlack{};
        for (uint32_t face = 0; face < kCubeFaceCount; ++face) {
            commandList->writeTexture(m_noProbeRadiance, face, 0, kBlack.data(), sizeof(kBlack));
        }
        m_noProbeRadianceWritten = true;
    }
    // The probes, where they fit; a view with none writes nothing, and reads nothing.
    const size_t probeCount = std::min(environment.probes.size(), size_t{kMaxViewProbes});
    if (probeCount > 0) {
        commandList->writeBuffer(m_probes, environment.probes.data(), probeCount * sizeof(GpuEnvironmentProbe));
    }
    return environment.probeHarmonics != nullptr ? static_cast<uint32_t>(probeCount) : 0;
}

void ViewBindings::update(nvrhi::ICommandList* commandList, const SceneView& view, const LightClusters& lights,
                          nvrhi::ITexture* cookies, const ViewShadows& shadows, const ViewEnvironment& environment,
                          nvrhi::ITexture* fogVolume) {
    const bool hasEnvironment = environment.environment != nullptr && environment.environment->radiance != nullptr &&
                                environment.environment->sky != nullptr;
    CORDITE_ASSERT(commandList != nullptr && m_constants != nullptr && cookies != nullptr && shadows.atlas != nullptr &&
                       hasEnvironment,
                   "update needs a command list, created bindings, cookies, a shadow atlas and an environment");
    if (commandList == nullptr || m_constants == nullptr || cookies == nullptr || shadows.atlas == nullptr ||
        !hasEnvironment) {
        return;
    }

    // The shadow maps, as the shaders look things up in them. A plan holds
    // kMaxShadowViews of them at most; one that holds more is not used at all, because
    // lights would refer to maps that are not there.
    const size_t plannedViews = shadows.plan != nullptr ? shadows.plan->views.size() : 0;
    CORDITE_ASSERT(plannedViews <= kMaxShadowViews, "a shadow plan holds at most kMaxShadowViews views");
    const auto atlasSize = static_cast<float>(shadows.atlas->getDesc().width);
    std::array<GpuShadowView, kMaxShadowViews> shadowViews{};
    const size_t shadowViewCount = plannedViews <= kMaxShadowViews ? plannedViews : 0;
    for (size_t index = 0; index < shadowViewCount; ++index) {
        shadowViews[index] = toGpuShadowView(shadows.plan->views[index], atlasSize);
    }
    const size_t shadowBytes = shadowViewCount * sizeof(GpuShadowView);
    const bool hasShadows =
        shadowViewCount > 0 && reserve(m_shadowViews, shadowBytes, sizeof(GpuShadowView), "Shadow views");
    if (hasShadows) {
        commandList->writeBuffer(m_shadowViews, shadowViews.data(), shadowBytes);
    }

    // The lights, where they fit. If a buffer cannot grow, the view is drawn without its
    // point, spot and rect lights rather than with lists that point past the end. Lights
    // whose shadow maps cannot be described are sent as lights without shadows.
    static const GpuLight kNoLight;
    static const uint32_t kNoIndex = 0;
    std::vector<GpuLight> unshadowed;
    if (!hasShadows && plannedViews > 0) {
        unshadowed = lights.lights;
        for (GpuLight& light : unshadowed) {
            light.shadowView = -1;
        }
    }
    const std::span<const GpuLight> lightData =
        orPlaceholder(unshadowed.empty() ? lights.lights : unshadowed, kNoLight);
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
    constants.previousViewProjection = view.previousViewProjection.value_or(view.camera.viewProjection);
    constants.cameraPosition = view.camera.position;
    constants.orthographic = view.camera.orthographic ? 1.0f : 0.0f;
    constants.cameraForward = normalizeOr(view.camera.forward, kWorldForward);
    constants.directionalLightCount = fits ? lights.directionalCount : 0;
    constants.skyColor = view.skyColor;
    constants.clusterDepthScale = lights.depthMapping.scale;
    constants.groundColor = view.groundColor;
    constants.clusterDepthBias = lights.depthMapping.bias;
    constants.screenOcclusion = view.ambientOcclusion ? 1.0f : 0.0f;
    constants.targetSize = size;
    constants.inverseTargetSize = 1.0f / size;
    if (hasShadows) {
        const ShadowCascades& cascades = shadows.plan->cascades;
        constants.cascadeEnds = Vec4(cascades.ends[0], cascades.ends[1], cascades.ends[2], cascades.ends[3]);
        constants.cascadeCount = cascades.count;
        constants.shadowFadeStart = cascades.fadeStart;
        constants.shadowFadeEnd = cascades.fadeEnd;
        constants.shadowViewCount = static_cast<uint32_t>(shadowViewCount);
    }
    constants.shadowFilterRadius = std::min(shadows.filterRadius, 3u);

    // The environment: nothing of it that is not a number reaches the shaders.
    const GpuEnvironment& surroundings = *environment.environment;
    const float intensity = std::isfinite(environment.intensity) ? std::max(environment.intensity, 0.0f) : 0.0f;
    const float rotation = std::isfinite(environment.rotation) ? environment.rotation : 0.0f;
    constants.environmentIntensity = intensity;
    constants.environmentLastLevel = static_cast<float>(std::max(surroundings.levelCount, 1u) - 1);
    constants.environmentVisible = environment.visible && intensity > 0.0f ? 1.0f : 0.0f;
    constants.environmentTurn = Vec2(std::sin(rotation), std::cos(rotation));
    constants.probeCount = writeProbes(commandList, environment);
    nvrhi::IBuffer* harmonics =
        environment.probeHarmonics != nullptr ? environment.probeHarmonics : m_noProbeHarmonics.Get();
    nvrhi::ITexture* reflections =
        environment.probeRadiance != nullptr ? environment.probeRadiance : m_noProbeRadiance.Get();
    for (size_t index = 0; index < surroundings.diffuse.size(); ++index) {
        const Vec3& coefficient = surroundings.diffuse[index];
        constants.environmentDiffuse[index] = Vec4(isFinite(coefficient) ? coefficient : Vec3(0.0f), 0.0f);
    }

    // The fog, likewise: every number of it finite and in its range.
    const FogSettings fog = sanitized(view.fog);
    constants.fogColor = fog.color;
    constants.fogDensity = fog.isEnabled() ? fog.density : 0.0f;
    constants.fogSunColor = fog.sunColor;
    constants.fogSunExponent = fog.sunExponent;
    constants.fogTowardsSun = fog.towardsSun;
    constants.fogHeight = fog.height;
    constants.fogHeightFalloff = fog.heightFalloff;
    constants.fogStartDistance = fog.startDistance;
    constants.fogMaxOpacity = fog.maxOpacity;
    // What lights throw into it is read only where it has been worked out, for the grid it
    // was worked out for.
    const bool fogIsLit = fogVolume != nullptr && fog.isLitByLights() && !view.fogVolume.isEmpty();
    constants.fogAnisotropy = fog.anisotropy;
    constants.fogScattering = fog.scattering;
    constants.fogVolumeDistance = fogIsLit ? fog.volumetricDistance : 0.0f;
    constants.fogVolumeSize = Vec3(static_cast<float>(view.fogVolume.columns), static_cast<float>(view.fogVolume.rows),
                                   static_cast<float>(view.fogVolume.slices));
    constants.backgroundColor = isFinite(view.backgroundColor) ? view.backgroundColor : Vec3(0.0f);
    commandList->writeBuffer(m_constants, &constants, sizeof(constants));

    if (!m_noFogVolumeWritten) {
        // Written, not cleared: a texture that is only cleared is not kept alive by the
        // command list (AGENTS.md, known pitfalls).
        constexpr std::array<uint16_t, 4> kNothing{};
        commandList->writeTexture(m_noFogVolume, 0, 0, kNothing.data(), sizeof(kNothing), sizeof(kNothing));
        m_noFogVolumeWritten = true;
    }
    if (!m_rectLightTablesWritten) {
        // Once: the fit is the same for every view, and never changes.
        writeRectLightTable(commandList, m_rectLightMatrices, ltcMatrixTexels());
        writeRectLightTable(commandList, m_rectLightMagnitudes, ltcMagnitudeTexels());
        m_rectLightTablesWritten = true;
    }
    nvrhi::ITexture* fogLight = fogIsLit ? fogVolume : m_noFogVolume.Get();
    if (m_bindingsOutdated || !m_bindings || m_atlas != shadows.atlas ||
        m_environmentRadiance != surroundings.radiance || m_environmentSky != surroundings.sky ||
        m_cookies != cookies || m_fogVolume != fogLight || m_probeHarmonics != harmonics ||
        m_probeRadiance != reflections) {
        m_probeHarmonics = harmonics;
        m_probeRadiance = reflections;
        m_atlas = shadows.atlas;
        m_cookies = cookies;
        m_fogVolume = fogLight;
        m_environmentRadiance = surroundings.radiance;
        m_environmentSky = surroundings.sky;
        nvrhi::BindingSetDesc setDesc;
        setDesc.bindings = {
            nvrhi::BindingSetItem::ConstantBuffer(0, m_constants),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(0, m_lights),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(1, m_clusterGrid),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(2, m_lightIndices),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(3, m_shadowViews),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(9, m_probes),
            nvrhi::BindingSetItem::StructuredBuffer_SRV(10, m_probeHarmonics),
            nvrhi::BindingSetItem::Texture_SRV(4, m_atlas),
            nvrhi::BindingSetItem::Texture_SRV(5, m_environmentRadiance),
            nvrhi::BindingSetItem::Texture_SRV(6, m_environmentSky),
            nvrhi::BindingSetItem::Texture_SRV(7, m_cookies),
            nvrhi::BindingSetItem::Texture_SRV(8, m_fogVolume),
            nvrhi::BindingSetItem::Texture_SRV(11, m_probeRadiance),
            nvrhi::BindingSetItem::Texture_SRV(12, m_rectLightMatrices),
            nvrhi::BindingSetItem::Texture_SRV(13, m_rectLightMagnitudes),
            nvrhi::BindingSetItem::Sampler(0, m_shadowSampler),
            nvrhi::BindingSetItem::Sampler(1, m_environmentSampler),
            nvrhi::BindingSetItem::Sampler(2, m_skySampler),
            nvrhi::BindingSetItem::Sampler(3, m_cookieSampler),
            nvrhi::BindingSetItem::Sampler(4, m_fogVolumeSampler),
        };
        m_bindings = m_device->createBindingSet(setDesc, m_layout);
        CORDITE_VERIFY(m_bindings, "cannot create the view bindings");
        m_bindingsOutdated = false;
    }
}

} // namespace cordite
