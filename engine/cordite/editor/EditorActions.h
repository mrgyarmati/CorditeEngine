#pragma once

/// @file
/// The edits the editor can make to a scene, each of them undoable.
///
/// Menus, panels and commands all change the scene through these functions rather than
/// through the World directly. Each one validates its input, makes the change, and
/// records in the context's history how to take it back and how to make it again. A
/// function that fails has changed nothing and recorded nothing.
///
/// @code
/// EditorActions actions(context);
/// CORDITE_TRY_ASSIGN(const Entity eye,
///                    actions.createEntity({.name = "Eye", .components = {{"Camera", Json::object()}}}));
/// CORDITE_TRY(actions.setComponentProperties(eye, "Transform", {{"position", {0.0, 1.7, 4.0}}}));
/// CORDITE_TRY(context.history().undo());      // the camera is back at the origin
/// @endcode
///
/// Entities are remembered in the history by id, never by handle, so an edit can be
/// undone and redone across the entity being destroyed and restored in between.

#include "cordite/core/Json.h"
#include "cordite/core/Result.h"
#include "cordite/editor/EditorContext.h"
#include "cordite/engine/ModelInstance.h"
#include "cordite/world/Entity.h"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cordite {

struct CreateEntityOptions {
    std::string name = "Entity";
    /// The new entity becomes the last child of this one; null makes it a root.
    Entity parent;
    /// Components to give it, by name, each with the properties that differ from the
    /// defaults: `{"Camera": {"verticalFovDegrees": 45}}`. "Transform" places the entity.
    Json components = Json::object();
};

class EditorActions {
public:
    /// The context must outlive the actions object, and the history entries it records.
    explicit EditorActions(EditorContext& context) : m_context(context) {}

    [[nodiscard]] Result<Entity> createEntity(const CreateEntityOptions& options = {});

    /// Deletes the entities and everything below them. Entities listed twice or below
    /// another listed entity are handled once; invalid handles are ignored. Fails with
    /// InvalidArgument when nothing valid is left.
    [[nodiscard]] Status deleteEntities(std::span<const Entity> entities);

    /// Copies the entities, with everything below them, next to the originals. The
    /// copies get new ids, and references among them point at the copies. Returns the
    /// copies of the listed entities and selects them.
    [[nodiscard]] Result<std::vector<Entity>> duplicateEntities(std::span<const Entity> entities);

    /// Places a model in the scene: an entity for each of its nodes, under one new root
    /// (see instantiateModel in cordite/engine/ModelInstance.h). Returns that root and
    /// selects it. Fails with NotFound when `model` names no asset, with InvalidArgument
    /// when it names something other than a model, and with the importer's error when
    /// the model cannot be loaded.
    [[nodiscard]] Result<Entity> instantiateModel(const Uuid& model, const ModelInstanceOptions& options = {});

    /// Fails with InvalidArgument for an empty name or one containing '/', which
    /// separates the names in entity paths.
    [[nodiscard]] Status renameEntity(Entity entity, std::string_view name);

    [[nodiscard]] Status setEntityEnabled(Entity entity, bool enabled);

    /// Moves an entity under another parent (null for none) and, optionally, to a
    /// position among its new siblings; without one it becomes the last. See
    /// World::setParent for `keepWorldTransform`.
    [[nodiscard]] Status setParent(Entity entity, Entity parent, std::optional<size_t> siblingIndex = std::nullopt,
                                   bool keepWorldTransform = true);

    /// Fails with NotFound for an unknown component and AlreadyExists when the entity
    /// has one of that type.
    [[nodiscard]] Status addComponent(Entity entity, std::string_view component,
                                      const Json& properties = Json::object());

    /// Fails with InvalidArgument for a component every entity must have.
    [[nodiscard]] Status removeComponent(Entity entity, std::string_view component);

    /// Changes some properties of a component, as an edit: read-only properties and
    /// unknown names are rejected (see TypeInfo::parseEdit).
    [[nodiscard]] Status setComponentProperties(Entity entity, std::string_view component, const Json& properties);

    /// Records a change that has already been made to a component, given what the
    /// component looked like before (TypeInfo::toJson). For edits that are applied
    /// continuously while the user drags a control and become one history entry when the
    /// control is released. Records nothing when the component is unchanged.
    [[nodiscard]] Status recordComponentEdit(Entity entity, std::string_view component, const Json& before);

private:
    EditorContext& m_context;
};

} // namespace cordite
