/// @file
/// Generates the textures and models of the showcase project.
///
/// The engine ships no artwork, and its sample must not depend on any: everything in
/// samples/showcase/assets that is not text is made by this program, from formulas. Run
/// it after changing what it makes, look at the result, and commit it:
///
///     cordite-sample-assets samples/showcase/assets
///
/// It writes `textures/*.png` and `models/*`. The materials and the scene of the showcase
/// are written by hand and refer to these files by the ids in their `.meta` files, which
/// the editor creates and which stay the same when the files are regenerated.

#include "cordite/assets/GltfExporter.h"
#include "cordite/assets/Image.h"
#include "cordite/assets/Mesh.h"
#include "cordite/assets/MeshProcessing.h"
#include "cordite/assets/Primitives.h"
#include "cordite/core/Assert.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Log.h"
#include "cordite/math/Color.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace cordite;

/// Side of every generated texture, in pixels.
constexpr uint32_t kTextureSize = 256;

/// `value` brought into [0, length) by whole multiples of `length`: where a coordinate
/// lands in something that repeats.
int32_t wrapIndex(int32_t value, int32_t length) {
    return ((value % length) + length) % length;
}

// --- Noise -------------------------------------------------------------------------------

/// A number in [0, 1) that depends on a lattice point and a seed, and on nothing else.
float latticeValue(int32_t x, int32_t y, uint32_t seed) {
    uint32_t hash =
        static_cast<uint32_t>(x) * 0x8da6b343u ^ static_cast<uint32_t>(y) * 0xd8163841u ^ seed * 0xcb1ab31fu;
    hash ^= hash >> 15;
    hash *= 0x2c1b3c6du;
    hash ^= hash >> 12;
    hash *= 0x297a2d39u;
    hash ^= hash >> 15;
    return static_cast<float>(hash >> 8) / static_cast<float>(1u << 24);
}

/// Smooth noise in [0, 1] that repeats every `period.x` units across and every
/// `period.y` units down, so that a texture made from it tiles.
float tilingNoise(Vec2 point, IVec2 period, uint32_t seed) {
    const Vec2 cell = glm::floor(point);
    const Vec2 within = point - cell;
    const Vec2 blend = within * within * (3.0f - 2.0f * within);
    const int32_t x0 = wrapIndex(static_cast<int32_t>(cell.x), period.x);
    const int32_t y0 = wrapIndex(static_cast<int32_t>(cell.y), period.y);
    const int32_t x1 = wrapIndex(x0 + 1, period.x);
    const int32_t y1 = wrapIndex(y0 + 1, period.y);
    const float top = glm::mix(latticeValue(x0, y0, seed), latticeValue(x1, y0, seed), blend.x);
    const float bottom = glm::mix(latticeValue(x0, y1, seed), latticeValue(x1, y1, seed), blend.x);
    return glm::mix(top, bottom, blend.y);
}

/// Several octaves of tilingNoise: large shapes with ever finer detail on top. `uv` is in
/// [0, 1]; `frequency` is how many lattice cells the first octave has across and down
/// the texture, in whole numbers.
float fractalNoise(Vec2 uv, IVec2 frequency, int octaves, uint32_t seed) {
    float sum = 0.0f;
    float weight = 0.5f;
    float total = 0.0f;
    int32_t scale = 1;
    for (int octave = 0; octave < octaves; ++octave) {
        // Each octave is scaled by a whole number, so that it still tiles.
        const IVec2 cells = frequency * scale;
        const float sample = tilingNoise(uv * Vec2(cells), cells, seed + static_cast<uint32_t>(octave));
        sum += sample * weight;
        total += weight;
        weight *= 0.5f;
        scale *= 2;
    }
    return sum / total;
}

// --- Surfaces ----------------------------------------------------------------------------

/// What a material looks like at one point of its texture.
struct SurfacePoint {
    /// Linear color.
    Vec3 baseColor{0.5f};
    float opacity = 1.0f;
    /// Relative height, in texture widths: bumps are positive, grooves negative.
    float height = 0.0f;
    float roughness = 0.5f;
    float metallic = 0.0f;
    /// Linear color of emitted light.
    Vec3 emissive{0.0f};
};

using Surface = std::function<SurfacePoint(Vec2 uv)>;

/// 1 inside a groove of the given half width around a line every `spacing`, 0 away from it.
float groove(float coordinate, float spacing, float halfWidth) {
    const float distance = std::abs(std::fmod(coordinate + spacing * 0.5f, spacing) - spacing * 0.5f);
    return 1.0f - glm::smoothstep(halfWidth * 0.5f, halfWidth, distance);
}

/// A round bump of the given radius centred on `centre`: 1 at the top, 0 at the rim.
float dome(Vec2 uv, Vec2 centre, float radius) {
    const float distance = glm::distance(uv, centre) / radius;
    return distance >= 1.0f ? 0.0f : std::sqrt(1.0f - distance * distance);
}

/// Poured concrete in four slabs: rough, grey, with darker seams and stains.
SurfacePoint concrete(Vec2 uv) {
    const float seams = std::max(groove(uv.x, 0.5f, 0.012f), groove(uv.y, 0.5f, 0.012f));
    const float grain = fractalNoise(uv, IVec2(24), 4, 11);
    const float stains = glm::smoothstep(0.52f, 0.72f, fractalNoise(uv, IVec2(3), 4, 23));

    SurfacePoint point;
    const float shade = 0.20f + 0.07f * fractalNoise(uv, IVec2(6), 5, 5) + 0.03f * grain;
    point.baseColor = Vec3(shade, shade * 0.99f, shade * 0.96f) * (1.0f - 0.35f * stains) * (1.0f - 0.5f * seams);
    point.height = 0.004f * grain - 0.010f * seams;
    point.roughness = 0.82f - 0.12f * stains + 0.08f * grain;
    return point;
}

/// Steel plates, four to a texture, with bevelled edges, rivets, scratches and rust
/// creeping in from the edges.
SurfacePoint plating(Vec2 uv) {
    const float edge = std::max(groove(uv.x, 0.5f, 0.016f), groove(uv.y, 0.5f, 0.016f));

    // A rivet near each corner of each plate.
    const Vec2 inPlate = glm::fract(uv * 2.0f);
    float rivet = 0.0f;
    for (const Vec2 corner : {Vec2(0.12f, 0.12f), Vec2(0.88f, 0.12f), Vec2(0.12f, 0.88f), Vec2(0.88f, 0.88f)}) {
        rivet = std::max(rivet, dome(inPlate, corner, 0.045f));
    }

    const float scratches = fractalNoise(uv, IVec2(4, 64), 3, 31);
    const float wear = fractalNoise(uv, IVec2(5), 5, 47);
    // Rust where the noise is high, and more of it near the edges of a plate.
    const Vec2 fromEdge = glm::abs(inPlate - 0.5f) * 2.0f;
    const float nearEdge = glm::smoothstep(0.55f, 1.0f, std::max(fromEdge.x, fromEdge.y));
    const float rust = glm::smoothstep(0.60f, 0.76f, wear + 0.24f * nearEdge) * (1.0f - rivet);

    SurfacePoint point;
    const Vec3 steel = Vec3(0.24f, 0.26f, 0.29f) * (0.85f + 0.3f * scratches);
    const Vec3 rustColor =
        glm::mix(Vec3(0.09f, 0.035f, 0.018f), Vec3(0.19f, 0.085f, 0.035f), fractalNoise(uv, IVec2(32), 3, 59));
    point.baseColor = glm::mix(steel, rustColor, rust) * (1.0f - 0.6f * edge);
    point.height = 0.012f * rivet - 0.014f * edge + 0.0015f * scratches + 0.003f * rust * wear;
    point.roughness = glm::mix(0.32f + 0.2f * scratches, 0.85f, rust);
    point.metallic = 1.0f - rust;
    return point;
}

/// Four weathered planks, with grain along their length, gaps between them and a nail
/// at each end.
SurfacePoint planks(Vec2 uv) {
    const float plank = std::floor(uv.y * 4.0f);
    const float gap = groove(uv.y, 0.25f, 0.010f);
    // Grain: noise stretched along the plank, different on every plank.
    const float grain = fractalNoise(uv + Vec2(plank * 0.37f, 0.0f), IVec2(3, 48), 4, 71);
    const float tone = latticeValue(static_cast<int32_t>(plank), 3, 83);

    const float inPlankV = glm::fract(uv.y * 4.0f);
    float nail = 0.0f;
    for (const float u : {0.06f, 0.94f}) {
        nail = std::max(nail, dome(Vec2(uv.x, inPlankV * 0.25f), Vec2(u, 0.125f), 0.012f));
    }

    SurfacePoint point;
    const Vec3 wood = glm::mix(Vec3(0.20f, 0.11f, 0.05f), Vec3(0.36f, 0.22f, 0.11f), 0.35f * tone + 0.65f * grain);
    point.baseColor = glm::mix(wood * (1.0f - 0.7f * gap), Vec3(0.25f, 0.25f, 0.27f), nail);
    point.height = 0.003f * grain - 0.012f * gap + 0.004f * nail;
    point.roughness = glm::mix(0.62f + 0.2f * grain, 0.35f, nail);
    point.metallic = nail;
    return point;
}

/// Yellow and black warning stripes, chipped where the paint has worn off.
SurfacePoint hazardPaint(Vec2 uv) {
    const float stripe = glm::smoothstep(0.47f, 0.53f, std::abs(glm::fract((uv.x + uv.y) * 4.0f) * 2.0f - 1.0f));
    const float chipped = glm::smoothstep(0.62f, 0.68f, fractalNoise(uv, IVec2(12), 4, 97));

    SurfacePoint point;
    const Vec3 paint = glm::mix(Vec3(0.02f, 0.02f, 0.02f), Vec3(0.80f, 0.52f, 0.03f), stripe);
    point.baseColor = glm::mix(paint, Vec3(0.26f, 0.27f, 0.29f), chipped);
    point.height = 0.002f * (1.0f - chipped);
    point.roughness = glm::mix(0.45f, 0.30f, chipped);
    point.metallic = chipped;
    return point;
}

/// A metal grating: bars with square holes between them, cut out by the opacity.
SurfacePoint grating(Vec2 uv) {
    const Vec2 inCell = glm::abs(glm::fract(uv * 8.0f) - 0.5f) * 2.0f;
    const float fromHole = std::max(inCell.x, inCell.y);

    SurfacePoint point;
    point.opacity = glm::smoothstep(0.60f, 0.68f, fromHole);
    point.baseColor = Vec3(0.12f, 0.12f, 0.13f) * (0.8f + 0.4f * fractalNoise(uv, IVec2(16), 3, 113));
    // The bars are rounded off towards the holes.
    point.height = 0.008f * glm::smoothstep(0.60f, 0.95f, fromHole);
    point.roughness = 0.45f;
    point.metallic = 1.0f;
    return point;
}

/// A dark panel with three strips that glow.
SurfacePoint lampPanel(Vec2 uv) {
    const float strip = glm::smoothstep(0.30f, 0.26f, std::abs(glm::fract(uv.y * 3.0f) - 0.5f)) *
                        glm::smoothstep(0.02f, 0.06f, uv.x) * glm::smoothstep(0.98f, 0.94f, uv.x);

    SurfacePoint point;
    point.baseColor = glm::mix(Vec3(0.03f, 0.03f, 0.035f), Vec3(0.6f), strip);
    point.emissive = Vec3(strip);
    point.height = -0.004f * strip;
    point.roughness = glm::mix(0.5f, 0.15f, strip);
    return point;
}

/// A barrel's side: painted metal with two raised hoops and a worn band of bare steel.
SurfacePoint barrelSide(Vec2 uv) {
    const float hoops = std::max(glm::smoothstep(0.035f, 0.02f, std::abs(uv.y - 0.3f)),
                                 glm::smoothstep(0.035f, 0.02f, std::abs(uv.y - 0.7f)));
    const float worn = glm::smoothstep(0.55f, 0.65f, fractalNoise(uv, IVec2(8, 6), 5, 131) + 0.25f * hoops);

    SurfacePoint point;
    const Vec3 paint = Vec3(0.05f, 0.16f, 0.22f) * (0.8f + 0.4f * fractalNoise(uv, IVec2(4, 24), 3, 149));
    point.baseColor = glm::mix(paint, Vec3(0.33f, 0.34f, 0.36f), worn);
    point.height = 0.010f * hoops + 0.002f * worn;
    point.roughness = glm::mix(0.55f, 0.30f, worn);
    point.metallic = worn;
    return point;
}

// --- Textures ----------------------------------------------------------------------------

/// The textures of one material: what its files hold.
struct TextureSet {
    /// sRGB color, opacity in alpha.
    Image baseColor;
    /// Tangent-space normals, +Y up.
    Image normal;
    /// Occlusion in red, roughness in green, metallic in blue.
    Image packed;
    /// sRGB color.
    Image emissive;
};

Image makeImage() {
    Result<Image> image = Image::create(kTextureSize, kTextureSize, ImageFormat::Rgba8);
    CORDITE_VERIFY(image.has_value(), "cannot create a {0}x{0} image", kTextureSize);
    return std::move(*image);
}

/// Evaluates a surface at every pixel and derives what cannot be stated directly: the
/// normal from the slope of the height, the occlusion from how deep a point lies below
/// its surroundings.
TextureSet bake(const Surface& surface) {
    const auto size = static_cast<int32_t>(kTextureSize);
    const auto at = [&](int32_t x, int32_t y) {
        // Sampled at pixel centres; coordinates wrap, because the textures tile.
        return surface(Vec2((static_cast<float>(wrapIndex(x, size)) + 0.5f) / static_cast<float>(size),
                            (static_cast<float>(wrapIndex(y, size)) + 0.5f) / static_cast<float>(size)));
    };

    std::vector<float> heights(static_cast<size_t>(size) * size);
    for (int32_t y = 0; y < size; ++y) {
        for (int32_t x = 0; x < size; ++x) {
            heights[static_cast<size_t>(y) * size + x] = at(x, y).height;
        }
    }
    const auto heightAt = [&](int32_t x, int32_t y) {
        return heights[static_cast<size_t>(wrapIndex(y, size)) * size + wrapIndex(x, size)];
    };

    TextureSet textures{makeImage(), makeImage(), makeImage(), makeImage()};
    for (int32_t y = 0; y < size; ++y) {
        for (int32_t x = 0; x < size; ++x) {
            const SurfacePoint point = at(x, y);
            const auto px = static_cast<uint32_t>(x);
            const auto py = static_cast<uint32_t>(y);

            // Heights are in texture widths, so one pixel is 1 / size wide. The image's
            // rows run downwards while the normal map's Y points up.
            const float slopeX = (heightAt(x + 1, y) - heightAt(x - 1, y)) * 0.5f * static_cast<float>(size);
            const float slopeUp = (heightAt(x, y - 1) - heightAt(x, y + 1)) * 0.5f * static_cast<float>(size);
            const Vec3 normal = glm::normalize(Vec3(-slopeX, -slopeUp, 1.0f));

            // How far the point lies below the average of its surroundings.
            float around = 0.0f;
            constexpr int32_t kReach = 4;
            for (int32_t offsetY = -kReach; offsetY <= kReach; offsetY += 2) {
                for (int32_t offsetX = -kReach; offsetX <= kReach; offsetX += 2) {
                    around += heightAt(x + offsetX, y + offsetY);
                }
            }
            around /= 25.0f;
            const float occlusion = glm::clamp(1.0f - (around - point.height) * 90.0f, 0.25f, 1.0f);

            textures.baseColor.setPixel(px, py,
                                        Vec4(linearToSrgb(glm::clamp(point.baseColor, 0.0f, 1.0f)), point.opacity));
            textures.normal.setPixel(px, py, Vec4(normal * 0.5f + 0.5f, 1.0f));
            textures.packed.setPixel(px, py, Vec4(occlusion, point.roughness, point.metallic, 1.0f));
            textures.emissive.setPixel(px, py, Vec4(linearToSrgb(glm::clamp(point.emissive, 0.0f, 1.0f)), 1.0f));
        }
    }
    return textures;
}

// --- Models ------------------------------------------------------------------------------

/// Appends a part to the vertex and index lists of a submesh under construction, moved
/// by `offset`.
void appendPart(std::vector<MeshVertex>& vertices, std::vector<uint32_t>& indices, const MeshData& part,
                const Vec3& offset) {
    const auto first = static_cast<uint32_t>(vertices.size());
    for (MeshVertex vertex : part.vertices) {
        vertex.position += offset;
        vertices.push_back(vertex);
    }
    for (const uint32_t index : part.indices) {
        indices.push_back(first + index);
    }
}

/// A shipping crate: a wooden box held together by steel strips along its twelve edges.
GltfDocument makeCrate() {
    constexpr float kSize = 1.0f;
    constexpr float kStrip = 0.07f;
    const float half = 0.5f * kSize;

    GltfMesh mesh;
    mesh.mesh.name = "Crate";

    // The wood: slightly smaller than the strips, which stand proud of it.
    const MeshData body = makeBox(Vec3(kSize - 0.02f));
    appendSubmesh(mesh.mesh, body.vertices, body.indices);

    // The strips: one box along each edge of the cube.
    std::vector<MeshVertex> stripVertices;
    std::vector<uint32_t> stripIndices;
    const float inset = half - 0.5f * kStrip;
    for (const float a : {-inset, inset}) {
        for (const float b : {-inset, inset}) {
            appendPart(stripVertices, stripIndices, makeBox(Vec3(kSize, kStrip, kStrip)), Vec3(0.0f, a, b));
            appendPart(stripVertices, stripIndices, makeBox(Vec3(kStrip, kSize, kStrip)), Vec3(a, 0.0f, b));
            appendPart(stripVertices, stripIndices, makeBox(Vec3(kStrip, kStrip, kSize)), Vec3(a, b, 0.0f));
        }
    }
    appendSubmesh(mesh.mesh, stripVertices, stripIndices);
    optimizeMesh(mesh.mesh);
    mesh.materials = {0, 1};

    GltfDocument document;
    document.images = {
        {.name = "Planks color", .uri = "../textures/planks_basecolor.png"},
        {.name = "Planks normal", .uri = "../textures/planks_normal.png"},
        {.name = "Planks surface", .uri = "../textures/planks_orm.png"},
        {.name = "Plating color", .uri = "../textures/plating_basecolor.png"},
        {.name = "Plating normal", .uri = "../textures/plating_normal.png"},
        {.name = "Plating surface", .uri = "../textures/plating_orm.png"},
    };

    GltfMaterial wood;
    wood.name = "Crate wood";
    // Fully metallic and fully rough as far as the factors go: the texture decides.
    wood.material.metallic = 1.0f;
    wood.material.roughness = 1.0f;
    wood.images = {.baseColor = 0, .metallicRoughness = 2, .normal = 1, .occlusion = 2};
    GltfMaterial steel;
    steel.name = "Crate strips";
    steel.material.metallic = 1.0f;
    steel.material.roughness = 1.0f;
    steel.images = {.baseColor = 3, .metallicRoughness = 5, .normal = 4, .occlusion = 5};
    document.materials = {wood, steel};

    document.meshes.push_back(std::move(mesh));
    // The crate stands on the ground: its origin is the middle of its base.
    GltfNode node;
    node.name = "Crate";
    node.mesh = 0;
    node.transform.position = Vec3(0.0f, half, 0.0f);
    document.nodes.push_back(node);
    document.roots = {0};
    return document;
}

/// A lamp post made of three parts in a hierarchy: a pole, an arm at its top, and a
/// lamp head at the end of the arm whose underside glows.
GltfDocument makeLampPost() {
    constexpr float kHeight = 3.2f;

    GltfDocument document;
    GltfMaterial paint;
    paint.name = "Post paint";
    paint.material.baseColor = Vec4(0.05f, 0.07f, 0.06f, 1.0f);
    paint.material.metallic = 0.0f;
    paint.material.roughness = 0.45f;
    GltfMaterial glow;
    glow.name = "Lamp glow";
    glow.material.baseColor = Vec4(0.9f, 0.9f, 0.9f, 1.0f);
    glow.material.roughness = 0.3f;
    glow.material.emissiveColor = Vec3(1.0f, 0.62f, 0.28f);
    glow.material.emissiveIntensity = 6.0f;
    document.materials = {paint, glow};

    GltfMesh pole;
    pole.mesh = makeCylinder(0.06f, kHeight, 16);
    pole.mesh.name = "Pole";
    pole.materials = {0};
    GltfMesh arm;
    arm.mesh = makeBox(Vec3(0.9f, 0.06f, 0.06f));
    arm.mesh.name = "Arm";
    arm.materials = {0};

    // The head: a housing, and the glowing pane set into its underside.
    GltfMesh head;
    head.mesh.name = "Head";
    const MeshData housing = makeBox(Vec3(0.34f, 0.10f, 0.20f));
    appendSubmesh(head.mesh, housing.vertices, housing.indices);
    std::vector<MeshVertex> paneVertices;
    std::vector<uint32_t> paneIndices;
    appendPart(paneVertices, paneIndices, makeBox(Vec3(0.28f, 0.02f, 0.14f)), Vec3(0.0f, -0.055f, 0.0f));
    appendSubmesh(head.mesh, paneVertices, paneIndices);
    head.materials = {0, 1};
    document.meshes = {pole, arm, head};

    GltfNode poleNode;
    poleNode.name = "Lamp post";
    poleNode.mesh = 0;
    poleNode.transform.position = Vec3(0.0f, 0.5f * kHeight, 0.0f);
    poleNode.children = {1};
    GltfNode armNode;
    armNode.name = "Arm";
    armNode.parent = 0;
    armNode.mesh = 1;
    armNode.transform.position = Vec3(0.42f, 0.5f * kHeight - 0.05f, 0.0f);
    armNode.children = {2};
    GltfNode headNode;
    headNode.name = "Head";
    headNode.parent = 1;
    headNode.mesh = 2;
    headNode.transform.position = Vec3(0.5f, -0.06f, 0.0f);
    // Raised a little at its far end, as the head of a street lamp is, so that what
    // shines out of its underside is thrown away from the pole. It also shows that
    // rotations come through.
    headNode.transform.rotation = glm::angleAxis(toRadians(12.0f), Vec3(0.0f, 0.0f, 1.0f));
    document.nodes = {poleNode, armNode, headNode};
    document.roots = {0};
    return document;
}

/// A steel barrel in a single binary file, with its textures inside it.
Result<GltfDocument> makeBarrel(const TextureSet& textures) {
    GltfDocument document;
    CORDITE_TRY_ASSIGN(std::vector<std::byte> baseColor, textures.baseColor.encodePng());
    CORDITE_TRY_ASSIGN(std::vector<std::byte> normal, textures.normal.encodePng());
    CORDITE_TRY_ASSIGN(std::vector<std::byte> packed, textures.packed.encodePng());
    document.images = {
        {.name = "Barrel color", .encoded = std::move(baseColor)},
        {.name = "Barrel normal", .encoded = std::move(normal)},
        {.name = "Barrel surface", .encoded = std::move(packed)},
    };

    GltfMaterial material;
    material.name = "Barrel";
    material.material.metallic = 1.0f;
    material.material.roughness = 1.0f;
    // The texture goes round the barrel twice.
    material.material.uvScale = Vec2(2.0f, 1.0f);
    material.images = {.baseColor = 0, .metallicRoughness = 2, .normal = 1, .occlusion = 2};
    document.materials = {material};

    GltfMesh mesh;
    mesh.mesh = makeCylinder(0.3f, 0.9f, 24);
    mesh.mesh.name = "Barrel";
    mesh.materials = {0};
    document.meshes.push_back(std::move(mesh));

    GltfNode node;
    node.name = "Barrel";
    node.mesh = 0;
    node.transform.position = Vec3(0.0f, 0.45f, 0.0f);
    document.nodes.push_back(node);
    document.roots = {0};
    return document;
}

// --- Output ------------------------------------------------------------------------------

Status writeTextures(const std::filesystem::path& directory, std::string_view name, const TextureSet& textures,
                     bool withEmissive) {
    const auto file = [&](std::string_view suffix) {
        return directory / pathFromUtf8(fmt::format("{}_{}.png", name, suffix));
    };
    CORDITE_TRY(textures.baseColor.savePng(file("basecolor")));
    CORDITE_TRY(textures.normal.savePng(file("normal")));
    CORDITE_TRY(textures.packed.savePng(file("orm")));
    if (withEmissive) {
        CORDITE_TRY(textures.emissive.savePng(file("emissive")));
    }
    return {};
}

Status generate(const std::filesystem::path& assetDirectory) {
    const std::filesystem::path textureDirectory = assetDirectory / "textures";
    const std::filesystem::path modelDirectory = assetDirectory / "models";
    CORDITE_TRY(ensureDirectory(textureDirectory));
    CORDITE_TRY(ensureDirectory(modelDirectory));

    CORDITE_TRY(writeTextures(textureDirectory, "concrete", bake(concrete), false));
    CORDITE_TRY(writeTextures(textureDirectory, "plating", bake(plating), false));
    CORDITE_TRY(writeTextures(textureDirectory, "planks", bake(planks), false));
    CORDITE_TRY(writeTextures(textureDirectory, "hazard", bake(hazardPaint), false));
    CORDITE_TRY(writeTextures(textureDirectory, "grating", bake(grating), false));
    CORDITE_TRY(writeTextures(textureDirectory, "lamp", bake(lampPanel), true));

    CORDITE_TRY(exportGltf(makeCrate(), modelDirectory / "crate.gltf"));
    CORDITE_TRY(exportGltf(makeLampPost(), modelDirectory / "lamp_post.gltf"));
    CORDITE_TRY_ASSIGN(const GltfDocument barrel, makeBarrel(bake(barrelSide)));
    CORDITE_TRY(exportGltf(barrel, modelDirectory / "barrel.glb"));
    return {};
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            CORDITE_LOG_ERROR("Tool", "usage: cordite-sample-assets <asset directory>");
            return EXIT_FAILURE;
        }
        const std::filesystem::path assetDirectory = pathFromUtf8(argv[1]);
        if (const Status generated = generate(assetDirectory); !generated) {
            CORDITE_LOG_ERROR("Tool", "{}", generated.error().describe());
            return EXIT_FAILURE;
        }
        CORDITE_LOG_INFO("Tool", "wrote the sample assets to {}", pathToUtf8(assetDirectory));
        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        CORDITE_LOG_CRITICAL("Tool", "unhandled exception: {}", exception.what());
    } catch (...) {
        CORDITE_LOG_CRITICAL("Tool", "unhandled exception of unknown type");
    }
    return EXIT_FAILURE;
}
