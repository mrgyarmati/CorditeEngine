#include "cordite/assets/GltfExporter.h"

#include "cordite/core/BinaryStream.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Json.h"
#include "cordite/core/StringUtils.h"
#include "cordite/core/Version.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cordite {
namespace {

// Numbers the glTF specification assigns.
constexpr uint32_t kGlbMagic = 0x46546C67;
constexpr uint32_t kGlbVersion = 2;
constexpr uint32_t kGlbJsonChunk = 0x4E4F534A;
constexpr uint32_t kGlbBinaryChunk = 0x004E4942;
constexpr int kArrayBufferTarget = 34962;
constexpr int kElementArrayBufferTarget = 34963;
constexpr int kUnsignedShortType = 5123;
constexpr int kUnsignedIntType = 5125;
constexpr int kFloatType = 5126;
constexpr int kNearestFilter = 9728;
constexpr int kLinearFilter = 9729;
constexpr int kLinearMipmapLinearFilter = 9987;

/// What an orthographic camera gets as its far plane when the document has none: glTF
/// requires one.
constexpr float kDefaultOrthographicDepth = 1000.0f;

Json floats(std::initializer_list<float> values) {
    Json array = Json::array();
    for (const float value : values) {
        array.push_back(jsonFromFloat(value));
    }
    return array;
}

int wrapCode(TextureWrap wrap) {
    switch (wrap) {
        case TextureWrap::Repeat:
            return 10497;
        case TextureWrap::MirroredRepeat:
            return 33648;
        case TextureWrap::Clamp:
            return 33071;
    }
    return 10497;
}

std::string_view alphaModeName(AlphaMode mode) {
    switch (mode) {
        case AlphaMode::Opaque:
            return "OPAQUE";
        case AlphaMode::Masked:
            return "MASK";
        case AlphaMode::Blended:
            return "BLEND";
    }
    return "OPAQUE";
}

/// The media type of encoded image data, from the bytes every such file starts with.
std::optional<std::string_view> imageMediaType(std::span<const std::byte> encoded) {
    constexpr std::array<std::byte, 4> kPng = {std::byte{0x89}, std::byte{'P'}, std::byte{'N'}, std::byte{'G'}};
    constexpr std::array<std::byte, 2> kJpeg = {std::byte{0xFF}, std::byte{0xD8}};
    if (encoded.size() >= kPng.size() && std::equal(kPng.begin(), kPng.end(), encoded.begin())) {
        return "image/png";
    }
    if (encoded.size() >= kJpeg.size() && std::equal(kJpeg.begin(), kJpeg.end(), encoded.begin())) {
        return "image/jpeg";
    }
    return std::nullopt;
}

/// Escapes what may not appear literally in a relative URI reference.
std::string encodeUri(std::string_view path) {
    constexpr std::string_view kUnreserved = "-._~/!$&'()*+,;=:@";
    std::string encoded;
    for (const char character : path) {
        const auto byte = static_cast<unsigned char>(character);
        const bool isAlphanumeric =
            (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
        if (isAlphanumeric || kUnreserved.find(character) != std::string_view::npos) {
            encoded.push_back(character);
        } else {
            encoded += fmt::format("%{:02X}", byte);
        }
    }
    return encoded;
}

/// Checks that images, materials and meshes can be written, and that what they refer
/// to by index exists.
Status validateContents(const GltfDocument& document) {
    for (const GltfImage& image : document.images) {
        if (image.isEmbedded() && !imageMediaType(image.encoded)) {
            return makeError(ErrorCode::InvalidArgument, "image '{}' is embedded but holds neither PNG nor JPEG data",
                             image.name);
        }
    }

    for (const GltfMaterial& material : document.materials) {
        for (const std::optional<uint32_t>& image :
             {material.images.baseColor, material.images.metallicRoughness, material.images.normal,
              material.images.occlusion, material.images.emissive}) {
            if (image && *image >= document.images.size()) {
                return makeError(ErrorCode::InvalidArgument, "material '{}' refers to image {}, but there are only {}",
                                 material.name, *image, document.images.size());
            }
        }
    }

    for (const GltfMesh& mesh : document.meshes) {
        CORDITE_TRY(validateMesh(mesh.mesh));
        if (mesh.mesh.indices.empty()) {
            // A glTF mesh has at least one part, and parts without triangles are left out.
            return makeError(ErrorCode::InvalidArgument, "mesh '{}' has no triangles", mesh.mesh.name);
        }
        for (const std::optional<uint32_t>& material : mesh.materials) {
            if (material && *material >= document.materials.size()) {
                return makeError(ErrorCode::InvalidArgument, "mesh '{}' refers to material {}, but there are only {}",
                                 mesh.mesh.name, *material, document.materials.size());
            }
        }
    }
    return {};
}

/// Whether every key of a curve goes on to the next the same way: a glTF file says how
/// once for a whole curve.
template <typename Value>
bool hasOneInterpolation(const Curve<Value>& curve) {
    const std::span<const Keyframe<Value>> keys = curve.keys();
    return std::ranges::all_of(
        keys, [&](const Keyframe<Value>& key) { return key.interpolation == keys.front().interpolation; });
}

/// Checks that skins and animations can be written, and that the nodes they name exist.
Status validateMotion(const GltfDocument& document) {
    const size_t nodeCount = document.nodes.size();
    for (const GltfSkin& skin : document.skins) {
        const std::string& name = skin.skeleton.name;
        if (skin.joints.empty()) {
            // A glTF skin has at least one joint.
            return makeError(ErrorCode::InvalidArgument, "skin '{}' has no joints", name);
        }
        if (skin.joints.size() != skin.skeleton.joints.size()) {
            return makeError(ErrorCode::InvalidArgument, "skin '{}' names {} nodes for the {} joints of its skeleton",
                             name, skin.joints.size(), skin.skeleton.joints.size());
        }
        CORDITE_TRY(validateSkeleton(skin.skeleton));
        std::vector<uint32_t> sorted = skin.joints;
        std::ranges::sort(sorted);
        if (sorted.back() >= nodeCount || std::ranges::adjacent_find(sorted) != sorted.end()) {
            return makeError(ErrorCode::InvalidArgument,
                             "skin '{}' names a node that is not there, or the same node twice", name);
        }
    }

    for (const GltfAnimation& source : document.animations) {
        const AnimationData& animation = source.animation;
        CORDITE_TRY(validateAnimation(animation));
        if (source.nodes.size() != animation.tracks.size()) {
            return makeError(ErrorCode::InvalidArgument, "animation '{}' names {} nodes for its {} tracks",
                             animation.name, source.nodes.size(), animation.tracks.size());
        }
        size_t keyCount = 0;
        for (size_t index = 0; index < animation.tracks.size(); ++index) {
            const AnimationTrack& track = animation.tracks[index];
            if (source.nodes[index] >= nodeCount) {
                return makeError(ErrorCode::InvalidArgument, "animation '{}' moves node {}, but there are only {}",
                                 animation.name, source.nodes[index], nodeCount);
            }
            if (!hasOneInterpolation(track.translation) || !hasOneInterpolation(track.rotation) ||
                !hasOneInterpolation(track.scale)) {
                return makeError(ErrorCode::InvalidArgument,
                                 "the track for '{}' of animation '{}' has a curve whose keys do not all go on "
                                 "to the next the same way, which a glTF file cannot say",
                                 track.joint, animation.name);
            }
            keyCount += track.translation.keys().size() + track.rotation.keys().size() + track.scale.keys().size();
        }
        if (keyCount == 0) {
            // A glTF animation has at least one channel.
            return makeError(ErrorCode::InvalidArgument, "animation '{}' has no keys", animation.name);
        }
    }
    return {};
}

/// Checks that the nodes form trees: every node has at most one parent, following
/// parents always ends, and the roots are nodes without one.
Status validateHierarchy(const GltfDocument& document) {
    const size_t nodeCount = document.nodes.size();
    std::vector<std::optional<uint32_t>> parents(nodeCount);
    for (size_t index = 0; index < nodeCount; ++index) {
        const GltfNode& node = document.nodes[index];
        if (node.mesh && *node.mesh >= document.meshes.size()) {
            return makeError(ErrorCode::InvalidArgument, "node '{}' refers to mesh {}, but there are only {}",
                             node.name, *node.mesh, document.meshes.size());
        }
        if (node.skin) {
            if (*node.skin >= document.skins.size() || !node.mesh) {
                return makeError(ErrorCode::InvalidArgument,
                                 "node '{}' refers to a skin that is not there, or has a skin and no mesh", node.name);
            }
            const MeshData& mesh = document.meshes[*node.mesh].mesh;
            if (!mesh.isSkinned() || jointsUsed(mesh) > document.skins[*node.skin].joints.size()) {
                return makeError(ErrorCode::InvalidArgument,
                                 "node '{}' gives the mesh '{}' a skin that does not have the joints its vertices "
                                 "follow",
                                 node.name, mesh.name);
            }
        }
        for (const uint32_t child : node.children) {
            if (child >= nodeCount) {
                return makeError(ErrorCode::InvalidArgument, "node '{}' has the child {}, but there are only {} nodes",
                                 node.name, child, nodeCount);
            }
            if (parents[child]) {
                return makeError(ErrorCode::InvalidArgument, "node '{}' has more than one parent",
                                 document.nodes[child].name);
            }
            parents[child] = static_cast<uint32_t>(index);
        }
    }
    for (size_t index = 0; index < nodeCount; ++index) {
        size_t steps = 0;
        for (std::optional<uint32_t> ancestor = parents[index]; ancestor; ancestor = parents[*ancestor]) {
            if (++steps > nodeCount) {
                return makeError(ErrorCode::InvalidArgument, "node '{}' is its own ancestor",
                                 document.nodes[index].name);
            }
        }
    }
    for (const uint32_t root : document.roots) {
        if (root >= nodeCount || parents[root]) {
            return makeError(ErrorCode::InvalidArgument, "root {} is not a node without a parent", root);
        }
    }
    return {};
}

/// Checks everything that would otherwise produce a file other programs reject.
Status validateDocument(const GltfDocument& document) {
    CORDITE_TRY(validateContents(document));
    CORDITE_TRY(validateMotion(document));
    return validateHierarchy(document);
}

/// Builds the JSON and the binary buffer of a glTF file side by side.
class GltfBuilder {
public:
    explicit GltfBuilder(const GltfDocument& document) : m_document(document) {}

    void build() {
        Json asset = Json::object();
        asset["version"] = "2.0";
        asset["generator"] = fmt::format("{} {}", kEngineName, kEngineVersion);

        // A scene without nodes is allowed; an empty "nodes" array is not.
        Json scene = Json::object();
        if (!m_document.roots.empty()) {
            scene["nodes"] = m_document.roots;
        }
        Json scenes = Json::array();
        scenes.push_back(std::move(scene));

        m_root = Json::object();
        m_root["asset"] = std::move(asset);
        m_root["scene"] = 0;
        m_root["scenes"] = std::move(scenes);

        addNodes();
        addMeshes();
        addSkins();
        addAnimations();
        addMaterials();
        addImages();

        // Members that are empty arrays are not allowed in glTF; they are added last and
        // only when they have entries.
        addIfUsed("cameras", m_cameras);
        addIfUsed("textures", m_textures);
        addIfUsed("samplers", m_samplers);
        addIfUsed("accessors", m_accessors);
        addIfUsed("bufferViews", m_bufferViews);
        if (!m_extensions.empty()) {
            m_root["extensionsUsed"] = m_extensions;
        }
    }

    /// Names the buffer once its final home is known. `uri` is empty inside a .glb.
    void finishBuffer(std::string_view uri) {
        if (m_buffer.size() == 0) {
            return;
        }
        Json buffer = Json::object();
        if (!uri.empty()) {
            buffer["uri"] = uri;
        }
        buffer["byteLength"] = m_buffer.size();
        Json buffers = Json::array();
        buffers.push_back(std::move(buffer));
        m_root["buffers"] = std::move(buffers);
    }

    [[nodiscard]] const Json& json() const { return m_root; }

    [[nodiscard]] const std::vector<std::byte>& buffer() const { return m_buffer.data(); }

private:
    void addIfUsed(std::string_view name, Json& array) {
        if (!array.empty()) {
            m_root[std::string(name)] = std::move(array);
        }
    }

    void useExtension(std::string_view name) {
        if (std::ranges::find(m_extensions, name) == m_extensions.end()) {
            m_extensions.emplace_back(name);
        }
    }

    /// Appends bytes to the buffer, aligned to four bytes as accessors require, and
    /// returns the index of the view that describes them.
    uint32_t addView(std::span<const std::byte> bytes, std::optional<int> target = std::nullopt,
                     std::optional<size_t> stride = std::nullopt) {
        while (m_buffer.size() % 4 != 0) {
            m_buffer.write(std::byte{0});
        }
        Json view = Json::object();
        view["buffer"] = 0;
        view["byteOffset"] = m_buffer.size();
        view["byteLength"] = bytes.size();
        if (stride) {
            view["byteStride"] = *stride;
        }
        if (target) {
            view["target"] = *target;
        }
        m_buffer.writeBytes(bytes);
        m_bufferViews.push_back(std::move(view));
        return static_cast<uint32_t>(m_bufferViews.size() - 1);
    }

    /// The index of `entry` in `array`, appending it when it is not there yet.
    static uint32_t indexOrAppend(Json& array, Json entry) {
        for (size_t index = 0; index < array.size(); ++index) {
            if (array[index] == entry) {
                return static_cast<uint32_t>(index);
            }
        }
        array.push_back(std::move(entry));
        return static_cast<uint32_t>(array.size() - 1);
    }

    uint32_t addAccessor(Json accessor) {
        m_accessors.push_back(std::move(accessor));
        return static_cast<uint32_t>(m_accessors.size() - 1);
    }

    void addNodes() {
        if (m_document.nodes.empty()) {
            return;
        }
        Json nodes = Json::array();
        for (const GltfNode& source : m_document.nodes) {
            Json node = Json::object();
            node["name"] = source.name;
            const Transform& transform = source.transform;
            if (transform.position != Vec3(0.0f)) {
                node["translation"] = floats({transform.position.x, transform.position.y, transform.position.z});
            }
            if (transform.rotation != kIdentityQuat) {
                const Quat& rotation = transform.rotation;
                node["rotation"] = floats({rotation.x, rotation.y, rotation.z, rotation.w});
            }
            if (transform.scale != Vec3(1.0f)) {
                node["scale"] = floats({transform.scale.x, transform.scale.y, transform.scale.z});
            }
            if (!source.children.empty()) {
                node["children"] = source.children;
            }
            if (source.mesh) {
                node["mesh"] = *source.mesh;
            }
            if (source.skin) {
                node["skin"] = *source.skin;
            }
            if (source.camera) {
                node["camera"] = addCamera(*source.camera, source.name);
            }
            nodes.push_back(std::move(node));
        }
        m_root["nodes"] = std::move(nodes);
    }

    uint32_t addCamera(const ModelCamera& source, const std::string& name) {
        Json camera = Json::object();
        camera["name"] = name;
        if (source.orthographic) {
            const float halfHeight = 0.5f * source.orthographicHeight;
            const float farPlane =
                source.farPlane > source.nearPlane ? source.farPlane : source.nearPlane + kDefaultOrthographicDepth;
            camera["type"] = "orthographic";
            // The width depends on the shape of the image, which a camera does not know;
            // readers derive it, as the engine does.
            Json orthographic = Json::object();
            orthographic["xmag"] = jsonFromFloat(halfHeight);
            orthographic["ymag"] = jsonFromFloat(halfHeight);
            orthographic["znear"] = jsonFromFloat(source.nearPlane);
            orthographic["zfar"] = jsonFromFloat(farPlane);
            camera["orthographic"] = std::move(orthographic);
        } else {
            Json perspective = Json::object();
            perspective["yfov"] = jsonFromFloat(source.verticalFov);
            perspective["znear"] = jsonFromFloat(source.nearPlane);
            if (source.farPlane > source.nearPlane) {
                perspective["zfar"] = jsonFromFloat(source.farPlane);
            }
            camera["type"] = "perspective";
            camera["perspective"] = std::move(perspective);
        }
        m_cameras.push_back(std::move(camera));
        return static_cast<uint32_t>(m_cameras.size() - 1);
    }

    void addMeshes() {
        if (m_document.meshes.empty()) {
            return;
        }
        Json meshes = Json::array();
        for (const GltfMesh& source : m_document.meshes) {
            const MeshData& mesh = source.mesh;

            // The vertices are written as they are in memory: interleaved, 56 bytes each.
            const uint32_t vertexView =
                addView(std::as_bytes(std::span(mesh.vertices)), kArrayBufferTarget, sizeof(MeshVertex));
            const auto attribute = [&](size_t offset, std::string_view type) {
                Json accessor = Json::object();
                accessor["bufferView"] = vertexView;
                accessor["byteOffset"] = offset;
                accessor["componentType"] = kFloatType;
                accessor["count"] = mesh.vertices.size();
                accessor["type"] = type;
                return accessor;
            };

            Vec3 minimum = mesh.vertices.front().position;
            Vec3 maximum = minimum;
            for (const MeshVertex& vertex : mesh.vertices) {
                minimum = glm::min(minimum, vertex.position);
                maximum = glm::max(maximum, vertex.position);
            }
            Json positions = attribute(offsetof(MeshVertex, position), "VEC3");
            positions["min"] = floats({minimum.x, minimum.y, minimum.z});
            positions["max"] = floats({maximum.x, maximum.y, maximum.z});

            Json attributes = Json::object();
            attributes["POSITION"] = addAccessor(std::move(positions));
            attributes["NORMAL"] = addAccessor(attribute(offsetof(MeshVertex, normal), "VEC3"));
            attributes["TANGENT"] = addAccessor(attribute(offsetof(MeshVertex, tangent), "VEC4"));
            attributes["TEXCOORD_0"] = addAccessor(attribute(offsetof(MeshVertex, uv0), "VEC2"));
            attributes["TEXCOORD_1"] = addAccessor(attribute(offsetof(MeshVertex, uv1), "VEC2"));
            if (mesh.isSkinned()) {
                // How the vertices follow their joints, likewise as it is in memory: four
                // indices of sixteen bits and four weights a vertex.
                const uint32_t skinView =
                    addView(std::as_bytes(std::span(mesh.skin)), kArrayBufferTarget, sizeof(MeshSkinVertex));
                Json joints = Json::object();
                joints["bufferView"] = skinView;
                joints["byteOffset"] = offsetof(MeshSkinVertex, joints);
                joints["componentType"] = kUnsignedShortType;
                joints["count"] = mesh.skin.size();
                joints["type"] = "VEC4";
                Json weights = Json::object();
                weights["bufferView"] = skinView;
                weights["byteOffset"] = offsetof(MeshSkinVertex, weights);
                weights["componentType"] = kFloatType;
                weights["count"] = mesh.skin.size();
                weights["type"] = "VEC4";
                attributes["JOINTS_0"] = addAccessor(std::move(joints));
                attributes["WEIGHTS_0"] = addAccessor(std::move(weights));
            }

            // Sixteen-bit indices where they reach every vertex: half the size.
            const bool smallIndices = mesh.vertices.size() <= 0xFFFF;
            uint32_t indexView = 0;
            if (smallIndices) {
                std::vector<uint16_t> narrow(mesh.indices.size());
                std::ranges::transform(mesh.indices, narrow.begin(),
                                       [](uint32_t index) { return static_cast<uint16_t>(index); });
                indexView = addView(std::as_bytes(std::span(narrow)), kElementArrayBufferTarget);
            } else {
                indexView = addView(std::as_bytes(std::span(mesh.indices)), kElementArrayBufferTarget);
            }
            const size_t indexSize = smallIndices ? sizeof(uint16_t) : sizeof(uint32_t);

            Json primitives = Json::array();
            for (size_t index = 0; index < mesh.submeshes.size(); ++index) {
                const Submesh& submesh = mesh.submeshes[index];
                if (submesh.indexCount == 0) {
                    // glTF has no way to say "a part without triangles".
                    continue;
                }
                Json indices = Json::object();
                indices["bufferView"] = indexView;
                indices["byteOffset"] = submesh.firstIndex * indexSize;
                indices["componentType"] = smallIndices ? kUnsignedShortType : kUnsignedIntType;
                indices["count"] = submesh.indexCount;
                indices["type"] = "SCALAR";

                Json primitive = Json::object();
                primitive["attributes"] = attributes;
                primitive["indices"] = addAccessor(std::move(indices));
                if (index < source.materials.size() && source.materials[index]) {
                    primitive["material"] = *source.materials[index];
                }
                primitives.push_back(std::move(primitive));
            }
            Json entry = Json::object();
            entry["name"] = mesh.name;
            entry["primitives"] = std::move(primitives);
            meshes.push_back(std::move(entry));
        }
        m_root["meshes"] = std::move(meshes);
    }

    /// An accessor for numbers that are neither vertices nor indices: `count` values of
    /// `type`, in floats.
    uint32_t addFloats(std::span<const float> values, size_t count, std::string_view type) {
        Json accessor = Json::object();
        accessor["bufferView"] = addView(std::as_bytes(values));
        accessor["componentType"] = kFloatType;
        accessor["count"] = count;
        accessor["type"] = type;
        return addAccessor(std::move(accessor));
    }

    void addSkins() {
        if (m_document.skins.empty()) {
            return;
        }
        Json skins = Json::array();
        for (const GltfSkin& source : m_document.skins) {
            // Column after column, in the file as in memory.
            std::vector<float> matrices(source.skeleton.joints.size() * 16);
            for (size_t index = 0; index < source.skeleton.joints.size(); ++index) {
                std::memcpy(matrices.data() + index * 16, &source.skeleton.joints[index].inverseBind, sizeof(Mat4));
            }
            Json skin = Json::object();
            skin["name"] = source.skeleton.name;
            skin["joints"] = source.joints;
            skin["inverseBindMatrices"] = addFloats(matrices, source.skeleton.joints.size(), "MAT4");
            skins.push_back(std::move(skin));
        }
        m_root["skins"] = std::move(skins);
    }

    /// Writes the keys of one curve as a sampler of `animation` and a channel that
    /// moves `path` of `node` with it. A curve without keys moves nothing.
    template <typename Value>
    void addChannel(Json& animation, const Curve<Value>& curve, uint32_t node, std::string_view path) {
        const std::span<const Keyframe<Value>> keys = curve.keys();
        if (keys.empty()) {
            return;
        }
        const Interpolation interpolation = keys.front().interpolation;
        const auto width = static_cast<size_t>(Value::length());
        std::vector<float> times;
        std::vector<float> values;
        const auto append = [&](const Value& value) {
            for (glm::length_t component = 0; component < Value::length(); ++component) {
                values.push_back(value[component]);
            }
        };
        for (const Keyframe<Value>& key : keys) {
            times.push_back(key.time);
            // A cubic spline has a slope in, a value and a slope out for every key.
            if (interpolation == Interpolation::Cubic) {
                append(key.inTangent);
                append(key.value);
                append(key.outTangent);
            } else {
                append(key.value);
            }
        }

        // The times of a sampler have to say how far they reach.
        Json input = Json::object();
        input["bufferView"] = addView(std::as_bytes(std::span(times)));
        input["componentType"] = kFloatType;
        input["count"] = times.size();
        input["type"] = "SCALAR";
        input["min"] = floats({times.front()});
        input["max"] = floats({times.back()});

        Json sampler = Json::object();
        sampler["input"] = addAccessor(std::move(input));
        sampler["output"] = addFloats(values, values.size() / width, width == 4 ? "VEC4" : "VEC3");
        if (interpolation == Interpolation::Step) {
            sampler["interpolation"] = "STEP";
        } else if (interpolation == Interpolation::Cubic) {
            sampler["interpolation"] = "CUBICSPLINE";
        } else {
            sampler["interpolation"] = "LINEAR";
        }

        Json channel = Json::object();
        channel["sampler"] = animation["samplers"].size();
        channel["target"]["node"] = node;
        channel["target"]["path"] = path;
        animation["samplers"].push_back(std::move(sampler));
        animation["channels"].push_back(std::move(channel));
    }

    void addAnimations() {
        if (m_document.animations.empty()) {
            return;
        }
        Json animations = Json::array();
        for (const GltfAnimation& source : m_document.animations) {
            Json animation = Json::object();
            animation["name"] = source.animation.name;
            animation["samplers"] = Json::array();
            animation["channels"] = Json::array();
            for (size_t index = 0; index < source.animation.tracks.size(); ++index) {
                const AnimationTrack& track = source.animation.tracks[index];
                addChannel(animation, track.translation, source.nodes[index], "translation");
                addChannel(animation, track.rotation, source.nodes[index], "rotation");
                addChannel(animation, track.scale, source.nodes[index], "scale");
            }
            animations.push_back(std::move(animation));
        }
        m_root["animations"] = std::move(animations);
    }

    /// The glTF texture for an image as a material samples it; one per distinct pair.
    uint32_t textureFor(uint32_t image, const MaterialData& material) {
        Json sampler = Json::object();
        const bool nearest = material.filter == TextureFilter::Nearest;
        sampler["magFilter"] = nearest ? kNearestFilter : kLinearFilter;
        sampler["minFilter"] = nearest ? kNearestFilter : kLinearMipmapLinearFilter;
        sampler["wrapS"] = wrapCode(material.wrapU);
        sampler["wrapT"] = wrapCode(material.wrapV);

        Json texture = Json::object();
        texture["sampler"] = indexOrAppend(m_samplers, std::move(sampler));
        texture["source"] = image;
        return indexOrAppend(m_textures, std::move(texture));
    }

    /// A reference to a texture from a material. Every texture carries the material's
    /// coordinate set, scale and offset - except an occlusion texture that uses the
    /// other set of coordinates, which is used as it is.
    Json textureReference(uint32_t image, const MaterialData& material, bool isOcclusion) {
        Json reference = Json::object();
        reference["index"] = textureFor(image, material);
        const int32_t channel = isOcclusion ? material.occlusionUvChannel : material.uvChannel;
        if (channel != 0) {
            reference["texCoord"] = channel;
        }
        const bool laidOutLikeTheOthers = channel == material.uvChannel;
        if (laidOutLikeTheOthers && (material.uvScale != Vec2(1.0f) || material.uvOffset != Vec2(0.0f))) {
            useExtension("KHR_texture_transform");
            Json transform = Json::object();
            transform["offset"] = floats({material.uvOffset.x, material.uvOffset.y});
            transform["scale"] = floats({material.uvScale.x, material.uvScale.y});
            reference["extensions"]["KHR_texture_transform"] = std::move(transform);
        }
        return reference;
    }

    void addMaterials() {
        if (m_document.materials.empty()) {
            return;
        }
        Json materials = Json::array();
        for (const GltfMaterial& source : m_document.materials) {
            const MaterialData& data = source.material;
            const GltfMaterialImages& images = source.images;

            Json pbr = Json::object();
            pbr["baseColorFactor"] = floats({data.baseColor.r, data.baseColor.g, data.baseColor.b, data.baseColor.a});
            pbr["metallicFactor"] = jsonFromFloat(data.metallic);
            pbr["roughnessFactor"] = jsonFromFloat(data.roughness);
            if (images.baseColor) {
                pbr["baseColorTexture"] = textureReference(*images.baseColor, data, false);
            }
            if (images.metallicRoughness) {
                pbr["metallicRoughnessTexture"] = textureReference(*images.metallicRoughness, data, false);
            }

            Json material = Json::object();
            material["name"] = source.name;
            material["pbrMetallicRoughness"] = std::move(pbr);
            if (images.normal) {
                material["normalTexture"] = textureReference(*images.normal, data, false);
                material["normalTexture"]["scale"] = jsonFromFloat(data.normalScale);
            }
            if (images.occlusion) {
                material["occlusionTexture"] = textureReference(*images.occlusion, data, true);
                material["occlusionTexture"]["strength"] = jsonFromFloat(data.occlusionStrength);
            }
            if (images.emissive) {
                material["emissiveTexture"] = textureReference(*images.emissive, data, false);
            }

            // glTF limits the emissive factor to 1; anything brighter goes into the strength.
            const float peak = std::max({data.emissiveColor.r, data.emissiveColor.g, data.emissiveColor.b, 1.0f});
            const Vec3 emissiveFactor = data.emissiveColor / peak;
            const float emissiveStrength = data.emissiveIntensity * peak;
            material["emissiveFactor"] = floats({emissiveFactor.r, emissiveFactor.g, emissiveFactor.b});
            if (emissiveStrength != 1.0f) {
                useExtension("KHR_materials_emissive_strength");
                material["extensions"]["KHR_materials_emissive_strength"]["emissiveStrength"] =
                    jsonFromFloat(emissiveStrength);
            }

            // The reflectivity of a non-metal is expressed as the index of refraction
            // that produces it.
            if (data.specular != 0.5f) {
                const float root = std::sqrt(0.08f * data.specular);
                useExtension("KHR_materials_ior");
                material["extensions"]["KHR_materials_ior"]["ior"] = jsonFromFloat((1.0f + root) / (1.0f - root));
            }

            material["alphaMode"] = alphaModeName(data.alphaMode);
            if (data.alphaMode == AlphaMode::Masked) {
                material["alphaCutoff"] = jsonFromFloat(data.alphaCutoff);
            }
            material["doubleSided"] = data.doubleSided;
            materials.push_back(std::move(material));
        }
        m_root["materials"] = std::move(materials);
    }

    void addImages() {
        if (m_document.images.empty()) {
            return;
        }
        Json images = Json::array();
        for (const GltfImage& source : m_document.images) {
            Json image = Json::object();
            image["name"] = source.name;
            if (source.isEmbedded()) {
                image["bufferView"] = addView(source.encoded);
                image["mimeType"] = imageMediaType(source.encoded).value_or("image/png");
            } else {
                image["uri"] = encodeUri(source.uri);
            }
            images.push_back(std::move(image));
        }
        m_root["images"] = std::move(images);
    }

    const GltfDocument& m_document;
    Json m_root;
    Json m_cameras = Json::array();
    Json m_textures = Json::array();
    Json m_samplers = Json::array();
    Json m_accessors = Json::array();
    Json m_bufferViews = Json::array();
    std::vector<std::string> m_extensions;
    BinaryWriter m_buffer;
};

/// Appends a .glb chunk: its length, its type, its bytes, and padding to four bytes.
void writeChunk(BinaryWriter& writer, uint32_t type, std::span<const std::byte> bytes, std::byte padding) {
    const size_t paddedSize = (bytes.size() + 3) / 4 * 4;
    writer.write(static_cast<uint32_t>(paddedSize));
    writer.write(type);
    writer.writeBytes(bytes);
    for (size_t index = bytes.size(); index < paddedSize; ++index) {
        writer.write(padding);
    }
}

} // namespace

Status exportGltf(const GltfDocument& document, const std::filesystem::path& path) {
    const std::string extension = toLowerAscii(pathToUtf8(path.extension()));
    if (extension != ".gltf" && extension != ".glb") {
        return makeError(ErrorCode::InvalidArgument, "cannot export {}: the file name must end in .gltf or .glb",
                         pathToUtf8(path));
    }
    if (const Status valid = validateDocument(document); !valid) {
        return tl::unexpected(valid.error().withContext(fmt::format("cannot export {}", pathToUtf8(path))));
    }

    GltfBuilder builder(document);
    builder.build();

    if (extension == ".gltf") {
        std::filesystem::path binaryPath = path;
        binaryPath.replace_extension(".bin");
        builder.finishBuffer(encodeUri(pathToUtf8(binaryPath.filename())));
        if (!builder.buffer().empty()) {
            CORDITE_TRY(writeBinaryFile(binaryPath, builder.buffer()));
        }
        return writeJsonFile(path, builder.json());
    }

    builder.finishBuffer({});
    const std::string json = dumpJson(builder.json(), -1);
    const std::span<const std::byte> binary = builder.buffer();

    constexpr size_t kHeaderSize = 12;
    constexpr size_t kChunkHeaderSize = 8;
    const size_t jsonSize = (json.size() + 3) / 4 * 4;
    const size_t binarySize = (binary.size() + 3) / 4 * 4;
    const size_t totalSize =
        kHeaderSize + kChunkHeaderSize + jsonSize + (binary.empty() ? 0 : kChunkHeaderSize + binarySize);
    if (totalSize > 0xFFFFFFFFull) {
        return makeError(ErrorCode::OutOfRange, "cannot export {}: a .glb file cannot exceed 4 GiB; use .gltf",
                         pathToUtf8(path));
    }

    BinaryWriter writer;
    writer.write(kGlbMagic);
    writer.write(kGlbVersion);
    writer.write(static_cast<uint32_t>(totalSize));
    writeChunk(writer, kGlbJsonChunk, std::as_bytes(std::span(json)), std::byte{' '});
    if (!binary.empty()) {
        writeChunk(writer, kGlbBinaryChunk, binary, std::byte{0});
    }
    return writeBinaryFile(path, writer.data());
}

} // namespace cordite
