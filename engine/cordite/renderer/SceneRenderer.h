#pragma once

/// @file
/// Renders a world as a camera sees it.
///
/// The scene renderer is what turns entities with a MeshRenderer into a picture: it
/// finds the ones in view, makes sure their meshes and materials are on the GPU, and
/// takes them through its passes into a SceneTargets.
///
/// @code
/// CORDITE_TRY_ASSIGN(auto sceneRenderer, SceneRenderer::create(device, shaders, assets));
/// CORDITE_TRY_ASSIGN(SceneTargets targets, SceneTargets::create(device, {1280, 720}));
///
/// world.updateWorldTransforms();
/// const Entity camera = findPrimaryCamera(world);
/// const CameraView view = makeCameraView(world, camera, 16.0f / 9.0f);
/// sceneRenderer->render(commandList, world, view, targets);
/// show(targets.display());
/// @endcode
///
/// The passes of a view, in order:
///
/// | Pass              | Draws                                    | Into                  |
/// |-------------------|------------------------------------------|-----------------------|
/// | Shadow pass       | What casts shadows, as the lights see it | the shadow atlas      |
/// | Depth pre-pass    | Solid surfaces: opaque and masked        | depth                 |
/// | Base pass         | The same, where they are the nearest     | the G-buffer          |
/// | Decals            | The box of each decal (DecalPass.h)      | the G-buffer, over it |
/// | Ambient occlusion | Two triangles (AmbientOcclusionPass.h)   | ambient occlusion     |
/// | Deferred lighting | One triangle over the whole target       | scene color           |
/// | Forward pass      | Translucent surfaces, far to near        | scene color, over it  |
/// | Light shafts      | A few triangles (LightShaftPass.h)       | scene color, added    |
/// | Depth of field    | A few triangles (DepthOfFieldPass.h)     | scene color           |
/// | Motion blur       | A few triangles (MotionBlurPass.h)       | scene color           |
/// | Post-processing   | A few triangles (PostProcessPass.h)      | the display image     |
///
/// Scene color is light, without an upper limit; post-processing makes the picture of
/// it that a screen shows, as the PostProcess components of the world say
/// (ScenePostProcess.h, PostProcessSettings.h). A view that is not to be finished
/// (PostProcessQuality::Off) is only encoded for a display, and so is every debug view
/// (RenderDebugView), which replaces the lighting, or the passes altogether, by a
/// picture of something the renderer works with.
///
/// One scene renderer serves any number of views; give each view its own SceneTargets.
/// A scene is lit by its Light and AmbientLight components (RenderComponents.h): the
/// directional lights everywhere, the point, spot and rect lights where they reach, found
/// through a grid of clusters (LightClusters.h). Lights that cast shadows get shadow
/// maps in an atlas (ShadowPlan.h), drawn anew for every view. Its Environment is the
/// image of everything far away: it lights every surface, and the deferred lighting
/// draws it as the sky where nothing else was drawn (EnvironmentMaps.h).
/// Its Decal components lay materials onto what the base pass left in the G-buffer,
/// before anything reads it (Decals.h, DecalPass.h).
///
/// A skinned mesh is drawn in a pose: that of its own entity, or of the nearest entity
/// above it that has one (SkeletonPoseComponent, which AnimationSystem works out from an
/// Animator) - so the meshes of a figure hang under what animates it and move as one.
/// The pose moves the vertices where the mesh was modelled; the entity of the mesh then
/// places the result like any other mesh. Without a pose, and with one that has fewer
/// joints than the mesh follows, a skinned mesh is drawn as it was modelled. What is in
/// view and what casts a shadow is decided by where the pose puts the mesh.

#include "cordite/assets/AssetManager.h"
#include "cordite/core/Base.h"
#include "cordite/core/Result.h"
#include "cordite/math/Math.h"
#include "cordite/renderer/AmbientOcclusionPass.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/DebugViewPass.h"
#include "cordite/renderer/DecalPass.h"
#include "cordite/renderer/Decals.h"
#include "cordite/renderer/DeferredLightingPass.h"
#include "cordite/renderer/DepthOfFieldPass.h"
#include "cordite/renderer/DisplayEncodePass.h"
#include "cordite/renderer/EnvironmentMaps.h"
#include "cordite/renderer/EnvironmentProbes.h"
#include "cordite/renderer/LevelOfDetail.h"
#include "cordite/renderer/LightClusters.h"
#include "cordite/renderer/LightShaftPass.h"
#include "cordite/renderer/MeshDrawList.h"
#include "cordite/renderer/MeshPass.h"
#include "cordite/renderer/MotionBlurPass.h"
#include "cordite/renderer/MotionVectors.h"
#include "cordite/renderer/PostProcessPass.h"
#include "cordite/renderer/PostProcessSettings.h"
#include "cordite/renderer/RenderDebugView.h"
#include "cordite/renderer/RenderResources.h"
#include "cordite/renderer/SceneLights.h"
#include "cordite/renderer/ScenePostProcess.h"
#include "cordite/renderer/SceneTargets.h"
#include "cordite/renderer/ScreenEffects.h"
#include "cordite/renderer/ScreenReflectionPass.h"
#include "cordite/renderer/ScreenReflections.h"
#include "cordite/renderer/ShaderManager.h"
#include "cordite/renderer/ShadowAtlas.h"
#include "cordite/renderer/ShadowPlan.h"
#include "cordite/renderer/SkinPalettes.h"
#include "cordite/renderer/ViewBindings.h"
#include "cordite/renderer/VolumetricFog.h"
#include "cordite/renderer/VolumetricFogPass.h"
#include "cordite/world/Entity.h"
#include "cordite/world/World.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace cordite {

struct SceneRendererDesc {
    RenderResourcesDesc resources;

    /// Whether the adapter can draw the edges of triangles
    /// (AdapterInfo::supportsWireframe). Where it cannot, the wireframe debug view shows
    /// the lit picture instead, and says so in the log.
    bool supportsWireframe = true;
};

/// What a view shows of a scene besides the scene itself.
struct SceneRenderSettings {
    /// What shows where nothing is drawn. Linear color.
    Vec3 backgroundColor{0.012f, 0.014f, 0.018f};

    /// Light a world that has no light components at all with a key light and soft
    /// ambient light, so that what is in it can be seen (SceneLightsDesc). An editor
    /// does, for a scene that is being started; a game shows what its scene says.
    bool defaultLighting = false;

    /// How much the shadows of the view may cost: the size of the atlas they share and
    /// of the maps in it, and how softly their edges are filtered. Off draws none.
    /// Views that one renderer draws in turn are best given the same quality: the
    /// renderer has one atlas, and makes it anew whenever its size changes.
    ShadowQuality shadowQuality = ShadowQuality::Medium;

    /// How much the effects that are worked out from the screen may cost: ambient
    /// occlusion, so far (ScreenEffects.h). Off draws none. What they look like is
    /// part of the look of the scene, like the finishing of the picture below.
    ScreenEffectsQuality screenEffectsQuality = ScreenEffectsQuality::Medium;

    /// What to show instead of the finished picture; None shows the picture.
    RenderDebugView debugView = RenderDebugView::None;

    /// How large the error of a level of detail may look, in pixels (LevelOfDetail.h): a
    /// mesh is drawn at the coarsest of its levels whose surface is no further than this
    /// from its own. Zero draws every mesh at its finest.
    float lodErrorPixels = 1.0f;

    /// How much finishing the picture - exposing, tone mapping, grading it and the rest
    /// of PostProcessSettings.h - may cost. Off shows the light as it was worked out,
    /// with whatever is brighter than white cut off. Debug views are never finished:
    /// they show numbers.
    PostProcessQuality postProcessQuality = PostProcessQuality::Medium;
    /// How the edges of the finished picture are smoothed. How finely goes by the
    /// quality above; a picture that is not finished is not anti-aliased either.
    AntiAliasing antiAliasing = AntiAliasing::Fxaa;
    /// Whether what moved since the view was last drawn is blurred, as much as the look
    /// of the scene asks (PostProcessSettings::motionBlur). A game's views are; an
    /// editor's view through its own camera is not: that camera moves to look around,
    /// not as part of what is shown.
    bool motionBlur = true;
    /// Whether the environment probes of the world light the view where their boxes are
    /// (EnvironmentProbes.h). Their own pictures are taken without them: light bounces
    /// once.
    bool environmentProbes = true;
    /// Whether what the reflections of surfaces meet in the picture takes the place of the
    /// surroundings' reflections, as much as the look of the scene and the screen effects
    /// quality say (ScreenReflections.h). The pictures of probes are taken without them.
    bool screenReflections = true;
    /// What the picture is finished with, and what its screen effects look like. Left
    /// empty, the PostProcess components of the world say (ScenePostProcess.h). A tool
    /// or a test that wants one look whatever the scene says sets it here.
    std::optional<PostProcessSettings> postProcess;
    /// The entity whose camera the view is seen through, if it is one: a PostProcess
    /// component on a camera is for the views through that camera alone.
    Uuid viewEntity;
    /// How long ago the view was last drawn, in seconds, and the time: for what takes
    /// time - automatic exposure getting used to the light - or changes with it, as
    /// film grain does. A view that is drawn once leaves them alone. Where no time has
    /// passed, nothing has moved either: what changed since is no motion
    /// (MotionVectors.h), and is not blurred.
    float deltaSeconds = 1.0f / 60.0f;
    double timeSeconds = 0.0;
};

/// The most pictures of SceneCapture components a frame draws: each is a view, and the
/// views of the few frames on their way through the GPU share kMaxViewsInFlight.
inline constexpr uint32_t kMaxSceneCaptures = 4;

/// The most views a program may draw of its own in a frame, for the same reason: a game's
/// one, or the viewports of an editor and the preview of an asset.
inline constexpr uint32_t kMaxProgramViews = 5;

/// One word for everything a view may cost: what a player picks in the options of a
/// game. The look is the same at every quality; what changes is how finely it is
/// worked out.
enum class RenderQuality : uint8_t {
    Low,
    Medium,
    High,
};

inline constexpr std::array<RenderQuality, 3> kRenderQualities = {RenderQuality::Low, RenderQuality::Medium,
                                                                  RenderQuality::High};

/// The name used in files and by tools: "low", "medium", "high".
std::string_view nameOf(RenderQuality quality);

/// The name shown to people: "Low", "Medium", "High".
std::string_view displayNameOf(RenderQuality quality);

/// The quality with the given nameOf(), or nothing. Upper and lower case are not told
/// apart.
[[nodiscard]] std::optional<RenderQuality> renderQualityFromName(std::string_view name);

/// Sets what `quality` stands for - the quality of the shadows, of the screen effects
/// and of the finishing of the picture - and leaves everything else in `settings` as
/// it is.
void applyRenderQuality(RenderQuality quality, SceneRenderSettings& settings);

/// The quality `settings` are at, or nothing when its parts were set one by one to what
/// no single quality has.
[[nodiscard]] std::optional<RenderQuality> renderQualityOf(const SceneRenderSettings& settings);

/// The largest side, in texels, that a texture keeps at `quality`: 1024 at a low quality,
/// 2048 at a medium one, 4096 at a high one (RenderResources::setTextureLimit, TextureBudget.h).
/// It is the program's, not a view's: every view of a program shares its textures.
[[nodiscard]] uint32_t textureLimitFor(RenderQuality quality);

/// What SceneRenderer::render() did: in its last call, or in all the calls of a frame.
struct SceneRenderStatistics {
    /// The calls these numbers cover: one view each.
    uint32_t views = 0;
    /// Enabled entities with a mesh that could be loaded.
    uint32_t meshRenderers = 0;
    /// Of those, the ones outside the view, which were not drawn.
    uint32_t culled = 0;
    /// The meshes in view, by the level of detail they are drawn at: the mesh itself
    /// first.
    std::array<uint32_t, kMaxMeshLods + 1> lodInstances{};
    /// Of the mesh renderers, the ones whose mesh is skinned and was given a pose.
    uint32_t skinnedMeshes = 0;
    /// The joint matrices of the poses they are drawn in, sent to the GPU. A pose that
    /// several meshes are drawn in counts once; the matrices of the pose a figure was in
    /// when the view was last drawn, which go along for the motion of one that moved, are
    /// not counted.
    uint32_t jointMatrices = 0;
    /// Passes that draw over the whole picture, or over a smaller copy of it, or work over a
    /// grid laid on it, rather than draw meshes: the two of the ambient occlusion, the two
    /// of what lights throw into the fog, the lighting or the debug view, and what finishes
    /// the picture - the levels of the bloom down and up, the exposure, the final pass, the
    /// anti-aliasing.
    uint32_t screenPasses = 0;

    /// The lights that reach something in view: directional ones, and point, spot and
    /// rect lights that touch at least one cluster.
    uint32_t lights = 0;
    /// The entries of all the clusters' lists together: how much the lights overlap.
    uint32_t lightListEntries = 0;
    /// Lights left out because a view can only hold so many (kMaxDirectionalLights,
    /// kMaxLocalLights).
    uint32_t droppedLights = 0;

    /// The decals laid onto the G-buffer: those in view whose material could be used.
    uint32_t decals = 0;
    /// Decals left out because a view can only hold so many (kMaxDecals).
    uint32_t droppedDecals = 0;
    /// How often the normals of the G-buffer were copied for the decals to read: once for
    /// a view with decals, and again for each decal that comes to where one since bent
    /// them (DecalPass).
    uint32_t decalNormalCopies = 0;
    /// The memory of the copy of the normals the decals read, in bytes
    /// (SceneTargets::prepareDecalNormals). Over the views of a frame it is the largest of
    /// them.
    uint64_t decalBytes = 0;

    /// Pictures drawn by SceneCapture components (renderCaptures()), each a view of its
    /// own in the numbers here; and captures left out because a frame draws only so many
    /// (kMaxSceneCaptures).
    uint32_t captures = 0;
    uint32_t droppedCaptures = 0;

    /// The shadow maps drawn: one for a spot light, six for a point or a rect light, one
    /// for each cascade of the sun.
    uint32_t shadowMaps = 0;
    /// Lights that are to cast shadows but got no shadow map: the atlas was full, or
    /// the light is a directional one that is not the brightest
    /// (ShadowPlan::lightsWithoutRoom).
    uint32_t lightsWithoutShadows = 0;
    /// The memory of the shadow atlas on the GPU, in bytes, whether the view drew into
    /// it or not. The views of a renderer share the one atlas, so this is not summed
    /// up over the views of a frame: it is the largest the atlas was.
    uint64_t shadowAtlasBytes = 0;

    /// The cube maps made of environments: one the first time a view is lit by an
    /// environment, and none after that until its image changes. More than that, frame
    /// after frame, means environments are taking turns that should not.
    uint32_t environmentsMade = 0;
    /// The memory of the cube maps of the environments held, in bytes
    /// (kEnvironmentBytes each). The views of a renderer share them, so this is not
    /// summed up over the views of a frame: it is the most there was.
    uint64_t environmentBytes = 0;
    /// How many environment probes had their pictures taken (renderCaptures): six views
    /// each, which `views` counts as well.
    uint32_t probeCaptures = 0;
    /// The memory of what the pictures of the probes are taken and kept in, in bytes
    /// (EnvironmentProbes::byteSize()). Like that of the environments it is not summed up
    /// over the views of a frame.
    uint64_t probeBytes = 0;

    /// The memory of the image the ambient occlusion of the view is worked out in, in
    /// bytes: two half floats for every pixel, or for one in four
    /// (SceneTargets::tracedOcclusion; the result is another of the view's targets, and
    /// not in this number). Over the views of a frame it is the largest of them.
    uint64_t ambientOcclusionBytes = 0;
    /// The memory of the two images the depth of field of the view is worked out in, in
    /// bytes: four half floats for every block of four pixels, twice
    /// (SceneTargets::prepareDepthOfField). Over the views of a frame it is the largest
    /// of them.
    uint64_t depthOfFieldBytes = 0;
    /// The memory of the two images the light shafts of the view are worked out in, in
    /// bytes: four half floats for every block of four pixels, twice
    /// (SceneTargets::prepareLightShafts). Over the views of a frame it is the largest of
    /// them.
    uint64_t lightShaftBytes = 0;
    /// The memory of the two images the reflections from the screen of the view are drawn
    /// with, in bytes: a copy of the scene color and what was found, eight bytes a texel
    /// each (SceneTargets::prepareScreenReflections). Over the views of a frame it is the
    /// largest of them.
    uint64_t screenReflectionBytes = 0;
    /// The memory of the two volumes what lights throw into the fog of the view is worked
    /// out in, in bytes: four half floats for every box of its grid, twice
    /// (SceneTargets::prepareFogVolume). Over the views of a frame it is the largest of them.
    uint64_t fogVolumeBytes = 0;
    /// The memory of the images the motion blur of the view is worked out in, in bytes: a
    /// copy of the picture, four half floats a pixel, and two of two half floats a tile
    /// (SceneTargets::prepareMotionBlur). Over the views of a frame it is the largest of
    /// them.
    uint64_t motionBlurBytes = 0;

    /// What each of the passes that draw meshes did. A debug view that draws the meshes
    /// itself - wireframe, overdraw - counts as the forward pass.
    MeshPassStatistics shadowPass;
    MeshPassStatistics depthPass;
    MeshPassStatistics basePass;
    MeshPassStatistics forwardPass;
    /// All of them together: what the GPU was asked to draw.
    MeshPassStatistics meshes;

    SceneRenderStatistics& operator+=(const SceneRenderStatistics& other);
};

class SceneRenderer {
public:
    /// The device, the shader manager and the asset manager must outlive the renderer.
    [[nodiscard]] static Result<std::unique_ptr<SceneRenderer>>
    create(nvrhi::IDevice* device, ShaderManager& shaders, AssetManager& assets, const SceneRendererDesc& desc = {});
    ~SceneRenderer();
    CORDITE_NON_MOVABLE(SceneRenderer);

    /// Draws `world` as seen through `view` into `targets`: the G-buffer and depth, from
    /// those the scene color, and from that the display image. The command list must be
    /// open, the world transforms up to date (World::updateWorldTransforms), and `view`
    /// made for the aspect ratio of `targets`. The targets are the view's memory: the
    /// exposure it has got used to is read from them and left there for its next frame.
    ///
    /// Assets seen for the first time are loaded and sent to the GPU here, which makes
    /// the first frame of a scene slower than the ones that follow. One that cannot be
    /// loaded is reported in the log and left out; rendering itself does not fail for it.
    void render(nvrhi::ICommandList* commandList, const World& world, const CameraView& view, SceneTargets& targets,
                const SceneRenderSettings& settings = {});

    /// Takes the pictures of the environment probes of `world` that need them
    /// (EnvironmentProbes::gather()), as the scene is lit without probes and otherwise as
    /// `settings` say, unfinished; then draws the pictures of the SceneCapture components of
    /// `world` into their render
    /// targets (RenderResources::renderTarget()): each as the Camera of its entity sees the
    /// world, with the aspect of its target, finished as a game's view is with `settings` -
    /// kMaxSceneCaptures at the most, by the ids of their entities. Call it before the
    /// views of the frame that may show the pictures, with what render() needs. A capture
    /// whose interval has not passed since its last picture keeps it; one without a camera
    /// is reported once. Each picture is a view in the statistics of the frame.
    void renderCaptures(nvrhi::ICommandList* commandList, const World& world, const SceneRenderSettings& settings);

    /// Has the pictures of all environment probes taken again, by the next calls to
    /// renderCaptures(): what is around them has changed.
    void retakeProbes();

    /// The environment probes: what their pictures are taken in, and their harmonics.
    [[nodiscard]] const EnvironmentProbes& probes() const { return m_passes.probes; }

    /// What the last call to render() did.
    [[nodiscard]] const SceneRenderStatistics& statistics() const { return m_statistics; }

    /// What every call to render() since the last call to this function did, added up,
    /// and starts counting afresh. The engine takes it once per frame: with several
    /// views, a frame costs what they cost together.
    [[nodiscard]] SceneRenderStatistics takeFrameStatistics();

    /// The GPU copies of the assets drawn so far.
    [[nodiscard]] const RenderResources& resources() const { return *m_resources; }

    [[nodiscard]] RenderResources& resources() { return *m_resources; }

private:
    /// The passes of the renderer.
    struct Passes {
        MeshPass depth;
        MeshPass base;
        MeshPass forward;
        MeshPass wireframe;
        MeshPass overdraw;
        MeshPass lod;
        DeferredLightingPass lighting;
        DebugViewPass debugView;
        DisplayEncodePass encode;
        ShadowAtlas shadows;
        EnvironmentMaps environments;
        DecalPass decals;
        AmbientOcclusionPass occlusion;
        VolumetricFogPass fogLight;
        EnvironmentProbes probes;
        LightShaftPass lightShafts;
        ScreenReflectionPass screenReflections;
        DepthOfFieldPass depthOfField;
        MotionBlurPass motionBlur;
        PostProcessPass postProcess;
    };

    /// The pose a skinned mesh is drawn in: where its joint matrices are among those
    /// of the view, where those of the pose it was in when the view was last drawn are,
    /// and the matrices themselves. kNoPalette and none without a pose.
    struct MeshPose {
        uint32_t palette = kNoPalette;
        uint32_t previousPalette = kNoPalette;
        std::span<const Mat4> skinning;
    };

    /// Where the joint matrices of a pose are among those of the view: of the pose now,
    /// and of the pose it was in when the view was last drawn.
    struct PosePalettes {
        uint32_t current = kNoPalette;
        uint32_t previous = kNoPalette;
    };

    /// A mesh renderer of the world: where it is, and which of m_sceneDraws draw it.
    struct SceneMesh {
        Aabb bounds;
        uint32_t firstDraw = 0;
        uint32_t drawCount = 0;
        uint32_t lightChannels = 1;
        bool castsShadows = true;
    };

    SceneRenderer(nvrhi::IDevice* device, std::unique_ptr<RenderResources> resources, ViewBindings viewBindings,
                  SkinPalettes palettes, Passes passes, bool supportsWireframe);

    /// Fills m_sceneMeshes and m_sceneDraws with everything `world` draws, m_draws
    /// with what of it shows through `view`, each mesh at the level of detail `lodView`
    /// chooses for it, and counts. Each draw is placed and posed as it was in `before` -
    /// what the view remembers of its last picture - where that has it; m_placements and
    /// m_poses get what the next picture will remember.
    void collectDraws(const World& world, const CameraView& view, const LodView& lodView, const MotionHistory* before);

    /// The pose the skinned mesh `mesh` of `entity` is drawn in, its matrices - and
    /// those of the pose it was in before, where `before` has another - added to
    /// m_palettes the first time the pose is asked for. `meshId` names the mesh where a
    /// pose that does not fit it is reported.
    [[nodiscard]] MeshPose poseFor(const World& world, Entity entity, const GpuMesh& mesh, const Uuid& meshId,
                                   const MotionHistory* before);

    /// Sends the poses collectDraws() gathered to the GPU. Where that fails, no draw
    /// keeps its pose.
    void uploadPoses(nvrhi::ICommandList* commandList);

    /// Gives every light of m_clusters the layer of its cookie in the cookie array
    /// (RenderResources::cookieLayer): the first time a cookie is used, it is put there.
    void assignCookies();

    /// The grid of the boxes over the view that what lights throw into its fog is worked out
    /// for, with the volumes of `targets` made for it (SceneTargets::prepareFogVolume) - or
    /// none, with the volumes let go of, where nothing of that is to be seen: a fog that no
    /// light lights, a quality of the view that has none, a debug view that shows no light.
    /// After the lights of the view are known (m_lights, m_clusters).
    /// Takes the pictures of the probes gather() asks for, before any view is drawn.
    void captureProbes(nvrhi::ICommandList* commandList, const World& world, const SceneRenderSettings& settings);

    /// The probes that light a view: none where the settings say so.
    [[nodiscard]] std::vector<GpuEnvironmentProbe> viewProbes(const World& world, const CameraView& view,
                                                              const SceneRenderSettings& settings) const;

    [[nodiscard]] FogVolumeGrid prepareFogLight(SceneTargets& targets, const SceneRenderSettings& settings,
                                                RenderDebugView debugView);

    /// Draws the shadow maps of m_shadowPlan into the atlas, each with what casts a
    /// shadow in the light it belongs to.
    void drawShadows(nvrhi::ICommandList* commandList, const ShadowSettings& settings);

    /// Makes the display image of `targets` of the light in their scene color, seen from
    /// `view`: with the shafts of the sun added, blurred where it is out of focus and
    /// where it moved - when the picture `continues` from one before -, and finished with
    /// `look`; or only encoded, in a debug view and where `settings` ask for no
    /// finishing. In the debug views of the light shafts and of the depth of field, what
    /// they are about takes the place of the light first.
    void finishPicture(nvrhi::ICommandList* commandList, SceneTargets& targets, nvrhi::IBindingSet* viewBindings,
                       const CameraView& view, const PostProcessSettings& look, const SceneRenderSettings& settings,
                       RenderDebugView debugView, bool continues);

    /// Lays the decals of the scene that `view` sees onto what the base pass left in the
    /// G-buffer of `targets`; gives back what they read when none are drawn.
    void drawDecals(nvrhi::ICommandList* commandList, SceneTargets& targets, nvrhi::IBindingSet* viewBindings,
                    const CameraView& view);

    /// Adds the light shafts of the scene's sun to the scene color of `targets`, as `look`
    /// says, or - in their debug view - puts them in its place; gives back the images they
    /// are worked out in when the view has none.
    /// Puts what the reflections of the view's surfaces meet in its picture in the place of
    /// the surroundings' reflections - or shows it, in the debug view of its own - after the
    /// lighting and before anything is drawn over it.
    /// Whether anything the view lays into its G-buffer - a solid mesh, or a decal on one -
    /// can be smoother than `maxRoughness`: without, reflections from the screen change
    /// nothing, and are not drawn.
    [[nodiscard]] bool hasSurfaceSmootherThan(float maxRoughness) const;

    void drawScreenReflections(nvrhi::ICommandList* commandList, SceneTargets& targets,
                               nvrhi::IBindingSet* viewBindings, const PostProcessSettings& look,
                               const SceneRenderSettings& settings, RenderDebugView debugView);

    void drawLightShafts(nvrhi::ICommandList* commandList, SceneTargets& targets, nvrhi::IBindingSet* viewBindings,
                         const CameraView& view, const PostProcessSettings& look, bool isFinished,
                         RenderDebugView debugView);

    nvrhi::IDevice* m_device;
    std::unique_ptr<RenderResources> m_resources;
    ViewBindings m_viewBindings;
    /// The joint matrices of the view being drawn, and where those of each pose are in
    /// them, by the entity that has the pose.
    SkinPalettes m_palettes;
    std::unordered_map<Entity, PosePalettes> m_paletteOfPose;
    /// What the view being drawn is to remember of its picture (MotionHistory): where
    /// every mesh renderer placed its mesh, and the poses meshes were drawn in, by the ids
    /// of their entities. Handed to the view's targets when it is drawn.
    std::vector<std::pair<Uuid, Mat4>> m_placements;
    std::vector<std::pair<Uuid, std::vector<Mat4>>> m_poses;
    /// The meshes that were given a pose that does not fit them, so that each is
    /// reported once, not every frame.
    std::unordered_set<Uuid> m_reportedPoses;
    MeshDrawList m_drawList;
    Passes m_passes;
    bool m_supportsWireframe;
    /// So that the missing wireframe view is reported once, not every frame.
    bool m_reportedWireframe = false;

    /// The lights of the view being drawn. Members so that their memory is reused.
    SceneLights m_lights;
    SceneDecals m_decals;
    LightClusterBuilder m_clusterBuilder;
    LightClusters m_clusters;
    /// So that lights that were left out are reported once, not every frame.
    bool m_reportedDroppedLights = false;
    bool m_reportedFogLight = false;
    /// What the faces of the probes are drawn into, made with the first of them.
    SceneTargets m_probeTargets;
    bool m_reportedProbes = false;
    bool m_reportedDroppedDecals = false;

    /// What a SceneCapture keeps from one frame to the next: the targets of its view, and
    /// how long ago its last picture was drawn.
    struct CaptureView {
        SceneTargets targets;
        float sinceLastPicture = 0.0f;
        bool hasPicture = false;
        /// The last call to renderCaptures() that found the capture: one that is no
        /// longer found lets go of its targets.
        uint64_t seen = 0;
    };

    std::unordered_map<Uuid, CaptureView> m_captureViews;
    uint64_t m_captureCalls = 0;
    /// Captures already reported for having no camera, or no targets that could be made.
    std::unordered_set<Uuid> m_reportedCaptures;
    bool m_reportedDroppedCaptures = false;

    /// The shadow maps of the view being drawn, and a draw list for each of them: a
    /// list owns the buffer its instances are in.
    ShadowPlan m_shadowPlan;
    std::vector<MeshDrawList> m_shadowDrawLists;
    /// So that shadows that could not be drawn are reported once, not every frame.
    bool m_reportedShadowRoom = false;
    bool m_reportedShadowAtlas = false;

    SceneRenderStatistics m_statistics;
    SceneRenderStatistics m_frameStatistics;
    /// Rebuilt every call; members so that their memory is reused: everything the
    /// world draws, what of it the camera sees, and what one light sees of it.
    std::vector<SceneMesh> m_sceneMeshes;
    std::vector<MeshDraw> m_sceneDraws;
    std::vector<MeshDraw> m_draws;
    std::vector<MeshDraw> m_shadowDraws;
};

} // namespace cordite
