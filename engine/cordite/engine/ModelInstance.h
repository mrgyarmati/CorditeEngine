#pragma once

/// @file
/// Puts a model into a world.
///
/// A model (see cordite/assets/Model.h) is a hierarchy of nodes; instantiating it makes
/// an entity for each node of its scene, with the transforms, mesh renderers and cameras
/// the nodes describe, under one new root entity.
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
#include "cordite/world/Entity.h"
#include "cordite/world/World.h"

#include <string>

namespace cordite {

struct ModelInstanceOptions {
    /// The instance becomes the last child of this entity; null makes it a root.
    Entity parent;
    /// The name of the instance's root entity. Empty uses the name of the model.
    std::string name;
};

/// Creates the entities of a model and returns the root of the instance: a new entity
/// with an identity transform whose children are the model's own root nodes. Moving,
/// turning or scaling that root places the whole model.
///
/// Cameras of the model are created with `primary` off, so that placing a model never
/// changes what the game is seen through.
Entity instantiateModel(World& world, const ModelData& model, const ModelInstanceOptions& options = {});

} // namespace cordite
