#include "cordite/editor/AssetPreview.h"

#include "cordite/assets/Material.h"
#include "cordite/assets/Mesh.h"
#include "cordite/assets/Prefab.h"
#include "cordite/editor/PrefabLinks.h"
#include "cordite/engine/ModelInstance.h"
#include "cordite/math/Color.h"
#include "cordite/renderer/CameraView.h"
#include "cordite/renderer/PostProcessSettings.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/renderer/RenderResources.h"
#include "cordite/world/Components.h"
#include "cordite/world/Prefab.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <utility>

namespace cordite {
namespace {

/// From in front and above, a little from the side: as a thing is shown in a catalogue.
constexpr float kYawDegrees = 35.0f;
constexpr float kPitchDegrees = -25.0f;
/// Narrow, so that what is near does not swell.
constexpr float kFieldOfViewDegrees = 30.0f;
/// Room around what is seen.
constexpr float kMargin = 1.1f;
/// Behind what is drawn: a gray that neither black nor white things vanish into. A color
/// of the display.
constexpr Vec3 kBackground(0.22f, 0.23f, 0.25f);

} // namespace

bool AssetPreview::hasPicture(AssetType type) {
    return type == AssetType::Texture || isDrawn(type);
}

bool AssetPreview::isDrawn(AssetType type) {
    return type == AssetType::Mesh || type == AssetType::Material || type == AssetType::Model ||
           type == AssetType::Prefab;
}

Status AssetPreview::show(SceneRenderer& renderer, nvrhi::IDevice* device, nvrhi::ICommandList* commandList,
                          AssetManager& assets, const ComponentRegistry& components, const Uuid& asset, uint32_t size,
                          ShadowQuality shadows) {
    const uint64_t revision = assets.database().revision();
    if (m_asked && asset == m_asset && size == m_size && revision == m_revision) {
        // As it went: a picture, or what kept it from being one - tried once, not in every frame.
        if (m_failure) {
            return tl::unexpected(*m_failure);
        }
        return {};
    }
    m_asked = true;
    m_asset = asset;
    m_size = size;
    m_revision = revision;
    m_picture = nullptr;
    m_failure.reset();
    ++m_attempts;
    const Status made = make(renderer, device, commandList, assets, components, shadows);
    if (!made) {
        m_picture = nullptr;
        m_failure = made.error();
    }
    return made;
}

Status AssetPreview::make(SceneRenderer& renderer, nvrhi::IDevice* device, nvrhi::ICommandList* commandList,
                          AssetManager& assets, const ComponentRegistry& components, ShadowQuality shadows) {
    const AssetInfo* asset = assets.database().find(m_asset);
    if (asset == nullptr) {
        return makeError(ErrorCode::NotFound, "there is no asset {}", m_asset.toString());
    }
    if (!hasPicture(asset->type)) {
        return makeError(ErrorCode::InvalidArgument, "{} is {}, which has no picture", asset->reference(),
                         withArticle(asset->type));
    }
    if (m_size < kMinSize || m_size > kMaxSize) {
        return makeError(ErrorCode::InvalidArgument, "a picture is from {} to {} pixels square, not {}", kMinSize,
                         kMaxSize, m_size);
    }

    if (asset->type == AssetType::Texture) {
        // Loaded first, for the reason when it cannot be: the renderer only says so in the log.
        CORDITE_TRY(assets.loadTexture(m_asset));
        m_picture = renderer.resources().colorTexture(m_asset);
        if (m_picture == nullptr) {
            return makeError(ErrorCode::IoError, "{} could not be made ready for the GPU", asset->reference());
        }
        // Sent now: nothing else is drawn before the interface shows it, and the scene of
        // this frame, which would send it, may have been drawn already.
        renderer.resources().flushUploads();
        return {};
    }

    CORDITE_TRY(compose(assets, components, *asset));
    if (!m_targets.isValid()) {
        CORDITE_TRY_ASSIGN(m_targets, SceneTargets::create(device, UVec2(m_size), "Asset preview"));
    } else {
        CORDITE_TRY(m_targets.resize(device, UVec2(m_size)));
    }
    draw(renderer, commandList, assets, shadows);
    m_picture = m_targets.display();
    return {};
}

Status AssetPreview::compose(AssetManager& assets, const ComponentRegistry& components, const AssetInfo& asset) {
    m_world.clear();
    if (asset.type == AssetType::Mesh || asset.type == AssetType::Material) {
        // Loaded first, for the reason when it cannot be: the renderer leaves out quietly
        // what it cannot draw.
        MeshRendererComponent drawn;
        if (asset.type == AssetType::Mesh) {
            CORDITE_TRY(assets.loadMesh(asset.id));
            drawn.mesh = asset.id;
        } else {
            CORDITE_TRY(assets.loadMaterial(asset.id));
            drawn.mesh = builtinMeshId(BuiltinMesh::Sphere);
            drawn.materials = {asset.id};
        }
        m_world.add<MeshRendererComponent>(m_world.createEntity(asset.name)) = std::move(drawn);
    } else if (asset.type == AssetType::Model) {
        CORDITE_TRY_ASSIGN(const std::shared_ptr<const ModelData> model, assets.loadModel(asset.id));
        instantiateModel(m_world, *model);
    } else if (asset.type == AssetType::Prefab) {
        CORDITE_TRY_ASSIGN(const Json entities, currentPrefabEntities(assets, components, asset.id));
        CORDITE_TRY(instantiatePrefab(m_world, components, entities, asset.id));
    } else {
        return makeError(ErrorCode::InvalidArgument, "{} is {}, which is not drawn", asset.reference(),
                         withArticle(asset.type));
    }
    m_world.updateWorldTransforms();
    return {};
}

void AssetPreview::draw(SceneRenderer& renderer, nvrhi::ICommandList* commandList, AssetManager& assets,
                        ShadowQuality shadows) {
    // Seen whole: the box around every mesh, where it is placed.
    Aabb bounds;
    m_world.traverse([&](Entity entity, uint32_t /*depth*/) {
        const auto* drawn = m_world.tryGet<MeshRendererComponent>(entity);
        if (drawn == nullptr || drawn->mesh.isNil()) {
            return;
        }
        if (const Result<std::shared_ptr<const MeshData>> mesh = assets.loadMesh(drawn->mesh); mesh) {
            bounds.expand((*mesh)->bounds.transformed(m_world.worldMatrix(entity)));
        }
    });
    // What has no mesh - a prefab of lights - is a metre around where it is.
    const Vec3 center = bounds.isEmpty() ? Vec3(0.0f) : bounds.center();
    const float radius = bounds.isEmpty() ? 1.0f : std::max(glm::length(bounds.extents()), 0.01f);

    const Quat turn =
        glm::angleAxis(toRadians(kYawDegrees), kWorldUp) * glm::angleAxis(toRadians(kPitchDegrees), kWorldRight);
    const float distance = radius / std::sin(0.5f * toRadians(kFieldOfViewDegrees)) * kMargin;
    const Mat4 cameraToWorld =
        glm::translate(Mat4(1.0f), center - turn * kWorldForward * distance) * glm::mat4_cast(turn);
    const CameraComponent lens{.verticalFovDegrees = kFieldOfViewDegrees,
                               .nearPlane = 0.5f * (distance - radius),
                               .farPlane = distance + 2.0f * radius};

    SceneRenderSettings settings;
    settings.backgroundColor = srgbToLinear(kBackground);
    settings.defaultLighting = true;
    settings.shadowQuality = shadows;
    settings.postProcess = PostProcessSettings{};
    // Drawn once: nothing has gone by, and no grain changes.
    settings.deltaSeconds = 0.0f;
    settings.timeSeconds = 0.0;
    renderer.render(commandList, m_world, makeCameraView(cameraToWorld, lens, 1.0f), m_targets, settings);
}

} // namespace cordite
