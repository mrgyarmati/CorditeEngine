#include "cordite/math/Math.h"
#include "cordite/math/Transform.h"
#include "cordite/physics/PhysicsComponents.h"
#include "cordite/physics/PhysicsSystem.h"
#include "cordite/physics/PhysicsWorld.h"
#include "cordite/scripting/ScriptRuntime.h"
#include "cordite/scripting/ScriptSystem.h"
#include "cordite/world/Entity.h"
#include "cordite/world/World.h"

#include "support/MathTestSupport.h"
#include "unit/scripting/ScriptTestSupport.h"

#include <string>
#include <vector>

using namespace cordite;
using cordite::test::ScriptStage;

namespace {

using Lines = std::vector<std::string>;

/// Lua that is put before what a test runs. What the simulation gives back has been
/// through single floats and through a solver: `near` compares numbers and vectors to a
/// thousandth, and `names` lists entities by name, sorted - the order of what an overlap
/// finds is the simulation's own.
constexpr const char* kHelpers = R"(
    local function near(a, b)
        if type(a) == "number" then return math.abs(a - b) < 1.0e-3 end
        return math.abs(a.x - b.x) < 1.0e-3 and math.abs(a.y - b.y) < 1.0e-3 and math.abs(a.z - b.z) < 1.0e-3
    end
    local function names(entities)
        local list = {}
        for _, found in ipairs(entities) do list[#list + 1] = found:name() end
        table.sort(list)
        return table.concat(list, " ")
    end
    local vec3 = cordite.vec3
)";

/// What most of these tests look at:
///
///     Floor   a slab whose top is the plane y = 0
///     Post    a box of 1 x 4 x 1 that stands on the floor at x = 5
///     Ball    a ball of half a metre, ten metres up, which gravity leaves alone
///     Zone    a trigger of two metres each way, at z = 5
struct Range {
    ScriptStage stage;
    Entity floor;
    Entity post;
    Entity ball;
    Entity zone;

    Range() {
        stage.simulate();
        World& world = stage.world;
        floor = world.createEntity("Floor");
        world.add<ColliderComponent>(floor) = {.size = Vec3(50.0f, 1.0f, 50.0f), .center = Vec3(0.0f, -0.5f, 0.0f)};
        post = world.createEntity("Post");
        world.transform(post).position = Vec3(5.0f, 2.0f, 0.0f);
        world.add<ColliderComponent>(post) = {.size = Vec3(1.0f, 4.0f, 1.0f)};
        ball = world.createEntity("Ball");
        world.transform(ball).position = Vec3(0.0f, 10.0f, 0.0f);
        world.add<ColliderComponent>(ball) = {.shape = ShapeType::Sphere, .radius = 0.5f};
        world.add<RigidBodyComponent>(ball) = {
            .mass = 2.0f, .linearDamping = 0.0f, .angularDamping = 0.0f, .gravityScale = 0.0f, .allowSleep = false};
        zone = world.createEntity("Zone");
        world.transform(zone).position = Vec3(0.0f, 1.0f, 5.0f);
        world.add<ColliderComponent>(zone) = {.size = Vec3(2.0f), .trigger = true};
        // The frame that makes the bodies.
        stage.update();
    }

    [[nodiscard]] std::string line(const std::string& body) { return stage.line(std::string(kHelpers) + body); }

    [[nodiscard]] const RigidBodyComponent& body() { return *stage.world.tryGet<RigidBodyComponent>(ball); }
};

} // namespace

TEST_SUITE("ScriptPhysicsApi") {
    TEST_CASE("a script asks what a ray hits: whose body, where, which way the surface faces and how far it is") {
        Range range;

        // Along +X at the height of one metre: the side of the post that faces the ray.
        CHECK(range.line("local hit = cordite.physics.raycast(vec3(0, 1, 0), vec3(1, 0, 0))\n"
                         "print(hit.entity, near(hit.position, vec3(4.5, 1, 0)), near(hit.normal, vec3(-1, 0, 0)), "
                         "near(hit.distance, 4.5), getmetatable(hit.position) == cordite.Vec3)") ==
              "Entity(Post)\ttrue\ttrue\ttrue\ttrue");
        // A direction need not have the length one, and tables do for vectors.
        CHECK(range.line("local hit = cordite.physics.raycast({0, 1, 0}, {25, 0, 0}, 10)\n"
                         "print(hit.entity, near(hit.distance, 4.5))") == "Entity(Post)\ttrue");
        // Down, beside the post: the floor.
        CHECK(range.line("local hit = cordite.physics.raycast(vec3(2, 5, 2), vec3(0, -1, 0))\n"
                         "print(hit.entity, near(hit.position, vec3(2, 0, 2)), near(hit.normal, vec3(0, 1, 0)), "
                         "near(hit.distance, 5))") == "Entity(Floor)\ttrue\ttrue\ttrue");

        // What is further than the ray reaches is not hit; nor is anything by a ray that
        // reaches nowhere, or goes where nothing is.
        CHECK(range.line("print(cordite.physics.raycast(vec3(0, 1, 0), vec3(1, 0, 0), 4), "
                         "cordite.physics.raycast(vec3(0, 1, 0), vec3(1, 0, 0), 0), "
                         "cordite.physics.raycast(vec3(0, 1, 0), vec3(0, 1, 1)))") == "nil\tnil\tnil");
        // A trigger stops nothing, a ray included: this one goes through the zone.
        CHECK(range.line("print(cordite.physics.raycast(vec3(0, 1, 2), vec3(0, 0, 1), 20))") == "nil");
        // A ray that starts inside a body hits it at once.
        CHECK(range.line("local hit = cordite.physics.raycast(vec3(5, 1, 0), vec3(1, 0, 0))\n"
                         "print(hit.entity, hit.distance)") == "Entity(Post)\t0.0");

        CHECK(range.stage.refusal("cordite.physics.raycast(cordite.vec3(), cordite.vec3())") ==
              "bad argument #2 to 'raycast' (a direction: a vector of some length)");
        CHECK(range.stage.refusal("cordite.physics.raycast(cordite.vec3(), cordite.vec3(1, 0, 0), -1)") ==
              "bad argument #3 to 'raycast' (how far a ray reaches is a number from 0 up)");
        CHECK(range.stage.refusal("cordite.physics.raycast(cordite.vec3(), cordite.vec3(1, 0, 0), 0 / 0)") ==
              "bad argument #3 to 'raycast' (how far a ray reaches is a number from 0 up)");
        CHECK(range.stage.refusal("cordite.physics.raycast(cordite.vec3(), cordite.vec3(1, 0, 0), 1e300)") ==
              "bad argument #3 to 'raycast' (how far a ray reaches is a number from 0 up)");
        CHECK(range.stage.refusal("cordite.physics.raycast()") ==
              "bad argument #1 to 'raycast' (Vec3 expected, got no value)");
        CHECK(range.stage.refusal("cordite.physics.raycast(cordite.vec3(), 'east')") ==
              "bad argument #2 to 'raycast' (Vec3 expected, got string)");
    }

    TEST_CASE("a script moves a ball along a ray and asks what it touches first") {
        Range range;

        // A ball of half a metre touches the post half a metre before a ray would hit it:
        // where the two touch is on the post, and the normal points back at the ball.
        CHECK(range.line("local hit = cordite.physics.sphereCast(vec3(0, 1, 0), vec3(1, 0, 0), 0.5)\n"
                         "print(hit.entity, near(hit.distance, 4), near(hit.position, vec3(4.5, 1, 0)), "
                         "near(hit.normal, vec3(-1, 0, 0)))") == "Entity(Post)\ttrue\ttrue\ttrue");
        // A ray along the floor, a hand above it, hits nothing; a ball that wide rolls
        // into the post all the same - and past it, when it goes by at a distance.
        CHECK(range.line("print(cordite.physics.sphereCast(vec3(0, 1, 2), vec3(1, 0, 0), 0.5, 20), "
                         "cordite.physics.sphereCast(vec3(0, 1, 0.9), vec3(1, 0, 0), 0.5, 20).entity, "
                         "cordite.physics.sphereCast(vec3(0, 1, 0), vec3(1, 0, 0), 0.5, 3))") ==
              "nil\tEntity(Post)\tnil");

        CHECK(range.stage.refusal("cordite.physics.sphereCast(cordite.vec3(), cordite.vec3(1, 0, 0))") ==
              "bad argument #3 to 'sphereCast' (number expected, got no value)");
        CHECK(range.stage.refusal("cordite.physics.sphereCast(cordite.vec3(), cordite.vec3(1, 0, 0), -0.5)") ==
              "bad argument #3 to 'sphereCast' (the radius of a ball is a number from 0 up)");
        CHECK(range.stage.refusal("cordite.physics.sphereCast(cordite.vec3(), cordite.vec3(), 0.5)") ==
              "bad argument #2 to 'sphereCast' (a direction: a vector of some length)");
        CHECK(range.stage.refusal("cordite.physics.sphereCast(cordite.vec3(), cordite.vec3(1, 0, 0), 0.5, -1)") ==
              "bad argument #4 to 'sphereCast' (how far a ball is moved is a number from 0 up)");
    }

    TEST_CASE("a script asks what reaches into a ball or a box") {
        Range range;

        CHECK(range.line("print(names(cordite.physics.overlapSphere(vec3(5, 0, 0), 1)), "
                         "names(cordite.physics.overlapSphere(vec3(0, 10, 0), 0.6)), "
                         "'[' .. names(cordite.physics.overlapSphere(vec3(0, 5, 0), 1)) .. ']')") ==
              "Floor Post\tBall\t[]");
        // A trigger is not a body that reaches into anything: in the middle of the zone
        // there is nothing.
        CHECK(range.line("print(#cordite.physics.overlapSphere(vec3(0, 1, 5), 0.5))") == "0");
        // A box, as large as it is said to be along its axes - and turned, when it is:
        // a plank of four metres along X reaches the post from the middle of the range
        // only when it is not turned a quarter away from it.
        CHECK(range.line("print(names(cordite.physics.overlapBox(vec3(5, 2, 0), vec3(0.5, 0.5, 0.5))), "
                         "names(cordite.physics.overlapBox(vec3(3, 2, 0), vec3(4, 0.2, 0.2))), "
                         "'[' .. names(cordite.physics.overlapBox(vec3(3, 2, 0), vec3(4, 0.2, 0.2), "
                         "cordite.euler(0, 90, 0))) .. ']')") == "Post\tPost\t[]");
        // A list, with the entities in it as scripts know them.
        CHECK(range.line("local found = cordite.physics.overlapSphere(vec3(0, 10, 0), 0.6)\n"
                         "print(#found, found[1] == cordite.world.find('Ball'), found[2])") == "1\ttrue\tnil");

        CHECK(range.stage.refusal("cordite.physics.overlapSphere(cordite.vec3())") ==
              "bad argument #2 to 'overlapSphere' (number expected, got no value)");
        CHECK(range.stage.refusal("cordite.physics.overlapSphere(cordite.vec3(), -1)") ==
              "bad argument #2 to 'overlapSphere' (the radius of a ball is a number from 0 up)");
        CHECK(range.stage.refusal("cordite.physics.overlapBox(cordite.vec3(), cordite.vec3(1, -1, 1))") ==
              "bad argument #2 to 'overlapBox' (the size of a box: three numbers from 0 up)");
        CHECK(range.stage.refusal("cordite.physics.overlapBox(cordite.vec3(), cordite.vec3(1, 1, 1), 5)") ==
              "bad argument #3 to 'overlapBox' (Quat expected, got number)");
    }

    TEST_CASE("the body of an entity that is gone is no entity to a script") {
        Range range;
        // Destroyed between two frames: the simulation has not looked yet, and the body
        // is still in the way of a ray. The floor was the first body made, and is the
        // first the simulation finds: a list goes on after it, with no gap where it was.
        range.stage.world.destroyEntity(range.floor);
        CHECK(range.stage.lines(std::string(kHelpers) +
                                "local hit = cordite.physics.raycast(vec3(2, 5, 2), vec3(0, -1, 0))\n"
                                "print(hit.entity, near(hit.distance, 5))\n"
                                "local found = cordite.physics.overlapSphere(vec3(5, 0, 0), 1)\n"
                                "print(#found, found[1], found[2])") == Lines{"nil\ttrue", "1\tEntity(Post)\tnil"});
        // The step of that frame has taken the body away.
        CHECK(range.line("print(cordite.physics.raycast(vec3(2, 5, 2), vec3(0, -1, 0)))") == "nil");
    }

    TEST_CASE("a script asks what pulls at everything, and changes it") {
        Range range;
        CHECK(range.line("print(cordite.physics.gravity(), getmetatable(cordite.physics.gravity()) == cordite.Vec3)") ==
              "(0.0, -9.81, 0.0)\ttrue");
        CHECK(range.line("cordite.physics.setGravity({0, -1.5, 0})\nprint(cordite.physics.gravity())") ==
              "(0.0, -1.5, 0.0)");
        CHECK(range.stage.physics->simulation().gravity() == Vec3(0.0f, -1.5f, 0.0f));
        CHECK(range.stage.refusal("cordite.physics.setGravity(9.81)") ==
              "bad argument #1 to 'setGravity' (Vec3 expected, got number)");
        CHECK(range.stage.refusal("cordite.physics.setGravity(cordite.vec3(0, 0 / 0, 0))") ==
              "bad argument #1 to 'setGravity' (a vector of numbers that are finite, and not too large, expected)");
        CHECK(range.stage.physics->simulation().gravity() == Vec3(0.0f, -1.5f, 0.0f));
    }

    TEST_CASE("a script pushes, knocks and twists the body of an entity") {
        Range range;
        ScriptStage& stage = range.stage;
        REQUIRE(range.body().linearVelocity == Vec3(0.0f));

        // A knock changes the speed at once, by the impulse divided by the mass: two
        // kilograms, six newton seconds, three metres a second.
        CHECK(stage.lines("cordite.world.find('Ball'):addImpulse(cordite.vec3(6, 0, 0))").empty());
        CHECK_NEAR(range.body().linearVelocity, Vec3(3.0f, 0.0f, 0.0f), 1.0e-3f);

        // A force acts for one step - a sixtieth of a second - and is then forgotten:
        // 240 newtons on two kilograms add two metres a second, once. The frame that
        // takes the probe away again adds nothing.
        CHECK(stage.lines("cordite.world.find('Ball'):addForce({0, 240, 0})").empty());
        CHECK_NEAR(range.body().linearVelocity, Vec3(3.0f, 2.0f, 0.0f), 1.0e-3f);

        // A twist turns it: about the axis of the torque, the way of the right hand.
        CHECK(range.body().angularVelocity == Vec3(0.0f));
        CHECK(stage.lines("cordite.world.find('Ball'):addTorque(cordite.vec3(0, 12, 0))").empty());
        CHECK(range.body().angularVelocity.y > 0.1f);
        CHECK(range.body().angularVelocity.x == doctest::Approx(0.0));
        CHECK(range.body().angularVelocity.z == doctest::Approx(0.0));

        // What stands still by its nature is not moved, and says nothing against it.
        CHECK(stage.lines("cordite.world.find('Floor'):addImpulse(cordite.vec3(100, 0, 0))").empty());
        CHECK(stage.world.transform(range.floor).position == Vec3(0.0f));

        // An entity without a body cannot be pushed: one that has no collider, and one
        // that was only just given one - its body is made by the step of the frame.
        const Entity bare = stage.world.createEntity("Bare");
        static_cast<void>(bare);
        const std::string noBody = "the entity has no body to push: it needs a Collider, and has a body from the "
                                   "frame after it was given one";
        CHECK(stage.refusal("cordite.world.find('Bare'):addForce(cordite.vec3(1, 0, 0))") == noBody);
        CHECK(stage.refusal("cordite.world.find('Bare'):addImpulse(cordite.vec3(1, 0, 0))") == noBody);
        CHECK(stage.refusal("cordite.world.find('Bare'):addTorque(cordite.vec3(1, 0, 0))") == noBody);
        CHECK(stage.refusal("local made = cordite.world.create('Made')\n"
                            "made:add('Collider')\n"
                            "made:addImpulse(cordite.vec3(1, 0, 0))") == noBody);
        // In the next frame it has one.
        CHECK(stage.lines("cordite.world.find('Made'):addImpulse(cordite.vec3(1, 0, 0))").empty());

        CHECK(stage.refusal("cordite.world.find('Ball'):addForce()") ==
              "bad argument #1 to 'addForce' (Vec3 expected, got no value)");
        CHECK(stage.refusal("cordite.world.find('Ball'):addImpulse(cordite.vec3(0 / 0, 0, 0))") ==
              "bad argument #1 to 'addImpulse' (a vector of numbers that are finite, and not too large, expected)");
        CHECK(stage.refusal("cordite.world.find('Ball'):addTorque('twist')") ==
              "bad argument #1 to 'addTorque' (Vec3 expected, got string)");
    }

    TEST_CASE("a world that is not simulated has no physics to ask, and says so") {
        ScriptStage stage;
        const std::string none = "nothing is simulated here: this world has no physics to ask";
        for (const char* asked :
             {"cordite.physics.raycast(cordite.vec3(), cordite.vec3(1, 0, 0))",
              "cordite.physics.sphereCast(cordite.vec3(), cordite.vec3(1, 0, 0), 1)",
              "cordite.physics.overlapSphere(cordite.vec3(), 1)",
              "cordite.physics.overlapBox(cordite.vec3(), cordite.vec3(1, 1, 1))", "cordite.physics.gravity()",
              "cordite.physics.setGravity(cordite.vec3())", "entity:addForce(cordite.vec3())",
              "entity:addImpulse(cordite.vec3())", "entity:addTorque(cordite.vec3())"}) {
            CAPTURE(asked);
            CHECK(stage.refusal(asked) == none);
        }
    }

    TEST_CASE("the functions of the simulation are said in words") {
        const ScriptStage stage;
        Lines names;
        for (const ScriptFunctionInfo& function : stage.scripts->runtime().functions()) {
            if (function.name.starts_with("cordite.physics.")) {
                names.push_back(function.name);
                CHECK(function.signature.starts_with(function.name + "("));
            }
        }
        CHECK(names == Lines{"cordite.physics.raycast", "cordite.physics.sphereCast", "cordite.physics.overlapSphere",
                             "cordite.physics.overlapBox", "cordite.physics.gravity", "cordite.physics.setGravity"});
    }
}
