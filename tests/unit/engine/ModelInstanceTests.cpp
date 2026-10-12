#include "cordite/engine/ModelInstance.h"

#include "cordite/animation/AnimationComponents.h"
#include "cordite/renderer/RenderComponents.h"
#include "cordite/world/Components.h"

#include "support/MathTestSupport.h"
#include "support/TestSupport.h"

#include <string>
#include <vector>

using namespace cordite;

namespace {

const Uuid kBodyMesh = *Uuid::parse("11111111-1111-4111-8111-111111111111");
const Uuid kWheelMesh = *Uuid::parse("22222222-2222-4222-8222-222222222222");
const Uuid kPaint = *Uuid::parse("33333333-3333-4333-8333-333333333333");
const Uuid kRubber = *Uuid::parse("44444444-4444-4444-8444-444444444444");
const Uuid kBodySkeleton = *Uuid::parse("55555555-5555-4555-8555-555555555555");
const Uuid kTailSkeleton = *Uuid::parse("66666666-6666-4666-8666-666666666666");

/// A cart: a body with two wheels and a camera looking back from it.
///
///     Body          (mesh, two materials)
///       Front wheel (mesh)
///       Rear wheel  (mesh)
///       Mirror      (camera)
///     Flag          (nothing)
ModelData makeCart() {
    ModelData model;
    model.name = "cart";

    ModelNode body;
    body.name = "Body";
    body.transform.position = Vec3(0.0f, 0.5f, 0.0f);
    body.children = {1, 2, 3};
    body.mesh = kBodyMesh;
    body.materials = {kPaint, Uuid()};

    ModelNode front;
    front.name = "Front wheel";
    front.parent = 0;
    front.transform.position = Vec3(0.0f, -0.3f, 1.0f);
    front.transform.scale = Vec3(0.5f);
    front.mesh = kWheelMesh;
    front.materials = {kRubber};

    ModelNode rear = front;
    rear.name = "Rear wheel";
    rear.transform.position = Vec3(0.0f, -0.3f, -1.0f);
    rear.transform.rotation = glm::angleAxis(kHalfPi, Vec3(1.0f, 0.0f, 0.0f));

    ModelNode mirror;
    mirror.name = "Mirror";
    mirror.parent = 0;
    mirror.camera = ModelCamera{.verticalFov = toRadians(40.0f), .nearPlane = 0.2f, .farPlane = 80.0f};

    ModelNode flag;
    flag.name = "Flag";

    model.nodes = {body, front, rear, mirror, flag};
    model.roots = {0, 4};
    return model;
}

/// A figure as a modelling program exports one: an armature with a chain of joints,
/// and two meshes skinned to them - one beside the armature, one inside it.
///
///     Body           (mesh, skinned)
///     Armature       (nothing; turned and scaled)
///       Hips         (joint)
///         Spine      (joint)
///       Hat          (mesh, skinned)
ModelData makeFigure() {
    ModelData model;
    model.name = "figure";

    ModelNode body;
    body.name = "Body";
    // Where the node of a skinned mesh is does not matter.
    body.transform.position = Vec3(7.0f, 8.0f, 9.0f);
    body.mesh = kBodyMesh;
    body.materials = {kPaint};
    body.skeleton = kBodySkeleton;

    ModelNode armature;
    armature.name = "Armature";
    armature.transform.scale = Vec3(0.01f);
    armature.children = {2, 4};

    ModelNode hips;
    hips.name = "Hips";
    hips.parent = 1;
    hips.children = {3};
    hips.joint = true;
    ModelNode spine;
    spine.name = "Spine";
    spine.parent = 2;
    spine.joint = true;

    ModelNode hat;
    hat.name = "Hat";
    hat.parent = 1;
    hat.mesh = kWheelMesh;
    hat.materials = {kRubber};
    hat.skeleton = kBodySkeleton;

    model.nodes = {body, armature, hips, spine, hat};
    model.roots = {0, 1};
    return model;
}

/// Entity names in hierarchy order, indented by depth with dots.
std::vector<std::string> outline(const World& world) {
    std::vector<std::string> lines;
    world.traverse(
        [&](Entity entity, uint32_t depth) { lines.push_back(std::string(depth, '.') + world.name(entity)); });
    return lines;
}

} // namespace

TEST_SUITE("ModelInstance") {
    TEST_CASE("a model becomes one entity per node under a root named after the model") {
        World world;
        const Entity instance = instantiateModel(world, makeCart());

        CHECK(outline(world) ==
              std::vector<std::string>{"cart", ".Body", "..Front wheel", "..Rear wheel", "..Mirror", ".Flag"});
        CHECK(world.name(instance) == "cart");
        CHECK(world.parent(instance).isNull());
        // The root is where the instance is placed; it starts where it is put.
        CHECK(world.transform(instance) == Transform());
        CHECK_FALSE(world.has<MeshRendererComponent>(instance));
    }

    TEST_CASE("nodes keep their placement, meshes and materials") {
        World world;
        instantiateModel(world, makeCart());

        const Entity body = world.findByName("Body");
        CHECK(world.transform(body).position == Vec3(0.0f, 0.5f, 0.0f));
        const auto* bodyRenderer = world.tryGet<MeshRendererComponent>(body);
        REQUIRE(bodyRenderer != nullptr);
        CHECK(bodyRenderer->mesh == kBodyMesh);
        CHECK(bodyRenderer->materials == std::vector<Uuid>{kPaint, Uuid()});

        const Entity rear = world.findByName("Rear wheel");
        CHECK(world.transform(rear).position == Vec3(0.0f, -0.3f, -1.0f));
        CHECK(world.transform(rear).scale == Vec3(0.5f));
        CHECK(
            test::sameRotation(world.transform(rear).rotation, glm::angleAxis(kHalfPi, Vec3(1.0f, 0.0f, 0.0f)), 1e-6f));
        CHECK(world.tryGet<MeshRendererComponent>(rear)->mesh == kWheelMesh);

        // A node without a mesh has no renderer.
        CHECK_FALSE(world.has<MeshRendererComponent>(world.findByName("Flag")));
        CHECK_FALSE(world.has<MeshRendererComponent>(world.findByName("Mirror")));

        // Placement composes through the hierarchy as in the model.
        world.updateWorldTransforms();
        CHECK_NEAR(Vec3(world.worldMatrix(rear)[3]), Vec3(0.0f, 0.2f, -1.0f), 1.0e-6f);
    }

    TEST_CASE("cameras come along, but never as the camera the game renders from") {
        World world;
        instantiateModel(world, makeCart());

        const auto* camera = world.tryGet<CameraComponent>(world.findByName("Mirror"));
        REQUIRE(camera != nullptr);
        CHECK(camera->projection == CameraProjection::Perspective);
        CHECK(camera->verticalFovDegrees == doctest::Approx(40.0f));
        CHECK(camera->nearPlane == 0.2f);
        CHECK(camera->farPlane == 80.0f);
        CHECK_FALSE(camera->primary);
    }

    TEST_CASE("camera settings a component would refuse are brought into range") {
        ModelData model;
        ModelNode wide;
        wide.name = "Wide";
        wide.camera = ModelCamera{.verticalFov = 4.0f, .nearPlane = 0.0f, .farPlane = 0.0f};
        ModelNode flat;
        flat.name = "Flat";
        flat.camera =
            ModelCamera{.orthographic = true, .orthographicHeight = -3.0f, .nearPlane = 2.0f, .farPlane = 1.0f};
        model.nodes = {wide, flat};
        model.roots = {0, 1};

        World world;
        instantiateModel(world, model);

        const auto* perspective = world.tryGet<CameraComponent>(world.findByName("Wide"));
        REQUIRE(perspective != nullptr);
        CHECK(perspective->verticalFovDegrees == 179.0f);
        CHECK(perspective->nearPlane == 0.001f);
        // A file that gives no far plane gets the engine's.
        CHECK(perspective->farPlane == 1000.0f);

        const auto* orthographic = world.tryGet<CameraComponent>(world.findByName("Flat"));
        REQUIRE(orthographic != nullptr);
        CHECK(orthographic->projection == CameraProjection::Orthographic);
        CHECK(orthographic->orthographicHeight == 0.001f);
        CHECK(orthographic->nearPlane == 2.0f);
        // A far plane in front of the near one is no far plane.
        CHECK(orthographic->farPlane == 1000.0f);
    }

    TEST_CASE("an instance can be given a parent and a name") {
        World world;
        const Entity depot = world.createEntity("Depot");
        world.transform(depot).position = Vec3(10.0f, 0.0f, 0.0f);
        const Entity existing = world.createEntity("Existing");
        REQUIRE(world.setParent(existing, depot).has_value());

        const Entity instance = instantiateModel(world, makeCart(), {.parent = depot, .name = "Cart 7"});
        CHECK(world.name(instance) == "Cart 7");
        CHECK(world.parent(instance) == depot);
        // The last child of its parent, and at the parent's origin.
        CHECK(world.children(depot) == std::vector<Entity>{existing, instance});
        CHECK(world.transform(instance) == Transform());

        world.updateWorldTransforms();
        CHECK_NEAR(Vec3(world.worldMatrix(world.findByName("Body"))[3]), Vec3(10.0f, 0.5f, 0.0f), 1.0e-6f);
    }

    TEST_CASE("placing a model twice gives two independent sets of entities") {
        World world;
        const Entity first = instantiateModel(world, makeCart());
        const Entity second = instantiateModel(world, makeCart());

        CHECK(first != second);
        CHECK(world.id(first) != world.id(second));
        CHECK(world.entityCount() == 12);
        CHECK(world.children(first).size() == 2);
        CHECK(world.children(second).size() == 2);
        CHECK(world.id(world.children(first)[0]) != world.id(world.children(second)[0]));
    }

    TEST_CASE("names that an entity may not have are replaced") {
        ModelData model;
        model.name = "props/cart";
        ModelNode unnamed;
        ModelNode slashed;
        slashed.name = "wheel/front";
        model.nodes = {unnamed, slashed};
        model.roots = {0, 1};

        World world;
        instantiateModel(world, model);
        // A slash separates the names in an entity path, so it cannot be in one.
        CHECK(outline(world) == std::vector<std::string>{"props_cart", ".Node", ".wheel_front"});

        World other;
        instantiateModel(other, ModelData());
        CHECK(outline(other) == std::vector<std::string>{"Model"});
    }

    TEST_CASE("a model with a skeleton is laid out for animation") {
        World world;
        const test::LogCapture log;
        const Entity instance = instantiateModel(world, makeFigure());

        // The joints are no entities. What poses the skeleton is one, directly under
        // the instance, and the meshes skinned to it hang under that - wherever their
        // nodes are in the model.
        CHECK(outline(world) == std::vector<std::string>{"figure", ".Skeleton", "..Body", "..Hat", ".Armature"});
        const Entity skeleton = world.findByName("Skeleton");
        CHECK(world.parent(skeleton) == instance);
        CHECK(world.transform(skeleton) == Transform());
        const AnimatorComponent* animator = world.tryGet<AnimatorComponent>(skeleton);
        REQUIRE(animator != nullptr);
        CHECK(animator->skeleton == kBodySkeleton);
        // It plays nothing until it is given a clip.
        CHECK(animator->clip.isNil());
        CHECK(animator->blendClip.isNil());
        CHECK(animator->time == 0.0f);
        CHECK(animator->playing);

        // A skinned mesh is placed by its joints: its entity is not moved.
        const Entity body = world.findByName("Body");
        CHECK(world.transform(body) == Transform());
        const MeshRendererComponent* renderer = world.tryGet<MeshRendererComponent>(body);
        REQUIRE(renderer != nullptr);
        CHECK(renderer->mesh == kBodyMesh);
        CHECK(renderer->materials == std::vector<Uuid>{kPaint});
        CHECK_FALSE(world.has<AnimatorComponent>(body));
        const Entity hat = world.findByName("Hat");
        CHECK(world.parent(hat) == skeleton);
        CHECK(world.tryGet<MeshRendererComponent>(hat)->mesh == kWheelMesh);

        // What is no joint is a node like any other, with nothing below it now.
        const Entity armature = world.findByName("Armature");
        CHECK(world.transform(armature).scale == Vec3(0.01f));
        CHECK(world.children(armature).empty());
        CHECK(world.findByName("Hips").isNull());
        CHECK(world.findByName("Spine").isNull());
        CHECK(log.messages().empty());
    }

    TEST_CASE("every skeleton of a model gets an animator of its own") {
        ModelData model = makeFigure();
        ModelNode tail;
        tail.name = "Tail";
        tail.mesh = kWheelMesh;
        tail.skeleton = kTailSkeleton;
        // A mesh that names no skeleton is not skinned, whatever the mesh is.
        ModelNode stand;
        stand.name = "Stand";
        stand.mesh = kBodyMesh;
        stand.transform.position = Vec3(0.0f, -1.0f, 0.0f);
        // A skeleton without a mesh is not one a node is drawn with.
        ModelNode marker;
        marker.name = "Marker";
        marker.skeleton = kTailSkeleton;
        model.nodes.push_back(tail);
        model.nodes.push_back(stand);
        model.nodes.push_back(marker);
        model.roots = {5, 0, 1, 6, 7};

        World world;
        instantiateModel(world, model);
        // The animators come in the order their skeletons are first needed.
        CHECK(outline(world) == std::vector<std::string>{"figure", ".Skeleton", "..Tail", ".Skeleton 2", "..Body",
                                                         "..Hat", ".Armature", ".Stand", ".Marker"});
        CHECK(world.tryGet<AnimatorComponent>(world.findByName("Skeleton"))->skeleton == kTailSkeleton);
        CHECK(world.tryGet<AnimatorComponent>(world.findByName("Skeleton 2"))->skeleton == kBodySkeleton);
        CHECK(world.transform(world.findByName("Stand")).position == Vec3(0.0f, -1.0f, 0.0f));
        CHECK_FALSE(world.has<AnimatorComponent>(world.findByName("Marker")));
    }

    TEST_CASE("what hangs on a joint is placed where the joint rests, and that is said") {
        ModelData model = makeFigure();
        // A sword in a hand: a mesh that is not skinned, on a joint below the spine; and
        // a camera that is itself a joint.
        ModelNode hand;
        hand.name = "Hand";
        hand.parent = 3;
        hand.transform.position = Vec3(0.0f, 0.5f, 0.0f);
        hand.children = {6};
        hand.joint = true;
        ModelNode sword;
        sword.name = "Sword";
        sword.parent = 5;
        sword.mesh = kWheelMesh;
        ModelNode eye;
        eye.name = "Eye";
        eye.parent = 2;
        eye.joint = true;
        eye.camera = ModelCamera{.verticalFov = toRadians(60.0f), .nearPlane = 0.1f, .farPlane = 50.0f};
        model.nodes[3].children = {5};
        model.nodes[2].children = {3, 7};
        model.nodes.push_back(hand);
        model.nodes.push_back(sword);
        model.nodes.push_back(eye);

        World world;
        const test::LogCapture log;
        instantiateModel(world, model);
        // The joints that carry something are entities after all, down to what they carry.
        CHECK(outline(world) == std::vector<std::string>{"figure", ".Skeleton", "..Body", "..Hat", ".Armature",
                                                         "..Hips", "...Spine", "....Hand", ".....Sword", "...Eye"});
        CHECK(world.transform(world.findByName("Hand")).position == Vec3(0.0f, 0.5f, 0.0f));
        CHECK(world.has<MeshRendererComponent>(world.findByName("Sword")));
        CHECK(world.has<CameraComponent>(world.findByName("Eye")));
        CHECK(log.count(LogLevel::Warn) == 1);
        CHECK(log.contains("the model 'figure' has something hanging on 4 of its joints: it is placed where the "
                           "joints rest, and does not follow a pose"));
    }

    TEST_CASE("a skinned mesh below a joint keeps the joint from being an entity no more than nothing would") {
        ModelData model = makeFigure();
        // The hat hangs on the spine in the file. It is placed by its joints all the
        // same, under the animator, and the spine stays what it is.
        model.nodes[1].children = {2};
        model.nodes[3].children = {4};
        model.nodes[4].parent = 3;

        World world;
        const test::LogCapture log;
        instantiateModel(world, model);
        CHECK(outline(world) == std::vector<std::string>{"figure", ".Skeleton", "..Body", "..Hat", ".Armature"});
        CHECK(log.messages().empty());
    }

    TEST_CASE("a model that is not a tree is still instantiated, each node once") {
        ModelData model;
        model.name = "tangle";
        ModelNode a;
        a.name = "A";
        // Its own child, a child that does not exist, and B twice.
        a.children = {0, 9, 1, 1};
        ModelNode b;
        b.name = "B";
        b.children = {0};
        model.nodes = {a, b};
        // A root listed twice, and one that does not exist.
        model.roots = {0, 0, 5};

        World world;
        instantiateModel(world, model);
        CHECK(outline(world) == std::vector<std::string>{"tangle", ".A", "..B"});
    }
}
