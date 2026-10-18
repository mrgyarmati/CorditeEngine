#pragma once

/// @file
/// Environment probes (EnvironmentProbeComponent): pictures of everything around points of
/// a scene, taken from the scene itself, which light what is near them from all around in
/// the place of the environment.
///
/// A probe is taken by drawing the scene six times from its point - once for each face of a
/// cube, kProbeFaceSize texels a side, through probeFaceView() - and copying each picture
/// into a cube map (copyFace(), which turns it the way a cube map is read). Of that come
/// two things, in the probe's slot: its reflections - the cube map blurred level by level
/// for rough surfaces, as an environment's is (makeLevels(), then
/// EnvironmentMaps::prefilterCube() into prepareRadiance()) - and nine spherical harmonics,
/// which a compute pass projects it onto, for matte ones (project()). SceneRenderer::
/// renderCaptures does that for the probes that need it (gather()); this class keeps what
/// it takes and what comes of it.
///
/// A view is given the probes that have been taken and touch it (viewList()), the highest
/// priority first. A surface takes from each probe whose box it is in as much as the probe's
/// weight there (probeWeight()) leaves of what the probes before it left, and from the
/// environment the rest. That arithmetic is written down twice, here and in
/// shaders/common/Environment.slang, and so is the projection (shaders/passes/
/// ProbeHarmonics.slang against SphericalHarmonics9): change both.
///
/// @code
/// CORDITE_TRY_ASSIGN(EnvironmentProbes probes, EnvironmentProbes::create(device, shaders));
///
/// // Once a frame, before any view:
/// for (const ProbeCapture& capture : probes.gather(world, resources.revision())) {
///     for (uint32_t face = 0; face < kCubeFaceCount; ++face) {
///         renderer.render(commandList, world, probeFaceView(capture.position, face), targets, captureSettings);
///         probes.copyFace(commandList, targets.sceneColor(), face);
///     }
///     probes.makeLevels(commandList);
///     nvrhi::ITexture* radiance = probes.prepareRadiance(commandList, capture.slot);
///     const bool reflects =
///         radiance != nullptr && environments.prefilterCube(commandList, probes.captureCube(), radiance, capture.slot);
///     probes.project(commandList, capture, reflects);
/// }
/// // For each view:
/// const std::vector<GpuEnvironmentProbe> lit = probes.viewList(world, cameraView);
/// @endcode

#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"
#include "cordite/math/Math.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/ComputePass.h"
#include "cordite/renderer/EnvironmentMaps.h"
#include "cordite/renderer/FullscreenPass.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/renderer/ShaderManager.h"
#include "cordite/world/World.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <vector>

namespace cordite {

/// The faces of a cube map, in the order of its layers: +X, -X, +Y, -Y, +Z, -Z.
inline constexpr uint32_t kCubeFaceCount = 6;

/// How many texels a side each face of a probe's picture has.
inline constexpr uint32_t kProbeFaceSize = 128;

/// How many levels the reflections of a probe have: the steps of roughness of an
/// environment's cube map, from a mirror to the roughest surface.
inline constexpr uint32_t kProbeLevels = kEnvironmentLevels;

/// What a probe does besides lighting matte surfaces (GpuEnvironmentProbe::flags): its
/// reflections are corrected for its box, ...
inline constexpr uint32_t kProbeBoxProjection = 1;
/// ... and it has reflections at all: one whose cube of reflections could not be made
/// leaves surfaces the environment's.
inline constexpr uint32_t kProbeReflects = 2;

/// The most probes a world has taken at once; more are left out, and said once.
inline constexpr uint32_t kMaxEnvironmentProbes = 32;

/// The most probes a view is lit by: those of the highest priority among the ones it sees.
inline constexpr uint32_t kMaxViewProbes = 16;

/// The most probes taken in one frame: each is six views, and the views of the few frames
/// on their way through the GPU share kMaxViewsInFlight (SceneRenderer.cpp adds them up).
/// One a frame also keeps a frame that takes a picture from taking much longer than the
/// others. The others are taken in the frames after.
inline constexpr uint32_t kMaxProbeCapturesPerFrame = 1;

/// A probe as the shaders read it: `EnvironmentProbe` in shaders/common/Environment.slang.
struct GpuEnvironmentProbe {
    /// From the world to the box's own space, in which the box is the cube from -0.5 to 0.5.
    Mat4 worldToBox{1.0f};
    /// How many metres one unit of the box's own space is, along each of its axes.
    Vec3 boxSize{1.0f};
    /// How far inside its faces the probe fades to nothing, in metres.
    float blendDistance = 0.0f;
    /// Where the picture was taken, in the world.
    Vec3 capturePosition{0.0f};
    /// Scales its light.
    float intensity = 1.0f;
    /// Where its nine harmonics are in the buffer of harmonics - from slot * 9 on - and its
    /// cube in the array of reflections.
    uint32_t slot = 0;
    /// kProbeBoxProjection and kProbeReflects.
    uint32_t flags = 0;
    uint32_t padding1 = 0;
    uint32_t padding2 = 0;
};

static_assert(sizeof(GpuEnvironmentProbe) == 112, "must match EnvironmentProbe in shaders/common/Environment.slang");

/// The probe of a component on an entity placed by `placement` (its world matrix), whose
/// harmonics are in `slot`. Numbers that are none count as nothing.
[[nodiscard]] GpuEnvironmentProbe gpuProbeOf(const Mat4& placement, const EnvironmentProbeComponent& component,
                                             uint32_t slot, bool reflects = true);

/// How much of the light from all around a point `position` takes from `probe`, before any
/// probe of a higher priority has taken its share: 1 inside the box and at least the blend
/// distance from each face, falling evenly to nothing at the faces; nothing outside.
[[nodiscard]] float probeWeight(const GpuEnvironmentProbe& probe, const Vec3& position);

/// Which way a surface at `position`, inside the box of `probe`, looks up in the probe's
/// picture what it reflects along the unit vector `direction`: with kProbeBoxProjection,
/// towards where the reflection meets the box, from where the picture was taken; without,
/// `direction` itself. probeReflectionDirection() in shaders/common/Environment.slang.
[[nodiscard]] Vec3 probeReflectionDirection(const GpuEnvironmentProbe& probe, const Vec3& position,
                                            const Vec3& direction);

/// The unit vector a place on face `face` of a cube map stands for: `uv` from (0, 0) at the
/// top left of the face to (1, 1). faceDirection() in shaders/common/CubeMap.slang.
[[nodiscard]] Vec3 cubeFaceDirection(uint32_t face, Vec2 uv);

/// The camera that takes face `face` of a probe's picture from `position`: a quarter turn
/// wide each way, square, looking along the face's direction. Its picture is the face seen
/// from inside the cube, which is the face as a cube map has it turned left to right: copy
/// it with EnvironmentProbes::copyFace().
[[nodiscard]] CameraView probeFaceView(const Vec3& position, uint32_t face);

/// A probe whose picture is to be taken.
struct ProbeCapture {
    Uuid id;
    /// Where its entity is in the world: where the picture is taken from.
    Vec3 position{0.0f};
    /// Where its harmonics go.
    uint32_t slot = 0;
};

class EnvironmentProbes {
public:
    /// Loads the shaders and makes the cube map the faces are copied into and the buffer of
    /// harmonics. Fails when either cannot be made.
    [[nodiscard]] static Result<EnvironmentProbes> create(nvrhi::IDevice* device, ShaderManager& shaders);

    /// The probes of `world` whose pictures are to be taken now, by the ids of their
    /// entities: those not taken yet, those whose entity has moved since, and all of them
    /// when what they were drawn with has been made anew - the assets read again, or the
    /// textures made another size (`resourceRevision` is RenderResources::revision()) - or
    /// capture() has been called - at most kMaxProbeCapturesPerFrame of them; the rest
    /// come in the calls after. The probes of entities that are gone or switched off are
    /// forgotten. After each, copyFace() for every face and project().
    [[nodiscard]] std::vector<ProbeCapture> gather(const World& world, uint64_t resourceRevision);

    /// Has the pictures of all probes taken again, from the next gather() on.
    void capture();

    /// Makes the cube map the faces of a probe are copied into, if there is none yet: it
    /// is made with the first probe that is taken. False when it cannot be made, and
    /// then no probe is taken.
    [[nodiscard]] bool prepareCapture();

    /// Copies the picture of face `face` of the probe being taken - `sceneColor`, the light
    /// a view of probeFaceView() was drawn with, kProbeFaceSize pixels a side - into the
    /// cube map. The command list must be open.
    void copyFace(nvrhi::ICommandList* commandList, nvrhi::ITexture* sceneColor, uint32_t face);

    /// Makes the smaller levels of the cube map the faces were copied into, each the
    /// average of the one before - which the prefilter reads to blur with few reads. The
    /// command list must be open.
    void makeLevels(nvrhi::ICommandList* commandList);

    /// The array of cube maps the reflections of the probes are drawn into, one a slot
    /// (EnvironmentMaps::prefilterCube()), made - or made twice as large, and what it held
    /// copied over - to have room for `slot`. Null when it cannot be made, which is said
    /// once. The command list must be open.
    [[nodiscard]] nvrhi::ITexture* prepareRadiance(nvrhi::ICommandList* commandList, uint32_t slot);

    /// The array of reflections as it is: null before the first probe.
    [[nodiscard]] nvrhi::ITexture* radiance() const { return m_radiance; }

    /// Projects the cube map onto the harmonics of `probe`, once its six faces are in, and
    /// counts it taken - with reflections when `reflects`, its cube in the array of
    /// reflections having been drawn. The command list must be open.
    void project(nvrhi::ICommandList* commandList, const ProbeCapture& probe, bool reflects);

    /// The probes that have been taken, of the enabled entities of `world`, whose boxes
    /// touch what `view` sees: the highest priority first - of the same, the lowest id -
    /// at most kMaxViewProbes.
    [[nodiscard]] std::vector<GpuEnvironmentProbe> viewList(const World& world, const CameraView& view) const;

    /// The harmonics of the probes: nine float4 a slot, the diffuse coefficients of
    /// SphericalHarmonics9 in rgb.
    [[nodiscard]] nvrhi::IBuffer* harmonics() const { return m_harmonics; }

    /// The cube map the faces of the probe being taken are copied into: kProbeFaceSize a
    /// side, of light, with all its smaller levels. Null before prepareCapture().
    [[nodiscard]] nvrhi::ITexture* captureCube() const { return m_captureCube; }

    /// How many pictures have been taken so far.
    [[nodiscard]] uint32_t captureCount() const { return m_captureCount; }

    /// How many probes are known: those the last gather() found.
    [[nodiscard]] uint32_t count() const { return static_cast<uint32_t>(m_states.size()); }

    /// How many probes have been left out, because a world had more than
    /// kMaxEnvironmentProbes, in the last gather().
    [[nodiscard]] uint32_t dropped() const { return m_dropped; }

    /// The memory of the cube map, the array of reflections and the buffer, in bytes.
    [[nodiscard]] uint64_t byteSize() const;

private:
    /// What is known of the probe of one entity.
    struct State {
        /// The id of the entity.
        Uuid id;
        uint32_t slot = 0;
        /// Where its picture was taken from, and with which resources; not taken while false.
        bool taken = false;
        /// Whether its cube of reflections was drawn.
        bool reflects = false;
        Vec3 takenAt{0.0f};
        uint64_t resourceRevision = 0;
        /// Asked to be taken again (capture()); lights with its old picture until it is.
        bool retake = false;
    };

    /// What create() makes, handed to the constructor in one piece.
    struct Parts {
        nvrhi::IDevice* device;
        nvrhi::BufferHandle harmonics;
        nvrhi::BufferHandle constants;
        nvrhi::BindingLayoutHandle copyLayout;
        nvrhi::BindingLayoutHandle projectLayout;
        FullscreenPass copy;
        FullscreenPass downsample;
        ComputePass project;
    };

    explicit EnvironmentProbes(Parts parts);

    nvrhi::IDevice* m_device = nullptr;
    nvrhi::TextureHandle m_captureCube;
    nvrhi::BufferHandle m_harmonics;
    nvrhi::BufferHandle m_constants;
    nvrhi::BindingLayoutHandle m_copyLayout;
    nvrhi::BindingLayoutHandle m_projectLayout;
    FullscreenPass m_copy;
    FullscreenPass m_downsample;
    ComputePass m_project;
    nvrhi::TextureHandle m_radiance;
    /// How many cubes m_radiance has room for.
    uint32_t m_radianceCubes = 0;
    bool m_reportedRadiance = false;
    /// One for each probe the last gather() found: a world has few.
    std::vector<State> m_states;
    /// The slots no probe holds.
    std::vector<uint32_t> m_freeSlots;
    /// The resources of the last gather(), which the pictures it asks for are taken with.
    uint64_t m_resourceRevision = 0;
    uint32_t m_captureCount = 0;
    uint32_t m_dropped = 0;
    bool m_reportedDropped = false;
};

} // namespace cordite
