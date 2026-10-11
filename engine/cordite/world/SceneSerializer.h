#pragma once

/// @file
/// Saving and loading worlds as JSON.
///
/// A scene file is a JSON document that people and agents can read, diff and edit:
///
/// @code{.json}
/// {
///     "format": "cordite.scene",
///     "version": 1,
///     "name": "Depot",
///     "entities": [
///         {
///             "id": "5f0c7c1e-...",
///             "name": "Lamp",
///             "enabled": true,
///             "parent": null,
///             "components": {
///                 "Transform": { "position": [0, 2.5, 0], "rotation": [0, 0, 0, 1], "scale": [1, 1, 1] },
///                 "Camera": { "projection": "perspective", "verticalFovDegrees": 60 }
///             }
///         }
///     ]
/// }
/// @endcode
///
/// Entities are listed parents first, so `parent` always refers to an earlier entry.
/// Properties a component leaves out get their default values.
///
/// Loading is strict: an unknown component, an unknown property, a value of the wrong
/// type or out of range, a duplicate id or a dangling parent is an error that names the
/// entity involved. Everything is checked before anything is created, so a failed load
/// leaves the destination world exactly as it was.
///
/// @code
/// CORDITE_TRY(saveScene(path, world, components));
/// CORDITE_TRY(loadScene(path, world, components));
///
/// // Duplicate a subtree: serialize it, then instantiate it with fresh identifiers.
/// const Entity selection[] = {crate};
/// const Json snapshot = serializeEntities(world, selection, components);
/// CORDITE_TRY_ASSIGN(const std::vector<Entity> copies,
///                    instantiateEntities(world, snapshot, components, {.generateNewIds = true}));
/// @endcode

#include "cordite/core/JsonFwd.h"
#include "cordite/core/Result.h"
#include "cordite/world/ComponentRegistry.h"
#include "cordite/world/World.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <vector>

namespace cordite {

inline constexpr uint32_t kSceneFormatVersion = 1;

/// Serializes the whole world into a scene document.
[[nodiscard]] Json serializeScene(const World& world, const ComponentRegistry& components);

/// Replaces the content of `world` with the scene in `document`. On failure the world is
/// unchanged. Fails with VersionMismatch for a document written by a newer engine.
[[nodiscard]] Status deserializeScene(World& world, const Json& document, const ComponentRegistry& components);

[[nodiscard]] Status saveScene(const std::filesystem::path& path, const World& world,
                               const ComponentRegistry& components);
[[nodiscard]] Status loadScene(const std::filesystem::path& path, World& world, const ComponentRegistry& components);

/// Serializes the given entities and everything below them into a JSON array in the
/// same form as a scene's "entities". An entity that is listed twice, or that is below
/// another listed entity, is written once; invalid handles are skipped. Used for copy
/// and paste, prefabs and undo.
[[nodiscard]] Json serializeEntities(const World& world, std::span<const Entity> roots,
                                     const ComponentRegistry& components);

struct InstantiateOptions {
    /// Where the top-level entities of the array - those whose parent is not part of it -
    /// are attached:
    /// - not set: under the parent they were serialized with if that entity exists in the
    ///   world, as roots otherwise. This puts entities back where they were.
    /// - a null Entity: as roots.
    /// - an entity: as its children.
    std::optional<Entity> parent;

    /// Give every created entity a new identifier, and redirect entity references
    /// inside the group to the new copies. Use it when duplicating; leave it off when
    /// restoring entities that must keep their identity (undo), which then fails with
    /// AlreadyExists if one of the identifiers is in use.
    bool generateNewIds = false;
};

/// Creates entities from an array produced by serializeEntities, appending them to the
/// children of their parents. Returns the created entities in array order. On failure
/// nothing is created.
[[nodiscard]] Result<std::vector<Entity>> instantiateEntities(World& world, const Json& entities,
                                                              const ComponentRegistry& components,
                                                              const InstantiateOptions& options = {});

} // namespace cordite
