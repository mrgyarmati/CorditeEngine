#include "cordite/assets/FbxImporter.h"

#include "cordite/assets/Mesh.h"
#include "cordite/assets/MeshProcessing.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/StringUtils.h"

#include <fmt/format.h>
#include <ufbx.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace cordite {
namespace {

/// What ufbx may allocate for one file, for the scene and while it works. Models are far
/// smaller; a file made to take all memory is refused when it reaches this.
constexpr size_t kMaxUfbxBytes = size_t{1} << 31;

struct SceneDeleter {
    void operator()(ufbx_scene* scene) const { ufbx_free_scene(scene); }
};

std::string_view viewOf(const ufbx_string& text) {
    return {text.data, text.length};
}

/// What the import reads besides the file, and where.
struct OpenContext {
    std::filesystem::path baseDirectory;
    std::filesystem::path rootDirectory;
    /// The MTL files read, relative to the file's directory.
    std::vector<std::string> files;
};

/// Collects warnings, each only once however often its cause occurs.
class Warnings {
public:
    explicit Warnings(std::vector<std::string>& target) : m_target(target) {}

    void add(std::string message) {
        if (std::ranges::find(m_target, message) == m_target.end()) {
            m_target.push_back(std::move(message));
        }
    }

private:
    std::vector<std::string>& m_target;
};

/// `path` as a file names it, made absolute against its directory.
std::filesystem::path resolve(const OpenContext& context, std::string_view path) {
    const std::filesystem::path requested = pathFromUtf8(path);
    return (requested.is_absolute() ? requested : context.baseDirectory / requested).lexically_normal();
}

void addOnce(std::vector<std::string>& list, std::string entry) {
    if (std::ranges::find(list, entry) == list.end()) {
        list.push_back(std::move(entry));
    }
}

/// Opens what an OBJ file names - its MTL files - from inside the root directory, and
/// nothing else: geometry caches and the like are not read.
bool openReferencedFile(void* user, ufbx_stream* stream, const char* path, size_t pathLength,
                        const ufbx_open_file_info* info) {
    auto& context = *static_cast<OpenContext*>(user);
    if (info->type != UFBX_OPEN_FILE_OBJ_MTL) {
        return false;
    }
    const std::filesystem::path resolved = resolve(context, std::string_view(path, pathLength));
    if (!relativePathWithin(context.rootDirectory, resolved)) {
        return false;
    }
    const Result<std::vector<std::byte>> bytes = readBinaryFile(resolved);
    if (!bytes) {
        return false;
    }
    // ufbx keeps a copy of what it is given.
    const ufbx_open_memory_opts options{};
    if (!ufbx_open_memory(stream, bytes->data(), bytes->size(), &options, nullptr)) {
        return false;
    }
    addOnce(context.files, pathToUtf8(resolved.lexically_relative(context.baseDirectory)));
    return true;
}

ErrorCode errorCodeOf(const ufbx_error& error) {
    switch (error.type) {
        case UFBX_ERROR_OUT_OF_MEMORY:
        case UFBX_ERROR_MEMORY_LIMIT:
        case UFBX_ERROR_ALLOCATION_LIMIT:
            return ErrorCode::OutOfRange;
        case UFBX_ERROR_FILE_NOT_FOUND:
            return ErrorCode::NotFound;
        default:
            return ErrorCode::ParseError;
    }
}

float finite(ufbx_real value, float fallback = 0.0f) {
    const auto converted = static_cast<float>(value);
    return std::isfinite(converted) ? converted : fallback;
}

Vec3 finite(const ufbx_vec3& value) {
    return {finite(value.x), finite(value.y), finite(value.z)};
}

float unitInterval(ufbx_real value, float fallback) {
    return std::clamp(finite(value, fallback), 0.0f, 1.0f);
}

/// Turns the images of the scene's materials into the document's, each once, and says
/// which image each texture became.
class Images {
public:
    Images(GltfDocument& document, bool loadData, Warnings& warnings)
        : m_document(document), m_loadData(loadData), m_warnings(warnings) {}

    /// The image of `texture`, if it has one the engine can use.
    std::optional<uint32_t> of(const ufbx_texture* texture) {
        if (texture == nullptr) {
            return std::nullopt;
        }
        if (const auto found = m_images.find(texture); found != m_images.end()) {
            return found->second;
        }
        std::optional<uint32_t> image = add(*texture);
        m_images.emplace(texture, image);
        return image;
    }

private:
    std::optional<uint32_t> add(const ufbx_texture& texture) {
        if (texture.type != UFBX_TEXTURE_FILE) {
            m_warnings.add(fmt::format("texture '{}' is not an image file, and is not used", viewOf(texture.name)));
            return std::nullopt;
        }
        GltfImage image;
        image.name = std::string(viewOf(texture.name));
        if (texture.content.size > 0) {
            // Inside the file.
            if (m_loadData) {
                const auto* bytes = static_cast<const std::byte*>(texture.content.data);
                image.encoded.assign(bytes, bytes + texture.content.size);
            }
        } else {
            // As glTF files do, an image is named relative to the file, and is found as an
            // asset where that leads; it is not read here. An image named only by where it
            // was on the machine that made the file cannot be found anywhere else.
            // Files made on Windows separate directories with backslashes; and a drive is
            // a drive on every system, though only Windows reads it as one.
            std::string named(texture.relative_filename.length > 0 ? viewOf(texture.relative_filename)
                                                                   : viewOf(texture.filename));
            std::ranges::replace(named, '\\', '/');
            const std::filesystem::path relative = pathFromUtf8(named).lexically_normal();
            if (named.empty() || named.find(':') != std::string::npos || relative.is_absolute() ||
                relative.has_root_name() || relative.has_root_directory()) {
                m_warnings.add(fmt::format("texture '{}' names its image by where it was ({}), and is not used",
                                           viewOf(texture.name), named));
                return std::nullopt;
            }
            image.uri = pathToUtf8(relative);
            addOnce(m_document.externalFiles, image.uri);
        }
        m_document.images.push_back(std::move(image));
        return static_cast<uint32_t>(m_document.images.size() - 1);
    }

    GltfDocument& m_document;
    bool m_loadData;
    Warnings& m_warnings;
    std::unordered_map<const ufbx_texture*, std::optional<uint32_t>> m_images;
};

GltfMaterial convertMaterial(const ufbx_material& source, size_t index, Images& images, Warnings& warnings) {
    GltfMaterial result;
    result.name = source.name.length > 0 ? std::string(viewOf(source.name)) : fmt::format("Material {}", index);
    const ufbx_material_pbr_maps& pbr = source.pbr;
    MaterialData& material = result.material;

    const float factor = pbr.base_factor.has_value ? std::max(finite(pbr.base_factor.value_real, 1.0f), 0.0f) : 1.0f;
    if (pbr.base_color.has_value) {
        const ufbx_vec4& color = pbr.base_color.value_vec4;
        material.baseColor = Vec4(std::max(finite(color.x) * factor, 0.0f), std::max(finite(color.y) * factor, 0.0f),
                                  std::max(finite(color.z) * factor, 0.0f), 1.0f);
    }
    if (pbr.opacity.has_value) {
        material.baseColor.a = unitInterval(pbr.opacity.value_real, 1.0f);
        if (material.baseColor.a < 1.0f) {
            material.alphaMode = AlphaMode::Blended;
        }
    }
    if (pbr.metalness.has_value) {
        material.metallic = unitInterval(pbr.metalness.value_real, 0.0f);
    }
    if (pbr.roughness.has_value) {
        material.roughness = unitInterval(pbr.roughness.value_real, 0.5f);
    }
    if (pbr.emission_color.has_value) {
        const float strength =
            pbr.emission_factor.has_value ? std::max(finite(pbr.emission_factor.value_real, 1.0f), 0.0f) : 1.0f;
        const Vec3 color = finite(pbr.emission_color.value_vec3);
        material.emissiveColor = Vec3(std::max(color.x, 0.0f), std::max(color.y, 0.0f), std::max(color.z, 0.0f));
        material.emissiveIntensity = strength;
    }

    result.images.baseColor = images.of(pbr.base_color.texture);
    result.images.normal = images.of(pbr.normal_map.texture);
    result.images.emissive = images.of(pbr.emission_color.texture);
    result.images.occlusion = images.of(pbr.ambient_occlusion.texture);
    // glTF packs roughness and metalness into one image, green and blue; a file of its
    // own for either would be read in the wrong channel.
    if (pbr.roughness.texture != nullptr || pbr.metalness.texture != nullptr) {
        warnings.add(
            fmt::format("material '{}' has images of its roughness or metalness, which are not used", result.name));
    }
    return result;
}

/// The material slots of a mesh: its parts that have triangles, in order.
std::vector<const ufbx_mesh_part*> slotsOf(const ufbx_mesh& mesh) {
    std::vector<const ufbx_mesh_part*> slots;
    for (size_t index = 0; index < mesh.material_parts.count; ++index) {
        if (mesh.material_parts.data[index].num_triangles > 0) {
            slots.push_back(&mesh.material_parts.data[index]);
        }
    }
    return slots;
}

MeshVertex vertexAt(const ufbx_mesh& mesh, uint32_t corner) {
    MeshVertex vertex;
    vertex.position = finite(ufbx_get_vertex_vec3(&mesh.vertex_position, corner));
    if (mesh.vertex_normal.exists) {
        vertex.normal = finite(ufbx_get_vertex_vec3(&mesh.vertex_normal, corner));
    }
    // The origin of FBX and OBJ texture coordinates is at the bottom of an image, glTF's at
    // the top.
    if (mesh.vertex_uv.exists) {
        const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh.vertex_uv, corner);
        vertex.uv0 = Vec2(finite(uv.x), 1.0f - finite(uv.y));
    }
    if (mesh.uv_sets.count > 1 && mesh.uv_sets.data[1].vertex_uv.exists) {
        const ufbx_vec2 uv = ufbx_get_vertex_vec2(&mesh.uv_sets.data[1].vertex_uv, corner);
        vertex.uv1 = Vec2(finite(uv.x), 1.0f - finite(uv.y));
    }
    return vertex;
}

Result<GltfMesh> convertMesh(const ufbx_mesh& source, size_t index, const GltfImportOptions& options,
                             size_t& fileVertices, Warnings& warnings) {
    GltfMesh result;
    result.mesh.name = source.name.length > 0 ? std::string(viewOf(source.name)) : fmt::format("Mesh {}", index);
    const std::string& name = result.mesh.name;
    if (source.num_point_faces > 0 || source.num_line_faces > 0) {
        warnings.add(fmt::format("mesh '{}' has points or lines, which are not imported", name));
    }
    if (source.vertex_color.exists) {
        warnings.add(fmt::format("mesh '{}' has vertex colors, which are not imported", name));
    }
    if (source.skin_deformers.count > 0) {
        warnings.add(fmt::format("mesh '{}' is skinned, which is not imported", name));
    }
    if (source.blend_deformers.count > 0) {
        warnings.add(fmt::format("mesh '{}' has blend shapes, which are not imported", name));
    }

    const std::vector<const ufbx_mesh_part*> slots = slotsOf(source);
    for (const ufbx_mesh_part* slot : slots) {
        result.materials.push_back(slot->index < source.materials.count && source.materials.data[slot->index] != nullptr
                                       ? std::optional<uint32_t>(source.materials.data[slot->index]->typed_id)
                                       : std::nullopt);
    }
    if (!options.loadData || slots.empty()) {
        result.mesh.submeshes.resize(slots.size());
        if (slots.empty()) {
            warnings.add(fmt::format("mesh '{}' is empty", name));
        }
        return result;
    }

    // Every corner of every triangle is a vertex of its own until optimizeMesh() merges
    // those that are equal, and an index; the limit of vertices is the lower of the two.
    const size_t corners = source.num_triangles * 3;
    static_assert(kMaxMeshVertices <= kMaxMeshIndices);
    if (corners > kMaxMeshVertices) {
        return makeError(ErrorCode::OutOfRange, "mesh '{}' is too large: the limits are {} vertices and {} indices",
                         name, kMaxMeshVertices, kMaxMeshIndices);
    }
    // Neither sum overflows: `corners` is bounded above, and `fileVertices` by the limits.
    if (fileVertices + corners > size_t{options.maxVertices} || fileVertices + corners > size_t{options.maxIndices}) {
        return makeError(ErrorCode::OutOfRange,
                         "it holds more geometry than can be imported: the limits for a whole file are {} vertices "
                         "and {} indices",
                         options.maxVertices, options.maxIndices);
    }
    fileVertices += corners;

    std::vector<uint32_t> triangle(source.max_face_triangles * 3);
    MeshData& mesh = result.mesh;
    mesh.vertices.reserve(corners);
    mesh.indices.reserve(corners);
    for (const ufbx_mesh_part* slot : slots) {
        Submesh submesh;
        submesh.firstIndex = static_cast<uint32_t>(mesh.indices.size());
        for (size_t faceIndex = 0; faceIndex < slot->face_indices.count; ++faceIndex) {
            const ufbx_face face = source.faces.data[slot->face_indices.data[faceIndex]];
            if (face.num_indices < 3) {
                continue;
            }
            const uint32_t triangles = ufbx_triangulate_face(triangle.data(), triangle.size(), &source, face);
            for (uint32_t corner = 0; corner < triangles * 3; ++corner) {
                const MeshVertex vertex = vertexAt(source, triangle[corner]);
                submesh.bounds.expand(vertex.position);
                mesh.indices.push_back(static_cast<uint32_t>(mesh.vertices.size()));
                mesh.vertices.push_back(vertex);
            }
        }
        submesh.indexCount = static_cast<uint32_t>(mesh.indices.size()) - submesh.firstIndex;
        mesh.bounds.expand(submesh.bounds);
        mesh.submeshes.push_back(submesh);
    }
    repairTangentFrames(mesh);
    generateTangents(mesh);
    optimizeMesh(mesh);
    CORDITE_TRY(validateMesh(mesh));
    return result;
}

Transform transformOf(const ufbx_transform& source) {
    Transform transform;
    transform.position = finite(source.translation);
    const Quat rotation(finite(source.rotation.w, 1.0f), finite(source.rotation.x), finite(source.rotation.y),
                        finite(source.rotation.z));
    const float length = glm::length(rotation);
    transform.rotation = length > 0.0f ? rotation / length : kIdentityQuat;
    transform.scale = finite(source.scale);
    return transform;
}

/// The nodes of the scene, but for its root, whose children are the document's roots.
void convertNodes(const ufbx_scene& scene, GltfDocument& document) {
    std::unordered_map<const ufbx_node*, uint32_t> indices;
    for (size_t index = 0; index < scene.nodes.count; ++index) {
        const ufbx_node* node = scene.nodes.data[index];
        if (!node->is_root) {
            indices.emplace(node, static_cast<uint32_t>(indices.size()));
        }
    }
    document.nodes.resize(indices.size());
    for (const auto& [node, index] : indices) {
        GltfNode& converted = document.nodes[index];
        converted.name = node->name.length > 0 ? std::string(viewOf(node->name)) : fmt::format("Node {}", index);
        converted.transform = transformOf(node->local_transform);
        if (node->parent != nullptr && !node->parent->is_root) {
            converted.parent = indices.at(node->parent);
        }
        for (size_t child = 0; child < node->children.count; ++child) {
            converted.children.push_back(indices.at(node->children.data[child]));
        }
        if (node->mesh != nullptr) {
            converted.mesh = node->mesh->typed_id;
        }
    }
    if (scene.root_node != nullptr) {
        for (size_t child = 0; child < scene.root_node->children.count; ++child) {
            document.roots.push_back(indices.at(scene.root_node->children.data[child]));
        }
    }
}

Result<GltfDocument> convertScene(const ufbx_scene& scene, const GltfImportOptions& options,
                                  const OpenContext& context) {
    GltfDocument document;
    Warnings warnings(document.warnings);
    for (size_t index = 0; index < scene.metadata.warnings.count; ++index) {
        warnings.add(std::string(viewOf(scene.metadata.warnings.data[index].description)));
    }
    if (scene.anim_curves.count > 0) {
        warnings.add("it has animations, which are not imported");
    }
    if (scene.cameras.count > 0 || scene.lights.count > 0) {
        warnings.add("it has cameras or lights, which are not imported");
    }
    document.externalFiles = context.files;

    Images images(document, options.loadData, warnings);
    for (size_t index = 0; index < scene.materials.count; ++index) {
        document.materials.push_back(convertMaterial(*scene.materials.data[index], index, images, warnings));
    }
    size_t fileVertices = 0;
    for (size_t index = 0; index < scene.meshes.count; ++index) {
        CORDITE_TRY_ASSIGN(GltfMesh mesh,
                           convertMesh(*scene.meshes.data[index], index, options, fileVertices, warnings));
        document.meshes.push_back(std::move(mesh));
    }
    convertNodes(scene, document);
    return document;
}

} // namespace

Result<GltfDocument> importFbx(const std::filesystem::path& path, const GltfImportOptions& options) {
    const std::string fileName = pathToUtf8(path);
    const auto fail = [&fileName](const Error& error) {
        return tl::unexpected(error.withContext(fmt::format("cannot import {}", fileName)));
    };
    const std::string extension = toLowerAscii(pathToUtf8(path.extension()));
    if (extension != ".fbx" && extension != ".obj") {
        return fail(Error{ErrorCode::InvalidArgument, "it is not an FBX or OBJ file (.fbx or .obj)"});
    }
    // Read here rather than by ufbx, which would open it with the wrong idea of what a path
    // is on Windows.
    Result<std::vector<std::byte>> bytes = readBinaryFile(path);
    if (!bytes) {
        return fail(bytes.error());
    }

    OpenContext context;
    context.baseDirectory = std::filesystem::absolute(path).lexically_normal().parent_path();
    context.rootDirectory = options.rootDirectory.empty()
                                ? context.baseDirectory
                                : std::filesystem::absolute(options.rootDirectory).lexically_normal();
    const std::string absolute = pathToUtf8(std::filesystem::absolute(path).lexically_normal());

    ufbx_load_opts load{};
    load.temp_allocator.memory_limit = kMaxUfbxBytes;
    load.result_allocator.memory_limit = kMaxUfbxBytes;
    load.ignore_embedded = !options.loadData;
    load.ignore_animation = true;
    load.load_external_files = true;
    load.ignore_missing_external_files = true;
    load.generate_missing_normals = true;
    load.normalize_normals = true;
    load.target_axes = ufbx_axes_right_handed_y_up;
    load.target_unit_meters = 1.0f;
    load.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    load.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
    load.file_format = extension == ".fbx" ? UFBX_FILE_FORMAT_FBX : UFBX_FILE_FORMAT_OBJ;
    load.no_format_from_content = true;
    load.no_format_from_extension = true;
    load.obj_search_mtl_by_filename = true;
    load.filename = ufbx_string{absolute.data(), absolute.size()};
    load.open_file_cb.fn = &openReferencedFile;
    load.open_file_cb.user = &context;

    ufbx_error error{};
    const std::unique_ptr<ufbx_scene, SceneDeleter> scene(
        ufbx_load_memory(bytes->data(), bytes->size(), &load, &error));
    if (scene == nullptr) {
        std::string description(viewOf(error.description));
        if (description.empty()) {
            description = "it is not a model ufbx can read";
        }
        return fail(Error{errorCodeOf(error), std::move(description)});
    }
    Result<GltfDocument> document = convertScene(*scene, options, context);
    if (!document) {
        return fail(document.error());
    }
    return document;
}

Result<GltfDocument> importModelFile(const std::filesystem::path& path, const GltfImportOptions& options) {
    const std::string extension = toLowerAscii(pathToUtf8(path.extension()));
    if (extension == ".gltf" || extension == ".glb") {
        return importGltf(path, options);
    }
    if (extension == ".fbx" || extension == ".obj") {
        return importFbx(path, options);
    }
    return makeError(ErrorCode::InvalidArgument, "{} is not a model file (.gltf, .glb, .fbx or .obj)",
                     pathToUtf8(path));
}

} // namespace cordite
