#pragma once

/// @file
/// A model: a hierarchy of named, placed parts that refer to meshes and materials.
///
/// It is what a glTF file's scene becomes once its meshes and materials are assets with
/// ids of their own. Putting a model into a world creates one entity per node (see
/// cordite/engine/ModelInstance.h). A model whose meshes are skinned also names their
/// skeletons and the animations that came with them (Skeleton.h, Animation.h).
///
/// @code
/// CORDITE_TRY_ASSIGN(const std::shared_ptr<const ModelData> model, assets.loadModel(modelId));
/// for (const uint32_t root : model->roots) {
///     visit(*model, model->nodes[root]);
/// }
/// @endcode

#include "cordite/core/Uuid.h"
#include "cordite/math/Transform.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace cordite {

/// A camera as a model file describes it.
struct ModelCamera {
    bool orthographic = false;
    /// For a perspective camera: the vertical field of view, in radians.
    float verticalFov = 1.0f;
    /// For an orthographic camera: the height of what it sees, in metres.
    float orthographicHeight = 10.0f;
    float nearPlane = 0.1f;
    /// Zero when the file gives none.
    float farPlane = 0.0f;

    bool operator==(const ModelCamera&) const = default;
};

struct ModelNode {
    std::string name;
    /// Relative to the parent.
    Transform transform;
    std::optional<uint32_t> parent;
    std::vector<uint32_t> children;
    /// The mesh drawn at this node, or nil.
    Uuid mesh;
    /// One per submesh of `mesh`; nil where the file names no material.
    std::vector<Uuid> materials;
    /// The skeleton that moves `mesh`, for a mesh that is skinned; nil for one that is
    /// rigid. A skinned mesh is placed by its joints: its node is where the model is.
    Uuid skeleton;
    std::optional<ModelCamera> camera;

    bool operator==(const ModelNode&) const = default;
};

struct ModelData {
    std::string name;
    /// Indexed as in the file the model came from, which keeps the index of a node -
    /// and everything derived from it - the same when the file is imported again.
    std::vector<ModelNode> nodes;
    /// The nodes without a parent that make up the model.
    std::vector<uint32_t> roots;
    /// The animations of the file, in its order: clips for the skeletons above.
    std::vector<Uuid> animations;

    bool operator==(const ModelData&) const = default;
};

} // namespace cordite
