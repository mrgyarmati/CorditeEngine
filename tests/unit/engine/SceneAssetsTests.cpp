#include "cordite/engine/SceneAssets.h"

#include "cordite/assets/GltfExporter.h"
#include "cordite/assets/Prefab.h"
#include "cordite/core/Json.h"
#include "cordite/core/Reflection.h"
#include "cordite/engine/EngineComponents.h"
#include "cordite/physics/PhysicsComponents.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/world/Components.h"
#include "cordite/world/Prefab.h"
#include "cordite/world/SceneSerializer.h"

#include "support/SkinnedModelSupport.h"
#include "support/TestSupport.h"

#include <fmt/format.h>

#include <array>
#include <string>
#include <string_view>
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
    TEST_CASE("what a scene file refers to is read from its document, each once, as the document has it") {
        Fixture fixture;
        const Uuid cube = builtinMeshId(BuiltinMesh::Cube);
        const Uuid sphere = builtinMeshId(BuiltinMesh::Sphere);
        const Uuid paint = defaultMaterialId();
        // Entities are written parents first, and the components of one in the order of the
        // registry.
        const Entity room = fixture.world.createEntity("Room");
        Emitter& emitter = fixture.world.add<Emitter>(room);
        emitter.sprite = kMissing;
        emitter.decal.anything = cube;
        const Entity crate = fixture.addRenderer("Crate", cube, {paint, Uuid(), paint});
        REQUIRE(fixture.world.setParent(crate, room).has_value());
        fixture.addRenderer("Ball", sphere, {kMissing});
        fixture.addRenderer("Unset", Uuid());

        const Json document = serializeScene(fixture.world, fixture.components);
        const std::vector<Uuid> used = test::unwrap(assetsReferencedBy(AssetType::Scene, document, fixture.components));
        CHECK(used == std::vector<Uuid>{kMissing, cube, paint, sphere});
        // What the engine hands its assets reads the same.
        CHECK(test::unwrap(assetReferenceReader(fixture.components)(AssetType::Scene, document)) == used);

        // A scene of nothing uses nothing.
        World empty;
        CHECK(test::unwrap(
                  assetsReferencedBy(AssetType::Scene, serializeScene(empty, fixture.components), fixture.components))
                  .empty());
    }

    TEST_CASE("a scene that would not load still says what it uses") {
        Fixture fixture;
        const Uuid cube = builtinMeshId(BuiltinMesh::Cube);
        const Json document = test::unwrap(parseJson(fmt::format(R"({{
            "format": "cordite.scene", "version": 1, "extra": true,
            "entities": [
                {{"id": "not an id", "components": {{
                    "Bogus": {{"mesh": "{0}"}},
                    "MeshRenderer": {{"mesh": "{0}", "castShadows": "sometimes", "materials": [7, "{1}", null]}}
                }}}},
                "no entity",
                {{"components": "none"}},
                {{"components": {{"MeshRenderer": {{"mesh": "not an id either"}}}}}}
            ]
        }})",
                                                                 cube.toString(), kMissing.toString())));
        const std::vector<Uuid> used = test::unwrap(assetsReferencedBy(AssetType::Scene, document, fixture.components));
        // The unknown component is passed over, and so is every value that is no id.
        CHECK(used == std::vector<Uuid>{cube, kMissing});
        // Loading it is what says what is wrong with it.
        World world;
        CHECK_FALSE(deserializeScene(world, document, fixture.components).has_value());
    }

    TEST_CASE("the mark of the root of a prefab is no use of anything, an instance below it is") {
        Fixture fixture;
        const Uuid cube = builtinMeshId(BuiltinMesh::Cube);
        const Uuid sphere = builtinMeshId(BuiltinMesh::Sphere);
        const Uuid outer = *Uuid::parse("22222222-2222-4222-8222-222222222222");
        const Uuid inner = *Uuid::parse("33333333-3333-4333-8333-333333333333");

        const Entity cart = fixture.addRenderer("Cart", cube);
        fixture.world.add<PrefabInstanceComponent>(cart).prefab = outer;
        const Entity wheel = fixture.addRenderer("Wheel", sphere);
        fixture.world.add<PrefabInstanceComponent>(wheel).prefab = inner;
        REQUIRE(fixture.world.setParent(wheel, cart).has_value());

        // As a file would have it if a hand had written the mark of the root into it:
        // placing the prefab replaces it, so it names nothing that is used.
        Json entities = serializeEntities(fixture.world, std::array<Entity, 1>{cart}, fixture.components);
        entities.front()["parent"] = nullptr;
        const Json document = prefabDocument({.name = "Cart", .entities = entities});
        CHECK(test::unwrap(assetsReferencedBy(AssetType::Prefab, document, fixture.components)) ==
              std::vector<Uuid>{cube, inner, sphere});
        // The prefab as the editor writes it, without the mark, reads the same.
        const Json written = prefabDocument(
            {.name = "Cart", .entities = serializePrefabEntities(fixture.world, fixture.components, cart)});
        CHECK(test::unwrap(assetsReferencedBy(AssetType::Prefab, written, fixture.components)) ==
              std::vector<Uuid>{cube, inner, sphere});
        // In a scene the mark is what it says: the scene was made with that prefab.
        CHECK(test::unwrap(assetsReferencedBy(AssetType::Scene, serializeScene(fixture.world, fixture.components),
                                              fixture.components)) == std::vector<Uuid>{outer, cube, inner, sphere});
    }

    TEST_CASE("what is no scene, no prefab or of another type of asset is refused") {
        Fixture fixture;
        const auto refusal = [&](AssetType type, std::string_view text) {
            const Result<std::vector<Uuid>> read =
                assetsReferencedBy(type, test::unwrap(parseJson(text)), fixture.components);
            REQUIRE_FALSE(read.has_value());
            return read.error();
        };
        CHECK(refusal(AssetType::Scene, R"({"format": "cordite.material", "version": 1})").code ==
              ErrorCode::ParseError);
        CHECK(refusal(AssetType::Scene, R"({"format": "cordite.scene", "version": 99, "entities": []})").code ==
              ErrorCode::VersionMismatch);
        const Error noEntities = refusal(AssetType::Scene, R"({"format": "cordite.scene", "version": 1})");
        CHECK(noEntities.code == ErrorCode::ParseError);
        CHECK(noEntities.message.find("'entities'") != std::string::npos);
        CHECK(refusal(AssetType::Scene, R"({"format": "cordite.scene", "version": 1, "entities": {}})").code ==
              ErrorCode::ParseError);
        CHECK(refusal(AssetType::Scene, "[]").code == ErrorCode::ParseError);
        CHECK(refusal(AssetType::Prefab, R"({"format": "cordite.prefab", "version": 1, "entities": []})").code ==
              ErrorCode::ParseError);
        // A scene document is no prefab.
        CHECK(refusal(AssetType::Prefab, R"({"format": "cordite.scene", "version": 1, "entities": []})").code ==
              ErrorCode::ParseError);
        const Error material = refusal(AssetType::Material, R"({"format": "cordite.material", "version": 1})");
        CHECK(material.code == ErrorCode::InvalidArgument);
        CHECK(material.message == "a material has no entities to refer to assets with");
    }
}
