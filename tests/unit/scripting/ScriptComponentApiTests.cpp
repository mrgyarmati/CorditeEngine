#include "cordite/core/Json.h"
#include "cordite/core/Reflection.h"
#include "cordite/core/Uuid.h"
#include "cordite/math/Math.h"
#include "cordite/math/MathReflection.h"
#include "cordite/math/Transform.h"
#include "cordite/scripting/ScriptComponents.h"
#include "cordite/scripting/ScriptSystem.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/Entity.h"

#include "support/MathTestSupport.h"
#include "unit/scripting/ScriptTestSupport.h"

#include <cstdint>
#include <string>
#include <vector>

using namespace cordite;
using cordite::test::ScriptStage;
using cordite::test::unwrap;

namespace {

using Lines = std::vector<std::string>;

enum class Mode : uint8_t {
    Off,
    Slow,
    Fast,
};

struct Mount {
    float gain = 1.0f;
    Vec3 offset{0.0f, 0.5f, 0.0f};
    int32_t slot = 2;
};

/// A component with a property of every kind the reflection has.
struct Gadget {
    bool on = false;
    int32_t count = 3;
    float speed = 1.5f;
    std::string label = "gadget";
    Vec2 size{1.0f, 2.0f};
    Vec3 color{1.0f, 0.5f, 0.25f};
    Vec4 tint{0.1f, 0.2f, 0.3f, 1.0f};
    Quat turn = kIdentityQuat;
    Uuid target;
    Uuid mesh;
    Mode mode = Mode::Slow;
    Mount mount;
    std::vector<float> weights = {1.0f, 2.0f};
    std::vector<Uuid> friends;
    std::vector<Vec3> path = {Vec3(1.0f, 0.0f, 0.0f)};
    std::vector<std::vector<float>> grid = {{1.0f, 2.0f}, {3.0f}};
    Json extra = Json::object();
    int32_t serial = 7;
    float heat = 0.0f;
};

const TypeInfo& mountType() {
    static const TypeInfo type = TypeBuilder<Mount>("Mount")
                                     .property("gain", &Mount::gain, {.minimum = 0.0, .maximum = 4.0})
                                     .property("offset", &Mount::offset)
                                     .property("slot", &Mount::slot, {.readOnly = true})
                                     .build();
    return type;
}

TypeInfo gadgetType() {
    return TypeBuilder<Gadget>("Gadget")
        .property("on", &Gadget::on)
        .property("count", &Gadget::count, {.minimum = 0.0, .maximum = 100.0})
        .property("speed", &Gadget::speed, {.minimum = 0.0, .maximum = 10.0})
        .property("label", &Gadget::label)
        .property("size", &Gadget::size)
        .property("color", &Gadget::color, {.semantic = PropertySemantic::Color})
        .property("tint", &Gadget::tint)
        .property("turn", &Gadget::turn)
        .property("target", &Gadget::target, {.semantic = PropertySemantic::EntityReference})
        .property("mesh", &Gadget::mesh, {.semantic = PropertySemantic::AssetReference, .referenceType = "Mesh"})
        .enumProperty("mode", &Gadget::mode, {{Mode::Off, "off"}, {Mode::Slow, "slow"}, {Mode::Fast, "fast"}})
        .objectProperty("mount", &Gadget::mount, mountType())
        .property("weights", &Gadget::weights, {.minimum = 0.0})
        .property("friends", &Gadget::friends, {.semantic = PropertySemantic::EntityReference})
        .property("path", &Gadget::path)
        .property("grid", &Gadget::grid)
        .property("extra", &Gadget::extra)
        .property("serial", &Gadget::serial, {.readOnly = true})
        .property("heat", &Gadget::heat, {.transient = true})
        .build();
}

/// A stage that knows the Gadget, with an entity "Box" that has one and two entities for
/// it to refer to.
struct Workshop {
    ScriptStage stage;
    Entity box;
    Entity anvil;
    Entity vise;

    Workshop() {
        REQUIRE(stage.components.add<Gadget>(gadgetType(), {.description = "Everything a component can hold."})
                    .has_value());
        box = stage.world.createEntity("Box");
        anvil = stage.world.createEntity("Anvil");
        vise = stage.world.createEntity("Vise");
        stage.world.add<Gadget>(box);
    }

    [[nodiscard]] Gadget& gadget() { return *stage.world.tryGet<Gadget>(box); }

    /// Lua that has the box as `box` before it does `body`.
    [[nodiscard]] static std::string onBox(const std::string& body) {
        return "local box = cordite.world.find('Box')\n" + body;
    }
};

} // namespace

TEST_SUITE("ScriptComponentApi") {
    TEST_CASE("a script reads a component as a table of what it holds") {
        Workshop shop;
        ScriptStage& stage = shop.stage;
        shop.gadget().target = stage.world.id(shop.anvil);
        shop.gadget().friends = {stage.world.id(shop.vise), Uuid(), stage.world.id(shop.anvil)};
        shop.gadget().mesh = Uuid::fromName(Uuid(), "a mesh");
        shop.gadget().extra = unwrap(parseJson(R"({"notes": ["oil", "paint"], "weight": 2.5})"));
        shop.gadget().turn = quatFromEulerDegrees(Vec3(0.0f, 90.0f, 0.0f));
        shop.gadget().heat = 0.75f;

        CHECK(stage.lines(Workshop::onBox(R"(
            local gadget = box:get("Gadget")
            print(gadget.on, gadget.count, math.type(gadget.count), gadget.speed, gadget.label, gadget.mode)
            print(gadget.size.x, gadget.size.y, getmetatable(gadget.size), gadget.tint.x, gadget.tint.w)
            print(gadget.color, getmetatable(gadget.color) == cordite.Vec3, getmetatable(gadget.turn) == cordite.Quat,
                  (gadget.turn * cordite.vec3(0, 0, -1) - cordite.vec3(-1, 0, 0)):length() < 1e-6)
            print(gadget.target, gadget.mesh, gadget.mount.gain, gadget.mount.offset, #gadget.weights, gadget.weights[2])
            print(#gadget.friends, gadget.friends[1], gadget.friends[2] == cordite.null, gadget.friends[3])
            print(#gadget.path, gadget.path[1], getmetatable(gadget.path[1]) == cordite.Vec3,
                  #gadget.grid, #gadget.grid[1], gadget.grid[1][2], gadget.grid[2][1])
            print(gadget.extra.weight, gadget.extra.notes[2], gadget.serial, gadget.heat)
        )")) == Lines{"false\t3\tinteger\t1.5\tgadget\tslow", "1.0\t2.0\tnil\t0.1\t1.0",
                      "(1.0, 0.5, 0.25)\ttrue\ttrue\ttrue",
                      "Entity(Anvil)\t" + shop.gadget().mesh.toString() + "\t1.0\t(0.0, 0.5, 0.0)\t2\t2.0",
                      "3\tEntity(Vise)\ttrue\tEntity(Anvil)", "1\t(1.0, 0.0, 0.0)\ttrue\t2\t2\t2.0\t3.0",
                      "2.5\tpaint\t7\t0.75"});

        // One property by its name; what refers to nothing is nil.
        shop.gadget().target = Uuid();
        CHECK(stage.line(Workshop::onBox(
                  "print(box:get('Gadget', 'speed'), box:get('Gadget', 'color'), box:get('Gadget', 'target'), "
                  "box:get('Gadget', 'mode'), box:get('Gadget', 'mount').offset.y)")) ==
              "1.5\t(1.0, 0.5, 0.25)\tnil\tslow\t0.5");
        // A reference to an entity that is no longer there is one to nothing.
        shop.gadget().target = Uuid::fromName(Uuid(), "long gone");
        CHECK(stage.line(Workshop::onBox("print(box:get('Gadget', 'target'), box:get('Gadget').target)")) ==
              "nil\tnil");

        // What is read is a copy: changing it changes nothing.
        CHECK(stage.line(Workshop::onBox("local gadget = box:get('Gadget')\n"
                                         "gadget.speed = 9\n"
                                         "gadget.color.x = 0\n"
                                         "print(box:get('Gadget', 'speed'), box:get('Gadget', 'color'))")) ==
              "1.5\t(1.0, 0.5, 0.25)");
        // A component that the entity does not have is nil, in whole and in part.
        CHECK(stage.line("print(entity:get('Gadget'), entity:get('Gadget', 'speed'), entity:has('Gadget'), "
                         "cordite.world.find('Box'):has('Gadget'), entity:has('Transform'))") ==
              "nil\tnil\tfalse\ttrue\ttrue");
        CHECK(stage.refusal("entity:get('Gizmo')") ==
              "bad argument #1 to 'get' (there is no component called 'Gizmo')");
        CHECK(stage.refusal("entity:get('Gadget', 'sped')") ==
              "bad argument #2 to 'get' (Gadget has no property 'sped')");
        CHECK(stage.refusal("entity:has()") == "bad argument #1 to 'has' (string expected, got no value)");
    }

    TEST_CASE("a script changes the properties of a component, one or several") {
        Workshop shop;
        ScriptStage& stage = shop.stage;

        CHECK(stage.lines(Workshop::onBox(R"(
            box:set("Gadget", "speed", 4)
            box:set("Gadget", "label", "anvil")
            box:set("Gadget", {
                on = true,
                count = 12,
                size = {3, 4},
                color = cordite.vec3(0.25, 0.5, 0.75),
                tint = {x = 1, y = 0, z = 0, w = 0.5},
                turn = cordite.euler(0, 90, 0),
                target = cordite.world.find("Anvil"),
                mode = "fast",
                mount = { gain = 2 },
                weights = {0.5, 0.25, 4},
                friends = {cordite.world.find("Vise"), cordite.null, cordite.world.find("Anvil")},
                path = {cordite.vec3(1, 2, 3), {4, 5, 6}, {x = 7, y = 8, z = 9}},
                grid = {{}, {0.5}, {1, 2, 3}},
                extra = { notes = {"oil"}, depth = { more = true } },
                heat = 0.5,
            })
            print("done")
        )")) == Lines{"done"});

        const Gadget& gadget = shop.gadget();
        CHECK(gadget.speed == 4.0f);
        CHECK(gadget.label == "anvil");
        CHECK(gadget.on);
        CHECK(gadget.count == 12);
        CHECK(gadget.size == Vec2(3.0f, 4.0f));
        CHECK(gadget.color == Vec3(0.25f, 0.5f, 0.75f));
        CHECK(gadget.tint == Vec4(1.0f, 0.0f, 0.0f, 0.5f));
        CHECK_NEAR(gadget.turn * kWorldForward, Vec3(-1.0f, 0.0f, 0.0f), 1.0e-5f);
        CHECK(gadget.target == stage.world.id(shop.anvil));
        CHECK(gadget.mode == Mode::Fast);
        // Of what holds several things, only what is named is changed.
        CHECK(gadget.mount.gain == 2.0f);
        CHECK(gadget.mount.offset == Vec3(0.0f, 0.5f, 0.0f));
        CHECK(gadget.weights == std::vector<float>{0.5f, 0.25f, 4.0f});
        CHECK(gadget.friends == std::vector<Uuid>{stage.world.id(shop.vise), Uuid(), stage.world.id(shop.anvil)});
        CHECK(gadget.path == std::vector<Vec3>{Vec3(1.0f, 2.0f, 3.0f), Vec3(4.0f, 5.0f, 6.0f), Vec3(7.0f, 8.0f, 9.0f)});
        CHECK(gadget.grid == std::vector<std::vector<float>>{{}, {0.5f}, {1.0f, 2.0f, 3.0f}});
        CHECK(dumpJson(gadget.extra, -1) == R"({"depth":{"more":true},"notes":["oil"]})");
        CHECK(gadget.heat == 0.5f);
        CHECK(gadget.serial == 7);

        // A reference is cleared with nil, or - in a table, where nil is nothing at all -
        // with cordite.null; an id as text is as good as the entity.
        CHECK(stage.line(Workshop::onBox("box:set('Gadget', 'target', nil)\n"
                                         "print(box:get('Gadget', 'target'))\n")) == "nil");
        CHECK(shop.gadget().target.isNil());
        const std::string viseId = stage.world.id(shop.vise).toString();
        CHECK(stage.line(Workshop::onBox("box:set('Gadget', 'target', '" + viseId +
                                         "')\n"
                                         "print(box:get('Gadget', 'target'))\n"
                                         "box:set('Gadget', {target = cordite.null, friends = {}, weights = {}})\n")) ==
              "Entity(Vise)");
        CHECK(shop.gadget().target.isNil());
        CHECK(shop.gadget().friends.empty());
        CHECK(shop.gadget().weights.empty());

        // Every entity has a Transform, and it is a component like the others.
        CHECK(stage.line(Workshop::onBox("box:set('Transform', 'position', cordite.vec3(1, 2, 3))\n"
                                         "box:set('Transform', {scale = {2, 2, 2}})\n"
                                         "print(box:position(), box:scale(), box:get('Transform', 'position') == "
                                         "box:position())")) == "(1.0, 2.0, 3.0)\t(2.0, 2.0, 2.0)\ttrue");
    }

    TEST_CASE("what a script writes is checked as the editor checks it, and a change that is refused changes nothing") {
        Workshop shop;
        ScriptStage& stage = shop.stage;
        const auto refusal = [&](const std::string& body) { return stage.refusal(Workshop::onBox(body)); };

        CHECK(refusal("box:set('Gadget', 'speed', 11)") ==
              "Gadget: property 'speed': 11 is outside the allowed range [0, 10]");
        CHECK(refusal("box:set('Gadget', 'speed', 'fast')") ==
              "Gadget: property 'speed': expected a number, found string");
        CHECK(refusal("box:set('Gadget', 'speed', 0 / 0)") ==
              "Gadget: 'speed': a number that is not finite cannot be stored");
        CHECK(refusal("box:set('Gadget', 'count', 2.5)") ==
              "Gadget: property 'count': expected a whole number, found 2.5");
        CHECK(refusal("box:set('Gadget', 'on', 1)") == "Gadget: property 'on': expected true or false, found number");
        CHECK(refusal("box:set('Gadget', 'mode', 'warp')") ==
              "Gadget: property 'mode': expected one of [off, slow, fast], found 'warp'");
        CHECK(refusal("box:set('Gadget', 'serial', 8)") == "Gadget: property 'serial' is read-only");
        CHECK(refusal("box:set('Gadget', 'sped', 1)") == "bad argument #2 to 'set' (Gadget has no property 'sped')");
        CHECK(refusal("box:set('Gadget', {sped = 1})") == "Gadget has no property 'sped'");
        CHECK(refusal("box:set('Gadget', 'color', {1, 2})").starts_with("Gadget: property 'color': "));
        CHECK(
            refusal("box:set('Gadget', 'color', cordite.vec3(1, 0 / 0, 0))").starts_with("Gadget: property 'color': "));
        CHECK(refusal("box:set('Gadget', 'turn', cordite.vec3(1, 2, 3))").starts_with("Gadget: property 'turn': "));
        CHECK(refusal("box:set('Gadget', 'mount', {gain = 9})") ==
              "Gadget: property 'mount': Mount: property 'gain': 9 is outside the allowed range [0, 4]");
        CHECK(refusal("box:set('Gadget', 'weights', {1, -1})").starts_with("Gadget: property 'weights': "));
        CHECK(refusal("box:set('Gadget', 'friends', {print})") ==
              "Gadget: 'friends': element 1: a function cannot be stored");
        // A list is a table with something at 1 to n and nothing else. One with names in
        // it is not the list of what stands at its positions - which, for a table of
        // names only, would be the list of nothing.
        CHECK(refusal("box:set('Gadget', 'weights', {first = 1})") ==
              "Gadget: property 'weights': expected an array, found object");
        CHECK(refusal("box:set('Gadget', 'weights', {1, 2, more = 3})") ==
              "Gadget: 'weights': a table can be stored when its keys are 1 to n, or names; this one has both");
        CHECK(refusal("box:set('Gadget', 'weights', {[1] = 1, [3] = 3})") ==
              "Gadget: 'weights': a table can be stored when its keys are 1 to n, or names; this one has a key 3");
        CHECK(refusal("box:set('Gadget', 'path', {cordite.vec3(1, 2, 3), {4, 5}})")
                  .starts_with("Gadget: property 'path': "));
        // A table that holds itself has no end, and is not followed to one.
        {
            const std::string endless = refusal("local row = {}\nrow[1] = row\nbox:set('Gadget', 'grid', row)");
            CHECK(endless.starts_with("Gadget: 'grid': element 1: element 1: "));
            CHECK(endless.ends_with("tables are nested more than 32 deep, or one of them holds itself"));
        }
        // An entity is what refers to an entity, and nothing else: not the id of an asset.
        CHECK(refusal("box:set('Gadget', 'mesh', cordite.world.find('Anvil'))") ==
              "Gadget: 'mesh': an entity was given to what does not refer to an entity");
        CHECK(refusal("box:set('Gadget', 'speed', cordite.world.find('Anvil'))") ==
              "Gadget: 'speed': a userdata cannot be stored");
        CHECK(refusal("box:set('Gadget', 'extra', {print})") ==
              "Gadget: 'extra': element 1: a function cannot be stored");
        CHECK(refusal("box:set('Gadget', 'extra', {1, 2})") ==
              "Gadget: property 'extra': expected an object, found array");
        CHECK(refusal("box:set('Gadget', 'target', 'the anvil')") ==
              "Gadget: property 'target': 'the anvil' is not a valid identifier");
        CHECK(refusal("box:set('Gadget', {[1] = true})") ==
              "Gadget: the properties of Gadget have names; a table of them has no other keys");
        CHECK(refusal("box:set('Gadget', 5)") == "Gadget: a table of the properties of Gadget expected, not a number");
        CHECK(refusal("box:set('Gadget', 'speed')") == "bad argument #3 to 'set' (value expected)");
        CHECK(refusal("box:set('Gizmo', {})") == "bad argument #1 to 'set' (there is no component called 'Gizmo')");
        CHECK(refusal("cordite.world.find('Anvil'):set('Gadget', {speed = 1})") ==
              "the entity has no Gadget; add() gives it one");

        // An entity that is gone cannot be referred to.
        const Uuid gone = stage.write("scripts/gone.lua", R"(
            local Gone = {}
            local anvil
            function Gone:start() anvil = cordite.world.find("Anvil") end
            function Gone:update()
                if not anvil:isValid() then cordite.world.find("Box"):set("Gadget", "target", anvil) end
            end
            return Gone
        )");
        const Entity keeper = stage.add("Keeper", gone);
        stage.update();
        stage.world.destroyEntity(shop.anvil);
        stage.update();
        REQUIRE(stage.problems().size() == 1);
        CHECK(stage.problems()[0].find("Gadget: 'target': the entity is gone: it was destroyed") != std::string::npos);
        stage.world.destroyEntity(keeper);
        stage.update();
        static_cast<void>(stage.errors());

        // Of a table of changes in which one is wrong, none is made.
        const Gadget before = shop.gadget();
        CHECK(refusal("box:set('Gadget', {label = 'changed', count = 50, speed = -1, on = true})") ==
              "Gadget: property 'speed': -1 is outside the allowed range [0, 10]");
        const Gadget& after = shop.gadget();
        CHECK(after.label == before.label);
        CHECK(after.count == before.count);
        CHECK(after.on == before.on);
        CHECK(after.speed == before.speed);
    }

    TEST_CASE("a table that was read is written back with what was changed in it, whatever else it holds") {
        Workshop shop;
        ScriptStage& stage = shop.stage;
        // Everything in it has a value, so that everything is read and written.
        shop.gadget().target = stage.world.id(shop.anvil);
        shop.gadget().friends = {stage.world.id(shop.vise), Uuid(), stage.world.id(shop.anvil)};
        shop.gadget().mesh = Uuid::fromName(Uuid(), "a mesh");
        shop.gadget().extra = unwrap(parseJson(R"({"notes": ["oil", null], "weight": 2.5, "empty": null})"));
        shop.gadget().turn = quatFromEulerDegrees(Vec3(10.0f, 20.0f, 30.0f));
        shop.gadget().color = Vec3(0.1f, 0.2f, 0.3f);
        shop.gadget().heat = 0.75f;
        const TypeInfo type = gadgetType();
        const std::string before = dumpJson(type.toJson(&shop.gadget()), -1);

        // As it is, nothing has changed: not a digit of a number, and not what may not
        // be changed at all - the serial number, the slot of the mount - which a script
        // that only hands back what it was handed does not attempt.
        CHECK(stage.line(Workshop::onBox("box:set('Gadget', box:get('Gadget'))\n"
                                         "box:set('Gadget', 'serial', 7)\n"
                                         "box:set('Gadget', 'mount', {slot = 2})\n"
                                         "print('written')")) == "written");
        CHECK(dumpJson(type.toJson(&shop.gadget()), -1) == before);
        CHECK(shop.gadget().heat == 0.75f);

        CHECK(stage.line(Workshop::onBox("local gadget = box:get('Gadget')\n"
                                         "gadget.speed = 2\n"
                                         "gadget.mount.gain = 3\n"
                                         "gadget.color.x = 0.5\n"
                                         "box:set('Gadget', gadget)\n"
                                         "print(box:get('Gadget', 'speed'), box:get('Gadget', 'serial'), "
                                         "box:get('Gadget', 'mount').gain, box:get('Gadget', 'mount').slot, "
                                         "box:get('Gadget', 'color'))")) == "2.0\t7\t3.0\t2\t(0.5, 0.2, 0.3)");

        // With another value in it, it is the attempt it looks like, and nothing of the
        // table is written.
        const auto refusal = [&](const std::string& body) { return stage.refusal(Workshop::onBox(body)); };
        CHECK(refusal(
                  "local gadget = box:get('Gadget')\ngadget.serial = 8\ngadget.speed = 5\nbox:set('Gadget', gadget)") ==
              "Gadget: property 'serial' is read-only");
        CHECK(refusal("local gadget = box:get('Gadget')\ngadget.mount.slot = 3\ngadget.speed = 5\n"
                      "box:set('Gadget', gadget)") == "Gadget: property 'mount': Mount: property 'slot' is read-only");
        CHECK(refusal("box:set('Gadget', 'serial', 8)") == "Gadget: property 'serial' is read-only");
        CHECK(shop.gadget().serial == 7);
        CHECK(shop.gadget().mount.slot == 2);
        CHECK(shop.gadget().speed == 2.0f);
    }

    TEST_CASE("a script gives an entity a component and takes one off") {
        Workshop shop;
        ScriptStage& stage = shop.stage;

        CHECK(stage.line("local anvil = cordite.world.find('Anvil')\n"
                         "anvil:add('Gadget')\n"
                         "print(anvil:has('Gadget'), anvil:get('Gadget', 'speed'), anvil:get('Gadget', 'label'))") ==
              "true\t1.5\tgadget");
        CHECK(stage.line(
                  "local vise = cordite.world.find('Vise')\n"
                  "vise:add('Gadget', {speed = 2, mode = 'off', target = cordite.world.find('Box')})\n"
                  "print(vise:get('Gadget', 'speed'), vise:get('Gadget', 'mode'), vise:get('Gadget', 'target'))") ==
              "2.0\toff\tEntity(Box)");
        CHECK(stage.world.tryGet<Gadget>(shop.vise)->count == 3);

        // One that is there is changed, not added; one that is wrong is not added at all.
        CHECK(stage.refusal("cordite.world.find('Vise'):add('Gadget')") ==
              "the entity has a Gadget already; set() changes it");
        CHECK(stage.refusal("entity:add('Gadget', {speed = 99})") ==
              "Gadget: property 'speed': 99 is outside the allowed range [0, 10]");
        CHECK(stage.refusal("entity:add('Gadget', {serial = 1})") == "Gadget: property 'serial' is read-only");
        CHECK(stage.refusal("entity:add('Gizmo')") ==
              "bad argument #1 to 'add' (there is no component called 'Gizmo')");
        CHECK(stage.refusal("entity:add('Transform')") == "every entity has a Transform; none is added");

        CHECK(stage.line("local vise = cordite.world.find('Vise')\n"
                         "vise:remove('Gadget')\n"
                         "vise:remove('Gadget')            -- one that is not there is as good as removed\n"
                         "print(vise:has('Gadget'), vise:get('Gadget'))") == "false\tnil");
        CHECK_FALSE(stage.world.has<Gadget>(shop.vise));
        CHECK(stage.refusal("entity:remove('Transform')") == "every entity has a Transform; it is not removed");
        CHECK(stage.refusal("entity:remove('Gizmo')") ==
              "bad argument #1 to 'remove' (there is no component called 'Gizmo')");
    }

    TEST_CASE("a Script is a component too: a script gives an entity a script, and it runs from the next update") {
        Workshop shop;
        ScriptStage& stage = shop.stage;
        const Uuid hello = stage.write("scripts/hello.lua", R"(
            local Hello = { properties = { greeting = "hello" } }
            function Hello:start() print(self.properties.greeting .. " from " .. self.entity:name()) end
            return Hello
        )");
        static_cast<void>(hello);

        CHECK(stage
                  .lines("local anvil = cordite.world.find('Anvil')\n"
                         "anvil:add('Script', {script = cordite.assets.find('scripts/hello.lua'), "
                         "properties = {greeting = 'good day'}})\n"
                         "print(anvil:script())")
                  .front() == "nil");
        // The probe is gone again, and what it started has started: lines() ran an update more.
        const ScriptComponent* script = stage.world.tryGet<ScriptComponent>(shop.anvil);
        REQUIRE(script != nullptr);
        CHECK(script->script == hello);
        CHECK(dumpJson(script->properties, -1) == R"({"greeting":"good day"})");
        stage.update();
        CHECK(stage.problems().empty());
        CHECK(stage.line("print(cordite.world.find('Anvil'):script().properties.greeting)") == "good day");
    }
}
