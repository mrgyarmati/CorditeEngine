#include "cordite/editor/EditorContext.h"

#include "cordite/core/FileSystem.h"
#include "cordite/editor/EditorActions.h"

#include "unit/editor/EditorTestSupport.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace cordite;
using cordite::test::EditorFixture;
using cordite::test::unwrap;

TEST_SUITE("EditorContext") {
    TEST_CASE("a new document has no file, no selection and no unsaved changes") {
        World world;
        ComponentRegistry components;
        const std::unique_ptr<AssetManager> assets = unwrap(AssetManager::create());
        const EditorContext context(world, components, *assets);

        CHECK(context.scenePath().empty());
        CHECK(context.selection().empty());
        CHECK(context.primarySelection().isNull());
        CHECK_FALSE(context.hasUnsavedChanges());
        CHECK_FALSE(context.history().canUndo());
    }

    TEST_CASE("the selection keeps its order and its last entity is the primary one") {
        EditorFixture fixture;
        EditorContext& context = fixture.context;

        const Entity both[] = {fixture.lamp, fixture.chair};
        context.setSelection(both);
        CHECK(context.selection() == std::vector<Entity>{fixture.lamp, fixture.chair});
        CHECK(context.primarySelection() == fixture.chair);
        CHECK(context.isSelected(fixture.lamp));
        CHECK_FALSE(context.isSelected(fixture.room));
        CHECK_FALSE(context.isSelected(Entity{}));

        // Repetitions and invalid handles are dropped.
        const Entity messy[] = {fixture.room, Entity{}, fixture.room, fixture.eye};
        context.setSelection(messy);
        CHECK(context.selection() == std::vector<Entity>{fixture.room, fixture.eye});

        context.clearSelection();
        CHECK(context.selection().empty());
        CHECK(context.primarySelection().isNull());
    }

    TEST_CASE("toggling adds an entity to the selection or takes it out") {
        EditorFixture fixture;
        EditorContext& context = fixture.context;

        context.toggleSelection(fixture.lamp);
        context.toggleSelection(fixture.eye);
        CHECK(context.selection() == std::vector<Entity>{fixture.lamp, fixture.eye});

        context.toggleSelection(fixture.lamp);
        CHECK(context.selection() == std::vector<Entity>{fixture.eye});
        CHECK_NOTHROW(context.toggleSelection(Entity{}));
        CHECK(context.selection().size() == 1);
    }

    TEST_CASE("destroyed entities leave the selection and return to it when restored") {
        EditorFixture fixture;
        EditorContext& context = fixture.context;
        const Entity selected[] = {fixture.table, fixture.eye};
        context.setSelection(selected);

        const Entity doomed[] = {fixture.table};
        REQUIRE(EditorActions(context).deleteEntities(doomed).has_value());
        CHECK(context.selection() == std::vector<Entity>{fixture.eye});
        CHECK(context.primarySelection() == fixture.eye);

        // Undo brings the entity back under the same id, so it is selected again.
        REQUIRE(context.history().undo().has_value());
        REQUIRE(context.selection().size() == 2);
        CHECK(fixture.world.name(context.selection()[0]) == "Table");
    }

    TEST_CASE("an entity is found by id, by unique name and by path") {
        EditorFixture fixture;
        const EditorContext& context = fixture.context;
        const std::string lampId = fixture.world.id(fixture.lamp).toString();

        CHECK(unwrap(context.resolveEntity(lampId)) == fixture.lamp);
        CHECK(unwrap(context.resolveEntity("  " + lampId + " ")) == fixture.lamp);
        CHECK(unwrap(context.resolveEntity("Lamp")) == fixture.lamp);
        CHECK(unwrap(context.resolveEntity("Room/Table/Lamp")) == fixture.lamp);
        CHECK(unwrap(context.resolveEntity("/Room/Table")) == fixture.table);
        CHECK(unwrap(context.resolveEntity("/Eye")) == fixture.eye);
        CHECK(unwrap(context.resolveEntity("Room/Chair/")) == fixture.chair);
    }

    TEST_CASE("references that match nothing say what is missing") {
        EditorFixture fixture;
        const EditorContext& context = fixture.context;

        struct Case {
            const char* reference;
            ErrorCode code;
            const char* fragment;
        };

        for (const Case& entry : {
                 Case{"", ErrorCode::InvalidArgument, "must not be empty"},
                 Case{"   ", ErrorCode::InvalidArgument, "must not be empty"},
                 Case{"/", ErrorCode::InvalidArgument, "does not name an entity"},
                 Case{"Sofa", ErrorCode::NotFound, "no entity is named 'Sofa'"},
                 Case{"lamp", ErrorCode::NotFound, "no entity is named 'lamp'"},
                 Case{"Room/Sofa", ErrorCode::NotFound,
                      "'Sofa' in the path 'Room/Sofa' does not name a child of 'Room'"},
                 Case{"Kitchen/Table", ErrorCode::NotFound, "does not name a root entity"},
                 Case{"/Table", ErrorCode::NotFound, "does not name a root entity"},
                 Case{"01234567-89ab-4def-8edc-ba9876543210", ErrorCode::NotFound, "no entity has the id"},
             }) {
            CAPTURE(entry.reference);
            const Result<Entity> result = context.resolveEntity(entry.reference);
            REQUIRE_FALSE(result.has_value());
            CHECK(result.error().code == entry.code);
            CHECK(result.error().message.find(entry.fragment) != std::string::npos);
        }
    }

    TEST_CASE("a name or path that matches several entities is refused") {
        EditorFixture fixture;
        const Entity secondLamp = fixture.world.createEntity("Lamp");
        const Entity thirdLamp = fixture.world.createEntity("Lamp");
        REQUIRE(fixture.world.setParent(thirdLamp, fixture.table).has_value());
        const EditorContext& context = fixture.context;

        const Result<Entity> byName = context.resolveEntity("Lamp");
        REQUIRE_FALSE(byName.has_value());
        CHECK(byName.error().code == ErrorCode::InvalidArgument);
        // The message offers the unambiguous ways to refer to each of them.
        CHECK(byName.error().message.find("3 entities are named 'Lamp'") != std::string::npos);
        CHECK(byName.error().message.find("Room/Table/Lamp") != std::string::npos);
        CHECK(byName.error().message.find(fixture.world.id(secondLamp).toString()) != std::string::npos);

        // A path narrows it down, but two siblings can still share a name.
        CHECK(unwrap(context.resolveEntity("/Lamp")) == secondLamp);
        const Result<Entity> byPath = context.resolveEntity("Room/Table/Lamp");
        REQUIRE_FALSE(byPath.has_value());
        CHECK(byPath.error().code == ErrorCode::InvalidArgument);
        CHECK(byPath.error().message.find("matches 2 entities") != std::string::npos);

        // An id always works.
        CHECK(unwrap(context.resolveEntity(fixture.world.id(thirdLamp).toString())) == thirdLamp);
    }

    TEST_CASE("long lists of matches are shortened") {
        World world;
        ComponentRegistry components;
        const std::unique_ptr<AssetManager> assets = unwrap(AssetManager::create());
        const EditorContext context(world, components, *assets);
        for (int index = 0; index < 10; ++index) {
            world.createEntity("Crate");
        }

        const Result<Entity> result = context.resolveEntity("Crate");
        REQUIRE_FALSE(result.has_value());
        CHECK(result.error().message.find("10 entities are named 'Crate'") != std::string::npos);
        CHECK(result.error().message.find("and 6 more") != std::string::npos);
    }

    TEST_CASE("the path of an entity leads from its root to it") {
        EditorFixture fixture;
        CHECK(fixture.context.entityPath(fixture.lamp) == "Room/Table/Lamp");
        CHECK(fixture.context.entityPath(fixture.eye) == "Eye");
        // A path can be handed back to resolveEntity.
        CHECK(unwrap(fixture.context.resolveEntity(fixture.context.entityPath(fixture.chair))) == fixture.chair);
    }

    TEST_CASE("a new scene empties the document") {
        EditorFixture fixture;
        EditorContext& context = fixture.context;
        const Entity selected[] = {fixture.lamp};
        context.setSelection(selected);
        REQUIRE(EditorActions(context).renameEntity(fixture.lamp, "Lantern").has_value());
        REQUIRE(context.hasUnsavedChanges());

        REQUIRE(context.newScene().has_value());
        CHECK(fixture.world.entityCount() == 0);
        CHECK(fixture.world.sceneName() == "Untitled");
        CHECK(context.selection().empty());
        CHECK_FALSE(context.history().canUndo());
        CHECK_FALSE(context.hasUnsavedChanges());
        CHECK(context.scenePath().empty());
    }

    TEST_CASE("a scene that is played is put back as it was, with its history and what was selected") {
        EditorFixture fixture;
        EditorContext& context = fixture.context;
        World& world = fixture.world;
        EditorActions actions(context);
        // An edit that is not saved yet, and something selected.
        REQUIRE(actions.renameEntity(fixture.lamp, "Lantern").has_value());
        const Uuid lamp = world.id(fixture.lamp);
        const Uuid chair = world.id(fixture.chair);
        const Entity selected[] = {fixture.chair, fixture.lamp};
        context.setSelection(selected);
        const std::string before = dumpJson(fixture.scene(), 2);
        CHECK_FALSE(context.isPlaying());

        REQUIRE(context.beginPlay().has_value());
        CHECK(context.isPlaying());
        // The history of the scene is set aside: what is edited now has one of its own.
        CHECK_FALSE(context.history().canUndo());
        // What counted as unsaved still does, and nothing that happens now adds to it.
        CHECK(context.hasUnsavedChanges());
        CHECK(context.selection() == std::vector<Entity>{fixture.chair, fixture.lamp});

        // The game runs: things move, come and go. And somebody edits while it does.
        world.transform(fixture.table).position = Vec3(40.0f, 2.0f, 9.0f);
        world.destroyEntity(fixture.chair);
        world.createEntity("Spawned");
        world.setSceneName("Somewhere else");
        REQUIRE(actions.renameEntity(fixture.lamp, "Torch").has_value());
        REQUIRE(actions.createEntity({.name = "Marker"}).has_value());
        CHECK(context.history().undoCount() == 2);
        REQUIRE(context.history().undo().has_value());
        CHECK(world.findByName("Marker").isNull());
        // What is gone is not selected.
        CHECK(context.selection() == std::vector<Entity>{fixture.lamp});

        // A scene that is played is not replaced, and not saved: a game half-way through
        // is not what the file is to hold.
        const test::TempDirectory directory;
        for (const Status& refused :
             {context.beginPlay(), context.newScene(), context.openScene(directory.path() / "any.scene.json"),
              context.saveScene(directory.path() / "played.scene.json")}) {
            REQUIRE_FALSE(refused.has_value());
            CHECK(refused.error().code == ErrorCode::InvalidState);
            CHECK(refused.error().message.starts_with("the scene is being played"));
        }
        CHECK_FALSE(std::filesystem::exists(directory.path() / "played.scene.json"));
        CHECK(world.findByName("Spawned").isNull() == false);

        REQUIRE(context.endPlay().has_value());
        CHECK_FALSE(context.isPlaying());
        // The scene is what it was, to the letter.
        CHECK(dumpJson(fixture.scene(), 2) == before);
        // Its entities are others than before - the world was put together anew - and
        // are found by the ids they always had. Both are selected again, in their order.
        const Entity lampNow = world.findById(lamp);
        const Entity chairNow = world.findById(chair);
        REQUIRE_FALSE(lampNow.isNull());
        REQUIRE_FALSE(chairNow.isNull());
        CHECK(context.selection() == std::vector<Entity>{chairNow, lampNow});
        // The history is the one from before, and it still works on what is there.
        CHECK(context.hasUnsavedChanges());
        REQUIRE(context.history().undoCount() == 1);
        CHECK(context.history().undoLabel() == "Rename 'Lamp' to 'Lantern'");
        REQUIRE(context.history().undo().has_value());
        CHECK(world.name(lampNow) == "Lamp");
        CHECK_FALSE(context.hasUnsavedChanges());
        REQUIRE(context.history().redo().has_value());
        CHECK(world.name(lampNow) == "Lantern");

        // Nothing is being played any more.
        const Status ended = context.endPlay();
        REQUIRE_FALSE(ended.has_value());
        CHECK(ended.error().code == ErrorCode::InvalidState);
        CHECK(ended.error().message == "the scene is not being played");
        // And the scene can be saved, replaced, and played again.
        CHECK(context.saveScene(directory.path() / "edited.scene.json").has_value());
        CHECK(context.beginPlay().has_value());
        CHECK(context.endPlay().has_value());
        CHECK(context.newScene().has_value());
    }

    TEST_CASE("playing a scene that has no unsaved changes leaves it without any") {
        EditorFixture fixture;
        EditorContext& context = fixture.context;
        REQUIRE_FALSE(context.hasUnsavedChanges());
        REQUIRE(context.beginPlay().has_value());
        // An edit while it plays is no change to the scene that is kept.
        REQUIRE(EditorActions(context).renameEntity(fixture.lamp, "Torch").has_value());
        CHECK_FALSE(context.hasUnsavedChanges());
        REQUIRE(context.endPlay().has_value());
        CHECK_FALSE(context.hasUnsavedChanges());
        CHECK_FALSE(context.history().canUndo());
        CHECK(fixture.world.findByName("Torch").isNull());
        CHECK_FALSE(fixture.world.findByName("Lamp").isNull());
    }

    TEST_CASE("a scene is saved and opened again") {
        EditorFixture fixture;
        EditorContext& context = fixture.context;
        const test::TempDirectory directory;
        const std::filesystem::path path = directory.path() / "scenes" / "room.scene.json";

        // There is nowhere to save to yet.
        const Status noFile = context.saveScene();
        REQUIRE_FALSE(noFile.has_value());
        CHECK(noFile.error().code == ErrorCode::InvalidState);

        // Missing directories are created.
        REQUIRE(context.saveScene(path).has_value());
        CHECK(fileExists(path));
        CHECK(context.scenePath() == path);
        const Json saved = fixture.scene();

        REQUIRE(EditorActions(context).renameEntity(fixture.lamp, "Lantern").has_value());
        const Entity selected[] = {fixture.chair};
        context.setSelection(selected);

        REQUIRE(context.openScene(path).has_value());
        CHECK(fixture.scene() == saved);
        // Handles from before the load are stale; the document starts afresh.
        CHECK(context.selection().empty());
        CHECK_FALSE(context.history().canUndo());
        CHECK_FALSE(context.hasUnsavedChanges());
        CHECK(unwrap(context.resolveEntity("Room/Table/Lamp")) == fixture.world.findByName("Lamp"));

        // With a file, saving needs no path.
        REQUIRE(EditorActions(context).renameEntity(fixture.world.findByName("Chair"), "Stool").has_value());
        REQUIRE(context.saveScene().has_value());
        REQUIRE(context.openScene(path).has_value());
        CHECK_FALSE(fixture.world.findByName("Stool").isNull());
    }

    TEST_CASE("unsaved changes follow the history relative to the last save") {
        EditorFixture fixture;
        EditorContext& context = fixture.context;
        const test::TempDirectory directory;
        EditorActions actions(context);
        CHECK_FALSE(context.hasUnsavedChanges());

        REQUIRE(actions.renameEntity(fixture.lamp, "Lantern").has_value());
        CHECK(context.hasUnsavedChanges());

        REQUIRE(context.saveScene(directory.path() / "a.scene.json").has_value());
        CHECK_FALSE(context.hasUnsavedChanges());

        REQUIRE(actions.setEntityEnabled(fixture.chair, false).has_value());
        CHECK(context.hasUnsavedChanges());
        // Undoing back to what was saved leaves nothing to save...
        REQUIRE(context.history().undo().has_value());
        CHECK_FALSE(context.hasUnsavedChanges());
        // ...and undoing past it does.
        REQUIRE(context.history().undo().has_value());
        CHECK(context.hasUnsavedChanges());
        REQUIRE(context.history().redo().has_value());
        CHECK_FALSE(context.hasUnsavedChanges());
    }

    TEST_CASE("a scene that fails to open leaves the document as it was") {
        EditorFixture fixture;
        EditorContext& context = fixture.context;
        const test::TempDirectory directory;
        const std::filesystem::path good = directory.path() / "good.scene.json";
        const std::filesystem::path bad = directory.path() / "bad.scene.json";
        REQUIRE(context.saveScene(good).has_value());
        REQUIRE(writeTextFile(bad, R"({"format": "cordite.scene", "version": 1, "entities": [{"id": "nope"}]})")
                    .has_value());

        REQUIRE(EditorActions(context).renameEntity(fixture.lamp, "Lantern").has_value());
        const Entity selected[] = {fixture.lamp};
        context.setSelection(selected);
        const Json before = fixture.scene();

        CHECK_FALSE(context.openScene(bad).has_value());
        CHECK_FALSE(context.openScene(directory.path() / "missing.scene.json").has_value());

        CHECK(fixture.scene() == before);
        CHECK(context.scenePath() == good);
        CHECK(context.selection() == std::vector<Entity>{fixture.lamp});
        CHECK(context.history().canUndo());
        CHECK(context.hasUnsavedChanges());
    }

    TEST_CASE("a failed save keeps the file association and the unsaved state") {
        EditorFixture fixture;
        EditorContext& context = fixture.context;
        const test::TempDirectory directory;
        REQUIRE(EditorActions(context).renameEntity(fixture.lamp, "Lantern").has_value());

        // A directory cannot be written as a file.
        CHECK_FALSE(context.saveScene(directory.path()).has_value());
        CHECK(context.scenePath().empty());
        CHECK(context.hasUnsavedChanges());
    }
}
