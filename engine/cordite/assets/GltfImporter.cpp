#include "cordite/assets/GltfImporter.h"

#include "cordite/assets/MeshProcessing.h"
#include "cordite/core/FileSystem.h"

// cgltf is compiled here.
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string_view>
#include <utility>

namespace cordite {
namespace {

/// Extensions a file may list as required and still be imported faithfully. A file that
/// requires anything else would be shown wrongly, or not at all.
constexpr std::array<std::string_view, 7> kSupportedExtensions = {
    "KHR_materials_emissive_strength", "KHR_materials_ior",   "KHR_materials_specular", "KHR_texture_transform",
    "KHR_mesh_quantization",           "KHR_materials_unlit", "KHR_lights_punctual",
};

/// The stride of an interleaved vertex buffer is at most 252 bytes in glTF; anything
/// far beyond is a corrupt file, and bounding it keeps the size arithmetic from
/// overflowing.
constexpr cgltf_size kMaxAccessorStride = 4096;

struct CgltfDeleter {
    void operator()(cgltf_data* data) const { cgltf_free(data); }
};

/// What the file callbacks below need: where relative paths start, and where they
/// have to stay.
struct FileContext {
    std::filesystem::path baseDirectory;
    std::filesystem::path rootDirectory;
    /// Why the last read failed, in words better than a cgltf result code.
    std::string error;
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

/// Counts what an import takes in against what the whole file may hold. Meshes and
/// images can share their bytes in a file but not in memory, so every use counts.
class ImportBudget {
public:
    explicit ImportBudget(const GltfImportOptions& options) : m_options(options) {}

    /// Counts the geometry of one part of a mesh in. Fails, counting nothing, when the
    /// file would then hold more than it may.
    [[nodiscard]] Status addGeometry(cgltf_size vertexCount, cgltf_size indexCount) {
        if (vertexCount > m_options.maxVertices - m_vertices || indexCount > m_options.maxIndices - m_indices) {
            return makeError(ErrorCode::OutOfRange,
                             "it holds more geometry than can be imported: the limits for a whole file are {} "
                             "vertices and {} indices",
                             m_options.maxVertices, m_options.maxIndices);
        }
        m_vertices += vertexCount;
        m_indices += indexCount;
        return {};
    }

    /// Counts the keys of one curve of an animation in, likewise.
    [[nodiscard]] Status addKeys(cgltf_size keyCount) {
        if (keyCount > m_options.maxAnimationKeys - m_keys) {
            return makeError(ErrorCode::OutOfRange,
                             "its animations have more keys than can be imported: the limit for a whole file "
                             "is {}",
                             m_options.maxAnimationKeys);
        }
        m_keys += keyCount;
        return {};
    }

    /// Counts the bytes of an embedded image in, likewise.
    [[nodiscard]] Status addImage(cgltf_size byteCount) {
        if (byteCount > m_options.maxEmbeddedImageBytes - m_imageBytes) {
            return makeError(ErrorCode::OutOfRange,
                             "its embedded images take more than the {} bytes a whole file may hold",
                             m_options.maxEmbeddedImageBytes);
        }
        m_imageBytes += byteCount;
        return {};
    }

private:
    const GltfImportOptions& m_options;
    // Never more than the limits, so the subtractions above cannot wrap around.
    cgltf_size m_vertices = 0;
    cgltf_size m_indices = 0;
    cgltf_size m_imageBytes = 0;
    cgltf_size m_keys = 0;
};

/// cgltf asks for the files a glTF refers to through this. `path` is the URI from the
/// file with its escapes resolved; `size` holds the number of bytes the glTF says the
/// file has, or zero for "all of it".
cgltf_result readReferencedFile(const cgltf_memory_options* /*memory*/, const cgltf_file_options* fileOptions,
                                const char* path, cgltf_size* size, void** data) {
    auto& context = *static_cast<FileContext*>(fileOptions->user_data);
    const std::string_view uri(path);

    const std::filesystem::path resolved = context.baseDirectory / pathFromUtf8(uri);
    if (const Result<std::string> inside = relativePathWithin(context.rootDirectory, resolved); !inside) {
        context.error = fmt::format("it refers to '{}', which is outside {}", uri, pathToUtf8(context.rootDirectory));
        return cgltf_result_file_not_found;
    }

    Result<std::vector<std::byte>> bytes = readBinaryFile(resolved);
    if (!bytes) {
        context.error = fmt::format("cannot read '{}', which it refers to: {}", uri, bytes.error().message);
        return cgltf_result_file_not_found;
    }
    const cgltf_size wanted = *size != 0 ? *size : bytes->size();
    if (bytes->size() < wanted) {
        context.error = fmt::format("'{}' has {} bytes, but the file expects {}", uri, bytes->size(), wanted);
        return cgltf_result_data_too_short;
    }

    // Released by releaseReferencedFile(). At least one byte, so that an empty file is
    // not mistaken for a failed allocation.
    void* copy = std::malloc(std::max<cgltf_size>(wanted, 1));
    if (copy == nullptr) {
        context.error = fmt::format("out of memory reading '{}'", uri);
        return cgltf_result_out_of_memory;
    }
    std::memcpy(copy, bytes->data(), wanted);
    *size = wanted;
    *data = copy;
    return cgltf_result_success;
}

void releaseReferencedFile(const cgltf_memory_options* /*memory*/, const cgltf_file_options* /*fileOptions*/,
                           void* data) {
    std::free(data);
}

std::string_view describe(cgltf_result result) {
    switch (result) {
        case cgltf_result_data_too_short:
            return "it is truncated, or refers to data that is not there (beyond the end of a buffer, or a vertex "
                   "beyond the last)";
        case cgltf_result_unknown_format:
            return "it is neither a .gltf nor a .glb file";
        case cgltf_result_invalid_json:
            return "its JSON is malformed";
        case cgltf_result_invalid_gltf:
            return "it does not follow the glTF 2.0 specification";
        case cgltf_result_file_not_found:
            return "a file it refers to cannot be found";
        case cgltf_result_io_error:
            return "a buffer it refers to cannot be read or decoded";
        case cgltf_result_out_of_memory:
            return "out of memory";
        case cgltf_result_legacy_gltf:
            return "it is a glTF 1.0 file; only glTF 2.0 is supported";
        default:
            return "it cannot be read";
    }
}

std::string nameOr(const char* name, std::string_view kind, size_t index) {
    return name != nullptr && name[0] != '\0' ? std::string(name) : fmt::format("{} {}", kind, index);
}

/// True when `byteCount` bytes starting `offset` bytes into the view lie inside the view,
/// and the view inside its buffer. Written so that no sum can overflow.
bool viewFits(const cgltf_buffer_view* view, cgltf_size offset, cgltf_size byteCount) {
    if (view == nullptr || view->buffer == nullptr) {
        return false;
    }
    const cgltf_size bufferSize = view->buffer->size;
    if (view->offset > bufferSize || view->size > bufferSize - view->offset) {
        return false;
    }
    return offset <= view->size && byteCount <= view->size - offset;
}

/// True when the bytes of a view are in memory. A view that an extension decodes into
/// memory of its own would have `data` set; none of those extensions is supported, and
/// their placeholder buffers hold nothing.
bool viewIsLoaded(const cgltf_buffer_view* view) {
    return view != nullptr && view->buffer != nullptr && view->data == nullptr && view->buffer->data != nullptr;
}

/// Checks that the numbers of every accessor describe data that exists. cgltf_validate()
/// checks the same relations, but with sums and products that a crafted file can make
/// overflow - after which it would read far outside the buffers. This runs first, bounds
/// every count and stride, and so makes cgltf's arithmetic safe as well.
Status checkAccessorLayouts(const cgltf_data& data) {
    for (cgltf_size index = 0; index < data.accessors_count; ++index) {
        const cgltf_accessor& accessor = data.accessors[index];
        const cgltf_size elementSize = cgltf_calc_size(accessor.type, accessor.component_type);
        if (accessor.count > kMaxMeshIndices) {
            return makeError(ErrorCode::OutOfRange, "accessor {} has {} elements; the limit is {}", index,
                             accessor.count, kMaxMeshIndices);
        }
        if (elementSize == 0 || accessor.stride > kMaxAccessorStride) {
            return makeError(ErrorCode::ParseError, "accessor {} has an invalid layout", index);
        }
        if (accessor.count == 0) {
            continue;
        }

        if (accessor.buffer_view != nullptr &&
            !viewFits(accessor.buffer_view, accessor.offset, accessor.stride * (accessor.count - 1) + elementSize)) {
            return makeError(ErrorCode::ParseError, "accessor {} refers to data beyond the end of its buffer", index);
        }
        if (accessor.is_sparse != 0) {
            const cgltf_accessor_sparse& sparse = accessor.sparse;
            const cgltf_size indexSize = cgltf_component_size(sparse.indices_component_type);
            if (sparse.count > accessor.count || indexSize == 0 ||
                !viewFits(sparse.indices_buffer_view, sparse.indices_byte_offset, indexSize * sparse.count) ||
                !viewFits(sparse.values_buffer_view, sparse.values_byte_offset, elementSize * sparse.count)) {
                return makeError(ErrorCode::ParseError, "accessor {} has sparse data beyond the end of its buffer",
                                 index);
            }
        }
    }
    return {};
}

/// Checks that an accessor is small enough for what it is used for and that its bytes
/// are in memory. Its layout has been checked by checkAccessorLayouts().
Status checkAccessor(const cgltf_accessor& accessor, cgltf_size maximumCount, std::string_view what) {
    if (accessor.count > maximumCount) {
        return makeError(ErrorCode::OutOfRange, "{} has {} elements; the limit is {}", what, accessor.count,
                         maximumCount);
    }
    const bool viewMissing = accessor.buffer_view != nullptr && !viewIsLoaded(accessor.buffer_view);
    const bool sparseMissing = accessor.is_sparse != 0 && (!viewIsLoaded(accessor.sparse.indices_buffer_view) ||
                                                           !viewIsLoaded(accessor.sparse.values_buffer_view));
    if (accessor.count > 0 && (viewMissing || sparseMissing)) {
        return makeError(ErrorCode::ParseError, "{} refers to data that is not in the file", what);
    }
    return {};
}

/// Reads a vertex attribute as floats, `componentCount` per vertex. Integer data is
/// converted, normalized or not, as the accessor says.
Result<std::vector<float>> readAttribute(const cgltf_accessor& accessor, cgltf_type expectedType,
                                         cgltf_size componentCount, std::string_view what) {
    if (accessor.type != expectedType) {
        return makeError(ErrorCode::ParseError, "{} has the wrong number of components", what);
    }
    CORDITE_TRY(checkAccessor(accessor, kMaxMeshVertices, what));

    std::vector<float> values(accessor.count * componentCount);
    if (!values.empty() && cgltf_accessor_unpack_floats(&accessor, values.data(), values.size()) != values.size()) {
        return makeError(ErrorCode::ParseError, "{} cannot be read", what);
    }
    return values;
}

/// The triangle list of a primitive: its indices, or 0, 1, 2, ... without any, with
/// strips and fans turned into separate triangles.
Result<std::vector<uint32_t>> readTriangles(const cgltf_primitive& primitive, size_t vertexCount,
                                            std::string_view what) {
    std::vector<uint32_t> sequence;
    if (primitive.indices != nullptr) {
        const cgltf_accessor& accessor = *primitive.indices;
        const std::string name = fmt::format("the index list of {}", what);
        if (accessor.is_sparse != 0 || accessor.type != cgltf_type_scalar) {
            return makeError(ErrorCode::ParseError, "{} has an invalid layout", name);
        }
        CORDITE_TRY(checkAccessor(accessor, kMaxMeshIndices, name));
        sequence.resize(accessor.count);
        if (!sequence.empty() && cgltf_accessor_unpack_indices(&accessor, sequence.data(), sizeof(uint32_t),
                                                               sequence.size()) != sequence.size()) {
            return makeError(ErrorCode::ParseError, "{} cannot be read", name);
        }
        for (const uint32_t index : sequence) {
            if (index >= vertexCount) {
                return makeError(ErrorCode::ParseError, "{} refers to vertex {}, but there are only {}", name, index,
                                 vertexCount);
            }
        }
    } else {
        sequence.resize(vertexCount);
        for (size_t index = 0; index < vertexCount; ++index) {
            sequence[index] = static_cast<uint32_t>(index);
        }
    }

    if (primitive.type == cgltf_primitive_type_triangles) {
        sequence.resize(sequence.size() - sequence.size() % 3);
        return sequence;
    }

    // Strips and fans describe one triangle per element after the first two.
    std::vector<uint32_t> triangles;
    if (sequence.size() < 3) {
        return triangles;
    }
    const size_t triangleCount = sequence.size() - 2;
    if (triangleCount > kMaxMeshIndices / 3) {
        return makeError(ErrorCode::OutOfRange, "{} has {} triangles; the limit is {}", what, triangleCount,
                         kMaxMeshIndices / 3);
    }
    triangles.reserve(triangleCount * 3);
    for (size_t index = 0; index < triangleCount; ++index) {
        if (primitive.type == cgltf_primitive_type_triangle_strip) {
            // Every other triangle of a strip is wound the other way round.
            const size_t odd = index % 2;
            triangles.push_back(sequence[index]);
            triangles.push_back(sequence[index + 1 + odd]);
            triangles.push_back(sequence[index + 2 - odd]);
        } else {
            triangles.push_back(sequence[index + 1]);
            triangles.push_back(sequence[index + 2]);
            triangles.push_back(sequence[0]);
        }
    }
    return triangles;
}

bool isTriangleType(cgltf_primitive_type type) {
    return type == cgltf_primitive_type_triangles || type == cgltf_primitive_type_triangle_strip ||
           type == cgltf_primitive_type_triangle_fan;
}

/// Whether a primitive ends up in the mesh. Decided from the structure alone, so that
/// a mesh has the same submeshes whether or not its data is loaded.
bool isImportable(const cgltf_primitive& primitive) {
    return isTriangleType(primitive.type) && primitive.has_draco_mesh_compression == 0 &&
           cgltf_find_accessor(&primitive, cgltf_attribute_type_position, 0) != nullptr;
}

/// The triangles of one glTF primitive, complete with normals and tangents.
/// How the vertices of a part of a mesh follow their joints: JOINTS_0 and WEIGHTS_0.
/// Empty for a part that has neither, or only one of the two, which says nothing.
/// The weights are as the file has them; convertMesh() brings them to one in all.
Result<std::vector<MeshSkinVertex>> readSkin(const cgltf_primitive& primitive, size_t vertexCount,
                                             std::string_view what, Warnings& warnings) {
    const cgltf_accessor* joints = cgltf_find_accessor(&primitive, cgltf_attribute_type_joints, 0);
    const cgltf_accessor* weights = cgltf_find_accessor(&primitive, cgltf_attribute_type_weights, 0);
    if (joints == nullptr && weights == nullptr) {
        return std::vector<MeshSkinVertex>();
    }
    if (joints == nullptr || weights == nullptr) {
        warnings.add(fmt::format("{} has joints without weights, or weights without joints; it is not skinned", what));
        return std::vector<MeshSkinVertex>();
    }
    if (cgltf_find_accessor(&primitive, cgltf_attribute_type_joints, 1) != nullptr) {
        warnings.add(fmt::format("{} has vertices that follow more than four joints; the first four are kept", what));
    }

    const std::string jointsName = fmt::format("JOINTS_0 of {}", what);
    const bool isIndexType =
        joints->component_type == cgltf_component_type_r_8u || joints->component_type == cgltf_component_type_r_16u;
    if (joints->type != cgltf_type_vec4 || !isIndexType) {
        return makeError(ErrorCode::ParseError, "{} is not four unsigned bytes or shorts a vertex", jointsName);
    }
    CORDITE_TRY(checkAccessor(*joints, kMaxMeshVertices, jointsName));
    CORDITE_TRY_ASSIGN(const std::vector<float> weightValues,
                       readAttribute(*weights, cgltf_type_vec4, 4, fmt::format("WEIGHTS_0 of {}", what)));
    if (joints->count != vertexCount || weightValues.size() != vertexCount * 4) {
        return makeError(ErrorCode::ParseError, "the joints and weights of {} are not one for each of its {} positions",
                         what, vertexCount);
    }

    std::vector<MeshSkinVertex> skin(vertexCount);
    for (size_t index = 0; index < vertexCount; ++index) {
        std::array<cgltf_uint, 4> indices{};
        if (cgltf_accessor_read_uint(joints, index, indices.data(), indices.size()) == 0) {
            return makeError(ErrorCode::ParseError, "{} cannot be read", jointsName);
        }
        for (size_t slot = 0; slot < indices.size(); ++slot) {
            if (indices[slot] >= kMaxSkeletonJoints) {
                return makeError(ErrorCode::OutOfRange, "{} names joint {}; a skeleton has {} at the most", jointsName,
                                 indices[slot], kMaxSkeletonJoints);
            }
            skin[index].joints[slot] = static_cast<uint16_t>(indices[slot]);
            skin[index].weights[slot] = weightValues[index * 4 + slot];
        }
    }
    return skin;
}

Result<MeshData> readPrimitive(const cgltf_primitive& primitive, std::string_view what, ImportBudget& budget,
                               Warnings& warnings) {
    const cgltf_accessor* positions = cgltf_find_accessor(&primitive, cgltf_attribute_type_position, 0);
    const cgltf_accessor* normals = cgltf_find_accessor(&primitive, cgltf_attribute_type_normal, 0);
    const cgltf_accessor* tangents = cgltf_find_accessor(&primitive, cgltf_attribute_type_tangent, 0);
    const cgltf_accessor* uv0 = cgltf_find_accessor(&primitive, cgltf_attribute_type_texcoord, 0);
    const cgltf_accessor* uv1 = cgltf_find_accessor(&primitive, cgltf_attribute_type_texcoord, 1);

    // Counted against what the file may hold before anything is read, in the numbers the
    // file states: its positions, and the triangle corners its indices (or, without
    // indices, its vertices) make. The part's own limit is checked first, for the sake
    // of a message that names the part.
    const std::string positionName = fmt::format("POSITION of {}", what);
    CORDITE_TRY(checkAccessor(*positions, kMaxMeshVertices, positionName));
    const cgltf_size elementCount = primitive.indices != nullptr ? primitive.indices->count : positions->count;
    const cgltf_size cornerCount = primitive.type == cgltf_primitive_type_triangles
                                       ? elementCount
                                       : 3 * (std::max<cgltf_size>(elementCount, 2) - 2);
    CORDITE_TRY(budget.addGeometry(positions->count, cornerCount));

    CORDITE_TRY_ASSIGN(const std::vector<float> positionValues,
                       readAttribute(*positions, cgltf_type_vec3, 3, positionName));
    const size_t vertexCount = positionValues.size() / 3;

    // Every attribute of a primitive describes the same vertices.
    const auto readOptional = [&](const cgltf_accessor* accessor, cgltf_type type, cgltf_size componentCount,
                                  std::string_view attributeName) -> Result<std::vector<float>> {
        if (accessor == nullptr) {
            return std::vector<float>();
        }
        CORDITE_TRY_ASSIGN(std::vector<float> values, readAttribute(*accessor, type, componentCount,
                                                                    fmt::format("{} of {}", attributeName, what)));
        if (values.size() != vertexCount * componentCount) {
            return makeError(ErrorCode::ParseError, "{} of {} has {} elements, but there are {} positions",
                             attributeName, what, values.size() / componentCount, vertexCount);
        }
        return values;
    };
    CORDITE_TRY_ASSIGN(const std::vector<float> normalValues, readOptional(normals, cgltf_type_vec3, 3, "NORMAL"));
    CORDITE_TRY_ASSIGN(const std::vector<float> tangentValues, readOptional(tangents, cgltf_type_vec4, 4, "TANGENT"));
    CORDITE_TRY_ASSIGN(const std::vector<float> uv0Values, readOptional(uv0, cgltf_type_vec2, 2, "TEXCOORD_0"));
    CORDITE_TRY_ASSIGN(const std::vector<float> uv1Values, readOptional(uv1, cgltf_type_vec2, 2, "TEXCOORD_1"));

    std::vector<MeshVertex> vertices(vertexCount);
    for (size_t index = 0; index < vertexCount; ++index) {
        MeshVertex& vertex = vertices[index];
        vertex.position = Vec3(positionValues[index * 3], positionValues[index * 3 + 1], positionValues[index * 3 + 2]);
        if (!normalValues.empty()) {
            vertex.normal = Vec3(normalValues[index * 3], normalValues[index * 3 + 1], normalValues[index * 3 + 2]);
        }
        if (!tangentValues.empty()) {
            vertex.tangent = Vec4(tangentValues[index * 4], tangentValues[index * 4 + 1], tangentValues[index * 4 + 2],
                                  tangentValues[index * 4 + 3]);
        }
        if (!uv0Values.empty()) {
            vertex.uv0 = Vec2(uv0Values[index * 2], uv0Values[index * 2 + 1]);
        }
        if (!uv1Values.empty()) {
            vertex.uv1 = Vec2(uv1Values[index * 2], uv1Values[index * 2 + 1]);
        }
    }

    CORDITE_TRY_ASSIGN(const std::vector<uint32_t> triangles, readTriangles(primitive, vertexCount, what));
    CORDITE_TRY_ASSIGN(const std::vector<MeshSkinVertex> skin, readSkin(primitive, vertexCount, what, warnings));

    MeshData mesh;
    appendSubmesh(mesh, vertices, skin, triangles);
    // The specification: without normals the surface is flat-shaded and any tangents
    // in the file are to be ignored; without tangents they are computed with MikkTSpace.
    if (normalValues.empty()) {
        generateFlatNormals(mesh);
    }
    repairTangentFrames(mesh);
    if (normalValues.empty() || tangentValues.empty()) {
        generateTangents(mesh);
    }
    return mesh;
}

/// The material slots of a glTF mesh: its distinct materials in the order in which its
/// importable primitives first use them.
std::vector<const cgltf_material*> materialSlots(const cgltf_mesh& mesh) {
    std::vector<const cgltf_material*> slots;
    for (cgltf_size index = 0; index < mesh.primitives_count; ++index) {
        const cgltf_primitive& primitive = mesh.primitives[index];
        if (isImportable(primitive) && std::ranges::find(slots, primitive.material) == slots.end()) {
            slots.push_back(primitive.material);
        }
    }
    return slots;
}

Result<GltfMesh> convertMesh(const cgltf_data& data, const cgltf_mesh& source, size_t meshIndex, bool loadData,
                             Warnings& warnings, ImportBudget& budget) {
    GltfMesh result;
    result.mesh.name = nameOr(source.name, "Mesh", meshIndex);
    const std::string& name = result.mesh.name;

    for (cgltf_size index = 0; index < source.primitives_count; ++index) {
        const cgltf_primitive& primitive = source.primitives[index];
        if (!isTriangleType(primitive.type)) {
            warnings.add(fmt::format("mesh '{}' contains points or lines, which are not imported", name));
        } else if (primitive.has_draco_mesh_compression != 0) {
            warnings.add(fmt::format("mesh '{}' is Draco-compressed, which is not supported", name));
        } else if (!isImportable(primitive)) {
            warnings.add(fmt::format("mesh '{}' has a part without positions, which is skipped", name));
        }
        if (primitive.targets_count > 0) {
            warnings.add(fmt::format("mesh '{}' has morph targets, which are not imported", name));
        }
        if (cgltf_find_accessor(&primitive, cgltf_attribute_type_color, 0) != nullptr) {
            warnings.add(fmt::format("mesh '{}' has vertex colors, which are not imported", name));
        }
    }

    const std::vector<const cgltf_material*> slots = materialSlots(source);
    for (const cgltf_material* material : slots) {
        result.materials.push_back(
            material != nullptr ? std::optional<uint32_t>(static_cast<uint32_t>(cgltf_material_index(&data, material)))
                                : std::nullopt);
    }
    if (!loadData || slots.empty()) {
        result.mesh.submeshes.resize(slots.size());
        return result;
    }

    // A mesh is skinned as a whole or not at all. Where only some of its parts say how
    // they follow joints, the others follow the first joint.
    const auto hasSkin = [](const cgltf_primitive& primitive) {
        return isImportable(primitive) && cgltf_find_accessor(&primitive, cgltf_attribute_type_joints, 0) != nullptr &&
               cgltf_find_accessor(&primitive, cgltf_attribute_type_weights, 0) != nullptr;
    };
    const bool isSkinned = std::any_of(source.primitives, source.primitives + source.primitives_count, hasSkin);

    // One submesh per slot, made of every primitive that uses the slot's material.
    for (const cgltf_material* slot : slots) {
        std::vector<MeshVertex> vertices;
        std::vector<MeshSkinVertex> skin;
        std::vector<uint32_t> indices;
        for (cgltf_size index = 0; index < source.primitives_count; ++index) {
            const cgltf_primitive& primitive = source.primitives[index];
            if (!isImportable(primitive) || primitive.material != slot) {
                continue;
            }
            CORDITE_TRY_ASSIGN(
                const MeshData part,
                readPrimitive(primitive, fmt::format("part {} of mesh '{}'", index, name), budget, warnings));
            if (vertices.size() + part.vertices.size() > kMaxMeshVertices ||
                indices.size() + part.indices.size() > kMaxMeshIndices) {
                return makeError(ErrorCode::OutOfRange,
                                 "mesh '{}' is too large: the limits are {} vertices and {} indices", name,
                                 kMaxMeshVertices, kMaxMeshIndices);
            }
            const auto firstVertex = static_cast<uint32_t>(vertices.size());
            vertices.insert(vertices.end(), part.vertices.begin(), part.vertices.end());
            if (isSkinned) {
                if (!part.isSkinned()) {
                    warnings.add(fmt::format("mesh '{}' has parts that follow no joints; they follow the first", name));
                }
                skin.insert(skin.end(), part.skin.begin(), part.skin.end());
                skin.resize(vertices.size());
            }
            for (const uint32_t index32 : part.indices) {
                indices.push_back(firstVertex + index32);
            }
        }
        appendSubmesh(result.mesh, vertices, skin, indices);
    }

    if (result.mesh.vertices.empty()) {
        // Parts that are there but hold no vertices: nothing to draw, nothing to keep.
        warnings.add(fmt::format("mesh '{}' is empty", name));
        result.mesh.submeshes.clear();
        result.mesh.indices.clear();
        result.materials.clear();
        return result;
    }
    if (result.mesh.vertices.size() > kMaxMeshVertices || result.mesh.indices.size() > kMaxMeshIndices) {
        return makeError(ErrorCode::OutOfRange, "mesh '{}' is too large: the limits are {} vertices and {} indices",
                         name, kMaxMeshVertices, kMaxMeshIndices);
    }

    if (normalizeSkinWeights(result.mesh) > 0) {
        warnings.add(fmt::format("mesh '{}' has vertices without weights; they follow their first joint", name));
    }
    optimizeMesh(result.mesh);
    CORDITE_TRY(validateMesh(result.mesh));
    return result;
}

TextureWrap wrapFrom(cgltf_wrap_mode mode) {
    switch (mode) {
        case cgltf_wrap_mode_clamp_to_edge:
            return TextureWrap::Clamp;
        case cgltf_wrap_mode_mirrored_repeat:
            return TextureWrap::MirroredRepeat;
        default:
            return TextureWrap::Repeat;
    }
}

/// How one texture of a glTF material is sampled, in the terms of MaterialData.
struct TextureUse {
    int32_t uvChannel = 0;
    Vec2 uvScale{1.0f};
    Vec2 uvOffset{0.0f};
    bool rotated = false;
    TextureWrap wrapU = TextureWrap::Repeat;
    TextureWrap wrapV = TextureWrap::Repeat;
    TextureFilter filter = TextureFilter::Linear;

    [[nodiscard]] bool sameSampler(const TextureUse& other) const {
        return wrapU == other.wrapU && wrapV == other.wrapV && filter == other.filter;
    }

    [[nodiscard]] bool sameCoordinates(const TextureUse& other) const {
        return uvChannel == other.uvChannel && uvScale == other.uvScale && uvOffset == other.uvOffset;
    }
};

/// A number of a file that may be anything: "1e999" is valid JSON and reads as infinity.
float finiteOr(float value, float fallback) {
    return std::isfinite(value) ? value : fallback;
}

float unitRange(float value, float fallback) {
    return std::clamp(finiteOr(value, fallback), 0.0f, 1.0f);
}

TextureUse describeUse(const cgltf_texture_view& view) {
    TextureUse use;
    use.uvChannel = view.texcoord;
    if (view.has_transform != 0) {
        const cgltf_texture_transform& transform = view.transform;
        use.uvScale = Vec2(finiteOr(transform.scale[0], 1.0f), finiteOr(transform.scale[1], 1.0f));
        use.uvOffset = Vec2(finiteOr(transform.offset[0], 0.0f), finiteOr(transform.offset[1], 0.0f));
        use.rotated = transform.rotation != 0.0f;
        if (transform.has_texcoord != 0) {
            use.uvChannel = transform.texcoord;
        }
    }
    if (const cgltf_sampler* sampler = view.texture->sampler; sampler != nullptr) {
        use.wrapU = wrapFrom(sampler->wrap_s);
        use.wrapV = wrapFrom(sampler->wrap_t);
        use.filter = sampler->mag_filter == cgltf_filter_type_nearest ? TextureFilter::Nearest : TextureFilter::Linear;
    }
    return use;
}

/// Fills in which images a material uses and how it samples them. The engine samples
/// all of a material's textures with one sampler and one layout (occlusion may use the
/// other set of coordinates); the first texture decides, and what the others do
/// differently is reported.
void convertMaterialTextures(const cgltf_data& data, const cgltf_material& source,
                             const cgltf_texture_view* baseColorView, const cgltf_texture_view* metallicRoughnessView,
                             GltfMaterial& result, Warnings& warnings) {
    const std::string& name = result.name;
    MaterialData& material = result.material;

    struct Slot {
        const cgltf_texture_view* view;
        std::optional<uint32_t>* image;
        bool isOcclusion;
    };

    const std::array<Slot, 5> slots = {{
        {baseColorView, &result.images.baseColor, false},
        {metallicRoughnessView, &result.images.metallicRoughness, false},
        {&source.normal_texture, &result.images.normal, false},
        {&source.emissive_texture, &result.images.emissive, false},
        {&source.occlusion_texture, &result.images.occlusion, true},
    }};

    std::optional<TextureUse> mainUse;
    std::optional<TextureUse> samplerUse;
    for (const Slot& slot : slots) {
        if (slot.view == nullptr || slot.view->texture == nullptr) {
            continue;
        }
        if (slot.view->texture->image == nullptr) {
            warnings.add(fmt::format("material '{}' uses a texture in a format that is not supported", name));
            continue;
        }
        *slot.image = static_cast<uint32_t>(cgltf_image_index(&data, slot.view->texture->image));

        TextureUse use = describeUse(*slot.view);
        if (use.uvChannel < 0 || use.uvChannel > 1) {
            warnings.add(fmt::format("material '{}' uses texture coordinate set {}; only sets 0 and 1 are imported",
                                     name, use.uvChannel));
            use.uvChannel = 0;
        }
        if (use.rotated) {
            warnings.add(fmt::format("material '{}' rotates a texture, which is not supported", name));
        }

        if (!samplerUse) {
            samplerUse = use;
        } else if (!samplerUse->sameSampler(use)) {
            warnings.add(fmt::format(
                "material '{}' wraps or filters its textures differently; the first one's way is used", name));
        }

        // The occlusion texture comes last, when it is known how the others are laid
        // out. In their set of coordinates it shares their layout; in a set of its own
        // it is used as it is.
        if (slot.isOcclusion) {
            material.occlusionUvChannel = use.uvChannel;
        }
        if (!mainUse) {
            mainUse = use;
        } else if (slot.isOcclusion && use.uvChannel != mainUse->uvChannel) {
            if (use.uvScale != Vec2(1.0f) || use.uvOffset != Vec2(0.0f)) {
                warnings.add(fmt::format("material '{}' scales or shifts an occlusion texture that has texture "
                                         "coordinates of its own, which is not supported",
                                         name));
            }
        } else if (!mainUse->sameCoordinates(use)) {
            warnings.add(fmt::format(
                "material '{}' lays its textures out differently from each other; the first one's layout is used",
                name));
        }
    }
    if (mainUse) {
        material.uvChannel = mainUse->uvChannel;
        material.uvScale = mainUse->uvScale;
        material.uvOffset = mainUse->uvOffset;
    }
    if (samplerUse) {
        material.wrapU = samplerUse->wrapU;
        material.wrapV = samplerUse->wrapV;
        material.filter = samplerUse->filter;
    }
}

GltfMaterial convertMaterial(const cgltf_data& data, const cgltf_material& source, size_t materialIndex,
                             Warnings& warnings) {
    GltfMaterial result;
    result.name = nameOr(source.name, "Material", materialIndex);
    const std::string& name = result.name;
    MaterialData& material = result.material;

    const cgltf_texture_view* baseColorView = nullptr;
    const cgltf_texture_view* metallicRoughnessView = nullptr;
    if (source.has_pbr_metallic_roughness != 0 || source.has_pbr_specular_glossiness == 0) {
        // Without the block, cgltf supplies the defaults of the specification: white,
        // fully metallic, fully rough.
        const cgltf_pbr_metallic_roughness& pbr = source.pbr_metallic_roughness;
        material.baseColor = Vec4(unitRange(pbr.base_color_factor[0], 1.0f), unitRange(pbr.base_color_factor[1], 1.0f),
                                  unitRange(pbr.base_color_factor[2], 1.0f), unitRange(pbr.base_color_factor[3], 1.0f));
        material.metallic = unitRange(pbr.metallic_factor, 1.0f);
        material.roughness = unitRange(pbr.roughness_factor, 1.0f);
        baseColorView = &pbr.base_color_texture;
        metallicRoughnessView = &pbr.metallic_roughness_texture;
    } else {
        const cgltf_pbr_specular_glossiness& legacy = source.pbr_specular_glossiness;
        warnings.add(fmt::format("material '{}' uses the specular-glossiness model, which is only approximated", name));
        material.baseColor = Vec4(unitRange(legacy.diffuse_factor[0], 1.0f), unitRange(legacy.diffuse_factor[1], 1.0f),
                                  unitRange(legacy.diffuse_factor[2], 1.0f), unitRange(legacy.diffuse_factor[3], 1.0f));
        material.metallic = 0.0f;
        material.roughness = 1.0f - unitRange(legacy.glossiness_factor, 1.0f);
        baseColorView = &legacy.diffuse_texture;
    }

    // Reflectivity of a non-metal at normal incidence, from the index of refraction and
    // the specular factor; `specular` 0.5 stands for the 4% of the default index 1.5.
    if (source.has_ior != 0 || source.has_specular != 0) {
        const float ior = source.has_ior != 0 ? std::max(finiteOr(source.ior.ior, 1.5f), 1.0f) : 1.5f;
        const float reflectance = ((ior - 1.0f) / (ior + 1.0f)) * ((ior - 1.0f) / (ior + 1.0f));
        const float factor = source.has_specular != 0 ? finiteOr(source.specular.specular_factor, 1.0f) : 1.0f;
        material.specular = std::clamp(reflectance / 0.08f * factor, 0.0f, 1.0f);
    }

    // The scale of a texture that is not there is zero in cgltf, not the default.
    if (source.normal_texture.texture != nullptr) {
        material.normalScale = std::clamp(finiteOr(source.normal_texture.scale, 1.0f), -8.0f, 8.0f);
    }
    if (source.occlusion_texture.texture != nullptr) {
        material.occlusionStrength = unitRange(source.occlusion_texture.scale, 1.0f);
    }
    material.emissiveColor = Vec3(std::max(finiteOr(source.emissive_factor[0], 0.0f), 0.0f),
                                  std::max(finiteOr(source.emissive_factor[1], 0.0f), 0.0f),
                                  std::max(finiteOr(source.emissive_factor[2], 0.0f), 0.0f));
    if (source.has_emissive_strength != 0) {
        material.emissiveIntensity = std::max(finiteOr(source.emissive_strength.emissive_strength, 1.0f), 0.0f);
    }

    switch (source.alpha_mode) {
        case cgltf_alpha_mode_mask:
            material.alphaMode = AlphaMode::Masked;
            break;
        case cgltf_alpha_mode_blend:
            material.alphaMode = AlphaMode::Blended;
            break;
        default:
            material.alphaMode = AlphaMode::Opaque;
            break;
    }
    material.alphaCutoff = unitRange(source.alpha_cutoff, 0.5f);
    material.doubleSided = source.double_sided != 0;
    if (source.unlit != 0) {
        warnings.add(fmt::format("material '{}' is unlit, which is not supported; it is imported as lit", name));
    }

    convertMaterialTextures(data, source, baseColorView, metallicRoughnessView, result, warnings);
    return result;
}

/// Where the payload of a `data:` URI starts, when it is base64.
std::optional<size_t> base64PayloadOffset(std::string_view uri) {
    constexpr std::string_view kMarker = ";base64,";
    const size_t marker = uri.find(kMarker);
    if (!uri.starts_with("data:") || marker == std::string_view::npos) {
        return std::nullopt;
    }
    return marker + kMarker.size();
}

Result<GltfImage> convertImage(const cgltf_image& source, size_t imageIndex, bool loadData,
                               const cgltf_options& options, ImportBudget& budget) {
    GltfImage image;
    image.name = nameOr(source.name, "Image", imageIndex);

    if (source.buffer_view != nullptr) {
        if (loadData) {
            if (!viewFits(source.buffer_view, 0, source.buffer_view->size) || !viewIsLoaded(source.buffer_view)) {
                return makeError(ErrorCode::ParseError, "image '{}' refers to data that is not in the file",
                                 image.name);
            }
            CORDITE_TRY(budget.addImage(source.buffer_view->size));
            const auto* bytes = reinterpret_cast<const std::byte*>(cgltf_buffer_view_data(source.buffer_view));
            image.encoded.assign(bytes, bytes + source.buffer_view->size);
        }
        return image;
    }
    if (source.uri == nullptr) {
        return makeError(ErrorCode::ParseError, "image '{}' has neither a file nor data", image.name);
    }

    const std::string_view uri(source.uri);
    if (!uri.starts_with("data:")) {
        // An ordinary reference to a file. Percent-escapes are resolved the way cgltf
        // resolves them for buffers.
        std::string decoded(uri);
        decoded.resize(cgltf_decode_uri(decoded.data()));
        image.uri = std::move(decoded);
        return image;
    }

    const std::optional<size_t> payloadOffset = base64PayloadOffset(uri);
    if (!payloadOffset) {
        return makeError(ErrorCode::ParseError, "image '{}' has a data URI that is not base64", image.name);
    }
    if (loadData) {
        // The payload runs to the end of the URI, which cgltf keeps null-terminated.
        const char* payload = source.uri + *payloadOffset;
        size_t length = uri.size() - *payloadOffset;
        while (length > 0 && payload[length - 1] == '=') {
            --length;
        }
        const size_t byteCount = length * 3 / 4;
        CORDITE_TRY(budget.addImage(byteCount));
        void* decoded = nullptr;
        if (byteCount == 0 ||
            cgltf_load_buffer_base64(&options, byteCount, payload, &decoded) != cgltf_result_success) {
            return makeError(ErrorCode::ParseError, "image '{}' has a data URI that cannot be decoded", image.name);
        }
        const auto* bytes = static_cast<const std::byte*>(decoded);
        image.encoded.assign(bytes, bytes + byteCount);
        std::free(decoded);
    }
    return image;
}

ModelCamera convertCamera(const cgltf_camera& source) {
    ModelCamera camera;
    if (source.type == cgltf_camera_type_orthographic) {
        const cgltf_camera_orthographic& orthographic = source.data.orthographic;
        camera.orthographic = true;
        // glTF gives half the height.
        camera.orthographicHeight = 2.0f * finiteOr(orthographic.ymag, 5.0f);
        camera.nearPlane = finiteOr(orthographic.znear, 0.1f);
        camera.farPlane = finiteOr(orthographic.zfar, 0.0f);
    } else {
        const cgltf_camera_perspective& perspective = source.data.perspective;
        camera.verticalFov = finiteOr(perspective.yfov, 1.0f);
        camera.nearPlane = finiteOr(perspective.znear, 0.1f);
        camera.farPlane = perspective.has_zfar != 0 ? finiteOr(perspective.zfar, 0.0f) : 0.0f;
    }
    return camera;
}

Transform convertTransform(const cgltf_node& node) {
    if (node.has_matrix != 0) {
        Mat4 matrix(1.0f);
        for (glm::length_t column = 0; column < 4; ++column) {
            for (glm::length_t row = 0; row < 4; ++row) {
                matrix[column][row] = finiteOr(node.matrix[column * 4 + row], column == row ? 1.0f : 0.0f);
            }
        }
        return Transform::fromMatrix(matrix);
    }

    Transform transform;
    if (node.has_translation != 0) {
        transform.position = Vec3(finiteOr(node.translation[0], 0.0f), finiteOr(node.translation[1], 0.0f),
                                  finiteOr(node.translation[2], 0.0f));
    }
    if (node.has_rotation != 0) {
        // glTF stores x, y, z, w; glm's constructor takes w first.
        const Quat rotation(node.rotation[3], node.rotation[0], node.rotation[1], node.rotation[2]);
        const float length = glm::length(rotation);
        transform.rotation = std::isfinite(length) && length > kEpsilon ? rotation / length : kIdentityQuat;
    }
    if (node.has_scale != 0) {
        transform.scale =
            Vec3(finiteOr(node.scale[0], 1.0f), finiteOr(node.scale[1], 1.0f), finiteOr(node.scale[2], 1.0f));
    }
    return transform;
}

void convertNodes(const cgltf_data& data, GltfDocument& document) {
    document.nodes.resize(data.nodes_count);
    for (cgltf_size index = 0; index < data.nodes_count; ++index) {
        const cgltf_node& source = data.nodes[index];
        GltfNode& node = document.nodes[index];

        node.transform = convertTransform(source);
        if (source.parent != nullptr) {
            node.parent = static_cast<uint32_t>(cgltf_node_index(&data, source.parent));
        }
        for (cgltf_size child = 0; child < source.children_count; ++child) {
            node.children.push_back(static_cast<uint32_t>(cgltf_node_index(&data, source.children[child])));
        }
        if (source.mesh != nullptr) {
            const auto mesh = static_cast<uint32_t>(cgltf_mesh_index(&data, source.mesh));
            // A mesh of which nothing could be imported is not worth an entity's renderer.
            if (!document.meshes[mesh].mesh.submeshes.empty()) {
                node.mesh = mesh;
            }
        }
        if (source.camera != nullptr) {
            node.camera = convertCamera(*source.camera);
        }

        if (source.name != nullptr && source.name[0] != '\0') {
            node.name = source.name;
        } else if (node.mesh) {
            node.name = document.meshes[*node.mesh].mesh.name;
        } else {
            node.name = fmt::format("Node {}", index);
        }
    }

    // The default scene, or the first one, or - in a file without scenes - every node
    // that has no parent.
    const cgltf_scene* scene = data.scene;
    if (scene == nullptr && data.scenes_count > 0) {
        scene = data.scenes;
    }
    if (scene != nullptr) {
        for (cgltf_size index = 0; index < scene->nodes_count; ++index) {
            const auto root = static_cast<uint32_t>(cgltf_node_index(&data, scene->nodes[index]));
            // Listed twice, a node would be walked twice. (That a scene lists roots
            // only, cgltf_validate() has checked.)
            if (std::ranges::find(document.roots, root) == document.roots.end()) {
                document.roots.push_back(root);
            }
        }
    } else {
        for (cgltf_size index = 0; index < data.nodes_count; ++index) {
            if (data.nodes[index].parent == nullptr) {
                document.roots.push_back(static_cast<uint32_t>(index));
            }
        }
    }
}

/// A skin of the file as a skeleton. One that makes no sense - too many joints, a joint
/// listed twice, joints that hang on themselves - stays in its place without joints,
/// and a warning says why.
Result<GltfSkin> convertSkin(const cgltf_data& data, const cgltf_skin& source, size_t skinIndex, bool loadData,
                             const GltfDocument& document, Warnings& warnings) {
    const std::string name = nameOr(source.name, "Skin", skinIndex);
    GltfSkin skin;
    skin.skeleton.name = name;
    if (source.joints_count == 0 || source.joints_count > kMaxSkeletonJoints) {
        warnings.add(fmt::format("skin '{}' has {} joints, which is none or more than the {} a skeleton may have; "
                                 "it is not imported",
                                 name, source.joints_count, kMaxSkeletonJoints));
        return skin;
    }

    std::vector<uint32_t> joints(source.joints_count);
    for (cgltf_size index = 0; index < source.joints_count; ++index) {
        joints[index] = static_cast<uint32_t>(cgltf_node_index(&data, source.joints[index]));
    }

    // Without matrices the joints are bound where they are. With them there is one for
    // every joint; of more, the first are taken.
    std::vector<Mat4> inverseBind;
    if (const cgltf_accessor* matrices = source.inverse_bind_matrices; matrices != nullptr && loadData) {
        const std::string what = fmt::format("the inverse bind matrices of skin '{}'", name);
        if (matrices->type != cgltf_type_mat4 || matrices->count < source.joints_count) {
            return makeError(ErrorCode::ParseError, "{} are not a matrix for each of its {} joints", what,
                             source.joints_count);
        }
        CORDITE_TRY(checkAccessor(*matrices, kMaxMeshIndices, what));
        inverseBind.resize(source.joints_count, Mat4(1.0f));
        bool hasNoNumber = false;
        for (cgltf_size index = 0; index < source.joints_count; ++index) {
            std::array<cgltf_float, 16> values{};
            if (cgltf_accessor_read_float(matrices, index, values.data(), values.size()) == 0) {
                return makeError(ErrorCode::ParseError, "{} cannot be read", what);
            }
            if (std::ranges::all_of(values, [](float value) { return std::isfinite(value); })) {
                // Column after column, in the file as in memory.
                inverseBind[index] = std::bit_cast<Mat4>(values);
            } else {
                hasNoNumber = true;
            }
        }
        if (hasNoNumber) {
            warnings.add(fmt::format("{} hold numbers that are none; those joints are bound where they are", what));
        }
    }

    Result<SkeletonData> skeleton = skeletonOf(document, joints, inverseBind, name);
    if (!skeleton) {
        warnings.add(fmt::format("{}; it is not imported", skeleton.error().message));
        return skin;
    }
    skin.skeleton = std::move(*skeleton);
    skin.joints = std::move(joints);
    return skin;
}

/// Says which nodes draw their mesh with which skin, for the pairs that fit: a mesh
/// whose vertices follow joints, and a skin that has the joints they follow.
void assignSkins(const cgltf_data& data, bool loadData, GltfDocument& document, Warnings& warnings) {
    for (cgltf_size index = 0; index < data.nodes_count; ++index) {
        const cgltf_node& source = data.nodes[index];
        GltfNode& node = document.nodes[index];
        if (source.skin == nullptr || !node.mesh) {
            continue;
        }
        const auto skinIndex = static_cast<uint32_t>(cgltf_skin_index(&data, source.skin));
        const GltfSkin& skin = document.skins[skinIndex];
        const MeshData& mesh = document.meshes[*node.mesh].mesh;
        if (skin.joints.empty()) {
            continue;
        }
        // Without the data of the file the vertices are not known; what is listed then
        // is what the file says there is.
        if (loadData && !mesh.isSkinned()) {
            warnings.add(fmt::format("node '{}' gives the mesh '{}' a skin, but its vertices follow no joints",
                                     node.name, mesh.name));
            continue;
        }
        if (loadData && jointsUsed(mesh) > skin.joints.size()) {
            warnings.add(fmt::format("the mesh '{}' follows joint {}, but the skin '{}' has only {}; it is drawn "
                                     "without the skin",
                                     mesh.name, jointsUsed(mesh) - 1, skin.skeleton.name, skin.joints.size()));
            continue;
        }
        node.skin = skinIndex;
    }
}

/// What a channel of an animation moves.
enum class ChannelPath : uint8_t {
    Translation,
    Rotation,
    Scale
};

std::string_view nameOf(ChannelPath path) {
    switch (path) {
        case ChannelPath::Translation:
            return "translation";
        case ChannelPath::Rotation:
            return "rotation";
        case ChannelPath::Scale:
            return "scale";
    }
    return "unknown";
}

/// What a channel moves, or nothing for what is not imported: the weights of morph targets.
std::optional<ChannelPath> pathOf(cgltf_animation_path_type type) {
    switch (type) {
        case cgltf_animation_path_type_translation:
            return ChannelPath::Translation;
        case cgltf_animation_path_type_rotation:
            return ChannelPath::Rotation;
        case cgltf_animation_path_type_scale:
            return ChannelPath::Scale;
        default:
            return std::nullopt;
    }
}

/// Whether a track already says what `path` of its joint does.
bool hasKeys(const AnimationTrack& track, ChannelPath path) {
    switch (path) {
        case ChannelPath::Translation:
            return !track.translation.empty();
        case ChannelPath::Rotation:
            return !track.rotation.empty();
        case ChannelPath::Scale:
            return !track.scale.empty();
    }
    return false;
}

Interpolation interpolationOf(cgltf_interpolation_type type) {
    switch (type) {
        case cgltf_interpolation_type_step:
            return Interpolation::Step;
        case cgltf_interpolation_type_cubic_spline:
            return Interpolation::Cubic;
        default:
            return Interpolation::Linear;
    }
}

/// The keys of one channel: its times, and for each of them `width` numbers - or three
/// times as many for a cubic spline, which has a slope in, a value and a slope out.
/// Nothing, with a warning, for a channel that makes no sense; an error for one that
/// cannot be read or is more than the file may hold.
struct ChannelKeys {
    std::vector<float> times;
    std::vector<float> values;
};

Result<std::optional<ChannelKeys>> readChannel(const cgltf_animation_sampler& sampler, cgltf_size width,
                                               std::string_view what, Warnings& warnings, ImportBudget& budget) {
    const cgltf_accessor* input = sampler.input;
    const cgltf_accessor* output = sampler.output;
    const cgltf_type valueType = width == 4 ? cgltf_type_vec4 : cgltf_type_vec3;
    const cgltf_size perKey = sampler.interpolation == cgltf_interpolation_type_cubic_spline ? 3 : 1;
    if (input == nullptr || output == nullptr || input->type != cgltf_type_scalar || output->type != valueType ||
        input->count == 0 || output->count != input->count * perKey) {
        warnings.add(fmt::format("{} has no keys, or not a value for each of them; it is left out", what));
        return std::optional<ChannelKeys>();
    }
    CORDITE_TRY(budget.addKeys(input->count));
    CORDITE_TRY(checkAccessor(*input, kMaxAnimationKeys, fmt::format("the times of {}", what)));
    CORDITE_TRY(checkAccessor(*output, kMaxMeshIndices, fmt::format("the values of {}", what)));

    ChannelKeys keys;
    keys.times.resize(input->count);
    keys.values.resize(output->count * width);
    if (cgltf_accessor_unpack_floats(input, keys.times.data(), keys.times.size()) != keys.times.size() ||
        cgltf_accessor_unpack_floats(output, keys.values.data(), keys.values.size()) != keys.values.size()) {
        return makeError(ErrorCode::ParseError, "{} cannot be read", what);
    }

    // Times that are numbers, not before the beginning, each later than the one before;
    // values that are numbers.
    float before = -1.0f;
    for (const float time : keys.times) {
        if (!std::isfinite(time) || time < 0.0f || time <= before) {
            warnings.add(fmt::format("{} has times that are none, or do not run forward; it is left out", what));
            return std::optional<ChannelKeys>();
        }
        before = time;
    }
    if (!std::ranges::all_of(keys.values, [](float value) { return std::isfinite(value); })) {
        warnings.add(fmt::format("{} has values that are no numbers; it is left out", what));
        return std::optional<ChannelKeys>();
    }
    return std::optional<ChannelKeys>(std::move(keys));
}

/// A curve from the keys of a channel of three numbers a value.
Curve<Vec3> curveOf(const ChannelKeys& keys, Interpolation interpolation) {
    const size_t perKey = interpolation == Interpolation::Cubic ? 3 : 1;
    const auto valueAt = [&](size_t index) {
        return Vec3(keys.values[index * 3], keys.values[index * 3 + 1], keys.values[index * 3 + 2]);
    };
    Curve<Vec3> curve;
    for (size_t index = 0; index < keys.times.size(); ++index) {
        Keyframe<Vec3> key;
        key.time = keys.times[index];
        key.interpolation = interpolation;
        if (perKey == 3) {
            key.inTangent = valueAt(index * 3);
            key.value = valueAt(index * 3 + 1);
            key.outTangent = valueAt(index * 3 + 2);
        } else {
            key.value = valueAt(index);
        }
        curve.addKey(key);
    }
    return curve;
}

/// How far from one the length of a rotation in a file may be and still be taken as it is.
constexpr float kUnitLengthTolerance = 1.0e-5f;

/// A curve of rotations from the keys of a channel of four numbers a value: of unit
/// length at the keys, and each key the same way round as the one before it, so that
/// what is between two keys is the short way from the one to the other.
Curve<Vec4> rotationCurveOf(const ChannelKeys& keys, Interpolation interpolation) {
    const size_t perKey = interpolation == Interpolation::Cubic ? 3 : 1;
    const auto valueAt = [&](size_t index) {
        return Vec4(keys.values[index * 4], keys.values[index * 4 + 1], keys.values[index * 4 + 2],
                    keys.values[index * 4 + 3]);
    };
    Curve<Vec4> curve;
    Vec4 previous(0.0f, 0.0f, 0.0f, 1.0f);
    for (size_t index = 0; index < keys.times.size(); ++index) {
        Keyframe<Vec4> key;
        key.time = keys.times[index];
        key.interpolation = interpolation;
        key.value = valueAt(perKey == 3 ? index * 3 + 1 : index);
        if (perKey == 3) {
            key.inTangent = valueAt(index * 3);
            key.outTangent = valueAt(index * 3 + 2);
        }
        // Brought to unit length - unless it is that already, as nearly as a float can
        // say: a file that is right is not to come back changed in its last bit. A
        // rotation without length is none: the joint is not turned.
        const float length = glm::length(key.value);
        if (std::abs(length - 1.0f) > kUnitLengthTolerance) {
            const float scale = length > kEpsilon ? 1.0f / length : 0.0f;
            key.value = scale > 0.0f ? key.value * scale : Vec4(0.0f, 0.0f, 0.0f, 1.0f);
            key.inTangent *= scale;
            key.outTangent *= scale;
        }
        if (index > 0 && glm::dot(previous, key.value) < 0.0f) {
            key.value = -key.value;
            key.inTangent = -key.inTangent;
            key.outTangent = -key.outTangent;
        }
        previous = key.value;
        curve.addKey(key);
    }
    return curve;
}

/// An animation of the file: a track for every node it moves. Channels that make no
/// sense are left out, with a warning each.
Result<GltfAnimation> convertAnimation(const cgltf_data& data, const cgltf_animation& source, size_t animationIndex,
                                       bool loadData, const GltfDocument& document, Warnings& warnings,
                                       ImportBudget& budget) {
    GltfAnimation result;
    AnimationData& animation = result.animation;
    animation.name = nameOr(source.name, "Animation", animationIndex);

    for (cgltf_size index = 0; index < source.channels_count; ++index) {
        const cgltf_animation_channel& channel = source.channels[index];
        if (channel.target_node == nullptr || channel.sampler == nullptr) {
            continue;
        }
        const std::optional<ChannelPath> path = pathOf(channel.target_path);
        if (!path) {
            warnings.add(fmt::format("animation '{}' moves morph targets, which are not imported", animation.name));
            continue;
        }

        // The track of the node, made when the first channel for the node comes by.
        const auto node = static_cast<uint32_t>(cgltf_node_index(&data, channel.target_node));
        auto found = std::ranges::find(result.nodes, node);
        if (found == result.nodes.end()) {
            result.nodes.push_back(node);
            animation.tracks.push_back({.joint = jointNameOf(document, node)});
            found = result.nodes.end() - 1;
        }
        AnimationTrack& track = animation.tracks[static_cast<size_t>(found - result.nodes.begin())];
        if (!loadData) {
            continue;
        }

        const bool isRotation = *path == ChannelPath::Rotation;
        const std::string what =
            fmt::format("the {} of '{}' in animation '{}'", nameOf(*path), track.joint, animation.name);
        if (hasKeys(track, *path)) {
            warnings.add(fmt::format("{} is given more than once; the first is kept", what));
            continue;
        }
        CORDITE_TRY_ASSIGN(const std::optional<ChannelKeys> keys,
                           readChannel(*channel.sampler, isRotation ? 4 : 3, what, warnings, budget));
        if (!keys) {
            continue;
        }
        const Interpolation interpolation = interpolationOf(channel.sampler->interpolation);
        if (isRotation) {
            track.rotation = rotationCurveOf(*keys, interpolation);
        } else if (*path == ChannelPath::Scale) {
            track.scale = curveOf(*keys, interpolation);
        } else {
            track.translation = curveOf(*keys, interpolation);
        }
    }
    animation.duration = lastKeyTime(animation);
    return result;
}

Status checkRequiredExtensions(const cgltf_data& data) {
    for (cgltf_size index = 0; index < data.extensions_required_count; ++index) {
        const std::string_view extension(data.extensions_required[index]);
        if (std::ranges::find(kSupportedExtensions, extension) == kSupportedExtensions.end()) {
            return makeError(ErrorCode::Unsupported, "it requires the extension {}, which is not supported", extension);
        }
    }
    return {};
}

Result<GltfDocument> convertDocument(const cgltf_data& data, const GltfImportOptions& importOptions,
                                     const cgltf_options& options) {
    GltfDocument document;
    Warnings warnings(document.warnings);
    ImportBudget budget(importOptions);
    const bool loadData = importOptions.loadData;

    if (data.lights_count > 0) {
        warnings.add("it contains lights, which are not imported");
    }

    document.images.reserve(data.images_count);
    for (cgltf_size index = 0; index < data.images_count; ++index) {
        CORDITE_TRY_ASSIGN(GltfImage image, convertImage(data.images[index], index, loadData, options, budget));
        document.images.push_back(std::move(image));
    }

    document.materials.reserve(data.materials_count);
    for (cgltf_size index = 0; index < data.materials_count; ++index) {
        document.materials.push_back(convertMaterial(data, data.materials[index], index, warnings));
    }

    document.meshes.reserve(data.meshes_count);
    for (cgltf_size index = 0; index < data.meshes_count; ++index) {
        CORDITE_TRY_ASSIGN(GltfMesh mesh, convertMesh(data, data.meshes[index], index, loadData, warnings, budget));
        document.meshes.push_back(std::move(mesh));
    }

    convertNodes(data, document);

    // Joints are nodes: the skins, and the animations that move them, come after.
    document.skins.reserve(data.skins_count);
    for (cgltf_size index = 0; index < data.skins_count; ++index) {
        CORDITE_TRY_ASSIGN(GltfSkin skin, convertSkin(data, data.skins[index], index, loadData, document, warnings));
        document.skins.push_back(std::move(skin));
    }
    assignSkins(data, loadData, document, warnings);

    document.animations.reserve(data.animations_count);
    for (cgltf_size index = 0; index < data.animations_count; ++index) {
        CORDITE_TRY_ASSIGN(GltfAnimation animation,
                           convertAnimation(data, data.animations[index], index, loadData, document, warnings, budget));
        document.animations.push_back(std::move(animation));
    }
    return document;
}

/// GltfDocument::externalFiles of a file: the URIs of its buffers and images that are no data.
std::vector<std::string> externalFilesOf(const cgltf_data& data) {
    std::vector<std::string> files;
    const auto add = [&files](const char* uri) {
        if (uri == nullptr || std::string_view(uri).starts_with("data:")) {
            return;
        }
        std::string decoded(uri);
        decoded.resize(cgltf_decode_uri(decoded.data()));
        if (std::ranges::find(files, decoded) == files.end()) {
            files.push_back(std::move(decoded));
        }
    };
    for (cgltf_size index = 0; index < data.buffers_count; ++index) {
        add(data.buffers[index].uri);
    }
    for (cgltf_size index = 0; index < data.images_count; ++index) {
        add(data.images[index].uri);
    }
    return files;
}

} // namespace

Result<GltfDocument> importGltf(const std::filesystem::path& path, const GltfImportOptions& importOptions) {
    const std::string fileName = pathToUtf8(path);
    const auto fail = [&fileName](const Error& error) {
        return tl::unexpected(error.withContext(fmt::format("cannot import {}", fileName)));
    };

    // The file is read here rather than by cgltf, which would open it with fopen() and
    // so with the wrong idea of what a path is on Windows.
    Result<std::vector<std::byte>> bytes = readBinaryFile(path);
    if (!bytes) {
        return fail(bytes.error());
    }

    FileContext context;
    context.baseDirectory = path.parent_path();
    context.rootDirectory = importOptions.rootDirectory.empty() ? context.baseDirectory : importOptions.rootDirectory;

    cgltf_options options{};
    options.file.read = readReferencedFile;
    options.file.release = releaseReferencedFile;
    options.file.user_data = &context;

    cgltf_data* parsed = nullptr;
    if (const cgltf_result result = cgltf_parse(&options, bytes->data(), bytes->size(), &parsed);
        result != cgltf_result_success) {
        return fail(Error{ErrorCode::ParseError, std::string(describe(result))});
    }
    // `parsed` points into `bytes`, which therefore has to outlive it.
    const std::unique_ptr<cgltf_data, CgltfDeleter> data(parsed);

    if (const Status supported = checkRequiredExtensions(*data); !supported) {
        return fail(supported.error());
    }

    if (importOptions.loadData) {
        // An empty path: the URIs reach readReferencedFile() as they are in the file.
        if (const cgltf_result result = cgltf_load_buffers(&options, data.get(), ""); result != cgltf_result_success) {
            std::string reason = context.error;
            if (reason.empty()) {
                // "Unknown format" is what cgltf says about a buffer it will not fetch.
                reason = result == cgltf_result_unknown_format ? "it keeps a buffer where it cannot be read: only "
                                                                 "files next to it and base64 data are supported"
                                                               : std::string(describe(result));
            }
            return fail(Error{ErrorCode::ParseError, std::move(reason)});
        }
    }
    if (const Status layouts = checkAccessorLayouts(*data); !layouts) {
        return fail(layouts.error());
    }
    if (const cgltf_result result = cgltf_validate(data.get()); result != cgltf_result_success) {
        return fail(Error{ErrorCode::ParseError, std::string(describe(result))});
    }

    Result<GltfDocument> document = convertDocument(*data, importOptions, options);
    if (!document) {
        return fail(document.error());
    }
    document->externalFiles = externalFilesOf(*data);
    return document;
}

} // namespace cordite
