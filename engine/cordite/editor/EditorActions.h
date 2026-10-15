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

#include "cordite/assets/AssetDatabase.h"
#include "cordite/core/Json.h"
#include "cordite/core/Result.h"
#include "cordite/core/Uuid.h"
#include "cordite/editor/EditorContext.h"
#include "cordite/engine/ModelInstance.h"
#include "cordite/world/Entity.h"
#include "cordite/world/Prefab.h"

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

    /// Places a prefab in the scene: copies of its entities, with ids of their own, the
    /// root of them marked as an instance of it (see instantiatePrefab in
    /// cordite/world/Prefab.h). Returns that root and selects it. Fails with NotFound
    /// when `prefab` names no asset, with InvalidArgument when it names something other
    /// than a prefab, and with the reader's error, which names the prefab, when it
    /// cannot be loaded or its entities are wrong.
    [[nodiscard]] Result<Entity> instantiatePrefab(const Uuid& prefab, const PrefabInstanceOptions& options = {});

    /// Places an asset in the scene, whichever it is of the kinds that can be placed
    /// (canBePlaced()): a model as instantiateModel() does, a prefab as
    /// instantiatePrefab() does. Without a name the root is called what the model or
    /// the prefab calls it. Fails with NotFound when `asset` names none, with
    /// InvalidArgument for an asset of another kind, and as those two do.
    [[nodiscard]] Result<Entity> instantiateAsset(const Uuid& asset, Entity parent = {}, std::string_view name = {});

    /// Makes a prefab of an entity and everything below it: writes them into the file
    /// `assetPath` of the asset directory, reads the assets again, and marks the entity
    /// as an instance of the new prefab. Returns the id of the prefab. An empty path is
    /// defaultPrefabPath() of the entity's name.
    ///
    /// Only the mark is an edit of the scene, and only that is undone: the file stays.
    /// A prefab that is written over keeps its id, and what was placed from it before
    /// stays as it is.
    ///
    /// Fails with InvalidState while the scene is played and in an editor without an
    /// asset directory; with InvalidArgument for a path that leaves that directory or
    /// does not end in ".prefab.json", and for more than kMaxPrefabEntities entities;
    /// with AlreadyExists for a file that is there, unless `overwrite`; and as writing a
    /// file and AssetManager::refresh() do. Nothing has been written before any of the
    /// first four.
    [[nodiscard]] Result<Uuid> createPrefab(Entity entity, std::string_view assetPath = {}, bool overwrite = false);

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

    /// Records changes that have already been made to one component of several entities as
    /// one edit - what dragging the gizmo of a selection does. `before` holds what the
    /// component of each looked like, in the same order. Entities whose component did not
    /// change are left out, and nothing is recorded when none did. Fails, recording
    /// nothing, as recordComponentEdit does for any of them, and with InvalidArgument when
    /// the two lists are not as long as each other.
    [[nodiscard]] Status recordComponentEdits(std::span<const Entity> entities, std::string_view component,
                                              std::span<const Json> before);

private:
    /// Records that `instance`, which was just created with everything below it, was
    /// placed - so that it can be taken away and put back - and selects it.
    void recordPlaced(Entity instance);

    EditorContext& m_context;
};

/// Whether an asset of this type is something to place in a scene: a model, a prefab.
[[nodiscard]] constexpr bool canBePlaced(AssetType type) {
    return type == AssetType::Model || type == AssetType::Prefab;
}

/// Where createPrefab() keeps the prefab of an entity called `entityName` when it is
/// not told: in "prefabs", in a file named after the entity as far as a file can be -
/// its letters and digits in lower case, what is between them a dash:
/// "prefabs/front-wheel.prefab.json" for "Front wheel". A name without any is "prefab".
[[nodiscard]] std::string defaultPrefabPath(std::string_view entityName);

} // namespace cordite
