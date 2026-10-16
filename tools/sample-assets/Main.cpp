/// @file
/// Generates the textures, models and sounds of the showcase project.
///
/// The engine ships no artwork, and its sample must not depend on any: everything in
/// samples/showcase/assets that is not text is made by this program, from formulas. Run
/// it after changing what it makes, look at the result, and commit it:
///
///     cordite-sample-assets samples/showcase/assets
///
/// It writes `textures/*.png` - the textures of the materials, and the lookup table that
/// grades the picture -, `textures/sky.hdr` - the sky that surrounds the scene and
/// lights it -, `models/*` and `sounds/*.wav`. The materials and the scene of the
/// showcase are written by hand and refer to these files by the ids in their `.meta`
/// files, which the editor creates and which stay the same when the files are
/// regenerated.

#include "cordite/assets/Animation.h"
#include "cordite/assets/GltfDocument.h"
#include "cordite/assets/GltfExporter.h"
#include "cordite/assets/Image.h"
#include "cordite/assets/Mesh.h"
#include "cordite/assets/MeshProcessing.h"
#include "cordite/assets/Primitives.h"
#include "cordite/assets/Skeleton.h"
#include "cordite/assets/Sound.h"
#include "cordite/core/Assert.h"
#include "cordite/core/FileSystem.h"
#include "cordite/core/Log.h"
#include "cordite/math/Color.h"
#include "cordite/math/Curve.h"
#include "cordite/math/Equirectangular.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <functional>
#include <numbers>
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

/// Grime for decals: dark, oily blotches with rust in them and a crack wandering through.
/// It fades out well before the edges of the image, ragged, so that a decal of it has no
/// border, and the image is never repeated.
SurfacePoint grime(Vec2 uv) {
    const float fromMiddle = glm::distance(uv, Vec2(0.5f)) * 2.0f;
    const float rim =
        1.0f - glm::smoothstep(0.4f, 0.8f, fromMiddle + 0.3f * (fractalNoise(uv, IVec2(5), 4, 131) - 0.5f));
    const float blotches = glm::smoothstep(0.38f, 0.62f, fractalNoise(uv, IVec2(6), 5, 137));
    const float wobble = 0.16f * (fractalNoise(uv, IVec2(4, 1), 4, 149) - 0.5f);
    const float crack = (1.0f - glm::smoothstep(0.003f, 0.008f, std::abs(uv.y - 0.5f - wobble))) *
                        (1.0f - glm::smoothstep(0.55f, 0.75f, fromMiddle));
    const float rust = glm::smoothstep(0.55f, 0.75f, fractalNoise(uv, IVec2(10), 4, 151));

    SurfacePoint point;
    point.baseColor = glm::mix(Vec3(0.035f, 0.03f, 0.025f), Vec3(0.17f, 0.07f, 0.03f), rust);
    point.opacity = std::max(rim * (0.55f + 0.45f * blotches), crack);
    point.height = -0.006f * crack;
    // Oil is shinier than what it lies on; rust is not.
    point.roughness = glm::mix(0.28f, 0.6f, rust);
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

// --- The animated model ------------------------------------------------------------------

/// How far apart the joints of the hoist's cable are, how many of them there are, and
/// how long the cable is: it ends one joint's length below the last joint.
constexpr float kCableJointSpacing = 0.25f;
constexpr uint32_t kCableJoints = 6;
constexpr float kCableLength = kCableJointSpacing * static_cast<float>(kCableJoints);
/// Where the cable hangs from: the end of the hoist's arm.
constexpr Vec3 kCableAnchor(1.0f, 2.5f, 0.0f);

/// How a point of the cable follows its joints, `distance` metres below where the cable
/// hangs from: between the joint above it and the joint below, by how near each is.
/// That is what lets a cable of straight pieces hang in a curve.
MeshSkinVertex cableSkinAt(float distance) {
    const float along = std::clamp(distance / kCableJointSpacing, 0.0f, static_cast<float>(kCableJoints));
    const uint32_t above = std::min(static_cast<uint32_t>(along), kCableJoints - 1);
    const float share = along - static_cast<float>(above);
    MeshSkinVertex skin;
    if (share <= 0.0f || above + 1 >= kCableJoints) {
        // On a joint, or below the last one: that joint alone.
        skin.joints = {static_cast<uint16_t>(above), 0, 0, 0};
    } else {
        skin.joints = {static_cast<uint16_t>(above), static_cast<uint16_t>(above + 1), 0, 0};
        skin.weights = {1.0f - share, share, 0.0f, 0.0f};
    }
    return skin;
}

/// The cable: a thin tube straight down from the anchor, in rings close enough for it
/// to bend smoothly, open at both ends - one disappears in the arm, the other in the
/// weight.
void appendCable(std::vector<MeshVertex>& vertices, std::vector<MeshSkinVertex>& skin, std::vector<uint32_t>& indices) {
    constexpr float kRadius = 0.018f;
    constexpr uint32_t kSides = 8;
    constexpr uint32_t kRings = 24;
    const auto first = static_cast<uint32_t>(vertices.size());
    for (uint32_t ring = 0; ring <= kRings; ++ring) {
        const float distance = kCableLength * static_cast<float>(ring) / static_cast<float>(kRings);
        // One more vertex than sides: the first again, with the other end of the texture.
        for (uint32_t side = 0; side <= kSides; ++side) {
            // The last column is the first, worked out from the same number: the sine of
            // a float two pi is not zero, and the seam would be a crack.
            const float angle = kTwoPi * static_cast<float>(side % kSides) / static_cast<float>(kSides);
            const Vec3 outward(std::cos(angle), 0.0f, std::sin(angle));
            MeshVertex vertex;
            vertex.position = kCableAnchor + outward * kRadius - Vec3(0.0f, distance, 0.0f);
            vertex.normal = outward;
            vertex.uv0 = Vec2(static_cast<float>(side) / static_cast<float>(kSides), distance / kCableLength);
            vertices.push_back(vertex);
            skin.push_back(cableSkinAt(distance));
        }
    }
    for (uint32_t ring = 0; ring < kRings; ++ring) {
        for (uint32_t side = 0; side < kSides; ++side) {
            const uint32_t upper = first + ring * (kSides + 1) + side;
            const uint32_t lower = upper + kSides + 1;
            // Counter-clockwise seen from outside.
            for (const uint32_t index : {upper, upper + 1, lower, upper + 1, lower + 1, lower}) {
                indices.push_back(index);
            }
        }
    }
}

/// A rotation as the numbers of a key: x, y, z, w.
Vec4 rotationKey(const Quat& rotation) {
    return {rotation.x, rotation.y, rotation.z, rotation.w};
}

/// A hoist: a post with an arm, and a cable with a weight on it that hangs from the end
/// of the arm and swings. The frame is a mesh like any other; the cable is skinned to a
/// chain of joints, and two clips move them.
Result<GltfDocument> makeHoist() {
    GltfDocument document;
    GltfMaterial paint;
    paint.name = "Hoist paint";
    paint.material.baseColor = Vec4(0.52f, 0.36f, 0.05f, 1.0f);
    paint.material.metallic = 0.0f;
    paint.material.roughness = 0.55f;
    GltfMaterial steel;
    steel.name = "Cable steel";
    steel.material.baseColor = Vec4(0.42f, 0.42f, 0.44f, 1.0f);
    steel.material.metallic = 1.0f;
    steel.material.roughness = 0.38f;
    GltfMaterial weightPaint;
    weightPaint.name = "Weight paint";
    weightPaint.material.baseColor = Vec4(0.45f, 0.06f, 0.04f, 1.0f);
    weightPaint.material.metallic = 0.0f;
    weightPaint.material.roughness = 0.5f;
    document.materials = {paint, steel, weightPaint};

    // The frame: a foot, a post, and an arm that reaches out over what is to be lifted.
    GltfMesh frame;
    frame.mesh.name = "Frame";
    std::vector<MeshVertex> frameVertices;
    std::vector<uint32_t> frameIndices;
    appendPart(frameVertices, frameIndices, makeBox(Vec3(0.5f, 0.04f, 0.5f)), Vec3(0.0f, 0.02f, 0.0f));
    appendPart(frameVertices, frameIndices, makeBox(Vec3(0.12f, 2.6f, 0.12f)), Vec3(0.0f, 1.3f, 0.0f));
    appendPart(frameVertices, frameIndices, makeBox(Vec3(1.16f, 0.1f, 0.1f)), Vec3(0.52f, 2.55f, 0.0f));
    appendSubmesh(frame.mesh, frameVertices, frameIndices);
    frame.materials = {0};

    // The cable, and at its end a weight, which follows the last joint alone.
    GltfMesh cable;
    cable.mesh.name = "Cable";
    std::vector<MeshVertex> cableVertices;
    std::vector<MeshSkinVertex> cableSkin;
    std::vector<uint32_t> cableIndices;
    appendCable(cableVertices, cableSkin, cableIndices);
    appendSubmesh(cable.mesh, cableVertices, cableSkin, cableIndices);

    constexpr float kWeightHeight = 0.2f;
    const MeshData weight = makeCylinder(0.07f, kWeightHeight, 16);
    std::vector<MeshVertex> weightVertices;
    std::vector<uint32_t> weightIndices;
    appendPart(weightVertices, weightIndices, weight,
               kCableAnchor - Vec3(0.0f, kCableLength + 0.5f * kWeightHeight, 0.0f));
    const std::vector<MeshSkinVertex> weightSkin(weightVertices.size(), cableSkinAt(kCableLength));
    appendSubmesh(cable.mesh, weightVertices, weightSkin, weightIndices);
    // The tube was given no tangents; the cylinder's go along with being worked out anew.
    generateTangents(cable.mesh);
    cable.materials = {1, 2};
    document.meshes = {frame, cable};

    // The nodes: the frame; the cable, whose mesh is placed by its joints wherever its
    // node is; and, under the frame, the point the cable hangs from, with the chain of
    // joints below it. That point is no joint, so where it is becomes part of where the
    // first joint rests.
    constexpr uint32_t kFrameNode = 0;
    constexpr uint32_t kCableNode = 1;
    constexpr uint32_t kAnchorNode = 2;
    constexpr uint32_t kFirstJointNode = 3;
    document.nodes.resize(kFirstJointNode + kCableJoints);
    document.nodes[kFrameNode] = {.name = "Frame", .children = {kAnchorNode}, .mesh = 0};
    document.nodes[kCableNode] = {.name = "Cable", .mesh = 1, .skin = 0};
    document.nodes[kAnchorNode] = {.name = "Cable anchor",
                                   .transform = {.position = kCableAnchor},
                                   .parent = kFrameNode,
                                   .children = {kFirstJointNode}};
    std::vector<uint32_t> joints;
    std::vector<Mat4> inverseBind;
    for (uint32_t joint = 0; joint < kCableJoints; ++joint) {
        const uint32_t node = kFirstJointNode + joint;
        GltfNode& jointNode = document.nodes[node];
        jointNode.name = fmt::format("Cable {}", joint + 1);
        jointNode.parent = joint == 0 ? kAnchorNode : node - 1;
        if (joint > 0) {
            jointNode.transform.position = Vec3(0.0f, -kCableJointSpacing, 0.0f);
        }
        if (joint + 1 < kCableJoints) {
            jointNode.children = {node + 1};
        }
        joints.push_back(node);
        // Bound where it hangs at rest: straight down from the anchor.
        const Vec3 rest = kCableAnchor - Vec3(0.0f, static_cast<float>(joint) * kCableJointSpacing, 0.0f);
        inverseBind.push_back(glm::translate(Mat4(1.0f), -rest));
    }
    document.roots = {kFrameNode, kCableNode};
    CORDITE_TRY_ASSIGN(SkeletonData skeleton, skeletonOf(document, joints, inverseBind, "Cable rig"));
    document.skins.push_back({.skeleton = std::move(skeleton), .joints = joints});

    // "Sway": the cable swings to and fro in the plane of the arm, like a pendulum, and
    // each joint lags a little behind the one above it, which makes the swing travel
    // down the cable. Sixteen keys to a swing, joined by arcs.
    constexpr uint32_t kSwayKeys = 16;
    GltfAnimation sway;
    sway.animation.name = "Sway";
    sway.animation.duration = 4.0f;
    for (uint32_t joint = 0; joint < kCableJoints; ++joint) {
        AnimationTrack track;
        track.joint = document.nodes[kFirstJointNode + joint].name;
        // The top joint carries the swing; the others add a bend that lags.
        const float amplitude = toRadians(joint == 0 ? 11.0f : 3.0f);
        const float lag = 0.45f * static_cast<float>(joint);
        for (uint32_t key = 0; key <= kSwayKeys; ++key) {
            // The last key is the first again, worked out from the same number, so that
            // the swing closes without a jump.
            const float phase = kTwoPi * static_cast<float>(key % kSwayKeys) / static_cast<float>(kSwayKeys);
            const float time = sway.animation.duration * static_cast<float>(key) / static_cast<float>(kSwayKeys);
            const Quat turn = glm::angleAxis(amplitude * std::sin(phase - lag), Vec3(0.0f, 0.0f, 1.0f));
            track.rotation.addKey(time, rotationKey(turn));
        }
        sway.animation.tracks.push_back(std::move(track));
        sway.nodes.push_back(kFirstJointNode + joint);
    }

    // "Swing": a slower swing across the arm, of the top of the cable alone, with keys
    // only where it turns and where it passes the middle, joined by a spline.
    GltfAnimation swing;
    swing.animation.name = "Swing";
    swing.animation.duration = 6.0f;
    AnimationTrack across;
    across.joint = document.nodes[kFirstJointNode].name;
    const std::array<float, 5> degrees = {0.0f, 9.0f, 0.0f, -9.0f, 0.0f};
    for (size_t key = 0; key < degrees.size(); ++key) {
        const float time = swing.animation.duration * static_cast<float>(key) / static_cast<float>(degrees.size() - 1);
        const Quat turn = glm::angleAxis(toRadians(degrees[key]), Vec3(1.0f, 0.0f, 0.0f));
        across.rotation.addKey(time, rotationKey(turn), Interpolation::Cubic);
    }
    across.rotation.computeSmoothTangents();
    swing.animation.tracks.push_back(std::move(across));
    swing.nodes.push_back(kFirstJointNode);

    document.animations = {sway, swing};
    return document;
}

// --- The color lookup table --------------------------------------------------------------

/// The lookup table has this many slices, each as many pixels wide and high.
constexpr uint32_t kLutSize = 16;

/// The grade of the showcase: what becomes of a color of the finished picture, as a
/// display has it. Muted - every color a little nearer to grey - with shadows towards
/// blue-green and highlights towards amber, and a shade more contrast: the look of a
/// console game of the late 2000s, where the engine's default look comes from.
Vec3 gradeColor(const Vec3& color) {
    const float brightness = luminance(color);
    Vec3 graded = glm::mix(Vec3(brightness), color, 0.85f);
    const Vec3 shadowTint(0.93f, 1.0f, 1.05f);
    const Vec3 highlightTint(1.04f, 1.0f, 0.94f);
    graded *= glm::mix(shadowTint, highlightTint, glm::smoothstep(0.0f, 1.0f, brightness));
    // A quarter of the way to a curve that is steeper in the middle and flatter at the ends.
    graded = glm::clamp(graded, Vec3(0.0f), Vec3(1.0f));
    graded = glm::mix(graded, graded * graded * (Vec3(3.0f) - 2.0f * graded), 0.25f);
    return graded;
}

/// The grade as a lookup table: slices side by side, red across each, green down it,
/// blue from slice to slice. A PostProcess component takes it as `colorLut`.
Image makeGradeLut() {
    Result<Image> created = Image::create(kLutSize * kLutSize, kLutSize, ImageFormat::Rgba8);
    CORDITE_VERIFY(created.has_value(), "cannot allocate the lookup table");
    Image image = std::move(*created);
    const auto last = static_cast<float>(kLutSize - 1);
    for (uint32_t y = 0; y < kLutSize; ++y) {
        for (uint32_t x = 0; x < kLutSize * kLutSize; ++x) {
            // Red across a slice, green down it, blue from slice to slice.
            const uint32_t slice = x / kLutSize;
            const Vec3 color(static_cast<float>(x % kLutSize) / last, static_cast<float>(y) / last,
                             static_cast<float>(slice) / last);
            image.setPixel(x, y, Vec4(gradeColor(color), 1.0f));
        }
    }
    return image;
}

// --- The sky -----------------------------------------------------------------------------

/// The image of the sky is this many pixels all the way round, and half as many from
/// straight up to straight down.
constexpr uint32_t kSkyWidth = 512;

/// How high the sun stands in the image of the sky, in degrees. It is in the middle of
/// the image - straight ahead, along -Z - and as high as the sun of the showcase's scene
/// shines from; the scene turns the image until the two are in the same place.
constexpr float kSunElevationDegrees = 53.0f;

/// The light of the showcase's sky from one direction, a unit vector: the end of a
/// day under broken cloud. Dull blue overhead, a warm haze along the horizon, clouds
/// that are dark underneath and lit where they face the sun, and the sun itself many
/// times brighter than white - which is what an image of light can hold and a
/// picture cannot. Below the horizon is the ground far away, darker than any sky.
Vec3 skyLight(const Vec3& direction) {
    const float elevation = toRadians(kSunElevationDegrees);
    const Vec3 towardsSun(0.0f, std::sin(elevation), -std::cos(elevation));
    const float height = direction.y;
    const float facingSun = std::max(glm::dot(direction, towardsSun), 0.0f);

    // Clear air: thicker towards the horizon, and bright around the sun.
    const float towardsHorizon = std::pow(1.0f - std::clamp(height, 0.0f, 1.0f), 5.0f);
    Vec3 air = glm::mix(Vec3(0.10f, 0.15f, 0.25f), Vec3(0.50f, 0.40f, 0.30f), towardsHorizon);
    air += Vec3(1.0f, 0.72f, 0.42f) * (0.30f * std::pow(facingSun, 8.0f) + 2.0f * std::pow(facingSun, 96.0f));

    // A sheet of cloud high above, seen from below: where a direction meets it. Towards
    // the horizon the sheet is far away, its clouds small and close together.
    const Vec2 onSheet = Vec2(direction.x, direction.z) / std::max(height, 0.05f);
    const float density = fractalNoise(onSheet * 0.11f + Vec2(0.37f, 0.81f), IVec2(4, 4), 5, 91);
    const float cover = glm::smoothstep(0.46f, 0.66f, density);
    // Dark where the sun is behind the viewer, lit where it shines through from ahead.
    const Vec3 cloud =
        glm::mix(Vec3(0.09f, 0.095f, 0.105f), Vec3(0.80f, 0.62f, 0.44f), 0.12f + 0.88f * std::pow(facingSun, 3.0f));
    // The haze swallows the clouds near the horizon, and around the sun they are thin:
    // it is seen, whatever the clouds are like elsewhere.
    const float clouded = cover * (1.0f - towardsHorizon) * (1.0f - std::pow(facingSun, 24.0f)) * 0.9f;
    Vec3 sky = glm::mix(air, cloud, clouded);
    // The sun, where no cloud is in front of it: a disc of two or three pixels.
    sky += Vec3(1.0f, 0.86f, 0.62f) * (24.0f * std::pow(facingSun, 3000.0f) * (1.0f - clouded));

    const Vec3 ground =
        glm::mix(Vec3(0.085f, 0.075f, 0.065f), Vec3(0.030f, 0.028f, 0.026f), glm::smoothstep(0.0f, 0.4f, -height));
    // No line where they meet: the haze hides it.
    return glm::mix(ground, sky, glm::smoothstep(-0.015f, 0.015f, height));
}

/// The sky as an image of everything around (cordite/math/Equirectangular.h says which
/// direction is where in it). An Environment component takes it as `texture`.
Image makeSky() {
    Result<Image> created = Image::create(kSkyWidth, kSkyWidth / 2, ImageFormat::Rgba32F);
    CORDITE_VERIFY(created.has_value(), "cannot allocate the image of the sky");
    Image image = std::move(*created);
    for (uint32_t y = 0; y < image.height(); ++y) {
        for (uint32_t x = 0; x < image.width(); ++x) {
            const Vec2 uv((static_cast<float>(x) + 0.5f) / static_cast<float>(image.width()),
                          (static_cast<float>(y) + 0.5f) / static_cast<float>(image.height()));
            image.setPixel(x, y, Vec4(skyLight(equirectDirection(uv)), 1.0f));
        }
    }
    return image;
}

// --- Sounds ------------------------------------------------------------------------------

/// Frames per second of every generated sound. Nothing in them is higher than a few
/// thousand hertz, and files of this rate are half the size.
constexpr uint32_t kSoundRate = 22050;

/// A sound of `seconds` in one channel, each of whose samples is `sample(time)`.
SoundData makeSound(double seconds, const std::function<double(double)>& sample) {
    SoundData sound{.sampleRate = kSoundRate, .channels = 1};
    const auto frames = static_cast<size_t>(std::lround(seconds * kSoundRate));
    sound.samples.reserve(frames);
    for (size_t frame = 0; frame < frames; ++frame) {
        sound.samples.push_back(static_cast<float>(sample(static_cast<double>(frame) / kSoundRate)));
    }
    return sound;
}

/// A sine of `hertz` at `time`.
double tone(double hertz, double time) {
    return std::sin(2.0 * std::numbers::pi * hertz * time);
}

/// From 0 to 1 over the first `attack` seconds of a sound of `length`, and back to 0
/// over its last `release`: what keeps a sound from beginning and ending with a click.
double edges(double time, double length, double attack, double release) {
    return std::clamp(time / attack, 0.0, 1.0) * std::clamp((length - time) / release, 0.0, 1.0);
}

/// What a lamp sounds like to somebody standing under it: mains hum and its overtones,
/// swelling a little once a second. Two seconds that go round without a seam, because
/// every part of it has a whole number of cycles in them.
SoundData makeHum() {
    return makeSound(2.0, [](double time) {
        const double hum =
            0.30 * tone(50.0, time) + 0.18 * tone(100.0, time) + 0.08 * tone(150.0, time) + 0.04 * tone(200.0, time);
        return hum * (0.85 + 0.15 * tone(1.0, time));
    });
}

/// Eight plucked notes, half a second each, that go round: every note has died away
/// before the next begins, the last one before the first.
SoundData makeTune() {
    constexpr std::array<double, 8> kNotes = {220.0, 261.63, 329.63, 440.0, 392.0, 329.63, 261.63, 329.63};
    constexpr double kNoteSeconds = 0.5;
    return makeSound(kNoteSeconds * static_cast<double>(kNotes.size()), [&](double time) {
        const auto index = std::min(static_cast<size_t>(time / kNoteSeconds), kNotes.size() - 1);
        const double since = time - static_cast<double>(index) * kNoteSeconds;
        const double pluck = std::exp(-6.0 * since) * edges(since, kNoteSeconds, 0.008, 0.03);
        return 0.35 * pluck * (tone(kNotes[index], since) + 0.3 * tone(2.0 * kNotes[index], since));
    });
}

/// Something springy leaving the ground: a tone that falls by an octave as it dies away.
SoundData makeHop() {
    constexpr double kSeconds = 0.25;
    return makeSound(kSeconds, [](double time) {
        // The pitch halves over the sound; the phase is what that adds up to.
        const double cycles = 220.0 * kSeconds / std::numbers::ln2 * (1.0 - std::pow(0.5, time / kSeconds));
        return 0.7 * std::exp(-12.0 * time) * edges(time, kSeconds, 0.003, 0.01) * tone(cycles, 1.0);
    });
}

/// A radio that calls: two short tones, the second a fifth above the first.
SoundData makeCall() {
    constexpr double kBeep = 0.15;
    return makeSound(0.4, [](double time) {
        if (time < kBeep) {
            return 0.4 * edges(time, kBeep, 0.005, 0.005) * tone(880.0, time);
        }
        const double since = time - 0.2;
        if (since >= 0.0 && since < kBeep) {
            return 0.4 * edges(since, kBeep, 0.005, 0.005) * tone(1320.0, since);
        }
        return 0.0;
    });
}

/// A click, as of a switch: a high tone that is over almost before it began.
SoundData makeClick() {
    constexpr double kSeconds = 0.05;
    return makeSound(kSeconds, [](double time) {
        return 0.6 * std::exp(-90.0 * time) * edges(time, kSeconds, 0.001, 0.005) * tone(1800.0, time);
    });
}

// --- Output ------------------------------------------------------------------------------

Status writeSound(const std::filesystem::path& file, const SoundData& sound) {
    CORDITE_TRY_ASSIGN(const std::vector<std::byte> encoded, encodeWav(sound));
    return writeBinaryFile(file, encoded);
}

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
    const std::filesystem::path soundDirectory = assetDirectory / "sounds";
    CORDITE_TRY(ensureDirectory(textureDirectory));
    CORDITE_TRY(ensureDirectory(modelDirectory));
    CORDITE_TRY(ensureDirectory(soundDirectory));

    CORDITE_TRY(writeTextures(textureDirectory, "concrete", bake(concrete), false));
    CORDITE_TRY(writeTextures(textureDirectory, "plating", bake(plating), false));
    CORDITE_TRY(writeTextures(textureDirectory, "planks", bake(planks), false));
    CORDITE_TRY(writeTextures(textureDirectory, "hazard", bake(hazardPaint), false));
    CORDITE_TRY(writeTextures(textureDirectory, "grating", bake(grating), false));
    CORDITE_TRY(writeTextures(textureDirectory, "lamp", bake(lampPanel), true));
    CORDITE_TRY(writeTextures(textureDirectory, "grime", bake(grime), false));
    CORDITE_TRY(makeGradeLut().savePng(textureDirectory / "grade_lut.png"));
    CORDITE_TRY(makeSky().saveHdr(textureDirectory / "sky.hdr"));

    CORDITE_TRY(exportGltf(makeCrate(), modelDirectory / "crate.gltf"));
    CORDITE_TRY(exportGltf(makeLampPost(), modelDirectory / "lamp_post.gltf"));
    CORDITE_TRY_ASSIGN(const GltfDocument barrel, makeBarrel(bake(barrelSide)));
    CORDITE_TRY(exportGltf(barrel, modelDirectory / "barrel.glb"));
    CORDITE_TRY_ASSIGN(const GltfDocument hoist, makeHoist());
    CORDITE_TRY(exportGltf(hoist, modelDirectory / "hoist.gltf"));

    CORDITE_TRY(writeSound(soundDirectory / "hum.wav", makeHum()));
    CORDITE_TRY(writeSound(soundDirectory / "tune.wav", makeTune()));
    CORDITE_TRY(writeSound(soundDirectory / "hop.wav", makeHop()));
    CORDITE_TRY(writeSound(soundDirectory / "call.wav", makeCall()));
    CORDITE_TRY(writeSound(soundDirectory / "click.wav", makeClick()));
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
