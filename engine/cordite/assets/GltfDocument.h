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
/// @code
/// CORDITE_TRY_ASSIGN(const GltfDocument document, importGltf(path));
/// for (const uint32_t root : document.roots) {
///     const GltfNode& node = document.nodes[root];
///     if (node.mesh) {
///         draw(document.meshes[*node.mesh].mesh, node.transform.toMatrix());
///     }
/// }
/// @endcode

#include "cordite/assets/Material.h"
#include "cordite/assets/Mesh.h"
#include "cordite/assets/Model.h"
#include "cordite/math/Transform.h"

#include <cstddef>
#include <cstdint>
#include <optional>
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
    std::optional<ModelCamera> camera;

    bool operator==(const GltfNode&) const = default;
};

struct GltfDocument {
    std::vector<GltfMesh> meshes;
    std::vector<GltfMaterial> materials;
    std::vector<GltfImage> images;
    /// Every node of the file. Those of the file's default scene are reachable from `roots`.
    std::vector<GltfNode> nodes;
    std::vector<uint32_t> roots;
    /// What the file contains that was left out or changed on the way in, one sentence
    /// each. Empty for a file the engine represents completely.
    std::vector<std::string> warnings;
};

} // namespace cordite
