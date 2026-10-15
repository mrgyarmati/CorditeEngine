#include "cordite/editor/EditorIcons.h"

#include "cordite/animation/AnimationComponents.h"
#include "cordite/audio/AudioComponents.h"
#include "cordite/physics/PhysicsComponents.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/scripting/ScriptComponents.h"
#include "cordite/world/Components.h"

#include "support/TestSupport.h"

#include <set>
#include <string>

using namespace cordite;

namespace {

bool sameIcon(const UiIcon& first, const UiIcon& second) {
    return first.codepoint == second.codepoint;
}

template <typename Component>
Entity entityWith(World& world, const char* name) {
    const Entity entity = world.createEntity(name);
    world.add<Component>(entity);
    return entity;
}

} // namespace

TEST_SUITE("EditorIcons") {
    TEST_CASE("every type of asset has an icon of its own") {
        std::set<char32_t> seen;
        for (const AssetType type : kAssetTypes) {
            CAPTURE(std::string(nameOf(type)));
            CHECK(seen.insert(iconOf(type).codepoint).second);
            CHECK_FALSE(sameIcon(iconOf(type), icons::kFile));
        }
        CHECK(sameIcon(iconOf(AssetType::Texture), icons::kTexture));
        CHECK(sameIcon(iconOf(AssetType::Model), icons::kModel));
        CHECK(sameIcon(iconOf(AssetType::Scene), icons::kScene));
        CHECK(sameIcon(iconOf(AssetType::Prefab), icons::kPrefab));
        CHECK(sameIcon(iconOf(AssetType::Sound), icons::kSound));
    }

    TEST_CASE("a directory is a folder, open or closed") {
        CHECK(sameIcon(directoryIcon(true), icons::kFolderOpen));
        CHECK(sameIcon(directoryIcon(false), icons::kFolder));
    }

    TEST_CASE("an entity is shown as what it is most") {
        World world;
        CHECK(sameIcon(entityIcon(world, world.createEntity("Plain")), icons::kEntity));
        CHECK(sameIcon(entityIcon(world, entityWith<CameraComponent>(world, "Camera")), icons::kCamera));
        const Entity sun = entityWith<LightComponent>(world, "Sun");
        world.tryGet<LightComponent>(sun)->type = LightType::Directional;
        CHECK(sameIcon(entityIcon(world, sun), icons::kSun));
        CHECK(sameIcon(entityIcon(world, entityWith<LightComponent>(world, "Lamp")), icons::kLight));
        CHECK(sameIcon(entityIcon(world, entityWith<EnvironmentComponent>(world, "Sky")), icons::kEnvironment));
        CHECK(sameIcon(entityIcon(world, entityWith<AmbientLightComponent>(world, "Fill")), icons::kEnvironment));
        CHECK(sameIcon(entityIcon(world, entityWith<FogComponent>(world, "Haze")), icons::kFog));
        CHECK(sameIcon(entityIcon(world, entityWith<PostProcessComponent>(world, "Grade")), icons::kPostProcess));
        CHECK(sameIcon(entityIcon(world, entityWith<MeshRendererComponent>(world, "Crate")), icons::kMesh));
        CHECK(sameIcon(entityIcon(world, entityWith<AnimatorComponent>(world, "Skeleton")), icons::kSkeleton));
        CHECK(
            sameIcon(entityIcon(world, entityWith<CharacterControllerComponent>(world, "Player")), icons::kCharacter));
        CHECK(sameIcon(entityIcon(world, entityWith<RigidBodyComponent>(world, "Body")), icons::kCollider));
        CHECK(sameIcon(entityIcon(world, entityWith<ColliderComponent>(world, "Wall")), icons::kCollider));
        CHECK(sameIcon(entityIcon(world, entityWith<JointComponent>(world, "Hinge")), icons::kJoint));
        CHECK(sameIcon(entityIcon(world, entityWith<AudioSourceComponent>(world, "Radio")), icons::kSound));
        CHECK(sameIcon(entityIcon(world, entityWith<AudioListenerComponent>(world, "Ears")), icons::kListener));
        CHECK(sameIcon(entityIcon(world, entityWith<ScriptComponent>(world, "Clock")), icons::kScript));
    }

    TEST_CASE("an entity of several kinds is shown as the first of them") {
        World world;
        // A placed prefab is a prefab, whatever it holds.
        const Entity bollard = world.createEntity("Bollard");
        world.add<PrefabInstanceComponent>(bollard);
        world.add<MeshRendererComponent>(bollard);
        CHECK(sameIcon(entityIcon(world, bollard), icons::kPrefab));
        const Entity placedCamera = world.createEntity("Placed camera");
        world.add<CameraComponent>(placedCamera);
        world.add<PrefabInstanceComponent>(placedCamera);
        CHECK(sameIcon(entityIcon(world, placedCamera), icons::kPrefab));
        // A camera that hears is a camera; a crate that is a body is a mesh; a light with a
        // script is a light.
        const Entity camera = world.createEntity("Camera");
        world.add<AudioListenerComponent>(camera);
        world.add<CameraComponent>(camera);
        CHECK(sameIcon(entityIcon(world, camera), icons::kCamera));
        const Entity crate = world.createEntity("Crate");
        world.add<RigidBodyComponent>(crate);
        world.add<MeshRendererComponent>(crate);
        CHECK(sameIcon(entityIcon(world, crate), icons::kMesh));
        const Entity lamp = world.createEntity("Lamp");
        world.add<ScriptComponent>(lamp);
        world.add<LightComponent>(lamp);
        CHECK(sameIcon(entityIcon(world, lamp), icons::kLight));
    }

    TEST_CASE("an entity that is gone is shown as a plain one") {
        World world;
        const Entity camera = world.createEntity("Camera");
        world.add<CameraComponent>(camera);
        world.destroyEntity(camera);
        CHECK(sameIcon(entityIcon(world, camera), icons::kEntity));
        CHECK(sameIcon(entityIcon(world, Entity{}), icons::kEntity));
    }
}
