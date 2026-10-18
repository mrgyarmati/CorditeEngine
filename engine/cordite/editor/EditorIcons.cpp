#include "cordite/editor/EditorIcons.h"

#include "cordite/animation/AnimationComponents.h"
#include "cordite/audio/AudioComponents.h"
#include "cordite/physics/PhysicsComponents.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/scripting/ScriptComponents.h"
#include "cordite/world/Components.h"

namespace cordite {

UiIcon iconOf(AssetType type) {
    switch (type) {
        case AssetType::Texture:
            return icons::kTexture;
        case AssetType::Mesh:
            return icons::kMesh;
        case AssetType::Material:
            return icons::kMaterial;
        case AssetType::Model:
            return icons::kModel;
        case AssetType::Scene:
            return icons::kScene;
        case AssetType::Skeleton:
            return icons::kSkeleton;
        case AssetType::Animation:
            return icons::kAnimation;
        case AssetType::Script:
            return icons::kScript;
        case AssetType::Sound:
            return icons::kSound;
        case AssetType::Prefab:
            return icons::kPrefab;
        case AssetType::RenderTarget:
            // What a camera draws into.
            return icons::kCamera;
        case AssetType::Sequence:
            return icons::kSequence;
    }
    return icons::kFile;
}

UiIcon directoryIcon(bool open) {
    return open ? icons::kFolderOpen : icons::kFolder;
}

UiIcon entityIcon(const World& world, Entity entity) {
    if (!world.isValid(entity)) {
        return icons::kEntity;
    }
    if (world.has<PrefabInstanceComponent>(entity)) {
        return icons::kPrefab;
    }
    if (world.has<CameraComponent>(entity)) {
        return icons::kCamera;
    }
    if (const auto* light = world.tryGet<LightComponent>(entity)) {
        return light->type == LightType::Directional ? icons::kSun : icons::kLight;
    }
    if (world.has<EnvironmentComponent>(entity) || world.has<AmbientLightComponent>(entity)) {
        return icons::kEnvironment;
    }
    if (world.has<FogComponent>(entity)) {
        return icons::kFog;
    }
    if (world.has<PostProcessComponent>(entity)) {
        return icons::kPostProcess;
    }
    if (world.has<MeshRendererComponent>(entity)) {
        return icons::kMesh;
    }
    if (world.has<AnimatorComponent>(entity)) {
        return icons::kSkeleton;
    }
    if (world.has<CharacterControllerComponent>(entity)) {
        return icons::kCharacter;
    }
    if (world.has<RigidBodyComponent>(entity) || world.has<ColliderComponent>(entity)) {
        return icons::kCollider;
    }
    if (world.has<JointComponent>(entity)) {
        return icons::kJoint;
    }
    if (world.has<AudioSourceComponent>(entity)) {
        return icons::kSound;
    }
    if (world.has<AudioListenerComponent>(entity)) {
        return icons::kListener;
    }
    if (world.has<ScriptComponent>(entity)) {
        return icons::kScript;
    }
    return icons::kEntity;
}

} // namespace cordite
