#include "cordite/assets/AssetDatabase.h"
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
        CHECK(stage.line("print(table.concat(cordite.world.componentTypes(), ' '))") == "Transform Camera Script");
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
                function.name.starts_with("cordite.assets.")) {
                names.push_back(function.name);
                // A function of a library is written as it is called.
                CHECK(function.signature.starts_with(function.name + "("));
            }
        }
        CHECK(names == Lines{"cordite.world.find", "cordite.world.findById", "cordite.world.create",
                             "cordite.world.roots", "cordite.world.withComponent", "cordite.world.componentTypes",
                             "cordite.time.seconds", "cordite.time.delta", "cordite.time.frame", "cordite.time.after",
                             "cordite.time.every", "cordite.time.cancel", "cordite.time.run", "cordite.time.wait",
                             "cordite.assets.find"});
    }
}
