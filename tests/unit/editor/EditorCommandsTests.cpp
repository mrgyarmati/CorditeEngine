#include "cordite/editor/EditorCommands.h"

#include "cordite/core/FileSystem.h"

#include "unit/editor/EditorTestSupport.h"

#include <set>
#include <string>
#include <vector>

using namespace cordite;
using cordite::test::EditorFixture;
using cordite::test::json;
using cordite::test::unwrap;

namespace {

/// An editor document with every scene-editing command registered on it.
struct CommandFixture : EditorFixture {
    CommandRegistry commands;

    CommandFixture() { REQUIRE(registerEditorCommands(commands, context).has_value()); }

    /// Runs a command that is expected to succeed.
    Json run(std::string_view name, std::string_view parameters = "{}") {
        const Result<Json> result = commands.execute(name, json(parameters));
        REQUIRE_MESSAGE(result.has_value(), result.error().describe());
        return *result;
    }

    /// Runs a command that is expected to fail and returns its error.
    Error fail(std::string_view name, std::string_view parameters = "{}") {
        const Result<Json> result = commands.execute(name, json(parameters));
        REQUIRE_FALSE(result.has_value());
        return result.error();
    }
};

std::vector<std::string> namesOf(const Json& entities) {
    std::vector<std::string> names;
    for (const Json& entity : entities) {
        names.push_back(entity["name"].get<std::string>());
    }
    return names;
}

} // namespace

TEST_SUITE("EditorCommands") {
    TEST_CASE("the catalogue lists every command with complete documentation") {
        CommandFixture fixture;
        const Json catalogue = fixture.run("editor.commands");

        std::set<std::string> names;
        for (const Json& command : catalogue) {
            const std::string name = command["name"].get<std::string>();
            CAPTURE(name);
            names.insert(name);
            // An agent has only these texts to go on.
            CHECK(command["description"].get<std::string>().size() > 20);
            for (const Json& parameter : command["parameters"]) {
                CAPTURE(parameter["name"].get<std::string>());
                CHECK(parameter["description"].get<std::string>().size() > 5);
            }
        }

        CHECK(names == std::set<std::string>{
                           "asset.get",       "asset.instantiate", "asset.list",        "asset.refresh",
                           "component.add",   "component.get",     "component.remove",  "component.set",
                           "component.types", "editor.commands",   "editor.redo",       "editor.state",
                           "editor.undo",     "entity.create",     "entity.delete",     "entity.duplicate",
                           "entity.get",      "entity.rename",     "entity.setEnabled", "entity.setParent",
                           "scene.describe",  "scene.get",         "scene.new",         "scene.open",
                           "scene.save",      "scene.validate",    "selection.get",     "selection.set",
                       });
        CHECK(catalogue.size() == fixture.commands.commands().size());
    }

    TEST_CASE("registering the commands twice fails without replacing anything") {
        CommandFixture fixture;
        const size_t count = fixture.commands.commands().size();
        const Status again = registerEditorCommands(fixture.commands, fixture.context);
        REQUIRE_FALSE(again.has_value());
        CHECK(again.error().code == ErrorCode::AlreadyExists);
        CHECK(fixture.commands.commands().size() == count);
    }

    TEST_CASE("an agent can build a scene from nothing") {
        CommandFixture fixture;
        fixture.run("scene.new");
        CHECK(fixture.world.entityCount() == 0);

        const Json rig = fixture.run("entity.create", R"({"name": "Rig"})");
        CHECK(rig["name"] == "Rig");
        CHECK(rig["path"] == "Rig");
        const std::string rigId = rig["id"].get<std::string>();
        REQUIRE(Uuid::parse(rigId).has_value());

        // The id that came back can be used to refer to the entity.
        const Json eye = fixture.run("entity.create", fmt::format(R"({{
            "name": "Eye",
            "parent": "{}",
            "components": {{"Transform": {{"position": [0, 1.7, 4]}}, "Camera": {{"verticalFovDegrees": 50}}}}
        }})",
                                                                  rigId));
        CHECK(eye["path"] == "Rig/Eye");

        const Json camera = fixture.run("component.get", R"({"entity": "Rig/Eye", "component": "Camera"})");
        CHECK(camera["verticalFovDegrees"] == 50.0);
        CHECK(camera["projection"] == "perspective");

        const Json changed =
            fixture.run("component.set",
                        R"({"entity": "Eye", "component": "Camera", "properties": {"projection": "orthographic"}})");
        CHECK(changed["projection"] == "orthographic");
        CHECK(changed["verticalFovDegrees"] == 50.0);

        fixture.run("component.add", R"({"entity": "Rig", "component": "Label", "properties": {"text": "main rig"}})");
        fixture.run("entity.rename", R"({"entity": "Rig", "name": "Camera rig"})");
        CHECK(fixture.run("entity.setEnabled", R"({"entity": "Camera rig/Eye", "enabled": false})")["path"] ==
              "Camera rig/Eye");

        const Json description = fixture.run("scene.describe");
        CHECK(description["entityCount"] == 2);
        CHECK(description["unsavedChanges"] == true);
        REQUIRE(description["entities"].size() == 1);
        const Json& root = description["entities"][0];
        CHECK(root["name"] == "Camera rig");
        CHECK(root["components"] == json(R"(["Transform", "Label"])"));
        REQUIRE(root["children"].size() == 1);
        CHECK(root["children"][0]["name"] == "Eye");
        CHECK(root["children"][0]["enabled"] == false);
        CHECK(root["children"][0]["components"] == json(R"(["Transform", "Camera"])"));
        CHECK(root["children"][0]["children"] == Json::array());
    }

    TEST_CASE("entity.get returns everything about an entity") {
        CommandFixture fixture;
        const Json lamp = fixture.run("entity.get", R"({"entity": "Lamp"})");

        CHECK(lamp["id"] == fixture.world.id(fixture.lamp).toString());
        CHECK(lamp["name"] == "Lamp");
        CHECK(lamp["path"] == "Room/Table/Lamp");
        CHECK(lamp["enabled"] == true);
        CHECK(lamp["parent"]["name"] == "Table");
        CHECK(lamp["children"] == Json::array());
        CHECK(lamp["components"]["Transform"]["position"] == json("[0.0, 0.75, 0.0]"));

        const Json room = fixture.run("entity.get", R"({"entity": "Room"})");
        CHECK(room["parent"].is_null());
        CHECK(namesOf(room["children"]) == std::vector<std::string>{"Table", "Chair"});

        const Json eye = fixture.run("entity.get", R"({"entity": "/Eye"})");
        CHECK(eye["components"]["Camera"]["verticalFovDegrees"] == 70.0);
    }

    TEST_CASE("component.types describes what can be edited") {
        CommandFixture fixture;
        const Json types = fixture.run("component.types");
        REQUIRE(types.size() == 7);

        CHECK(types[0]["name"] == "Transform");
        CHECK(types[0]["optional"] == false);
        CHECK_FALSE(types[0]["description"].get<std::string>().empty());
        CHECK(types[0]["properties"][0]["name"] == "position");
        CHECK(types[0]["properties"][0]["type"] == "vec3");

        CHECK(types[1]["name"] == "Camera");
        CHECK(types[1]["optional"] == true);
        CHECK(types[1]["properties"][0]["options"] == json(R"(["perspective", "orthographic"])"));

        // References to assets say what kind of asset they take.
        CHECK(types[2]["name"] == "MeshRenderer");
        CHECK(types[2]["properties"][0]["name"] == "mesh");
        CHECK(types[2]["properties"][0]["type"] == "uuid");
        CHECK(types[2]["properties"][0]["semantic"] == "asset");
        CHECK(types[2]["properties"][0]["assetType"] == "Mesh");
        CHECK(types[2]["properties"][1]["name"] == "materials");
        CHECK(types[2]["properties"][1]["type"] == "list");
        CHECK(types[2]["properties"][1]["assetType"] == "Material");
        // A number whose bits are switches says so, and how many of them there are.
        CHECK(types[2]["properties"][2]["name"] == "lightChannels");
        CHECK(types[2]["properties"][2]["type"] == "int");
        CHECK(types[2]["properties"][2]["semantic"] == "bitMask");
        CHECK(types[2]["properties"][2]["maximum"] == 255.0);

        CHECK(types[3]["name"] == "Light");
        CHECK(types[3]["properties"][0]["name"] == "type");
        CHECK(types[3]["properties"][0]["options"] == json(R"(["directional", "point", "spot"])"));
        CHECK(types[3]["properties"][1]["semantic"] == "color");
        CHECK(types[3]["properties"][3]["name"] == "range");
        CHECK(types[3]["properties"][3]["unit"] == "m");
        CHECK(types[4]["name"] == "AmbientLight");

        // The settings a component takes over from a struct it is built on are its own
        // properties like any other.
        CHECK(types[5]["name"] == "PostProcess");
        CHECK(types[5]["properties"][0]["name"] == "global");
        CHECK(types[5]["properties"][4]["name"] == "exposureMode");
        CHECK(types[5]["properties"][4]["options"] == json(R"(["manual", "automatic"])"));
        CHECK(types[5]["properties"][10]["name"] == "toneMapper");
        CHECK(types[5]["properties"][10]["options"] == json(R"(["filmic", "aces", "linear"])"));
        CHECK(types[5]["properties"][21]["name"] == "colorLut");
        CHECK(types[5]["properties"][21]["assetType"] == "Texture");

        CHECK(types[6]["name"] == "Label");
        CHECK(types[6]["properties"][1]["readOnly"] == true);
    }

    TEST_CASE("a scene is lit by commands: lights are made, changed and taken back like everything else") {
        CommandFixture fixture;
        const Json start = fixture.scene();

        // The lamp of the fixture gets the light it is named for.
        fixture.run("component.add", R"({"entity": "Lamp", "component": "Light", "properties": {
            "type": "spot", "color": [1, 0.8, 0.6], "intensity": 4, "range": 6}})");
        const Json light = fixture.run("component.get", R"({"entity": "Lamp", "component": "Light"})");
        CHECK(light["type"] == "spot");
        CHECK(light["intensity"] == 4.0);
        CHECK(light["range"] == 6.0);
        // What was not said has its default.
        CHECK(light["innerConeDegrees"] == 25.0);
        CHECK(light["outerConeDegrees"] == 35.0);
        CHECK(light["channels"] == 1);
        REQUIRE(fixture.world.has<LightComponent>(fixture.lamp));
        CHECK(fixture.world.tryGet<LightComponent>(fixture.lamp)->type == LightType::Spot);

        // Several properties at once.
        const Json changed = fixture.run("component.set", R"({"entity": "Lamp", "component": "Light",
            "properties": {"type": "point", "channels": 5}})");
        CHECK(changed["type"] == "point");
        CHECK(changed["channels"] == 5);
        CHECK(changed["intensity"] == 4.0);

        // A value a light cannot have is refused, and nothing of the request is applied.
        const Error tooManyChannels = fixture.fail("component.set", R"({"entity": "Lamp", "component": "Light",
            "properties": {"intensity": 9, "channels": 256}})");
        CHECK(tooManyChannels.code == ErrorCode::OutOfRange);
        CHECK(tooManyChannels.message.find("channels") != std::string::npos);
        const Error unknownKind = fixture.fail(
            "component.set", R"({"entity": "Lamp", "component": "Light", "properties": {"type": "area"}})");
        CHECK(unknownKind.message.find("expected one of [directional, point, spot]") != std::string::npos);
        CHECK(fixture.world.tryGet<LightComponent>(fixture.lamp)->intensity == 4.0f);
        CHECK(fixture.world.tryGet<LightComponent>(fixture.lamp)->channels == 5);

        // Light from all around, and something for the lamp to shine on that is in
        // lighting channels of its own.
        fixture.run("entity.create",
                    R"({"name": "Sky", "components": {"AmbientLight": {"skyColor": [0.2, 0.3, 0.5]}}})");
        CHECK(fixture.run("component.get", R"({"entity": "Sky", "component": "AmbientLight"})")["intensity"] == 1.0);
        fixture.run("component.add", R"({"entity": "Table", "component": "MeshRenderer",
            "properties": {"lightChannels": 4}})");
        CHECK(fixture.world.tryGet<MeshRendererComponent>(fixture.table)->lightChannels == 4);

        // All of it is in the history, and can be taken back.
        for (int step = 0; step < 4; ++step) {
            fixture.run("editor.undo");
        }
        CHECK(fixture.scene() == start);
        CHECK_FALSE(fixture.world.has<LightComponent>(fixture.lamp));
    }

    TEST_CASE("entities are moved, copied and deleted by reference") {
        CommandFixture fixture;

        fixture.run("entity.setParent", R"({"entity": "Lamp", "parent": "Room", "index": 0})");
        CHECK(fixture.outline() == std::vector<std::string>{"Room", ".Lamp", ".Table", ".Chair", "Eye"});
        fixture.run("entity.setParent", R"({"entity": "Lamp", "parent": null})");
        CHECK(fixture.world.parent(fixture.lamp).isNull());
        fixture.run("entity.setParent", R"({"entity": "Lamp", "parent": "Table", "keepWorldTransform": false})");
        CHECK(fixture.world.parent(fixture.lamp) == fixture.table);

        const Json copies = fixture.run("entity.duplicate", R"({"entities": ["Table"]})");
        REQUIRE(copies.size() == 1);
        CHECK(copies[0]["name"] == "Table");
        CHECK(copies[0]["id"] != fixture.world.id(fixture.table).toString());
        CHECK(fixture.world.entityCount() == 7);
        // The copies are selected.
        CHECK(fixture.run("selection.get") == copies);

        // Two tables now: the name alone no longer says which one.
        const Error ambiguous = fixture.fail("entity.delete", R"({"entities": ["Table"]})");
        CHECK(ambiguous.code == ErrorCode::InvalidArgument);
        CHECK(ambiguous.message.find("2 entities are named 'Table'") != std::string::npos);

        CHECK(fixture
                  .run("entity.delete",
                       fmt::format(R"({{"entities": ["{}", "Eye"]}})", copies[0]["id"].get<std::string>()))
                  .is_null());
        CHECK(fixture.outline() == std::vector<std::string>{"Room", ".Table", "..Lamp", ".Chair"});
    }

    TEST_CASE("every edit made by a command can be undone and redone") {
        CommandFixture fixture;
        const Json start = fixture.scene();

        fixture.run("entity.create", R"({"name": "Crate", "parent": "Room"})");
        fixture.run("component.add", R"({"entity": "Crate", "component": "Camera"})");
        fixture.run("component.set", R"({"entity": "Crate", "component": "Camera", "properties": {"primary": false}})");
        fixture.run("component.remove", R"({"entity": "Eye", "component": "Camera"})");
        fixture.run("entity.delete", R"({"entities": ["Chair"]})");
        const Json end = fixture.scene();

        Json state = fixture.run("editor.state");
        CHECK(state["history"]["canUndo"] == true);
        CHECK(state["history"]["undoLabel"] == "Delete 'Chair'");
        CHECK(state["history"]["canRedo"] == false);
        CHECK(state["scene"]["unsavedChanges"] == true);

        for (int step = 0; step < 5; ++step) {
            fixture.run("editor.undo");
        }
        CHECK(fixture.scene() == start);
        state = fixture.run("editor.state");
        CHECK(state["history"]["canUndo"] == false);
        CHECK(state["history"]["redoLabel"] == "Create 'Crate'");
        CHECK(state["scene"]["unsavedChanges"] == false);

        const Error nothingLeft = fixture.fail("editor.undo");
        CHECK(nothingLeft.code == ErrorCode::InvalidState);

        Json history;
        for (int step = 0; step < 5; ++step) {
            history = fixture.run("editor.redo");
        }
        CHECK(fixture.scene() == end);
        CHECK(history["canRedo"] == false);
        CHECK(fixture.fail("editor.redo").code == ErrorCode::InvalidState);
    }

    TEST_CASE("the selection is set and read by reference") {
        CommandFixture fixture;
        CHECK(fixture.run("selection.get") == Json::array());

        const Json selected = fixture.run("selection.set", R"({"entities": ["Lamp", "Room/Chair"]})");
        CHECK(namesOf(selected) == std::vector<std::string>{"Lamp", "Chair"});
        CHECK(fixture.context.primarySelection() == fixture.chair);
        CHECK(namesOf(fixture.run("editor.state")["selection"]) == std::vector<std::string>{"Lamp", "Chair"});

        // One bad reference leaves the selection as it was.
        fixture.fail("selection.set", R"({"entities": ["Room", "Sofa"]})");
        fixture.fail("selection.set", R"({"entities": ["Room", 5]})");
        CHECK(namesOf(fixture.run("selection.get")) == std::vector<std::string>{"Lamp", "Chair"});

        CHECK(fixture.run("selection.set", R"({"entities": []})") == Json::array());
    }

    TEST_CASE("scenes are saved, replaced and loaded") {
        CommandFixture fixture;
        const test::TempDirectory directory;
        const std::string path = pathToUtf8(directory.path() / "room.scene.json");
        const Json original = fixture.scene();

        // No file yet, so a path is needed.
        CHECK(fixture.fail("scene.save").code == ErrorCode::InvalidState);

        // Json::dump quotes the path and escapes whatever needs escaping in it.
        const Json saved = fixture.run("scene.save", fmt::format(R"({{"path": {}}})", Json(path).dump()));
        CHECK(saved["path"] == path);
        CHECK(saved["unsavedChanges"] == false);
        CHECK(fixture.run("scene.get") == original);

        const Json emptied = fixture.run("scene.new");
        CHECK(emptied["entityCount"] == 0);
        CHECK(emptied["path"].is_null());
        CHECK(emptied["name"] == "Untitled");

        const Json opened = fixture.run("scene.open", fmt::format(R"({{"path": {}}})", Json(path).dump()));
        CHECK(opened["entityCount"] == 5);
        CHECK(fixture.scene() == original);

        // With a file, saving again needs no path.
        fixture.run("entity.rename", R"({"entity": "Chair", "name": "Stool"})");
        CHECK(fixture.run("scene.save")["unsavedChanges"] == false);

        const Error missing = fixture.fail("scene.open", R"({"path": "does/not/exist.scene.json"})");
        CHECK(missing.message.find("scene.open: ") == 0);
        CHECK(fixture.world.entityCount() == 5);
    }

    TEST_CASE("mistakes are answered with errors that say what to fix") {
        CommandFixture fixture;
        const Json before = fixture.scene();

        struct Case {
            const char* command;
            const char* parameters;
            ErrorCode code;
            const char* fragment;
        };

        const std::vector<Case> cases = {
            {"entity.rename", R"({"entity": "Sofa", "name": "Couch"})", ErrorCode::NotFound,
             "no entity is named 'Sofa'"},
            {"entity.rename", R"({"entity": "Lamp"})", ErrorCode::InvalidArgument, "missing parameter 'name'"},
            {"entity.rename", R"({"entity": "Lamp", "name": "a/b"})", ErrorCode::InvalidArgument,
             "must not be empty or contain '/'"},
            {"entity.create", R"({"parent": "Sofa"})", ErrorCode::NotFound, "Sofa"},
            {"entity.create", R"({"components": {"Jetpack": {}}})", ErrorCode::NotFound, "unknown component 'Jetpack'"},
            {"entity.create", R"({"nmae": "Typo"})", ErrorCode::InvalidArgument, "unknown parameter 'nmae'"},
            {"entity.delete", R"({"entities": []})", ErrorCode::InvalidArgument, "no valid entity to delete"},
            {"entity.delete", R"({"entities": "Lamp"})", ErrorCode::InvalidArgument, "must be of type array"},
            {"entity.delete", R"({"entities": [7]})", ErrorCode::InvalidArgument, "array of entity references"},
            {"entity.setParent", R"({"entity": "Room", "parent": "Lamp"})", ErrorCode::InvalidArgument, "descendant"},
            {"entity.setParent", R"({"entity": "Lamp", "parent": 5})", ErrorCode::InvalidArgument,
             "entity reference or null"},
            {"entity.setParent", R"({"entity": "Lamp", "parent": null, "index": -1})", ErrorCode::InvalidArgument,
             "must not be negative"},
            {"entity.setParent", R"({"entity": "Lamp"})", ErrorCode::InvalidArgument, "missing parameter 'parent'"},
            {"entity.get", R"({"entity": ""})", ErrorCode::InvalidArgument, "must not be empty"},
            {"component.add", R"({"entity": "Eye", "component": "Camera"})", ErrorCode::AlreadyExists,
             "already has a Camera"},
            {"component.add", R"({"entity": "Lamp", "component": "Jetpack"})", ErrorCode::NotFound, "Jetpack"},
            {"component.remove", R"({"entity": "Lamp", "component": "Transform"})", ErrorCode::InvalidArgument,
             "cannot be removed"},
            {"component.set", R"({"entity": "Eye", "component": "Camera", "properties": {"nearPlane": -2}})",
             ErrorCode::OutOfRange, "nearPlane"},
            {"component.set", R"({"entity": "Eye", "component": "Camera", "properties": {"projection": "fisheye"}})",
             ErrorCode::InvalidArgument, "expected one of [perspective, orthographic]"},
            {"component.set", R"({"entity": "Eye", "component": "Camera"})", ErrorCode::InvalidArgument,
             "missing parameter 'properties'"},
            {"component.get", R"({"entity": "Lamp", "component": "Camera"})", ErrorCode::NotFound,
             "'Lamp' has no Camera component"},
            {"component.get", R"({"entity": "Lamp", "component": "Jetpack"})", ErrorCode::NotFound, "Jetpack"},
            {"scene.open", R"({})", ErrorCode::InvalidArgument, "missing parameter 'path'"},
        };
        for (const Case& entry : cases) {
            CAPTURE(entry.command);
            CAPTURE(entry.parameters);
            const Error error = fixture.fail(entry.command, entry.parameters);
            CAPTURE(error.message);
            CHECK(error.code == entry.code);
            CHECK(error.message.find(entry.fragment) != std::string::npos);
            // Every message starts with the command it came from.
            CHECK(error.message.find(std::string(entry.command) + ": ") == 0);
        }

        // None of the failed commands changed the scene or left something to undo.
        CHECK(fixture.scene() == before);
        CHECK_FALSE(fixture.context.history().canUndo());
    }
}
