#pragma once

/// @file
/// Puts a model into a world.
///
/// A model (see cordite/assets/Model.h) is a hierarchy of nodes; instantiating it makes
/// an entity for each node of its scene, with the transforms, mesh renderers and cameras
/// the nodes describe, under one new root entity.
///
/// A model with skeletons is laid out for animation. Its joints get no entities: where
/// they are is part of a pose. For each skeleton there is one entity with an Animator
/// ("Skeleton", "Skeleton 2", ...) directly under the root, and the meshes skinned to
/// that skeleton hang under it - so a figure of several meshes is animated in one
/// place. The animator plays nothing until it is given a clip; ModelData::animations
/// are the clips the file came with.
///
/// @code
/// CORDITE_TRY_ASSIGN(const std::shared_ptr<const ModelData> model, assets.loadModel(crateId));
/// const Entity crate = instantiateModel(world, *model, {.parent = room});
/// world.transform(crate).position = Vec3(2.0f, 0.0f, -1.0f);
/// @endcode
///
/// The entities are ordinary ones from then on: nothing ties them to the model they
/// came from, and they are saved with the scene like any others.

#include "cordite/assets/Model.h"
#include "cordite/core/Uuid.h"
#include "cordite/math/Math.h"
#include "cordite/world/Entity.h"
#include "cordite/world/World.h"

#include <optional>
#include <string>
#include <vector>

namespace cordite {

struct ModelInstanceOptions {
    /// The instance becomes the last child of this entity; null makes it a root.
    Entity parent;
    /// The name of the instance's root entity. Empty uses the name of the model.
    std::string name;
    /// Where the root of the instance is put, relative to its parent. Without, at the
    /// parent's origin.
    std::optional<Vec3> position;
};

/// Creates the entities of a model and returns the root of the instance: a new entity
/// with an identity transform - but for `options.position` - whose children are the
/// model's own root nodes. Moving, turning or scaling that root places the whole model.
///
/// Cameras of the model are created with `primary` off, so that placing a model never
/// changes what the game is seen through.
///
/// A skinned mesh is placed by its joints, as glTF has it: the entity of its node hangs
/// under the animator of its skeleton, unmoved, wherever the node is in the model. A
/// joint becomes an entity only when it has a mesh or a camera of its own, or a node
/// below it that is no joint; such an entity stands where the joint rests and does not
/// follow a pose, which the log then says.
Entity instantiateModel(World& world, const ModelData& model, const ModelInstanceOptions& options = {});

/// The materials `model` draws its mesh `mesh` with - which a mesh placed on its own is
/// given (EditorActions::instantiateMesh): those of the first of its nodes that draws it,
/// one per submesh, nil where the file names none. Empty for a mesh that no node draws.
[[nodiscard]] std::vector<Uuid> materialsOfMesh(const ModelData& model, const Uuid& mesh);

} // namespace cordite
