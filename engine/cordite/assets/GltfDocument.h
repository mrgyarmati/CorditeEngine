#pragma once

/// @file
/// The content of a glTF 2.0 file in the engine's terms.
///
/// importGltf() produces a GltfDocument and exportGltf() writes one, so what one of
/// them writes the other reads back. Everything is addressed by its index in the file:
/// node 3 refers to mesh 1 whose second submesh uses material 0, and so on. Those
/// indices are also what the identifiers of the assets inside a glTF file are derived
/// from, which is why entries are never dropped or reordered - a mesh that could not be
/// imported is still there, empty.
///
/// A file's skins become skeletons and its animations clips that move joints by name
/// (Skeleton.h, Animation.h). In a glTF file a joint is a node like any other; which
/// nodes are joints, what they hang on and where they rest is worked out from the
/// nodes by skeletonOf().
///
/// @code
/// CORDITE_TRY_ASSIGN(const GltfDocument document, importGltf(path));
/// for (const uint32_t root : document.roots) {
///     const GltfNode& node = document.nodes[root];
///     if (node.mesh) {
///         draw(document.meshes[*node.mesh].mesh, node.transform.toMatrix());
///     }
/// }
/// @endcode

#include "cordite/assets/Animation.h"
#include "cordite/assets/Material.h"
#include "cordite/assets/Mesh.h"
#include "cordite/assets/Model.h"
#include "cordite/assets/Skeleton.h"
#include "cordite/core/Result.h"
#include "cordite/math/Transform.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace cordite {

/// An image a glTF file uses: either a file of its own, or bytes inside the glTF.
struct GltfImage {
    std::string name;
    /// For an image in a file of its own: the path of that file relative to the
    /// directory of the glTF file, with forward slashes. Empty for an embedded image.
    std::string uri;
    /// For an embedded image: its encoded (PNG or JPEG) bytes. Only filled when the
    /// file's data was loaded.
    std::vector<std::byte> encoded;

    [[nodiscard]] bool isEmbedded() const { return uri.empty(); }

    bool operator==(const GltfImage&) const = default;
};

/// For each texture slot of a material, the index of the image in it.
struct GltfMaterialImages {
    std::optional<uint32_t> baseColor;
    std::optional<uint32_t> metallicRoughness;
    std::optional<uint32_t> normal;
    std::optional<uint32_t> occlusion;
    std::optional<uint32_t> emissive;

    bool operator==(const GltfMaterialImages&) const = default;
};

struct GltfMaterial {
    std::string name;
    /// Everything but the textures: the `...Texture` members are nil, because a glTF
    /// file knows its images by index, not by asset id. See `images`.
    MaterialData material;
    GltfMaterialImages images;

    bool operator==(const GltfMaterial&) const = default;
};

struct GltfMesh {
    /// Has no submeshes when nothing in the glTF mesh could be imported.
    MeshData mesh;
    /// For each submesh, the index of the material it was made with, if any.
    std::vector<std::optional<uint32_t>> materials;

    bool operator==(const GltfMesh&) const = default;
};

struct GltfNode {
    std::string name;
    /// Relative to the parent.
    Transform transform;
    std::optional<uint32_t> parent;
    std::vector<uint32_t> children;
    std::optional<uint32_t> mesh;
    /// The skin whose joints move `mesh`, for a mesh that is skinned. Such a mesh is
    /// placed by its joints alone: where its own node is does not matter, as glTF
    /// has it.
    std::optional<uint32_t> skin;
    std::optional<ModelCamera> camera;

    bool operator==(const GltfNode&) const = default;
};

/// A skin of the file: a skeleton, and which nodes its joints are.
struct GltfSkin {
    /// The joints in the order the file lists them, which is the order the vertices of
    /// a mesh name them in. What a joint is called, what it hangs on and where it rests
    /// is taken from its node and the nodes above it (skeletonOf()); only the inverse
    /// bind matrices are the skin's own. Without joints for a skin that could not be
    /// imported.
    SkeletonData skeleton;
    /// For each joint, the index of the node it is.
    std::vector<uint32_t> joints;

    bool operator==(const GltfSkin&) const = default;
};

/// An animation of the file: for each node it moves, a track.
struct GltfAnimation {
    /// The tracks name their nodes as jointNameOf() does. Tracks without keys when only
    /// the structure of the file was read.
    AnimationData animation;
    /// For each track, the index of the node it moves.
    std::vector<uint32_t> nodes;

    bool operator==(const GltfAnimation&) const = default;
};

struct GltfDocument {
    std::vector<GltfMesh> meshes;
    std::vector<GltfMaterial> materials;
    std::vector<GltfImage> images;
    /// Every node of the file. Those of the file's default scene are reachable from `roots`.
    std::vector<GltfNode> nodes;
    std::vector<uint32_t> roots;
    std::vector<GltfSkin> skins;
    std::vector<GltfAnimation> animations;
    /// What the file contains that was left out or changed on the way in, one sentence
    /// each. Empty for a file the engine represents completely.
    std::vector<std::string> warnings;
};

/// What a node of a document is called as a joint, and in the tracks of animations: by
/// its own name where no other node has it, and by its name and its index where one
/// does ("Hand #12"). Empty for a node the document does not have.
[[nodiscard]] std::string jointNameOf(const GltfDocument& document, uint32_t node);

/// The skeleton that the nodes `joints` of a document make. Each joint has the name
/// jointNameOf() gives its node and rests where its node is; it hangs on the nearest
/// node above it that is a joint too, and what the nodes in between come to is its
/// SkeletonJoint::parentOffset. `inverseBind` has a matrix for every joint, or none at
/// all for joints that are bound where they are.
///
/// Fails for more joints than a skeleton may have, a node that the document does not
/// have or that is listed twice, matrices of another number than joints, and whatever
/// validateSkeleton() rejects.
[[nodiscard]] Result<SkeletonData> skeletonOf(const GltfDocument& document, std::span<const uint32_t> joints,
                                              std::span<const Mat4> inverseBind, std::string name);

} // namespace cordite
