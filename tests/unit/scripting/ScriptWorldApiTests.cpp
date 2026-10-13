#include "cordite/assets/AssetDatabase.h"
#include "cordite/core/Json.h"
#include "cordite/core/Uuid.h"
#include "cordite/math/Math.h"
#include "cordite/math/Transform.h"
#include "cordite/scripting/ScriptRuntime.h"
#include "cordite/scripting/ScriptSystem.h"
#include "cordite/world/Components.h"
#include "cordite/world/Entity.h"
#include "cordite/world/World.h"

#include "support/MathTestSupport.h"
#include "unit/scripting/ScriptTestSupport.h"

#include <string>
#include <vector>

using namespace cordite;
using cordite::test::ScriptStage;
using cordite::test::unwrap;

namespace {

using Lines = std::vector<std::string>;

/// The world most of these tests look at:
///
///     Yard
///       Shed          (at 10, 0, 0 - turned a quarter to the left, twice as large)
///         Shelf       (at 0, 1, -1 under the shed)
///     Gate
struct Yard {
    ScriptStage stage;
    Entity yard = stage.world.createEntity("Yard");
    Entity shed = stage.world.createEntity("Shed");
    Entity shelf = stage.world.createEntity("Shelf");
    Entity gate = stage.world.createEntity("Gate");

    Yard() {
        World& world = stage.world;
        REQUIRE(world.setParent(shed, yard).has_value());
        REQUIRE(world.setParent(shelf, shed).has_value());
        world.transform(shed) = {.position = Vec3(10.0f, 0.0f, 0.0f),
                                 .rotation = quatFromEulerDegrees(Vec3(0.0f, 90.0f, 0.0f)),
                                 .scale = Vec3(2.0f)};
        world.transform(shelf).position = Vec3(0.0f, 1.0f, -1.0f);
    }

    [[nodiscard]] Vec3 worldPosition(Entity entity) const { return Vec3(stage.world.computeWorldMatrix(entity)[3]); }
};

/// Lua that tells whether two vectors, or two rotations, are the same but for what
/// single floats lose on the way.
constexpr const char* kNear = R"(
    local function near(a, b)
        local worst = 0
        for _, name in ipairs({"x", "y", "z", "w"}) do
            if a[name] ~= nil or b[name] ~= nil then worst = math.max(worst, math.abs(a[name] - b[name])) end
        end
        return worst < 1.0e-5
    end
)";

/// A prefab of a cart at (1, 2, 3) with a wheel under it, whose script says that it runs
/// and what it was given: "prefabs/cart.prefab.json". Returns its id.
Uuid writeCart(ScriptStage& stage) {
    const Uuid wheel = stage.write("scripts/wheel.lua", R"(
        local Wheel = { properties = { spokes = 3 } }
        function Wheel:start()
            print("start " .. self.entity:name() .. " of " .. self.entity:parent():name() .. " with " ..
                  self.properties.spokes)
        end
        return Wheel
    )");
    Json prefab = unwrap(parseJson(R"({"format": "cordite.prefab", "version": 1, "name": "Cart", "entities": [
        {"id": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name": "Cart", "enabled": true, "parent": null,
         "components": {"Transform": {"position": [1, 2, 3], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1]}}},
        {"id": "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "name": "Wheel", "enabled": true,
         "parent": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
         "components": {"Script": {"script": null, "properties": {"spokes": 5}}}}]})"));
    prefab["entities"][1]["components"]["Script"]["script"] = wheel.toString();
    return stage.write("prefabs/cart.prefab.json", dumpJson(prefab));
}

} // namespace

TEST_SUITE("ScriptWorldApi") {
    TEST_CASE("a script finds entities by name, by id and by what they have") {
        Yard yard;
        ScriptStage& stage = yard.stage;
        const std::string shedId = stage.world.id(yard.shed).toString();

        CHECK(
            stage.line(
                "print(cordite.world.find('Shed'), cordite.world.find('Shelf'):name(), cordite.world.find('Barn'))") ==
            "Entity(Shed)\tShelf\tnil");
        CHECK(stage.line("local shed = cordite.world.findById('" + shedId +
                         "')\n"
                         "print(shed, shed == cordite.world.find('Shed'), shed:id() == '" +
                         shedId +
                         "',\n"
                         "      cordite.world.findById('00000000-0000-4000-8000-000000000000'))") ==
              "Entity(Shed)\ttrue\ttrue\tnil");
        CHECK(stage.refusal("cordite.world.findById('the shed')") ==
              "bad argument #1 to 'findById' (the id of an entity expected, as entity:id() gives it)");
        CHECK(stage.refusal("cordite.world.find()") == "bad argument #1 to 'find' (string expected, got no value)");

        // Lists are in the order of the hierarchy.
        CHECK(stage.line("local names = {}\n"
                         "for _, root in ipairs(cordite.world.roots()) do names[#names + 1] = root:name() end\n"
                         "print(table.concat(names, ' '))") == "Yard Gate Probe");
        CHECK(stage.line("local names = {}\n"
                         "for _, found in ipairs(cordite.world.withComponent('Transform')) do names[#names + 1] = "
                         "found:name() end\n"
                         "print(table.concat(names, ' '), #cordite.world.withComponent('Script'), "
                         "#cordite.world.withComponent('Camera'))") == "Yard Shed Shelf Gate Probe\t1\t0");
        CHECK(stage.refusal("cordite.world.withComponent('Roof')") ==
              "bad argument #1 to 'withComponent' (the name of a component expected, such as \"Light\")");

        // Every kind of component there is, by the name it is asked for with: here, those
        // of the world and the one of the scripts, in the order they were registered.
        CHECK(stage.line("print(table.concat(cordite.world.componentTypes(), ' '))") ==
              "Transform Camera PrefabInstance Script");
    }

    TEST_CASE("a script makes entities, and they are there at once") {
        Yard yard;
        ScriptStage& stage = yard.stage;

        CHECK(stage.line("local crate = cordite.world.create('Crate')\n"
                         "local lid = cordite.world.create('Lid', crate)\n"
                         "local plain = cordite.world.create()\n"
                         "print(crate:isValid(), crate:parent(), lid:parent(), plain:name(), #crate:children(), "
                         "cordite.world.find('Lid') == lid)") == "true\tnil\tEntity(Crate)\tEntity\t1\ttrue");
        const Entity crate = stage.world.findByName("Crate");
        const Entity lid = stage.world.findByName("Lid");
        REQUIRE_FALSE(crate.isNull());
        REQUIRE_FALSE(lid.isNull());
        CHECK(stage.world.parent(lid) == crate);
        CHECK(stage.world.parent(crate).isNull());

        // A new entity under a parent is where the parent is, turned and sized as it is.
        CHECK(stage.line("local hook = cordite.world.create('Hook', cordite.world.find('Shed'))\n"
                         "print(hook:position(), hook:scale(), hook:worldPosition())") ==
              "(0.0, 0.0, 0.0)\t(1.0, 1.0, 1.0)\t(10.0, 0.0, 0.0)");
        CHECK(stage.refusal("cordite.world.create(5, 'Shed')") ==
              "bad argument #2 to 'create' (Entity expected, got string)");
    }

    TEST_CASE("a script reads the hierarchy and changes it") {
        Yard yard;
        ScriptStage& stage = yard.stage;

        CHECK(stage.line("local shed = cordite.world.find('Shed')\n"
                         "print(shed:parent(), cordite.world.find('Yard'):parent(), #shed:children(), "
                         "shed:children()[1], #cordite.world.find('Gate'):children())") ==
              "Entity(Yard)\tnil\t1\tEntity(Shelf)\t0");

        // Hung somewhere else, an entity stays where it is in the world - unless it is
        // told to keep its own place under the new parent instead.
        const Vec3 before = yard.worldPosition(yard.shelf);
        CHECK(stage.line("local shelf, gate = cordite.world.find('Shelf'), cordite.world.find('Gate')\n"
                         "shelf:setParent(gate)\n"
                         "print(shelf:parent(), #gate:children())") == "Entity(Gate)\t1");
        CHECK_NEAR(yard.worldPosition(yard.shelf), before, 1.0e-4f);
        CHECK(stage.line("local shelf = cordite.world.find('Shelf')\n"
                         "shelf:setParent(nil)\n"
                         "print(shelf:parent())") == "nil");
        CHECK_NEAR(yard.worldPosition(yard.shelf), before, 1.0e-4f);
        CHECK(stage.line("local shelf, shed = cordite.world.find('Shelf'), cordite.world.find('Shed')\n"
                         "shelf:setPosition(cordite.vec3(0, 1, -1))\n"
                         "shelf:setParent(shed, false)\n"
                         "print(shelf:parent(), shelf:position())") == "Entity(Shed)\t(0.0, 1.0, -1.0)");
        CHECK_NEAR(yard.worldPosition(yard.shelf), before, 1.0e-4f);

        // Nothing hangs under itself, or under what hangs under it.
        CHECK(stage.refusal("local shed = cordite.world.find('Shed') shed:setParent(shed)") ==
              "'Shed' cannot become a child of itself or of its descendant 'Shed'");
        CHECK(stage.refusal("cordite.world.find('Yard'):setParent(cordite.world.find('Shelf'))") ==
              "'Yard' cannot become a child of itself or of its descendant 'Shelf'");
        CHECK(stage.world.parent(yard.yard).isNull());
        CHECK(stage.refusal("entity:setParent('Yard')") ==
              "bad argument #1 to 'setParent' (Entity expected, got string)");
    }

    TEST_CASE("a script reads where an entity is and puts it somewhere else") {
        Yard yard;
        ScriptStage& stage = yard.stage;

        // Floats reach a script as the numbers a person would write for them.
        stage.world.transform(yard.gate).position = Vec3(0.1f, -0.2f, 1.0f / 3.0f);
        CHECK(stage.line("local gate = cordite.world.find('Gate')\n"
                         "print(gate:position(), gate:rotation(), gate:scale())") ==
              "(0.1, -0.2, 0.33333334)\t(0.0, 0.0, 0.0, 1.0)\t(1.0, 1.0, 1.0)");

        CHECK(stage.lines("local gate = cordite.world.find('Gate')\n"
                          "gate:setPosition(cordite.vec3(1, 2, 3))\n"
                          "gate:setRotation(cordite.euler(0, 90, 0))\n"
                          "gate:setScale({2, 2, 2})\n"
                          "gate:translate(cordite.vec3(0.5, 0, 0))\n"
                          "local position = gate:position()\n"
                          "position.y = 99                      -- a copy: the gate is not moved by this\n"
                          "print(gate:position(), gate:scale())") == Lines{"(1.5, 2.0, 3.0)\t(2.0, 2.0, 2.0)"});
        const Transform& gate = stage.world.transform(yard.gate);
        CHECK(gate.position == Vec3(1.5f, 2.0f, 3.0f));
        CHECK(gate.scale == Vec3(2.0f));
        CHECK_NEAR(gate.rotation * kWorldForward, Vec3(-1.0f, 0.0f, 0.0f), 1.0e-5f);

        // What is no place is refused: a number that is none, or too large for a float.
        const Transform before = gate;
        CHECK(stage.refusal("cordite.world.find('Gate'):setPosition(cordite.vec3(0 / 0, 0, 0))") ==
              "bad argument #1 to 'setPosition' (a vector of numbers that are finite, and not too large, expected)");
        CHECK(stage.refusal("cordite.world.find('Gate'):setPosition(cordite.vec3(1e300, 0, 0))") ==
              "bad argument #1 to 'setPosition' (a vector of numbers that are finite, and not too large, expected)");
        CHECK(stage.refusal("cordite.world.find('Gate'):setScale(cordite.vec3(1, math.huge, 1))") ==
              "bad argument #1 to 'setScale' (a vector of numbers that are finite, and not too large, expected)");
        CHECK(stage.refusal("cordite.world.find('Gate'):setPosition(5)") ==
              "bad argument #1 to 'setPosition' (Vec3 expected, got number)");
        CHECK(stage.refusal("cordite.world.find('Gate'):setRotation(cordite.vec3(0, 1, 0))") ==
              "bad argument #1 to 'setRotation' (Quat expected, got Vec3)");
        CHECK(stage.refusal("cordite.world.find('Gate'):setRotation({0, 0, 0, 0})") ==
              "bad argument #1 to 'setRotation' (a rotation has a length, and numbers that are finite)");
        CHECK(stage
                  .refusal("local gate = cordite.world.find('Gate')\n"
                           "gate:setPosition(cordite.vec3(3e38, 0, 0))\n"
                           "gate:translate(cordite.vec3(3e38, 0, 0))")
                  .find("the entity would be moved further than there are numbers for") != std::string::npos);
        stage.world.transform(yard.gate) = before;
    }

    TEST_CASE("a script asks where an entity is in the world, and puts it there") {
        Yard yard;
        ScriptStage& stage = yard.stage;

        // The shelf is one up and one forward under a shed that is turned a quarter to the
        // left and twice as large: two up, and two to the left of the shed.
        CHECK(stage.line(std::string(kNear) +
                         "local shed, shelf = cordite.world.find('Shed'), cordite.world.find('Shelf')\n"
                         "print(near(shelf:worldPosition(), cordite.vec3(8, 2, 0)), shed:worldPosition(), "
                         "shelf:position(),\n"
                         "      near(shelf:worldRotation(), cordite.euler(0, 90, 0)), near(shelf:forward(), "
                         "cordite.vec3(-1, 0, 0)),\n"
                         "      near(shelf:right(), cordite.vec3(0, 0, -1)), near(shelf:up(), cordite.vec3(0, 1, 0)),\n"
                         "      near(cordite.world.find('Gate'):forward(), cordite.vec3(0, 0, -1)))") ==
              "true\t(10.0, 0.0, 0.0)\t(0.0, 1.0, -1.0)\ttrue\ttrue\ttrue\ttrue\ttrue");

        // What was only just moved is where it was moved to: nothing waits for a frame.
        CHECK(stage.lines(std::string(kNear) +
                          "local shed, shelf = cordite.world.find('Shed'), cordite.world.find('Shelf')\n"
                          "shed:setPosition(cordite.vec3(0, 0, 0))\n"
                          "print(near(shelf:worldPosition(), cordite.vec3(-2, 2, 0)))\n"
                          "shelf:setWorldPosition(cordite.vec3(4, 6, -8))\n"
                          "print(near(shelf:worldPosition(), cordite.vec3(4, 6, -8)), near(shelf:position(), "
                          "cordite.vec3(4, 3, 2)))") == Lines{"true", "true\ttrue"});
        CHECK_NEAR(yard.worldPosition(yard.shelf), Vec3(4.0f, 6.0f, -8.0f), 1.0e-4f);
        CHECK(stage.line("local gate = cordite.world.find('Gate')\n"
                         "gate:setWorldPosition(cordite.vec3(1, 2, 3))\n"
                         "print(gate:position())") == "(1.0, 2.0, 3.0)");

        // Under something of no size there is no place that leads anywhere.
        stage.world.transform(yard.shed).scale = Vec3(1.0f, 0.0f, 1.0f);
        const Vec3 before = stage.world.transform(yard.shelf).position;
        CHECK(stage.refusal("cordite.world.find('Shelf'):setWorldPosition(cordite.vec3(1, 2, 3))") ==
              "the entity cannot be put there: what it hangs under has no size");
        CHECK(stage.world.transform(yard.shelf).position == before);

        // What is small all round is not flat: under a shed a hundred-thousandth its size
        // the shelf is put where it is asked to be, a hundred thousand times as far in
        // the shed's own measure.
        stage.world.transform(yard.shed).scale = Vec3(1.0e-5f);
        CHECK(stage.line(
                  std::string(kNear) +
                  "local shelf = cordite.world.find('Shelf')\n"
                  "shelf:setWorldPosition(cordite.vec3(1, 2, 3))\n"
                  "print(near(shelf:worldPosition(), cordite.vec3(1, 2, 3)), shelf:position():length() > 100000)") ==
              "true\ttrue");
        CHECK_NEAR(yard.worldPosition(yard.shelf), Vec3(1.0f, 2.0f, 3.0f), 1.0e-4f);

        // Under something small enough, a place of the world is further away than its
        // measure has numbers for.
        stage.world.transform(yard.shed).scale = Vec3(1.0e-10f);
        const Vec3 held = stage.world.transform(yard.shelf).position;
        CHECK(stage.refusal("cordite.world.find('Shelf'):setWorldPosition(cordite.vec3(0, 1e30, 0))") ==
              "the entity cannot be put there: the place is too far for what it hangs under");
        CHECK(stage.world.transform(yard.shelf).position == held);
    }

    TEST_CASE("a script turns an entity: further, or to face a point") {
        Yard yard;
        ScriptStage& stage = yard.stage;

        CHECK(
            stage.lines(std::string(kNear) +
                        "local gate = cordite.world.find('Gate')\n"
                        "gate:rotate(cordite.euler(0, 90, 0))\n"
                        "gate:rotate(cordite.euler(0, 90, 0))\n"
                        "print(near(gate:forward(), cordite.vec3(0, 0, 1)))\n"
                        "gate:setRotation(cordite.euler(90, 0, 0))    -- it looks up, its own up points backwards\n"
                        "gate:rotate(cordite.euler(0, 90, 0))         -- and about that it is turned, not the world's\n"
                        "print(near(gate:forward(), cordite.vec3(-1, 0, 0)), near(gate:up(), cordite.vec3(0, 0, 1)))\n"
                        "gate:setRotation(cordite.quat())\n"
                        "gate:setPosition(cordite.vec3(0, 0, 0))\n"
                        "gate:lookAt(cordite.vec3(10, 0, 0))\n"
                        "print(near(gate:forward(), cordite.vec3(1, 0, 0)), near(gate:up(), cordite.vec3(0, 1, 0)))\n"
                        "gate:lookAt(cordite.vec3(0, -5, 0), cordite.vec3(1, 0, 0))\n"
                        "print(near(gate:forward(), cordite.vec3(0, -1, 0)), near(gate:up(), cordite.vec3(1, 0, 0)))\n"
                        "local turned = gate:rotation()\n"
                        "gate:lookAt(gate:worldPosition())          -- where it is: it keeps looking as it did\n"
                        "print(gate:rotation() == turned)") ==
            Lines{"true", "true\ttrue", "true\ttrue", "true\ttrue", "true"});

        // Under a parent that is turned itself, facing a point of the world takes
        // another turn of its own.
        CHECK(stage.line(std::string(kNear) +
                         "local shelf = cordite.world.find('Shelf')\n"
                         "shelf:lookAt(shelf:worldPosition() + cordite.vec3(0, 0, -3))\n"
                         "print(near(shelf:forward(), cordite.vec3(0, 0, -1)), near(shelf:rotation(), "
                         "cordite.euler(0, -90, 0)))") == "true\ttrue");
        CHECK(stage.refusal("entity:lookAt()") == "bad argument #1 to 'lookAt' (Vec3 expected, got no value)");
        CHECK(stage.refusal("entity:rotate(cordite.vec3())") ==
              "bad argument #1 to 'rotate' (Quat expected, got Vec3)");
    }

    TEST_CASE("a script knows what time it is") {
        ScriptStage stage;
        const Uuid clock = stage.write("scripts/clock.lua",
                                       "return {update = function(self, deltaSeconds)\n"
                                       "    print(cordite.time.frame(), cordite.time.delta(), cordite.time.seconds(), "
                                       "deltaSeconds == cordite.time.delta())\n"
                                       "end}");
        stage.add("Clock", clock);
        stage.update(0.25);
        stage.update(0.5);
        stage.update(-3.0);
        stage.update(0.125);
        CHECK(stage.said() ==
              Lines{"1\t0.25\t0.25\ttrue", "2\t0.5\t0.75\ttrue", "3\t0.0\t0.75\ttrue", "4\t0.125\t0.875\ttrue"});
        CHECK(stage.scripts->seconds() == 0.875);
        CHECK(stage.scripts->updates() == 4);
        CHECK(stage.scripts->deltaSeconds() == 0.125);

        // Cleared, the scripts start from nothing: also their time.
        stage.scripts->clear(stage.context());
        CHECK(stage.scripts->seconds() == 0.0);
        stage.update(1.0);
        CHECK(stage.said() == Lines{"1\t1.0\t1.0\ttrue"});
    }

    TEST_CASE("a script finds an asset by where it is") {
        ScriptStage stage;
        const Uuid door = stage.write("scripts/door.lua", "return {}");
        const std::string cube = builtinMeshId(BuiltinMesh::Cube).toString();

        CHECK(stage.line("print(cordite.assets.find('scripts/door.lua'), cordite.assets.find('builtin#mesh/cube'), "
                         "cordite.assets.find('scripts/gone.lua'))") == door.toString() + "\t" + cube + "\tnil");
        CHECK(stage.refusal("cordite.assets.find({})") == "bad argument #1 to 'find' (string expected, got table)");
    }

    TEST_CASE("a script places a prefab at once, and the scripts in it start with the next frame") {
        ScriptStage stage;
        const Uuid cart = writeCart(stage);
        const Uuid spawner = stage.write("scripts/spawner.lua", R"(
            local Spawner = {}
            function Spawner:start()
                local cart = cordite.world.instantiate("prefabs/cart.prefab.json")
                local wheel = cart:children()[1]
                print(cart, cart:parent(), #cart:children(), wheel, wheel:parent() == cart)
                print(cart:position().x, cart:position().y, cart:position().z)
                print(cart:has("PrefabInstance"),
                      cart:get("PrefabInstance", "prefab") == cordite.assets.find("prefabs/cart.prefab.json"),
                      wheel:has("PrefabInstance"))
                print(cart:id() ~= "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", cordite.world.find("Cart") == cart)

                -- By its id as well - and that is another cart, with a wheel of its own.
                local second = cordite.world.instantiate(cordite.assets.find("prefabs/cart.prefab.json"))
                print(second ~= cart, second:id() ~= cart:id(), second:children()[1] ~= wheel,
                      #cordite.world.withComponent("PrefabInstance"))
            end
            return Spawner
        )");
        stage.add("Spawner", spawner);

        // The frame the prefab is placed in: its entities are there before the call returns.
        stage.update();
        CHECK(stage.said() == Lines{"Entity(Cart)\tnil\t1\tEntity(Wheel)\ttrue", "1.0\t2.0\t3.0", "true\ttrue\tfalse",
                                    "true\ttrue", "true\ttrue\ttrue\t2"});
        CHECK(stage.problems().empty());
        const World& world = stage.world;
        CHECK(world.entityCount() == 5);
        const Entity placed = world.findByName("Cart");
        REQUIRE(world.has<PrefabInstanceComponent>(placed));
        CHECK(world.tryGet<PrefabInstanceComponent>(placed)->prefab == cart);

        // The frame after: what the wheels are told to do, they do from here on - with
        // what the prefab gives them, not with what the script would have by itself.
        stage.update();
        CHECK(stage.said() == Lines{"start Wheel of Cart with 5", "start Wheel of Cart with 5"});
        stage.update();
        CHECK(stage.said().empty());
        CHECK(stage.problems().empty());
    }

    TEST_CASE("a script says where a prefab goes: under what, by what name, where and turned how") {
        Yard yard;
        ScriptStage& stage = yard.stage;
        writeCart(stage);

        CHECK(stage.line(std::string(kNear) + R"(
            local shed = cordite.world.find("Shed")
            local cart = cordite.world.instantiate("prefabs/cart.prefab.json", {
                parent = shed, name = "Wagon", position = cordite.vec3(0, 0, 4), rotation = cordite.euler(0, 90, 0)})
            print(cart, cart:parent(), shed:children()[2] == cart, near(cart:position(), cordite.vec3(0, 0, 4)),
                  near(cart:rotation(), cordite.euler(0, 90, 0)), cart:children()[1])
        )") == "Entity(Wagon)\tEntity(Shed)\ttrue\ttrue\ttrue\tEntity(Wheel)");
        // Relative to what it hangs under: the shed is at (10, 0, 0), turned a quarter to
        // the left and twice as large - four along its Z is eight along the world's X.
        const Entity wagon = stage.world.findByName("Wagon");
        CHECK_NEAR(yard.worldPosition(wagon), Vec3(18.0f, 0.0f, 0.0f), 1.0e-4f);

        // Each of them alone, and tables written in passing; what is not said is as the
        // prefab has it.
        CHECK(stage.lines(std::string(kNear) + R"(
            local moved = cordite.world.instantiate("prefabs/cart.prefab.json", {position = {7, 8, 9}})
            local turned = cordite.world.instantiate("prefabs/cart.prefab.json", {rotation = {0, 2, 0, 0}})
            local named = cordite.world.instantiate("prefabs/cart.prefab.json", {name = "Barrow"})
            local plain = cordite.world.instantiate("prefabs/cart.prefab.json", {})
            print(moved:name(), near(moved:position(), cordite.vec3(7, 8, 9)),
                  near(moved:rotation(), {x = 0, y = 0, z = 0, w = 1}))
            -- A rotation need not be of length one; it is made so.
            print(near(turned:rotation(), {x = 0, y = 1, z = 0, w = 0}), near(turned:position(), cordite.vec3(1, 2, 3)))
            print(named, near(named:position(), cordite.vec3(1, 2, 3)), named:parent())
            print(plain, cordite.world.instantiate("prefabs/cart.prefab.json", nil))
        )") == Lines{"Cart\ttrue\ttrue", "true\ttrue", "Entity(Barrow)\ttrue\tnil", "Entity(Cart)\tEntity(Cart)"});
    }

    TEST_CASE("what is no prefab, and an option there is none of, is an error of the script that says which") {
        Yard yard;
        ScriptStage& stage = yard.stage;
        writeCart(stage);
        stage.write("scripts/other.lua", "return {}");
        const char* twoRoots = R"({"format": "cordite.prefab", "version": 1, "entities": [
            {"id": "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", "name": "Cart", "enabled": true, "parent": null,
             "components": {}},
            {"id": "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", "name": "Wheel", "enabled": true, "parent": null,
             "components": {}}]})";
        stage.write("prefabs/two.prefab.json", twoRoots);
        stage.write("prefabs/words.prefab.json", "these are words");
        const auto refusal = [&stage](const std::string& arguments) {
            return stage.refusal("cordite.world.instantiate(" + arguments + ")");
        };

        // What names no prefab.
        CHECK(refusal("") == "bad argument #1 to 'instantiate' (string expected, got no value)");
        CHECK(refusal("'prefabs/none.prefab.json'") ==
              "bad argument #1 to 'instantiate' (there is no asset 'prefabs/none.prefab.json')");
        CHECK(refusal("'00000000-0000-4000-8000-000000000001'") ==
              "bad argument #1 to 'instantiate' (there is no asset '00000000-0000-4000-8000-000000000001')");
        CHECK(refusal("'scripts/other.lua'") ==
              "bad argument #1 to 'instantiate' ('scripts/other.lua' is of the kind script, not a prefab)");
        // Named by its id, what is no prefab is still called by where it is.
        CHECK(refusal("cordite.assets.find('scripts/other.lua')") ==
              "bad argument #1 to 'instantiate' ('scripts/other.lua' is of the kind script, not a prefab)");

        // A prefab that is none inside: the message begins with the file.
        CHECK(refusal("'prefabs/two.prefab.json'") ==
              "prefabs/two.prefab.json: the entities must have one root with all the others below it, but 2 of them "
              "have no parent among them");
        CHECK(refusal("'prefabs/words.prefab.json'").find("words.prefab.json") != std::string::npos);

        // Options that are none, and values that are none for the option.
        const std::string cart = "'prefabs/cart.prefab.json', ";
        CHECK(refusal(cart + "5") == "bad argument #2 to 'instantiate' (table expected, got number)");
        CHECK(refusal(cart + "{scale = 2}") ==
              "an instance has no option 'scale'; it has: parent, name, position, rotation");
        CHECK(refusal(cart + "{'Shed'}") == "the options of an instance have names: parent, name, position, rotation");
        CHECK(refusal(cart + "{parent = 'Shed'}") == "the option 'parent' is an entity, not a string");
        CHECK(refusal(cart + "{name = 5}") == "the option 'name' is text, not a number");
        CHECK(refusal(cart + "{position = 'here'}") == "the option 'position' is a vector: a table with x, y and z");
        CHECK(refusal(cart + "{position = {1, 2}}") == "the option 'position' is a vector: a table with x, y and z");
        CHECK(refusal(cart + "{rotation = {0, 0, 1}}") ==
              "the option 'rotation' is a rotation: a table with x, y, z and w");
        // A place that is none, which is for what places the prefab to say.
        CHECK(refusal(cart + "{position = {0, 1e39, 0}}") ==
              "prefabs/cart.prefab.json: the position of a prefab instance must be finite");
        CHECK(refusal(cart + "{rotation = {0, 0, 0, 0}}") ==
              "prefabs/cart.prefab.json: the rotation of a prefab instance must be finite and must not be zero");

        // An entity that is gone is nothing to hang a cart under. It is gone once the
        // frame it was destroyed in is over.
        const Uuid late = stage.write("scripts/late.lua", R"(
            local Late = {}
            function Late:start()
                self.gone = cordite.world.create("Gone")
                self.gone:destroy()
                self.frames = 0
            end
            function Late:update()
                self.frames = self.frames + 1
                if self.frames == 2 then
                    cordite.world.instantiate("prefabs/cart.prefab.json", {parent = self.gone})
                end
            end
            return Late
        )");
        stage.add("Late", late);
        stage.update();
        CHECK(stage.problems().empty());
        stage.update();
        REQUIRE(stage.problems().size() == 1);
        CHECK(stage.problems().front().find("the option 'parent' is an entity that is gone: it was destroyed") !=
              std::string::npos);

        // Nothing of all that has left a cart behind.
        CHECK(stage.world.findByName("Cart").isNull());
        CHECK(stage.world.findByName("Wheel").isNull());
    }

    TEST_CASE("a script asks for another scene, which whoever owns the world then loads") {
        ScriptStage stage;
        const char* empty = R"({"format": "cordite.scene", "version": 1, "name": "Cellar", "entities": []})";
        const Uuid cellar = stage.write("scenes/cellar.scene.json", empty);
        const Uuid attic = stage.write("scenes/attic.scene.json", empty);
        stage.write("scripts/other.lua", "return {}");
        CHECK_FALSE(stage.scripts->takeRequestedScene().has_value());

        // Asking changes nothing: the script goes on in the scene it is in.
        CHECK(stage.line("cordite.scene.load('scenes/cellar.scene.json')\n"
                         "print(entity:isValid(), cordite.world.find('Probe') == entity)") == "true\ttrue");
        CHECK(stage.scripts->takeRequestedScene() == cellar);
        // Taken, it is asked for no longer.
        CHECK_FALSE(stage.scripts->takeRequestedScene().has_value());

        // By its id as well, as cordite.assets.find() gives it; and of two that ask, the
        // last has its way.
        CHECK(stage
                  .lines("cordite.scene.load('scenes/cellar.scene.json')\n"
                         "cordite.scene.load(cordite.assets.find('scenes/attic.scene.json'))")
                  .empty());
        CHECK(stage.scripts->takeRequestedScene() == attic);

        // What is no scene is refused where it is asked for, and nothing is asked then.
        CHECK(stage.refusal("cordite.scene.load('scenes/garden.scene.json')") ==
              "bad argument #1 to 'load' (there is no asset 'scenes/garden.scene.json')");
        CHECK(stage.refusal("cordite.scene.load('scripts/other.lua')") ==
              "bad argument #1 to 'load' ('scripts/other.lua' is of the kind script, not a scene)");
        CHECK(stage.refusal("cordite.scene.load('00000000-0000-4000-8000-000000000001')") ==
              "bad argument #1 to 'load' (there is no asset '00000000-0000-4000-8000-000000000001')");
        CHECK(stage.refusal("cordite.scene.load()") == "bad argument #1 to 'load' (string expected, got no value)");
        CHECK_FALSE(stage.scripts->takeRequestedScene().has_value());

        // A system that is cleared - which is what loading a scene begins with - no
        // longer asks for what a script of the old scene wanted.
        CHECK(stage.lines("cordite.scene.load('scenes/cellar.scene.json')").empty());
        stage.scripts->clear(stage.context());
        CHECK_FALSE(stage.scripts->takeRequestedScene().has_value());
    }

    TEST_CASE("one script reaches another through its entity") {
        ScriptStage stage;
        const Uuid lamp = stage.write("scripts/lamp.lua", R"(
            local Lamp = {}
            function Lamp:start() self.lit = false end
            function Lamp:switch() self.lit = not self.lit return self.lit end
            return Lamp
        )");
        const Uuid switcher = stage.write("scripts/switch.lua", R"(
            local Switch = {}
            function Switch:update()
                local lamp = cordite.world.find("Lamp"):script()
                print(lamp:switch(), lamp.lit, lamp.entity:name(), self.entity:script() == self,
                      cordite.world.find("Wall"):script(), cordite.world.find("Dark"):script(),
                      cordite.world.find("Broken"):script())
            end
            return Switch
        )");
        stage.add("Lamp", lamp);
        stage.add("Switch", switcher);
        stage.world.createEntity("Wall");
        // A script that is off has no instance yet, and one that could not be loaded never has.
        stage.world.setEnabled(stage.add("Dark", lamp), false);
        stage.add("Broken", stage.write("scripts/broken.lua", "return 5"));
        stage.update();
        stage.update();
        CHECK(stage.said() ==
              Lines{"true\ttrue\tLamp\ttrue\tnil\tnil\tnil", "false\tfalse\tLamp\ttrue\tnil\tnil\tnil"});
        // Nothing went wrong but that: the script, and the entity that waits for it.
        const Lines problems = stage.problems();
        CHECK(problems.size() == 2);
        for (const std::string& problem : problems) {
            CAPTURE(problem);
            CHECK(problem.find("scripts/broken.lua") != std::string::npos);
        }
        static_cast<void>(stage.errors());
    }

    TEST_CASE("everything scripts are given is said in words, and what the world, the time and the assets are asked is "
              "counted") {
        const ScriptStage stage;
        Lines names;
        for (const ScriptFunctionInfo& function : stage.scripts->runtime().functions()) {
            // Whatever library it is in, now or later: how it is called, and what it does.
            CAPTURE(function.name);
            CHECK(function.signature.size() > 5);
            CHECK(function.description.size() > 20);
            CHECK(function.description.back() == '.');
            if (function.name.starts_with("cordite.world.") || function.name.starts_with("cordite.time.") ||
                function.name.starts_with("cordite.scene.") || function.name.starts_with("cordite.assets.")) {
                names.push_back(function.name);
                // A function of a library is written as it is called.
                CHECK(function.signature.starts_with(function.name + "("));
            }
        }
        CHECK(names == Lines{"cordite.world.find", "cordite.world.findById", "cordite.world.create",
                             "cordite.world.instantiate", "cordite.world.roots", "cordite.world.withComponent",
                             "cordite.world.componentTypes", "cordite.time.seconds", "cordite.time.delta",
                             "cordite.time.frame", "cordite.time.after", "cordite.time.every", "cordite.time.cancel",
                             "cordite.time.run", "cordite.time.wait", "cordite.scene.load", "cordite.assets.find"});
    }
}
