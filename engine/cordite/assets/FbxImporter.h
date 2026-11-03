#pragma once

/// @file
/// Reads FBX files (binary and ASCII) and OBJ files (with the MTL files they name) into a
/// GltfDocument, the engine's terms for a model, with ufbx.
///
/// @code
/// CORDITE_TRY_ASSIGN(const GltfDocument document, importFbx(projectDir / "assets/models/crate.fbx"));
/// for (const std::string& warning : document.warnings) {
///     CORDITE_LOG_WARN("Assets", "crate.fbx: {}", warning);
/// }
/// @endcode
///
/// What is imported: polygon meshes, triangulated, with positions, normals - smooth ones
/// computed where a file has none - and the first two sets of texture coordinates, and
/// tangents computed with MikkTSpace; one submesh per material of a mesh; materials,
/// in the metallic-roughness model, which ufbx works out from whatever model a file
/// uses (Phong for OBJ and most FBX files); the images they use, in files of their own
/// or inside an FBX file; and the hierarchy of nodes. A scene is brought to the
/// engine's axes and units, +Y up and metres, and texture coordinates to glTF's, whose
/// origin is at the top left of an image.
///
/// What is not, yet: skins, animations, blend shapes, cameras, lights and vertex colors.
/// A file that has them loads without them and says so in GltfDocument::warnings.
///
/// A file is untrusted input. ufbx bounds what it allocates; the geometry a file may
/// hold is bounded as for glTF (GltfImportOptions::maxVertices, maxIndices); and an OBJ
/// file's MTL files are only read from inside GltfImportOptions::rootDirectory. Images
/// are not read here: as a glTF file does, a file names them relative to itself
/// (GltfImage::uri), and the asset database finds them as assets where that leads. An
/// image a file names only by where it was on another machine is not used, and said in
/// a warning. GltfImportOptions::maxAnimationKeys does not apply: animations are not
/// imported.

#include "cordite/assets/GltfDocument.h"
#include "cordite/assets/GltfImporter.h"
#include "cordite/core/Result.h"

#include <filesystem>

namespace cordite {

/// Reads the FBX or OBJ file at `path`, as its extension says (`.fbx`, `.obj`, in any
/// case). The error names the file and what is wrong with it. Without
/// GltfImportOptions::loadData the meshes have their submeshes and materials, but no
/// vertices, and images no bytes: what a file holds, without reading all of it into
/// meshes.
[[nodiscard]] Result<GltfDocument> importFbx(const std::filesystem::path& path, const GltfImportOptions& options = {});

/// Reads a model file of any kind the engine reads: glTF (importGltf()) or FBX and OBJ
/// (importFbx()), as its extension says. Fails with InvalidArgument for another.
[[nodiscard]] Result<GltfDocument> importModelFile(const std::filesystem::path& path,
                                                   const GltfImportOptions& options = {});

} // namespace cordite
