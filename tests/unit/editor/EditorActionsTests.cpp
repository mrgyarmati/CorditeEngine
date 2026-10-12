#include "cordite/editor/EditorActions.h"

#include "support/MathTestSupport.h"
#include "unit/editor/EditorTestSupport.h"

#include <string>
#include <vector>

using namespace cordite;
using cordite::test::EditorFixture;
using cordite::test::json;
using cordite::test::Label;
using cordite::test::unwrap;

namespace {

/// Checks that undoing returns the scene to `before` and redoing to what it is now.
void checkUndoRedo(EditorFixture& fixture, const Json& before) {
    const Json after = fixture.scene();
    REQUIRE(after != before);

    const Status undone = fixture.context.history().undo();
    REQUIRE_MESSAGE(undone.has_value(), test::errorOf(undone));
    CHECK(fixture.scene() == before);

    const Status redone = fixture.context.history().redo();
    REQUIRE_MESSAGE(redone.has_value(), test::errorOf(redone));
    CHECK(fixture.scene() == after);
}

/// Checks that a failed action changed neither the scene nor the history.
void checkUntouched(const EditorFixture& fixture, const Json& before, size_t historySize = 0) {
    CHECK(fixture.scene() == before);
    CHECK(fixture.context.history().undoCount() == historySize);
}

Entity staleEntity(World& world) {
    const Entity entity = world.createEntity("Stale");
    world.destroyEntity(entity);
    return entity;
}

} // namespace

TEST_SUITE("EditorActions") {
    TEST_CASE("a created entity is a named root by default") {
        EditorFixture fixture;
        EditorActions actions(fixture.context);
        const Json before = fixture.scene();

        const Entity entity = unwrap(actions.createEntity());
        CHECK(fixture.world.name(entity) == "Entity");
        CHECK(fixture.world.parent(entity).isNull());
        CHECK(fixture.world.isEnabled(entity));
        CHECK(fixture.world.transform(entity) == Transform());
        CHECK(fixture.context.history().undoLabel() == "Create 'Entity'");

        const Uuid id = fixture.world.id(entity);
        checkUndoRedo(fixture, before);
        // Redoing brings back the same entity, so whatever remembered its id still finds it.
        CHECK(fixture.world.isValid(fixture.world.findById(id)));
    }

    TEST_CASE("an entity can be created under a parent with components") {
        EditorFixture fixture;
        EditorActions actions(fixture.context);
        const Json before = fixture.scene();

        const Entity entity = unwrap(actions.createEntity(
            {.name = "Spot",
             .parent = fixture.table,
             .components = json(R"({"Transform": {"position": [0, 2, 0]}, "Camera": {"verticalFovDegrees": 30}})")}));

        CHECK(fixture.world.parent(entity) == fixture.table);
        CHECK(fixture.world.children(fixture.table) == std::vector<Entity>{fixture.lamp, entity});
        CHECK(fixture.world.transform(entity).position == Vec3(0.0f, 2.0f, 0.0f));
        REQUIRE(fixture.world.has<CameraComponent>(entity));
        CHECK(fixture.world.tryGet<CameraComponent>(entity)->verticalFovDegrees == 30.0f);
        checkUndoRedo(fixture, before);
    }

    TEST_CASE("an entity that cannot be created leaves nothing behind") {
        EditorFixture fixture;
        EditorActions actions(fixture.context);
        const Entity stale = staleEntity(fixture.world);
        const Json before = fixture.scene();

        const std::vector<CreateEntityOptions> rejected = {
            {.name = ""},
            {.name = "a/b"},
            {.parent = stale},
            {.components = json("[]")},
            {.components = json(R"({"Jetpack": {}})")},
            {.components = json(R"({"Camera": {"verticalFovDegrees": 999}})")},
            {.components = json(R"({"Camera": {"zoom": 2}})")},
        };
        for (const CreateEntityOptions& options : rejected) {
            CAPTURE(dumpJson(options.components));
            CHECK_FALSE(actions.createEntity(options).has_value());
            checkUntouched(fixture, before);
        }
    }

    TEST_CASE("deleting an entity removes its subtree and undo restores it in place") {
        EditorFixture fixture;
        EditorActions actions(fixture.context);
        REQUIRE(fixture.outline() == std::vector<std::string>{"Room", ".Table", "..Lamp", ".Chair", "Eye"});
        const Json before = fixture.scene();

        const Entity doomed[] = {fixture.table};
        REQUIRE(actions.deleteEntities(doomed).has_value());
        CHECK(fixture.outline() == std::vector<std::string>{"Room", ".Chair", "Eye"});
        CHECK(fixture.context.history().undoLabel() == "Delete 'Table'");
        CHECK_FALSE(fixture.world.isValid(fixture.lamp));

        // The table returns as the first child, where it was, not appended at the end.
        checkUndoRedo(fixture, before);
    }

    TEST_CASE("several entities are deleted as one edit and return to their places") {
        EditorFixture fixture;
        EditorActions actions(fixture.context);
        const Entity shelf = fixture.world.createEntity("Shelf");
        const Entity rug = fixture.world.createEntity("Rug");
        REQUIRE(fixture.world.setParent(shelf, fixture.room).has_value());
        REQUIRE(fixture.world.setParent(rug, fixture.room).has_value());
        REQUIRE(fixture.outline() ==
                std::vector<std::string>{"Room", ".Table", "..Lamp", ".Chair", ".Shelf", ".Rug", "Eye"});
        const Json before = fixture.scene();

        // Listed out of order, with a repetition, a child of a listed entity and an
        // invalid handle.
        const Entity doomed[] = {fixture.eye, shelf, fixture.lamp, fixture.table, Entity{}, shelf};
        REQUIRE(actions.deleteEntities(doomed).has_value());
        CHECK(fixture.outline() == std::vector<std::string>{"Room", ".Chair", ".Rug"});
        CHECK(fixture.context.history().undoCount() == 1);
        CHECK(fixture.context.history().undoLabel() == "Delete 3 entities");

        checkUndoRedo(fixture, before);
    }

    TEST_CASE("deleting nothing valid is an error") {
        EditorFixture fixture;
        EditorActions actions(fixture.context);
        const Json before = fixture.scene();

        const Entity nothing[] = {Entity{}, staleEntity(fixture.world)};
        const Status status = actions.deleteEntities(nothing);
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ErrorCode::InvalidArgument);
        CHECK_FALSE(actions.deleteEntities({}).has_value());
        checkUntouched(fixture, before);
    }

    TEST_CASE("a duplicate sits next to its original with a new identity") {
        EditorFixture fixture;
        EditorActions actions(fixture.context);
        const Json before = fixture.scene();

        const Entity originals[] = {fixture.table};
        const std::vector<Entity> copies = unwrap(actions.duplicateEntities(originals));
        REQUIRE(copies.size() == 1);
        const Entity copy = copies.front();

        CHECK(fixture.outline() ==
              std::vector<std::string>{"Room", ".Table", "..Lamp", ".Table", "..Lamp", ".Chair", "Eye"});
        CHECK(fixture.world.children(fixture.room) == std::vector<Entity>{fixture.table, copy, fixture.chair});
        CHECK(fixture.world.id(copy) != fixture.world.id(fixture.table));
        CHECK(fixture.world.transform(copy) == fixture.world.transform(fixture.table));
        // The copies become the selection, ready to be moved.
        CHECK(fixture.context.selection() == copies);
        CHECK(fixture.context.history().undoLabel() == "Duplicate 'Table'");

        const Uuid copyId = fixture.world.id(copy);
        checkUndoRedo(fixture, before);
        // Redo recreates the copy under the id it had.
        CHECK(fixture.world.isValid(fixture.world.findById(copyId)));
    }

    TEST_CASE("duplicating several entities copies each once") {
        EditorFixture fixture;
        EditorActions actions(fixture.context);
        const Json before = fixture.scene();

        // Lamp is below Table, so it comes along with it rather than being copied twice.
        const Entity originals[] = {fixture.eye, fixture.lamp, fixture.table};
        const std::vector<Entity> copies = unwrap(actions.duplicateEntities(originals));
        REQUIRE(copies.size() == 2);
        CHECK(fixture.world.name(copies[0]) == "Eye");
        CHECK(fixture.world.name(copies[1]) == "Table");
        CHECK(fixture.world.has<CameraComponent>(copies[0]));
        CHECK(fixture.outline() ==
              std::vector<std::string>{"Room", ".Table", "..Lamp", ".Table", "..Lamp", ".Chair", "Eye", "Eye"});
        CHECK(fixture.context.history().undoLabel() == "Duplicate 2 entities");
        checkUndoRedo(fixture, before);

        CHECK_FALSE(actions.duplicateEntities({}).has_value());
    }

    TEST_CASE("renaming validates the name and can be undone") {
        EditorFixture fixture;
        EditorActions actions(fixture.context);
        const Json before = fixture.scene();

        REQUIRE(actions.renameEntity(fixture.lamp, "Lantern").has_value());
        CHECK(fixture.world.name(fixture.lamp) == "Lantern");
        CHECK(fixture.context.history().undoLabel() == "Rename 'Lamp' to 'Lantern'");
        checkUndoRedo(fixture, before);

        // The name it already has is not an edit.
        REQUIRE(actions.renameEntity(fixture.lamp, "Lantern").has_value());
        CHECK(fixture.context.history().undoCount() == 1);

        const Json renamed = fixture.scene();
        for (const char* name : {"", "a/b", "/"}) {
            CAPTURE(name);
            const Status status = actions.renameEntity(fixture.lamp, name);
            REQUIRE_FALSE(status.has_value());
            CHECK(status.error().code == ErrorCode::InvalidArgument);
        }
        CHECK_FALSE(actions.renameEntity(Entity{}, "Ghost").has_value());
        checkUntouched(fixture, renamed, 1);
    }

    TEST_CASE("enabling and disabling can be undone") {
        EditorFixture fixture;
        EditorActions actions(fixture.context);
        const Json before = fixture.scene();

        REQUIRE(actions.setEntityEnabled(fixture.room, false).has_value());
        CHECK_FALSE(fixture.world.isEnabled(fixture.room));
        CHECK(fixture.context.history().undoLabel() == "Disable 'Room'");
        checkUndoRedo(fixture, before);

        REQUIRE(actions.setEntityEnabled(fixture.room, false).has_value());
        CHECK(fixture.context.history().undoCount() == 1);
        REQUIRE(actions.setEntityEnabled(fixture.room, true).has_value());
        CHECK(fixture.context.history().undoLabel() == "Enable 'Room'");
        CHECK_FALSE(actions.setEntityEnabled(Entity{}, true).has_value());
    }

    TEST_CASE("moving an entity changes its parent and position, and undo puts it back") {
        EditorFixture fixture;
        EditorActions actions(fixture.context);
        const Json before = fixture.scene();

        REQUIRE(actions.setParent(fixture.lamp, fixture.room, 0).has_value());
        CHECK(fixture.outline() == std::vector<std::string>{"Room", ".Lamp", ".Table", ".Chair", "Eye"});
        CHECK(fixture.context.history().undoLabel() == "Move 'Lamp'");
        checkUndoRedo(fixture, before);
    }

    TEST_CASE("moving keeps the entity where it is in the world unless told otherwise") {
        EditorFixture fixture;
        EditorActions actions(fixture.context);
        // Lamp sits 0.75 above Table, which is at (1, 0, -2).
        const Vec3 worldBefore(fixture.world.computeWorldMatrix(fixture.lamp)[3]);
        REQUIRE(worldBefore == Vec3(1.0f, 0.75f, -2.0f));
        const Json before = fixture.scene();

        REQUIRE(actions.setParent(fixture.lamp, Entity{}).has_value());
        CHECK(fixture.world.parent(fixture.lamp).isNull());
        CHECK_NEAR(Vec3(fixture.world.computeWorldMatrix(fixture.lamp)[3]), worldBefore, 1e-5f);
        CHECK_NEAR(fixture.world.transform(fixture.lamp).position, worldBefore, 1e-5f);

        // Undo restores the local transform as well as the parent.
        checkUndoRedo(fixture, before);
        REQUIRE(fixture.context.history().undo().has_value());

        const bool keepWorldTransform = false;
        REQUIRE(actions.setParent(fixture.lamp, Entity{}, std::nullopt, keepWorldTransform).has_value());
        CHECK(fixture.world.transform(fixture.lamp).position == Vec3(0.0f, 0.75f, 0.0f));
    }

    TEST_CASE("only the position among siblings can change too") {
        EditorFixture fixture;
        EditorActions actions(fixture.context);
        const Json before = fixture.scene();

        REQUIRE(actions.setParent(fixture.chair, fixture.room, 0).has_value());
        CHECK(fixture.world.children(fixture.room) == std::vector<Entity>{fixture.chair, fixture.table});
        checkUndoRedo(fixture, before);
    }

    TEST_CASE("a move that changes nothing or is impossible records nothing") {
        EditorFixture fixture;
        EditorActions actions(fixture.context);
        const Json before = fixture.scene();

        // Already there.
        REQUIRE(actions.setParent(fixture.table, fixture.room, 0).has_value());
        REQUIRE(actions.setParent(fixture.eye, Entity{}).has_value());
        checkUntouched(fixture, before);

        // An entity cannot go below itself.
        const Status cycle = actions.setParent(fixture.room, fixture.lamp);
        REQUIRE_FALSE(cycle.has_value());
        CHECK(cycle.error().code == ErrorCode::InvalidArgument);
        CHECK_FALSE(actions.setParent(fixture.lamp, staleEntity(fixture.world)).has_value());
        CHECK_FALSE(actions.setParent(Entity{}, fixture.room).has_value());
        checkUntouched(fixture, before);
    }

    TEST_CASE("a component is added with its properties and removed by undo") {
        EditorFixture fixture;
        EditorActions actions(fixture.context);
        const Json before = fixture.scene();

        REQUIRE(
            actions.addComponent(fixture.lamp, "Camera", json(R"({"primary": false, "nearPlane": 0.5})")).has_value());
        const CameraComponent* camera = fixture.world.tryGet<CameraComponent>(fixture.lamp);
        REQUIRE(camera != nullptr);
        CHECK_FALSE(camera->primary);
        CHECK(camera->nearPlane == 0.5f);
        CHECK(camera->farPlane == CameraComponent().farPlane);
        CHECK(fixture.context.history().undoLabel() == "Add Camera to 'Lamp'");
        checkUndoRedo(fixture, before);

        // Without properties the component has its defaults.
        REQUIRE(actions.addComponent(fixture.chair, "Label").has_value());
        CHECK(fixture.world.tryGet<Label>(fixture.chair)->text == "label");
    }

    TEST_CASE("a component that cannot be added changes nothing") {
        EditorFixture fixture;
        EditorActions actions(fixture.context);
        const Json before = fixture.scene();

        const Status unknown = actions.addComponent(fixture.lamp, "Jetpack");
        REQUIRE_FALSE(unknown.has_value());
        CHECK(unknown.error().code == ErrorCode::NotFound);

        // Eye has a camera, and every entity has a transform.
        const Status twice = actions.addComponent(fixture.eye, "Camera");
        REQUIRE_FALSE(twice.has_value());
        CHECK(twice.error().code == ErrorCode::AlreadyExists);
        CHECK(twice.error().message == "'Eye' already has a Camera component");
        CHECK_FALSE(actions.addComponent(fixture.lamp, "Transform").has_value());

        CHECK_FALSE(actions.addComponent(fixture.lamp, "Camera", json(R"({"nearPlane": -1})")).has_value());
        CHECK_FALSE(actions.addComponent(fixture.lamp, "Camera", json(R"({"zoom": 1})")).has_value());
        CHECK_FALSE(actions.addComponent(fixture.lamp, "Camera", json("5")).has_value());
        CHECK_FALSE(actions.addComponent(Entity{}, "Camera").has_value());
        CHECK_FALSE(fixture.world.has<CameraComponent>(fixture.lamp));
        checkUntouched(fixture, before);
    }

    TEST_CASE("a removed component comes back with its values on undo") {
        EditorFixture fixture;
        EditorActions actions(fixture.context);
        fixture.world.tryGet<CameraComponent>(fixture.eye)->orthographicHeight = 3.5f;
        fixture.world.add<Label>(fixture.eye) = {"scout", 9};
        const Json before = fixture.scene();

        REQUIRE(actions.removeComponent(fixture.eye, "Camera").has_value());
        CHECK_FALSE(fixture.world.has<CameraComponent>(fixture.eye));
        CHECK(fixture.context.history().undoLabel() == "Remove Camera from 'Eye'");
        checkUndoRedo(fixture, before);

        // Values that tools cannot edit are restored as well.
        const Json withoutCamera = fixture.scene();
        REQUIRE(actions.removeComponent(fixture.eye, "Label").has_value());
        REQUIRE(fixture.context.history().undo().has_value());
        CHECK(fixture.scene() == withoutCamera);
        CHECK(fixture.world.tryGet<Label>(fixture.eye)->revision == 9);
    }

    TEST_CASE("a component that cannot be removed changes nothing") {
        EditorFixture fixture;
        EditorActions actions(fixture.context);
        const Json before = fixture.scene();

        const Status required = actions.removeComponent(fixture.lamp, "Transform");
        REQUIRE_FALSE(required.has_value());
        CHECK(required.error().code == ErrorCode::InvalidArgument);

        const Status missing = actions.removeComponent(fixture.lamp, "Camera");
        REQUIRE_FALSE(missing.has_value());
        CHECK(missing.error().code == ErrorCode::NotFound);
        CHECK(missing.error().message == "'Lamp' has no Camera component");

        CHECK_FALSE(actions.removeComponent(fixture.lamp, "Jetpack").has_value());
        CHECK_FALSE(actions.removeComponent(Entity{}, "Camera").has_value());
        checkUntouched(fixture, before);
    }

    TEST_CASE("properties are changed together and restored together") {
        EditorFixture fixture;
        EditorActions actions(fixture.context);
        const Json before = fixture.scene();

        REQUIRE(actions
                    .setComponentProperties(fixture.eye, "Camera",
                                            json(R"({"projection": "orthographic", "orthographicHeight": 6})"))
                    .has_value());
        const CameraComponent* camera = fixture.world.tryGet<CameraComponent>(fixture.eye);
        CHECK(camera->projection == CameraProjection::Orthographic);
        CHECK(camera->orthographicHeight == 6.0f);
        // What was not mentioned keeps its value.
        CHECK(camera->verticalFovDegrees == 70.0f);
        CHECK(fixture.context.history().undoLabel() == "Edit Camera of 'Eye'");
        checkUndoRedo(fixture, before);

        REQUIRE(
            actions.setComponentProperties(fixture.lamp, "Transform", json(R"({"position": [4, 5, 6]})")).has_value());
        CHECK(fixture.world.transform(fixture.lamp).position == Vec3(4.0f, 5.0f, 6.0f));
        CHECK(fixture.context.history().undoCount() == 2);
    }

    TEST_CASE("an invalid property edit changes nothing") {
        EditorFixture fixture;
        EditorActions actions(fixture.context);
        fixture.world.add<Label>(fixture.lamp);
        const Json before = fixture.scene();

        struct Case {
            Entity entity;
            const char* component;
            const char* properties;
            ErrorCode code;
        };

        const std::vector<Case> cases = {
            {fixture.eye, "Camera", R"({"verticalFovDegrees": 500})", ErrorCode::OutOfRange},
            {fixture.eye, "Camera", R"({"primary": false, "nearPlane": "close"})", ErrorCode::InvalidArgument},
            {fixture.eye, "Camera", R"({"zoom": 2})", ErrorCode::InvalidArgument},
            {fixture.eye, "Camera", R"([1])", ErrorCode::InvalidArgument},
            {fixture.eye, "Jetpack", R"({})", ErrorCode::NotFound},
            {fixture.lamp, "Camera", R"({"primary": false})", ErrorCode::NotFound},
            // A person or an agent may not change what is marked read-only.
            {fixture.lamp, "Label", R"({"text": "new", "revision": 5})", ErrorCode::InvalidArgument},
            {Entity{}, "Camera", R"({})", ErrorCode::InvalidArgument},
        };
        for (const Case& entry : cases) {
            CAPTURE(entry.component);
            CAPTURE(entry.properties);
            const Status status = actions.setComponentProperties(entry.entity, entry.component, json(entry.properties));
            REQUIRE_FALSE(status.has_value());
            CHECK(status.error().code == entry.code);
            checkUntouched(fixture, before);
        }
    }

    TEST_CASE("setting properties to the values they have is not an edit") {
        EditorFixture fixture;
        EditorActions actions(fixture.context);
        const Json before = fixture.scene();

        REQUIRE(
            actions.setComponentProperties(fixture.eye, "Camera", json(R"({"verticalFovDegrees": 70})")).has_value());
        REQUIRE(actions.setComponentProperties(fixture.eye, "Camera", json("{}")).has_value());
        checkUntouched(fixture, before);
    }

    TEST_CASE("a change made directly is recorded as one edit afterwards") {
        EditorFixture fixture;
        EditorActions actions(fixture.context);
        const Json before = fixture.scene();
        const ComponentDescriptor* transform = fixture.components.find("Transform");
        const Json componentBefore =
            transform->type.toJson(transform->getConst(fixture.world.registry(), fixture.lamp.handle));

        // What a drag in the inspector does: many small changes without history...
        for (int step = 1; step <= 10; ++step) {
            fixture.world.transform(fixture.lamp).position.x = static_cast<float>(step);
        }
        CHECK(fixture.context.history().undoCount() == 0);

        // ...and a single entry when the control is released.
        REQUIRE(actions.recordComponentEdit(fixture.lamp, "Transform", componentBefore).has_value());
        CHECK(fixture.context.history().undoCount() == 1);
        CHECK(fixture.context.history().undoLabel() == "Edit Transform of 'Lamp'");
        checkUndoRedo(fixture, before);
        CHECK(fixture.world.transform(fixture.lamp).position.x == 10.0f);

        // Nothing changed since: nothing to record.
        const Json componentNow =
            transform->type.toJson(transform->getConst(fixture.world.registry(), fixture.lamp.handle));
        REQUIRE(actions.recordComponentEdit(fixture.lamp, "Transform", componentNow).has_value());
        CHECK(fixture.context.history().undoCount() == 1);

        CHECK_FALSE(actions.recordComponentEdit(fixture.lamp, "Camera", componentNow).has_value());
        CHECK_FALSE(actions.recordComponentEdit(fixture.lamp, "Jetpack", componentNow).has_value());
        CHECK_FALSE(actions.recordComponentEdit(fixture.lamp, "Transform", json(R"({"position": "bad"})")).has_value());
        CHECK(fixture.context.history().undoCount() == 1);
    }

    TEST_CASE("a whole session can be undone to the start and redone to the end") {
        EditorFixture fixture;
        EditorActions actions(fixture.context);
        const Json start = fixture.scene();
        std::vector<Json> states = {start};
        const auto remember = [&] { states.push_back(fixture.scene()); };

        const Entity crate = unwrap(actions.createEntity({.name = "Crate", .parent = fixture.room}));
        remember();
        REQUIRE(actions.addComponent(crate, "Label", json(R"({"text": "fragile"})")).has_value());
        remember();
        REQUIRE(actions.setParent(crate, fixture.table, 0).has_value());
        remember();
        const Entity toCopy[] = {fixture.table};
        const std::vector<Entity> copies = unwrap(actions.duplicateEntities(toCopy));
        remember();
        REQUIRE(actions.renameEntity(copies.front(), "Second table").has_value());
        remember();
        REQUIRE(actions.setComponentProperties(copies.front(), "Transform", json(R"({"position": [5, 0, 5]})"))
                    .has_value());
        remember();
        const Entity toDelete[] = {fixture.table, fixture.eye};
        REQUIRE(actions.deleteEntities(toDelete).has_value());
        remember();
        REQUIRE(actions.setEntityEnabled(fixture.room, false).has_value());
        remember();
        REQUIRE(actions.removeComponent(fixture.world.findByName("Crate"), "Label").has_value());
        remember();

        // Every state on the way back matches what it was on the way there...
        for (size_t step = states.size() - 1; step > 0; --step) {
            CAPTURE(step);
            const Status undone = fixture.context.history().undo();
            REQUIRE_MESSAGE(undone.has_value(), test::errorOf(undone));
            CHECK(fixture.scene() == states[step - 1]);
        }
        CHECK_FALSE(fixture.context.history().canUndo());

        // ...and on the way forward again.
        for (size_t step = 1; step < states.size(); ++step) {
            CAPTURE(step);
            const Status redone = fixture.context.history().redo();
            REQUIRE_MESSAGE(redone.has_value(), test::errorOf(redone));
            CHECK(fixture.scene() == states[step]);
        }
        CHECK_FALSE(fixture.context.history().canRedo());
    }

    TEST_CASE("history entries fail cleanly when the scene was changed behind their back") {
        EditorFixture fixture;
        EditorActions actions(fixture.context);
        REQUIRE(actions.renameEntity(fixture.lamp, "Lantern").has_value());

        // Destroyed without going through the actions, so the history does not know.
        fixture.world.destroyEntity(fixture.lamp);

        const Status status = fixture.context.history().undo();
        REQUIRE_FALSE(status.has_value());
        CHECK(status.error().code == ErrorCode::InvalidState);
        CHECK(status.error().message.find("no longer exists") != std::string::npos);
        CHECK_FALSE(fixture.context.history().canUndo());
    }
}
