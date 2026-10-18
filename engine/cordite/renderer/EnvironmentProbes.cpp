#include "cordite/renderer/EnvironmentProbes.h"

#include "cordite/core/Assert.h"
#include "cordite/core/Log.h"
#include "cordite/math/Frustum.h"
#include "cordite/math/Geometry.h"
#include "cordite/math/SphericalHarmonics.h"
#include "cordite/math/Transform.h"
#include "cordite/renderer/ViewConstants.h"
#include "cordite/world/Components.h"

#include <nvrhi/utils.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace cordite {
namespace {

/// Mirrors ProbeHarmonicsConstants in shaders/passes/ProbeHarmonics.slang.
struct ProbeHarmonicsConstants {
    uint32_t slot;
    uint32_t faceSize;
    uint32_t padding0;
    uint32_t padding1;
};

static_assert(sizeof(ProbeHarmonicsConstants) == 16, "must match the cbuffer layout in ProbeHarmonics.slang");

/// Where the cameras of the faces look, and which way is up in their pictures: as a cube
/// map has its faces (cubeFaceDirection), seen from inside.
constexpr std::array<Vec3, kCubeFaceCount> kFaceForward = {Vec3(1.0f, 0.0f, 0.0f), Vec3(-1.0f, 0.0f, 0.0f),
                                                           Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f),
                                                           Vec3(0.0f, 0.0f, 1.0f), Vec3(0.0f, 0.0f, -1.0f)};
constexpr std::array<Vec3, kCubeFaceCount> kFaceUp = {Vec3(0.0f, 1.0f, 0.0f),  Vec3(0.0f, 1.0f, 0.0f),
                                                      Vec3(0.0f, 0.0f, -1.0f), Vec3(0.0f, 0.0f, 1.0f),
                                                      Vec3(0.0f, 1.0f, 0.0f),  Vec3(0.0f, 1.0f, 0.0f)};

/// How near and how far the cameras of a probe see: what touches the point is seen, and
/// a sky far away.
constexpr float kProbeNearPlane = 0.05f;
constexpr float kProbeFarPlane = 1000.0f;

/// How many levels the picture of a probe has: down to a texel a face.
constexpr uint32_t kCaptureLevels = [] {
    uint32_t levels = 1;
    for (uint32_t size = kProbeFaceSize; size > 1; size /= 2) {
        ++levels;
    }
    return levels;
}();

/// The largest numbers a probe is given.
constexpr float kMostBlendDistance = 100.0f;
constexpr float kMostIntensity = 100.0f;

float finiteOr(float value, float fallback) {
    return std::isfinite(value) ? value : fallback;
}

} // namespace

GpuEnvironmentProbe gpuProbeOf(const Mat4& placement, const EnvironmentProbeComponent& component, uint32_t slot,
                               bool reflects) {
    GpuEnvironmentProbe probe;
    const Mat4 inverse = glm::inverse(placement);
    if (isFinite(inverse) && isFinite(placement)) {
        probe.worldToBox = inverse;
    } else {
        // A box of no size has no inside: every point is far outside it.
        probe.worldToBox = Mat4(0.0f);
        probe.worldToBox[3] = Vec4(2.0f, 2.0f, 2.0f, 1.0f);
    }
    probe.boxSize =
        Vec3(glm::length(Vec3(placement[0])), glm::length(Vec3(placement[1])), glm::length(Vec3(placement[2])));
    if (!isFinite(probe.boxSize)) {
        probe.boxSize = Vec3(0.0f);
    }
    probe.blendDistance = std::clamp(finiteOr(component.blendDistance, 0.0f), 0.0f, kMostBlendDistance);
    probe.capturePosition = isFinite(Vec3(placement[3])) ? Vec3(placement[3]) : Vec3(0.0f);
    probe.intensity = std::clamp(finiteOr(component.intensity, 0.0f), 0.0f, kMostIntensity);
    probe.slot = slot;
    probe.flags = (component.boxProjection ? kProbeBoxProjection : 0u) | (reflects ? kProbeReflects : 0u);
    return probe;
}

float probeWeight(const GpuEnvironmentProbe& probe, const Vec3& position) {
    const Vec3 local = Vec3(probe.worldToBox * Vec4(position, 1.0f));
    const Vec3 inside = (Vec3(0.5f) - glm::abs(local)) * probe.boxSize;
    const float nearest = std::min({inside.x, inside.y, inside.z});
    // Also when it is no number.
    if (!(nearest >= 0.0f)) {
        return 0.0f;
    }
    return probe.blendDistance > 0.0f ? std::min(nearest / probe.blendDistance, 1.0f) : 1.0f;
}

Vec3 probeReflectionDirection(const GpuEnvironmentProbe& probe, const Vec3& position, const Vec3& direction) {
    if ((probe.flags & kProbeBoxProjection) == 0) {
        return direction;
    }
    // In the box's own space the line leaves it through the face it goes towards first; the
    // space is the world moved, turned and stretched, so how far along the line that is
    // holds in the world as well.
    const Vec3 origin = Vec3(probe.worldToBox * Vec4(position, 1.0f));
    const Vec3 along = Mat3(probe.worldToBox) * direction;
    // A line that does not go along an axis never reaches its faces.
    const auto safe = [](float value) { return std::abs(value) > 1.0e-6f ? value : 1.0e-6f; };
    const Vec3 across(safe(along.x), safe(along.y), safe(along.z));
    const Vec3 toFaces = (glm::sign(across) * 0.5f - origin) / across;
    const float leaves = std::max(std::min({toFaces.x, toFaces.y, toFaces.z}), 0.0f);
    return normalizeOr(position + direction * leaves - probe.capturePosition, direction);
}

Vec3 cubeFaceDirection(uint32_t face, Vec2 uv) {
    const Vec2 place = uv * 2.0f - 1.0f;
    Vec3 direction(-place.x, -place.y, -1.0f);
    switch (face) {
        case 0:
            direction = Vec3(1.0f, -place.y, -place.x);
            break;
        case 1:
            direction = Vec3(-1.0f, -place.y, place.x);
            break;
        case 2:
            direction = Vec3(place.x, 1.0f, place.y);
            break;
        case 3:
            direction = Vec3(place.x, -1.0f, -place.y);
            break;
        case 4:
            direction = Vec3(place.x, -place.y, 1.0f);
            break;
        default:
            break;
    }
    return glm::normalize(direction);
}

CameraView probeFaceView(const Vec3& position, uint32_t face) {
    CORDITE_ASSERT(face < kCubeFaceCount, "a cube has six faces, not {}", face + 1);
    const uint32_t index = std::min(face, kCubeFaceCount - 1);
    CameraComponent camera;
    camera.projection = CameraProjection::Perspective;
    camera.verticalFovDegrees = 90.0f;
    camera.nearPlane = kProbeNearPlane;
    camera.farPlane = kProbeFarPlane;
    const Mat4 placement =
        glm::translate(Mat4(1.0f), position) * glm::mat4_cast(lookRotation(kFaceForward[index], kFaceUp[index]));
    return makeCameraView(placement, camera, 1.0f);
}

EnvironmentProbes::EnvironmentProbes(Parts parts)
    : m_device(parts.device),
      m_harmonics(std::move(parts.harmonics)),
      m_constants(std::move(parts.constants)),
      m_copyLayout(std::move(parts.copyLayout)),
      m_projectLayout(std::move(parts.projectLayout)),
      m_copy(std::move(parts.copy)),
      m_downsample(std::move(parts.downsample)),
      m_project(std::move(parts.project)) {
    // Slot 0 is handed out first.
    for (uint32_t slot = kMaxEnvironmentProbes; slot > 0; --slot) {
        m_freeSlots.push_back(slot - 1);
    }
}

Result<EnvironmentProbes> EnvironmentProbes::create(nvrhi::IDevice* device, ShaderManager& shaders) {
    CORDITE_ASSERT(device != nullptr, "environment probes need a device");
    if (device == nullptr) {
        return makeError(ErrorCode::InvalidArgument, "environment probes need a device");
    }
    nvrhi::BufferDesc harmonicsDesc;
    harmonicsDesc.byteSize = sizeof(Vec4) * kSphericalHarmonicsCount * kMaxEnvironmentProbes;
    harmonicsDesc.structStride = sizeof(Vec4);
    harmonicsDesc.canHaveUAVs = true;
    harmonicsDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    harmonicsDesc.keepInitialState = true;
    harmonicsDesc.debugName = "Probe harmonics";
    nvrhi::BufferHandle harmonics = device->createBuffer(harmonicsDesc);

    nvrhi::BufferHandle constants = device->createBuffer(nvrhi::utils::CreateVolatileConstantBufferDesc(
        sizeof(ProbeHarmonicsConstants), "ProbeHarmonicsConstants", kMaxViewsInFlight));

    nvrhi::BindingLayoutDesc copyDesc;
    copyDesc.visibility = nvrhi::ShaderType::Pixel;
    copyDesc.registerSpace = 0;
    copyDesc.registerSpaceIsDescriptorSet = true;
    copyDesc.bindings = {nvrhi::BindingLayoutItem::Texture_SRV(0)};
    nvrhi::BindingLayoutHandle copyLayout = device->createBindingLayout(copyDesc);

    nvrhi::BindingLayoutDesc projectDesc;
    projectDesc.visibility = nvrhi::ShaderType::Compute;
    projectDesc.registerSpace = 0;
    projectDesc.registerSpaceIsDescriptorSet = true;
    projectDesc.bindings = {nvrhi::BindingLayoutItem::VolatileConstantBuffer(0),
                            nvrhi::BindingLayoutItem::Texture_SRV(0),
                            nvrhi::BindingLayoutItem::StructuredBuffer_UAV(0)};
    nvrhi::BindingLayoutHandle projectLayout = device->createBindingLayout(projectDesc);
    if (!harmonics || !constants || !copyLayout || !projectLayout) {
        return makeError(ErrorCode::DeviceError, "cannot create what the pictures of environment probes are taken in");
    }

    FullscreenPassDesc copyPass;
    copyPass.debugName = "Probe face";
    copyPass.pixelShader = {
        .path = "passes/ProbeFace.slang", .entryPoint = "probeFacePixel", .stage = ShaderStage::Pixel};
    copyPass.bindingLayouts = {copyLayout};
    FullscreenPassDesc downsamplePass = copyPass;
    downsamplePass.debugName = "Probe level";
    downsamplePass.pixelShader = {
        .path = "passes/ProbeDownsample.slang", .entryPoint = "probeDownsamplePixel", .stage = ShaderStage::Pixel};
    CORDITE_TRY_ASSIGN(FullscreenPass copy, FullscreenPass::create(device, shaders, std::move(copyPass)));
    CORDITE_TRY_ASSIGN(FullscreenPass downsample, FullscreenPass::create(device, shaders, std::move(downsamplePass)));
    CORDITE_TRY_ASSIGN(ComputePass project, ComputePass::create(device, shaders,
                                                                {.debugName = "Probe harmonics",
                                                                 .shader = {.path = "passes/ProbeHarmonics.slang",
                                                                            .entryPoint = "probeHarmonicsCompute",
                                                                            .stage = ShaderStage::Compute},
                                                                 .bindingLayouts = {projectLayout}}));
    return EnvironmentProbes(Parts{.device = device,
                                   .harmonics = std::move(harmonics),
                                   .constants = std::move(constants),
                                   .copyLayout = std::move(copyLayout),
                                   .projectLayout = std::move(projectLayout),
                                   .copy = std::move(copy),
                                   .downsample = std::move(downsample),
                                   .project = std::move(project)});
}

std::vector<ProbeCapture> EnvironmentProbes::gather(const World& world, uint64_t resourceRevision) {
    m_resourceRevision = resourceRevision;
    // By the ids of their entities: the same probes are left out on every run.
    std::vector<std::pair<Uuid, Vec3>> probes;
    const auto view = world.registry().view<const EnvironmentProbeComponent, const WorldTransform>();
    for (const auto [handle, component, placement] : view.each()) {
        const Entity entity{handle};
        if (world.isEnabledInHierarchy(entity)) {
            probes.emplace_back(world.id(entity), Vec3(placement.matrix[3]));
        }
    }
    std::ranges::sort(probes, {}, &std::pair<Uuid, Vec3>::first);
    m_dropped =
        probes.size() > kMaxEnvironmentProbes ? static_cast<uint32_t>(probes.size() - kMaxEnvironmentProbes) : 0;
    if (m_dropped > 0) {
        if (!m_reportedDropped) {
            CORDITE_LOG_WARN("Renderer", "too many environment probes: {} light nothing (a world takes {})", m_dropped,
                             kMaxEnvironmentProbes);
            m_reportedDropped = true;
        }
        probes.resize(kMaxEnvironmentProbes);
    }

    // The probes of entities that are gone give their slots back.
    const auto present = [&](const State& state) {
        return std::ranges::any_of(probes, [&](const auto& probe) { return probe.first == state.id; });
    };
    for (const State& state : m_states) {
        if (!present(state)) {
            m_freeSlots.push_back(state.slot);
        }
    }
    std::erase_if(m_states, [&](const State& state) { return !present(state); });

    std::vector<ProbeCapture> captures;
    for (const auto& [id, position] : probes) {
        auto found = std::ranges::find(m_states, id, &State::id);
        if (found == m_states.end()) {
            CORDITE_ASSERT(!m_freeSlots.empty(), "there is a slot for every probe a world takes");
            if (m_freeSlots.empty()) {
                continue;
            }
            m_states.push_back({.id = id, .slot = m_freeSlots.back()});
            m_freeSlots.pop_back();
            found = m_states.end() - 1;
        }
        const State& state = *found;
        // Compared exactly: a probe that has not moved is not taken again for rounding.
        const bool stale =
            !state.taken || state.retake || state.takenAt != position || state.resourceRevision != resourceRevision;
        if (stale && isFinite(position) && captures.size() < kMaxProbeCapturesPerFrame) {
            captures.push_back({.id = id, .position = position, .slot = state.slot});
        }
    }
    return captures;
}

bool EnvironmentProbes::prepareCapture() {
    if (m_captureCube) {
        return true;
    }
    nvrhi::TextureDesc cubeDesc;
    cubeDesc.dimension = nvrhi::TextureDimension::TextureCube;
    cubeDesc.width = kProbeFaceSize;
    cubeDesc.height = kProbeFaceSize;
    cubeDesc.arraySize = kCubeFaceCount;
    cubeDesc.mipLevels = kCaptureLevels;
    cubeDesc.format = nvrhi::Format::RGBA16_FLOAT;
    cubeDesc.isRenderTarget = true;
    cubeDesc.initialState = nvrhi::ResourceStates::ShaderResource;
    cubeDesc.keepInitialState = true;
    cubeDesc.debugName = "Probe picture";
    m_captureCube = m_device->createTexture(cubeDesc);
    return m_captureCube != nullptr;
}

void EnvironmentProbes::capture() {
    for (State& state : m_states) {
        state.retake = true;
    }
}

void EnvironmentProbes::copyFace(nvrhi::ICommandList* commandList, nvrhi::ITexture* sceneColor, uint32_t face) {
    CORDITE_ASSERT(commandList != nullptr && sceneColor != nullptr && face < kCubeFaceCount,
                   "a face of a probe needs a command list, a picture and one of six faces");
    if (commandList == nullptr || sceneColor == nullptr || face >= kCubeFaceCount) {
        return;
    }
    const nvrhi::FramebufferHandle framebuffer =
        m_device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(
            nvrhi::FramebufferAttachment().setTexture(m_captureCube).setArraySlice(face).setMipLevel(0)));
    nvrhi::BindingSetDesc setDesc;
    setDesc.bindings = {nvrhi::BindingSetItem::Texture_SRV(0, sceneColor)};
    const nvrhi::BindingSetHandle bindings = m_device->createBindingSet(setDesc, m_copyLayout);
    if (!framebuffer || !bindings) {
        CORDITE_LOG_ERROR("Renderer",
                          "a face of an environment probe cannot be copied: its GPU objects cannot be made");
        return;
    }
    const std::array<nvrhi::IBindingSet*, 1> sets = {bindings.Get()};
    m_copy.draw(commandList, framebuffer, sets);
}

void EnvironmentProbes::makeLevels(nvrhi::ICommandList* commandList) {
    CORDITE_ASSERT(commandList != nullptr, "the levels of the picture of a probe need a command list");
    if (commandList == nullptr) {
        return;
    }
    for (uint32_t level = 1; level < kCaptureLevels; ++level) {
        for (uint32_t face = 0; face < kCubeFaceCount; ++face) {
            const nvrhi::FramebufferHandle framebuffer =
                m_device->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(
                    nvrhi::FramebufferAttachment().setTexture(m_captureCube).setArraySlice(face).setMipLevel(level)));
            nvrhi::BindingSetDesc setDesc;
            setDesc.bindings = {nvrhi::BindingSetItem::Texture_SRV(0, m_captureCube, nvrhi::Format::UNKNOWN,
                                                                   nvrhi::TextureSubresourceSet(level - 1, 1, face, 1),
                                                                   nvrhi::TextureDimension::Texture2DArray)};
            const nvrhi::BindingSetHandle bindings = m_device->createBindingSet(setDesc, m_copyLayout);
            if (!framebuffer || !bindings) {
                CORDITE_LOG_ERROR("Renderer", "the levels of the picture of a probe cannot be made: their GPU objects "
                                              "cannot be made");
                return;
            }
            const std::array<nvrhi::IBindingSet*, 1> sets = {bindings.Get()};
            m_downsample.draw(commandList, framebuffer, sets);
        }
    }
}

nvrhi::ITexture* EnvironmentProbes::prepareRadiance(nvrhi::ICommandList* commandList, uint32_t slot) {
    CORDITE_ASSERT(commandList != nullptr && slot < kMaxEnvironmentProbes,
                   "the reflections of a probe need a command list and a slot");
    if (commandList == nullptr || slot >= kMaxEnvironmentProbes) {
        return nullptr;
    }
    if (m_radiance && slot < m_radianceCubes) {
        return m_radiance;
    }
    // Twice as many cubes as before, until the slot is in: the memory follows the probes.
    uint32_t cubes = std::max(m_radianceCubes, 1u);
    while (cubes <= slot) {
        cubes *= 2;
    }
    cubes = std::min(cubes, kMaxEnvironmentProbes);
    nvrhi::TextureDesc desc;
    desc.dimension = nvrhi::TextureDimension::TextureCubeArray;
    desc.width = kProbeFaceSize;
    desc.height = kProbeFaceSize;
    desc.arraySize = cubes * kCubeFaceCount;
    desc.mipLevels = kProbeLevels;
    desc.format = nvrhi::Format::RGBA16_FLOAT;
    desc.isRenderTarget = true;
    desc.initialState = nvrhi::ResourceStates::ShaderResource;
    desc.keepInitialState = true;
    desc.debugName = "Probe reflections";
    nvrhi::TextureHandle grown = m_device->createTexture(desc);
    if (!grown) {
        if (!m_reportedRadiance) {
            CORDITE_LOG_ERROR("Renderer",
                              "environment probes reflect nothing: there is no memory for {} cubes of {} texels a side",
                              cubes, kProbeFaceSize);
            m_reportedRadiance = true;
        }
        return nullptr;
    }
    // What the smaller array held. The command list keeps it alive until it is copied.
    if (m_radiance) {
        for (uint32_t layer = 0; layer < m_radianceCubes * kCubeFaceCount; ++layer) {
            for (uint32_t level = 0; level < kProbeLevels; ++level) {
                const nvrhi::TextureSlice slice = nvrhi::TextureSlice().setArraySlice(layer).setMipLevel(level);
                commandList->copyTexture(grown, slice, m_radiance, slice);
            }
        }
    }
    m_radiance = std::move(grown);
    m_radianceCubes = cubes;
    return m_radiance;
}

void EnvironmentProbes::project(nvrhi::ICommandList* commandList, const ProbeCapture& probe, bool reflects) {
    CORDITE_ASSERT(commandList != nullptr, "projecting a probe needs a command list");
    const auto found = std::ranges::find(m_states, probe.id, &State::id);
    if (commandList == nullptr || found == m_states.end()) {
        return;
    }
    nvrhi::BindingSetDesc setDesc;
    setDesc.bindings = {nvrhi::BindingSetItem::ConstantBuffer(0, m_constants),
                        nvrhi::BindingSetItem::Texture_SRV(0, m_captureCube, nvrhi::Format::UNKNOWN,
                                                           nvrhi::AllSubresources,
                                                           nvrhi::TextureDimension::Texture2DArray),
                        nvrhi::BindingSetItem::StructuredBuffer_UAV(0, m_harmonics)};
    const nvrhi::BindingSetHandle bindings = m_device->createBindingSet(setDesc, m_projectLayout);
    if (!bindings) {
        CORDITE_LOG_ERROR("Renderer", "an environment probe cannot be projected: its GPU objects cannot be made");
        return;
    }
    const ProbeHarmonicsConstants constants{
        .slot = probe.slot, .faceSize = kProbeFaceSize, .padding0 = 0, .padding1 = 0};
    commandList->writeBuffer(m_constants, &constants, sizeof(constants));
    const std::array<nvrhi::IBindingSet*, 1> sets = {bindings.Get()};
    m_project.dispatch(commandList, sets, UVec3(1, 1, 1));

    State& state = *found;
    state.taken = true;
    state.reflects = reflects;
    state.retake = false;
    state.takenAt = probe.position;
    state.resourceRevision = m_resourceRevision;
    ++m_captureCount;
}

std::vector<GpuEnvironmentProbe> EnvironmentProbes::viewList(const World& world, const CameraView& view) const {
    struct Candidate {
        int32_t priority;
        Uuid id;
        GpuEnvironmentProbe probe;
    };

    const Frustum frustum = Frustum::fromViewProjection(view.viewProjection);
    std::vector<Candidate> candidates;
    const auto probes = world.registry().view<const EnvironmentProbeComponent, const WorldTransform>();
    for (const auto [handle, component, placement] : probes.each()) {
        const Entity entity{handle};
        if (!world.isEnabledInHierarchy(entity)) {
            continue;
        }
        const Uuid id = world.id(entity);
        const auto state = std::ranges::find(m_states, id, &State::id);
        if (state == m_states.end() || !state->taken) {
            continue;
        }
        const Aabb box = Aabb::fromCenterExtents(Vec3(0.0f), Vec3(0.5f)).transformed(placement.matrix);
        if (!frustum.intersects(box)) {
            continue;
        }
        candidates.push_back(
            {component.priority, id, gpuProbeOf(placement.matrix, component, state->slot, state->reflects)});
    }
    std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) {
        return a.priority != b.priority ? a.priority > b.priority : a.id < b.id;
    });
    std::vector<GpuEnvironmentProbe> list;
    for (const Candidate& candidate : candidates) {
        if (list.size() == kMaxViewProbes) {
            break;
        }
        list.push_back(candidate.probe);
    }
    return list;
}

uint64_t EnvironmentProbes::byteSize() const {
    // Cubes of four half floats a texel, with their smaller levels: the picture with all of
    // them, and the reflections with theirs, a cube of the array each; nine float4 a slot.
    const auto cubeBytes = [](uint32_t levels) {
        uint64_t texels = 0;
        for (uint32_t level = 0; level < levels; ++level) {
            const uint64_t side = std::max(kProbeFaceSize >> level, 1u);
            texels += side * side * kCubeFaceCount;
        }
        return texels * 8;
    };
    return (m_captureCube ? cubeBytes(kCaptureLevels) : 0) + cubeBytes(kProbeLevels) * m_radianceCubes +
           uint64_t{sizeof(Vec4)} * kSphericalHarmonicsCount * kMaxEnvironmentProbes;
}

} // namespace cordite
