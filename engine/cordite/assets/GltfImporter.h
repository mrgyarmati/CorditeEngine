#pragma once

/// @file
/// Reads glTF 2.0 files (.gltf and .glb).
///
/// @code
/// CORDITE_TRY_ASSIGN(const GltfDocument document, importGltf(projectDir / "assets/models/crate.gltf"));
/// for (const std::string& warning : document.warnings) {
///     CORDITE_LOG_WARN("Assets", "crate.gltf: {}", warning);
/// }
/// @endcode
///
/// What is imported: triangle meshes (lists, strips and fans; indexed or not; with
/// positions and optionally normals, tangents and two sets of texture coordinates, in
/// any of the component types glTF and KHR_mesh_quantization allow), materials in the
/// metallic-roughness model with KHR_materials_emissive_strength, KHR_materials_ior,
/// KHR_materials_specular and KHR_texture_transform, images, cameras, and the node
/// hierarchy of the default scene. Missing normals are replaced by flat ones, as the
/// specification prescribes, and missing tangents are computed with MikkTSpace.
///
/// Skinned meshes come with the joints and weights of their vertices - the first four
/// of each, brought to one in all -, skins as skeletons, and animations as clips whose
/// tracks move nodes by name: translation, rotation and scale, stepped, linear or as
/// cubic splines. A skin or a track that makes no sense - a joint listed twice, times
/// that run backwards - is left out and reported; the rest of the file is not refused
/// for it.
///
/// What is not: morph targets, lights, vertex colors, and compressed geometry (Draco,
/// meshopt). A file that merely uses such a feature loads without it and says so in
/// GltfDocument::warnings; a file that requires one is refused.
///
/// A glTF file is untrusted input. Its numbers are checked against the data that is
/// really there; the size of each mesh (kMaxMeshVertices, kMaxMeshIndices) and of the
/// file as a whole (GltfImportOptions) is limited; and the files it refers to are only
/// read from inside GltfImportOptions::rootDirectory.

#include "cordite/assets/GltfDocument.h"
#include "cordite/assets/Mesh.h"
#include "cordite/core/Result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace cordite {

struct GltfImportOptions {
    /// Read the buffers: vertex data and embedded images. Without them only the
    /// structure is returned - names, materials, nodes, which submesh uses which material
    /// - with meshes whose submeshes are empty. That is enough to list what a file
    /// contains, and far cheaper.
    bool loadData = true;

    /// The directory every file the glTF refers to has to be inside. Empty means the
    /// directory the glTF file is in.
    std::filesystem::path rootDirectory;

    /// How much geometry the file may describe in all, counted as the file states it,
    /// before anything is merged. Any number of meshes may refer to the same few bytes,
    /// so a small file can describe an enormous model; these two bound the memory and
    /// the time an import can take. The defaults are what a single mesh may hold.
    uint32_t maxVertices = kMaxMeshVertices;
    uint32_t maxIndices = kMaxMeshIndices;

    /// How many bytes the images stored inside the file may have in all, encoded as
    /// they are. Textures beyond that belong in files of their own.
    size_t maxEmbeddedImageBytes = size_t{1} << 30;

    /// How many keys the animations of the file may have between them, counted as the
    /// file states them. The default is what a single animation may hold.
    uint32_t maxAnimationKeys = kMaxAnimationKeys;
};

/// Reads a glTF file. The error names the file and what is wrong with it.
[[nodiscard]] Result<GltfDocument> importGltf(const std::filesystem::path& path, const GltfImportOptions& options = {});

} // namespace cordite
