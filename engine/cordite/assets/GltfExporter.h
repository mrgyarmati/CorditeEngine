#pragma once

/// @file
/// Writes glTF 2.0 files, so that what the engine makes can be opened elsewhere.
///
/// @code
/// GltfDocument document;
/// document.meshes.push_back({.mesh = makeBox(), .materials = {0}});
/// document.materials.push_back({.name = "Paint", .material = {.baseColor = Vec4(0.8f, 0.1f, 0.1f, 1.0f)}});
/// document.nodes.push_back({.name = "Crate", .mesh = 0});
/// document.roots = {0};
///
/// CORDITE_TRY(exportGltf(document, outputDir / "crate.glb"));    // or "crate.gltf", with a crate.bin next to it
/// @endcode
///
/// The file holds what importGltf() reads back: meshes with normals, tangents and both
/// sets of texture coordinates, materials, images, cameras and the node hierarchy. A
/// document that was imported and is exported again describes the same scene; the bytes
/// differ, because buffers are laid out anew.

#include "cordite/assets/GltfDocument.h"
#include "cordite/core/Result.h"

#include <filesystem>

namespace cordite {

/// Writes `document` to `path`. The extension selects the form: `.glb` is one binary
/// file; `.gltf` is JSON with the geometry and embedded images in a `.bin` file of the
/// same name beside it. Images that are files of their own are referred to, not copied.
///
/// Fails without writing anything when the document is not consistent: a mesh that
/// does not pass validateMesh(), an index that refers to nothing, an embedded image
/// that is neither PNG nor JPEG.
[[nodiscard]] Status exportGltf(const GltfDocument& document, const std::filesystem::path& path);

} // namespace cordite
