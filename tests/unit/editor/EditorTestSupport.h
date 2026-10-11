#pragma once

/// @file
/// Fixtures shared by the editor tests: a scene document with a small hierarchy.

#include "cordite/core/Json.h"
#include "cordite/core/Reflection.h"
#include "cordite/editor/EditorContext.h"
#include "cordite/world/Components.h"
#include "cordite/world/SceneSerializer.h"

#include "support/TestSupport.h"

#include <string>
#include <string_view>
#include <vector>

namespace cordite::test {

/// A component with a property tools may not change, to exercise that rule.
struct Label {
    std::string text = "label";
    int32_t revision = 1;
};

inline TypeInfo describeLabel() {
    return TypeBuilder<Label>("Label")
        .property("text", &Label::text)
        .property("revision", &Label::revision, {.readOnly = true})
        .build();
}

/// A world, the component types and an editor document on top of them, holding:
///
///     Room
///       Table
///         Lamp
///       Chair
///     Eye          (Camera)
struct EditorFixture {
    World world;
    ComponentRegistry components;
    EditorContext context;

    Entity room;
    Entity table;
    Entity lamp;
    Entity chair;
    Entity eye;

    EditorFixture() : context(world, components) {
        REQUIRE(registerWorldComponents(components).has_value());
        REQUIRE(components.add<Label>(describeLabel()).has_value());

        room = world.createEntity("Room");
        table = world.createEntity("Table");
        lamp = world.createEntity("Lamp");
        chair = world.createEntity("Chair");
        eye = world.createEntity("Eye");
        REQUIRE(world.setParent(table, room).has_value());
        REQUIRE(world.setParent(lamp, table).has_value());
        REQUIRE(world.setParent(chair, room).has_value());
        world.transform(table).position = Vec3(1.0f, 0.0f, -2.0f);
        world.transform(lamp).position = Vec3(0.0f, 0.75f, 0.0f);
        world.add<CameraComponent>(eye).verticalFovDegrees = 70.0f;
    }

    /// The whole scene as JSON, for checking that an edit or an undo left it as expected.
    [[nodiscard]] Json scene() const { return serializeScene(world, components); }

    /// Entity names in hierarchy order, indented by depth with dots.
    [[nodiscard]] std::vector<std::string> outline() const {
        std::vector<std::string> lines;
        world.traverse(
            [&](Entity entity, uint32_t depth) { lines.push_back(std::string(depth, '.') + world.name(entity)); });
        return lines;
    }
};

inline Json json(std::string_view text) {
    return unwrap(parseJson(text));
}

} // namespace cordite::test
