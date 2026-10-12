#include "cordite/engine/SceneAssets.h"

#include "cordite/assets/GltfExporter.h"
#include "cordite/core/Reflection.h"
#include "cordite/engine/EngineComponents.h"
#include "cordite/physics/PhysicsComponents.h"
#include "cordite/renderer/RenderComponents.h"

#include "support/SkinnedModelSupport.h"
#include "support/TestSupport.h"

#include <string>
#include <utility>
#include <vector>

using namespace cordite;

namespace {

const Uuid kMissing = *Uuid::parse("11111111-1111-4111-8111-111111111111");

/// A component with asset references in every shape the reflection allows: plain,
/// nested in an object, and without a stated kind.
struct Decal {
    Uuid texture;
    Uuid anything;
};

struct Emitter {
    Uuid sprite;
    Decal decal;
    std::string note;
};

/// A component that asks for a kind of asset there is none of.
struct Billboard {
    Uuid sprite;
};

TypeInfo describeDecal() {
    return TypeBuilder<Decal>("Decal")
        .property("texture", &Decal::texture,
                  {.semantic = PropertySemantic::AssetReference, .referenceType = "Texture"})
        .property("anything", &Decal::anything, {.semantic = PropertySemantic::AssetReference})
        .build();
}

/// A world with the engine's components, and a database that knows the built-in assets.
struct Fixture {
    ComponentRegistry components;
    World world;
    AssetDatabase assets;
    TypeInfo decalType = describeDecal();

    Fixture() {
        REQUIRE(registerEngineComponents(components).has_value());
        REQUIRE(
            components
                .add<Emitter>(TypeBuilder<Emitter>("Emitter")
                                  .property("sprite", &Emitter::sprite,
                                            {.semantic = PropertySemantic::AssetReference, .referenceType = "Texture"})
                                  .objectProperty("decal", &Emitter::decal, decalType)
                                  .property("note", &Emitter::note)
                                  .build())
                .has_value());
    }

    Entity addRenderer(const std::string& name, const Uuid& mesh, std::vector<Uuid> materials = {}) {
        const Entity entity = world.createEntity(name);
        MeshRendererComponent& renderer = world.add<MeshRendererComponent>(entity);
        renderer.mesh = mesh;
        renderer.materials = std::move(materials);
        return entity;
    }
};

} // namespace

TEST_SUITE("SceneAssets") {
    TEST_CASE("a world without components that name assets uses none") {
        Fixture fixture;
        fixture.world.createEntity("Empty");
        CHECK(collectAssetReferences(fixture.world, fixture.components).empty());
        CHECK(findSceneProblems(fixture.world, fixture.components, fixture.assets).empty());
    }

    TEST_CASE("every reference is found, with the property it is in") {
        Fixture fixture;
        const Uuid cube = builtinMeshId(BuiltinMesh::Cube);
        const Uuid sphere = builtinMeshId(BuiltinMesh::Sphere);
        const Uuid paint = defaultMaterialId();

        const Entity room = fixture.world.createEntity("Room");
        const Entity crate = fixture.addRenderer("Crate", cube, {paint, Uuid(), kMissing});
        const Entity ball = fixture.addRenderer("Ball", sphere);
        REQUIRE(fixture.world.setParent(ball, room).has_value());
        // Nothing assigned: not a use of anything.
        fixture.addRenderer("Unset", Uuid());

        const std::vector<AssetReferenceUse> uses = collectAssetReferences(fixture.world, fixture.components);
        // In hierarchy order: Room's child first, then the roots that follow.
        REQUIRE(uses.size() == 4);
        CHECK(uses[0].entity == ball);
        CHECK(uses[0].component == "MeshRenderer");
        CHECK(uses[0].property == "mesh");
        CHECK(uses[0].asset == sphere);
        CHECK(uses[0].expectedType == "Mesh");

        CHECK(uses[1].entity == crate);
        CHECK(uses[1].property == "mesh");
        CHECK(uses[1].asset == cube);
        // Entries of a list carry their index; empty entries are skipped but still counted.
        CHECK(uses[2].property == "materials[0]");
        CHECK(uses[2].asset == paint);
        CHECK(uses[2].expectedType == "Material");
        CHECK(uses[3].property == "materials[2]");
        CHECK(uses[3].asset == kMissing);
    }

    TEST_CASE("references inside nested objects are found, and other properties are left alone") {
        Fixture fixture;
        const Entity entity = fixture.world.createEntity("Sparks");
        Emitter& emitter = fixture.world.add<Emitter>(entity);
        emitter.sprite = kMissing;
        emitter.decal.texture = defaultMaterialId();
        emitter.decal.anything = builtinMeshId(BuiltinMesh::Cube);
        // Looks like an id, but is text.
        emitter.note = kMissing.toString();

        const std::vector<AssetReferenceUse> uses = collectAssetReferences(fixture.world, fixture.components);
        REQUIRE(uses.size() == 3);
        CHECK(uses[0].component == "Emitter");
        CHECK(uses[0].property == "sprite");
        CHECK(uses[1].property == "decal.texture");
        CHECK(uses[1].expectedType == "Texture");
        CHECK(uses[2].property == "decal.anything");
        CHECK(uses[2].expectedType.empty());
    }

    TEST_CASE("a scene whose references all lead to the right kind of asset has no problems") {
        Fixture fixture;
        fixture.addRenderer("Crate", builtinMeshId(BuiltinMesh::Cube), {defaultMaterialId(), Uuid()});
        CHECK(findSceneProblems(fixture.world, fixture.components, fixture.assets).empty());
    }

    TEST_CASE("missing assets and assets of the wrong kind are reported") {
        Fixture fixture;
        const Entity ghost = fixture.addRenderer("Ghost", kMissing);
        const Entity mixed =
            fixture.addRenderer("Mixed", defaultMaterialId(), {builtinMeshId(BuiltinMesh::Cube), kMissing});

        const std::vector<SceneProblem> problems = findSceneProblems(fixture.world, fixture.components, fixture.assets);
        REQUIRE(problems.size() == 4);
        CHECK(problems[0].entity == ghost);
        CHECK(problems[0].message ==
              "MeshRenderer.mesh refers to the asset 11111111-1111-4111-8111-111111111111, which does not exist");
        CHECK(problems[1].entity == mixed);
        CHECK(problems[1].message ==
              "MeshRenderer.mesh refers to builtin#material/default, which is a material, not a mesh");
        CHECK(problems[2].message ==
              "MeshRenderer.materials[0] refers to builtin#mesh/cube, which is a mesh, not a material");
        CHECK(problems[3].message == "MeshRenderer.materials[1] refers to the asset "
                                     "11111111-1111-4111-8111-111111111111, which does not exist");
    }

    TEST_CASE("a kind is named with the article it takes") {
        const test::TempDirectory directory;
        REQUIRE(exportGltf(test::makeSkinnedBar(), directory.path() / "bar.gltf").has_value());
        AssetDatabase assets({.rootDirectory = directory.path()});
        REQUIRE(assets.refresh().has_value());
        const AssetInfo* clip = assets.findByReference("bar.gltf#animation/0");
        REQUIRE(clip != nullptr);

        Fixture fixture;
        const Entity dancer = fixture.addRenderer("Dancer", clip->id);
        std::vector<SceneProblem> problems = findSceneProblems(fixture.world, fixture.components, assets);
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].message ==
              "MeshRenderer.mesh refers to bar.gltf#animation/0, which is an animation, not a mesh");

        // A kind that no asset has is named as the component spells it.
        REQUIRE(
            fixture.components
                .add<Billboard>(TypeBuilder<Billboard>("Billboard")
                                    .property("sprite", &Billboard::sprite,
                                              {.semantic = PropertySemantic::AssetReference, .referenceType = "Sprite"})
                                    .build())
                .has_value());
        fixture.world.add<Billboard>(dancer).sprite = clip->id;
        problems = findSceneProblems(fixture.world, fixture.components, assets);
        REQUIRE(problems.size() == 2);
        CHECK(problems[1].message ==
              "Billboard.sprite refers to bar.gltf#animation/0, which is an animation, not a sprite");
    }

    TEST_CASE("a property that takes any asset is only checked for the asset being there") {
        Fixture fixture;
        const Entity entity = fixture.world.createEntity("Sparks");
        Emitter& emitter = fixture.world.add<Emitter>(entity);
        emitter.decal.anything = builtinMeshId(BuiltinMesh::Cube);
        CHECK(findSceneProblems(fixture.world, fixture.components, fixture.assets).empty());

        emitter.decal.anything = kMissing;
        const std::vector<SceneProblem> problems = findSceneProblems(fixture.world, fixture.components, fixture.assets);
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].message.find("Emitter.decal.anything") != std::string::npos);
    }

    TEST_CASE("colliders that no shape or body can be made of are problems of a scene") {
        Fixture fixture;
        const Entity ground = fixture.world.createEntity("Ground");
        fixture.world.add<ColliderComponent>(ground) = {.shape = ShapeType::Mesh,
                                                        .mesh = builtinMeshId(BuiltinMesh::Plane)};
        CHECK(findSceneProblems(fixture.world, fixture.components, fixture.assets).empty());

        // A mesh that is not there is found as any reference to nothing is; a shape of
        // a mesh that names none, and triangles that the simulation is to move, as
        // what the physics will make nothing of.
        fixture.world.add<RigidBodyComponent>(ground);
        const Entity rock = fixture.world.createEntity("Rock");
        fixture.world.add<ColliderComponent>(rock).shape = ShapeType::ConvexHull;
        const Entity ghost = fixture.world.createEntity("Ghost");
        fixture.world.add<ColliderComponent>(ghost) = {.shape = ShapeType::ConvexHull, .mesh = kMissing};
        const std::vector<SceneProblem> problems = findSceneProblems(fixture.world, fixture.components, fixture.assets);
        REQUIRE(problems.size() == 3);
        CHECK(problems[0].entity == ghost);
        CHECK(problems[0].message ==
              "Collider.mesh refers to the asset 11111111-1111-4111-8111-111111111111, which does not exist");
        CHECK(problems[1].entity == ground);
        CHECK(problems[1].message.find("Collider.shape is mesh, and the body it is part of - 'Ground' - is dynamic") !=
              std::string::npos);
        CHECK(problems[2].entity == rock);
        CHECK(problems[2].message ==
              "Collider.mesh names no mesh, and a collider of the shape convexHull is made of one");
    }

    TEST_CASE("a character that is a body as well, and what hangs on one in its way, are problems of a scene") {
        Fixture fixture;
        const Entity guard = fixture.world.createEntity("Guard");
        fixture.world.add<CharacterControllerComponent>(guard);
        const Entity reach = fixture.world.createEntity("Reach");
        REQUIRE(fixture.world.setParent(reach, guard, false).has_value());
        fixture.world.add<ColliderComponent>(reach).trigger = true;
        CHECK(findSceneProblems(fixture.world, fixture.components, fixture.assets).empty());

        fixture.world.add<RigidBodyComponent>(guard);
        fixture.world.tryGet<ColliderComponent>(reach)->trigger = false;
        const std::vector<SceneProblem> problems = findSceneProblems(fixture.world, fixture.components, fixture.assets);
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].entity == guard);
        CHECK(problems[0].message == "CharacterController: the entity is a body as well - it has a RigidBody - and "
                                     "cannot be both. A character has a capsule of its own");
    }

    TEST_CASE("joints that will hold nothing are problems of a scene") {
        Fixture fixture;
        const Entity frame = fixture.world.createEntity("Frame");
        fixture.world.add<ColliderComponent>(frame);
        const Entity door = fixture.world.createEntity("Door");
        fixture.world.add<ColliderComponent>(door);
        fixture.world.add<RigidBodyComponent>(door);
        fixture.world.add<JointComponent>(door) = {.type = JointType::Hinge, .connectedTo = fixture.world.id(frame)};
        CHECK(findSceneProblems(fixture.world, fixture.components, fixture.assets).empty());

        // The frame goes: the door is hinged to an entity that is not there.
        const Uuid frameId = fixture.world.id(frame);
        fixture.world.destroyEntity(frame);
        const std::vector<SceneProblem> problems = findSceneProblems(fixture.world, fixture.components, fixture.assets);
        REQUIRE(problems.size() == 1);
        CHECK(problems[0].entity == door);
        CHECK(problems[0].message ==
              "Joint.connectedTo refers to the entity " + frameId.toString() + ", which does not exist");
    }

    TEST_CASE("disabled entities are checked like any other") {
        // They are still in the scene file, and enabling one must not reveal a problem.
        Fixture fixture;
        const Entity ghost = fixture.addRenderer("Ghost", kMissing);
        fixture.world.setEnabled(ghost, false);
        CHECK(findSceneProblems(fixture.world, fixture.components, fixture.assets).size() == 1);
    }
}
