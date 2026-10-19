#include "cordite/renderer/ShadowPlan.h"

#include "cordite/core/Assert.h"
#include "cordite/core/StringUtils.h"
#include "cordite/math/Projection.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <optional>
#include <span>

namespace cordite {
namespace {

/// Nearer to a point, spot or rect light than this, nothing casts a shadow.
constexpr float kLocalNearPlane = 0.05f;

/// The widest cone a spot light's shadow map covers, from its axis. A wider light
/// throws no shadows in its outermost ring; such a light is better a point light.
constexpr float kMaxSpotShadowAngle = 1.4835f; // 85 degrees

/// The share of the shadow distance over which the sun's shadows fade out.
constexpr float kFadeShare = 0.1f;

/// The share of a cascade's depth, at its far end, over which the shaders go over from
/// its map to the next one's, so that no line shows where the cascades meet. The next
/// cascade's map has to hold that much of the one before it. Matches kCascadeBlend in
/// shaders/common/Shadows.slang.
constexpr float kCascadeBlend = 0.1f;

/// The furthest a surface is looked up off its place, in texels of the shadow map: what
/// LightComponent allows for shadowBias and shadowNormalOffset.
constexpr float kMaxShadowOffset = 16.0f;

/// Bounding spheres of cascades are rounded up to a multiple of this many metres, so
/// that the last bits of a computed radius cannot make the shadows crawl.
constexpr float kRadiusStep = 1.0f / 16.0f;

/// The most shadow maps one light has: the six of a point or a rect light.
constexpr uint32_t kMaxTilesPerLight = 6;

/// `value`, or `fallback` when it is "not a number": what a light says about its
/// shadows comes from a component, which code can fill with anything. Infinities are
/// left to the limits that every such value is held to afterwards.
float numberOr(float value, float fallback) {
    return std::isnan(value) ? fallback : value;
}

/// Hands out square tiles of a square atlas. Sizes are powers of two: a free square
/// that is larger than what is asked for is cut into four, of which one is cut again.
///
/// Nothing here allocates memory - shadows are planned for every view of every frame -
/// which also makes a copy cheap: taking several tiles or none is done on a copy.
class TileAllocator {
public:
    /// An atlas whose side is not a power of two is used up to the largest one in it.
    explicit TileAllocator(uint32_t atlasSize) {
        if (atlasSize >= kMinShadowTileSize) {
            m_free[m_count++] = {UVec2(0, 0), std::bit_floor(atlasSize)};
        }
    }

    /// The top-left corner of a tile of `size` texels a side, a power of two, or nothing
    /// when no free square is large enough.
    std::optional<UVec2> allocate(uint32_t size) {
        // The smallest free square that is large enough; of several, the first.
        size_t best = m_count;
        for (size_t index = 0; index < m_count; ++index) {
            if (m_free[index].size >= size && (best == m_count || m_free[index].size < m_free[best].size)) {
                best = index;
            }
        }
        if (best == m_count || size == 0) {
            return std::nullopt;
        }

        Tile tile = m_free[best];
        std::move(m_free.begin() + best + 1, m_free.begin() + m_count, m_free.begin() + best);
        --m_count;
        while (tile.size > size) {
            // Keep the top-left quarter; the other three are free again, in its place.
            const uint32_t half = tile.size / 2;
            insert(best, {tile.origin + UVec2(half, half), half});
            insert(best, {tile.origin + UVec2(0, half), half});
            insert(best, {tile.origin + UVec2(half, 0), half});
            tile.size = half;
        }
        return tile.origin;
    }

private:
    struct Tile {
        UVec2 origin{0, 0};
        uint32_t size = 0;
    };

    /// A square is only ever cut when no smaller free one would do, so there are never
    /// more than three free squares of a size - and there are fewer sizes than a
    /// 32-bit length has bits.
    static constexpr size_t kMaxFreeTiles = size_t{3} * 32;

    void insert(size_t index, const Tile& tile) {
        CORDITE_ASSERT(m_count < m_free.size(), "more free shadow tiles than there can be");
        if (m_count >= m_free.size()) {
            return;
        }
        std::move_backward(m_free.begin() + index, m_free.begin() + m_count, m_free.begin() + m_count + 1);
        m_free[index] = tile;
        ++m_count;
    }

    std::array<Tile, kMaxFreeTiles> m_free{};
    size_t m_count = 0;
};

/// The size a shadow map is to have: `base` texels a side times `scale`, as a power of
/// two that fits the atlas.
uint32_t tileSizeFor(uint32_t base, float scale, uint32_t atlasSize) {
    const float wanted = static_cast<float>(base) * std::clamp(numberOr(scale, 1.0f), 0.25f, 4.0f);
    uint32_t size = kMinShadowTileSize;
    while (static_cast<float>(size * 2) <= wanted && size * 2 <= atlasSize) {
        size *= 2;
    }
    return size;
}

/// The sizes of the `count` cascades of a sun that asks for `size` texels a side each:
/// that many, unless together they would take more than three quarters of the atlas.
/// The sun is planned first, and would otherwise be able to leave nothing to the lights
/// that come after it. The cascades farthest from the camera give way first, a halving
/// at a time: their texels are the ones looked at from farthest away.
std::array<uint32_t, kMaxShadowCascades> cascadeSizes(uint32_t count, uint32_t size, uint32_t atlasSize) {
    std::array<uint32_t, kMaxShadowCascades> sizes{};
    std::fill_n(sizes.begin(), count, size);
    const uint64_t side = std::bit_floor(atlasSize);
    const uint64_t budget = side * side / 4 * 3;
    const auto area = [&] {
        uint64_t total = 0;
        for (uint32_t cascade = 0; cascade < count; ++cascade) {
            total += static_cast<uint64_t>(sizes[cascade]) * sizes[cascade];
        }
        return total;
    };
    // From the farthest to the nearest and round again, until they fit or none of them
    // can be any smaller.
    uint32_t cascade = 0;
    uint32_t atSmallest = 0;
    while (area() > budget && atSmallest < count) {
        cascade = cascade == 0 ? count - 1 : cascade - 1;
        if (sizes[cascade] > kMinShadowTileSize) {
            sizes[cascade] /= 2;
            atSmallest = 0;
        } else {
            ++atSmallest;
        }
    }
    return sizes;
}

/// The tiles one light got, if it got any: where each of them is and how large.
struct LightTiles {
    std::array<UVec2, kMaxTilesPerLight> origins{};
    std::array<uint32_t, kMaxTilesPerLight> sizes{};
    bool found = false;
};

/// Takes a tile for each of `wanted` - sizes in texels a side - for one light: at those
/// sizes if there is room for all of them, with every tile halved, and halved again, if
/// not. When there is no room even for the smallest tiles, nothing is taken.
LightTiles allocateTiles(TileAllocator& allocator, std::span<const uint32_t> wanted) {
    CORDITE_ASSERT(wanted.size() <= kMaxTilesPerLight, "no light has more than {} shadow maps", kMaxTilesPerLight);
    LightTiles tiles;
    const size_t count = std::min(wanted.size(), tiles.sizes.size());
    std::copy_n(wanted.begin(), count, tiles.sizes.begin());
    while (true) {
        // On a copy, so that a light that does not get all its tiles takes none.
        TileAllocator attempt = allocator;
        size_t taken = 0;
        for (; taken < count; ++taken) {
            const std::optional<UVec2> origin = attempt.allocate(tiles.sizes[taken]);
            if (!origin) {
                break;
            }
            tiles.origins[taken] = *origin;
        }
        if (taken == count) {
            allocator = attempt;
            tiles.found = true;
            return tiles;
        }

        bool madeSmaller = false;
        for (size_t tile = 0; tile < count; ++tile) {
            if (tiles.sizes[tile] > kMinShadowTileSize) {
                tiles.sizes[tile] /= 2;
                madeSmaller = true;
            }
        }
        if (!madeSmaller) {
            return {};
        }
    }
}

/// A direction to take as "up" when looking along `direction`: the world's, unless the
/// two are nearly the same.
Vec3 upFor(const Vec3& direction) {
    return std::abs(direction.y) < 0.99f ? kWorldUp : Vec3(0.0f, 0.0f, 1.0f);
}

/// The directions in the world that a view matrix turns into its +X and its +Y: the
/// first two rows of its rotation.
Vec3 rightOf(const Mat4& view) {
    return {view[0][0], view[1][0], view[2][0]};
}

Vec3 upOf(const Mat4& view) {
    return {view[0][1], view[1][1], view[2][1]};
}

/// The view-space point that the camera sees at the clip-space place (x, y), `distance`
/// metres in front of it.
Vec3 viewPointAt(const CameraView& view, const Mat4& inverseProjection, float x, float y, float distance) {
    const Vec4 clip = view.projection * Vec4(0.0f, 0.0f, -distance, 1.0f);
    const Vec4 point = inverseProjection * Vec4(x, y, clip.z / clip.w, 1.0f);
    return Vec3(point) / point.w;
}

/// The shadow map of one cascade: an orthographic view along the light that holds the
/// slice of the camera's view from `start` to `end` metres.
ShadowView cascadeView(const CameraView& view, const Vec3& lightDirection, float start, float end, UVec2 origin,
                       uint32_t size) {
    // A sphere around the slice rather than a box: it is as large whichever way the
    // camera looks, so the size of a texel in the world does not change as it turns.
    const Mat4 inverseProjection = glm::inverse(view.projection);
    std::array<Vec3, 8> corners{};
    size_t count = 0;
    for (const float distance : {start, end}) {
        for (const float y : {-1.0f, 1.0f}) {
            for (const float x : {-1.0f, 1.0f}) {
                corners[count++] = viewPointAt(view, inverseProjection, x, y, distance);
            }
        }
    }
    Vec3 centre(0.0f);
    for (const Vec3& corner : corners) {
        centre += corner / 8.0f;
    }
    float radius = 0.0f;
    for (const Vec3& corner : corners) {
        radius = std::max(radius, glm::distance(corner, centre));
    }
    // Two texels wider than the slice needs: the map is moved by up to a texel below,
    // and the filter reads a texel around what it looks up.
    radius *= 1.0f + 4.0f / static_cast<float>(size);
    radius = std::ceil(radius / kRadiusStep) * kRadiusStep;

    // In the light's frame, which is turned like the light and stays where the world
    // is: there the map moves in whole texels, so that a shadow keeps its texels as the
    // camera moves and does not shimmer.
    const Mat4 lightRotation = lookAt(Vec3(0.0f), lightDirection, upFor(lightDirection));
    Vec3 lightCentre(lightRotation * glm::inverse(view.view) * Vec4(centre, 1.0f));
    const float texel = 2.0f * radius / static_cast<float>(size);
    lightCentre.x = std::floor(lightCentre.x / texel) * texel;
    lightCentre.y = std::floor(lightCentre.y / texel) * texel;

    // The light looks along -Z. The map begins well before the sphere, for what stands
    // between the light and the view, and ends where the sphere does.
    const float nearPlane = -(lightCentre.z + radius) - kShadowCasterDistance;
    const float farPlane = -(lightCentre.z - radius);
    ShadowView shadow;
    shadow.viewProjection = orthographic(lightCentre.x - radius, lightCentre.x + radius, lightCentre.y - radius,
                                         lightCentre.y + radius, nearPlane, farPlane) *
                            lightRotation;
    shadow.tileOrigin = origin;
    shadow.tileSize = size;
    shadow.texelSize = texel;
    shadow.right = rightOf(lightRotation);
    shadow.up = upOf(lightRotation);
    shadow.perspective = false;
    return shadow;
}

/// A shadow map that looks from `position` along `direction` and sees `halfAngle` to
/// every side, as far as `range`.
ShadowView perspectiveView(const Vec3& position, const Vec3& direction, float halfAngle, float range, UVec2 origin,
                           uint32_t size) {
    const float farPlane = std::max(range, 2.0f * kLocalNearPlane);
    const Mat4 lightView = lookAt(position, position + direction, upFor(direction));
    ShadowView shadow;
    shadow.viewProjection = perspective(2.0f * halfAngle, 1.0f, kLocalNearPlane, farPlane) * lightView;
    shadow.tileOrigin = origin;
    shadow.tileSize = size;
    shadow.texelSize = 2.0f * std::tan(halfAngle) / static_cast<float>(size);
    shadow.right = rightOf(lightView);
    shadow.up = upOf(lightView);
    shadow.perspective = true;
    return shadow;
}

/// The directions the six shadow maps of a point light look in, in the order of
/// pointShadowFace().
constexpr std::array<Vec3, 6> kFaceDirections = {Vec3(1.0f, 0.0f, 0.0f), Vec3(-1.0f, 0.0f, 0.0f),
                                                 Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, -1.0f, 0.0f),
                                                 Vec3(0.0f, 0.0f, 1.0f), Vec3(0.0f, 0.0f, -1.0f)};

/// Plans the cascades of the directional light `light`. Returns false when the view
/// has no room for them.
bool planCascades(const CameraView& view, const SceneLight& light, uint32_t lightIndex, const ShadowSettings& settings,
                  TileAllocator& allocator, ShadowPlan& plan) {
    const uint32_t count = std::clamp(light.shadow.cascades, 1u, kMaxShadowCascades);
    if (plan.views.size() + count > kMaxShadowViews) {
        return false;
    }
    const std::array<uint32_t, kMaxShadowCascades> sizes = cascadeSizes(
        count, tileSizeFor(settings.cascadeSize, light.shadow.resolutionScale, settings.atlasSize), settings.atlasSize);
    const LightTiles tiles = allocateTiles(allocator, std::span(sizes).first(count));
    if (!tiles.found) {
        return false;
    }

    const float nearPlane = std::max(view.nearPlane, 1.0e-3f);
    // No further than the camera sees, and never nothing.
    const float distance = std::max(
        std::min(numberOr(light.shadow.distance, SceneLightShadow().distance), view.farPlane), nearPlane * 2.0f);
    const Vec3 direction = normalizeOr(light.direction, -kWorldUp);

    ShadowCascades& cascades = plan.cascades;
    cascades.count = count;
    cascades.ends = cascadeEnds(nearPlane, distance, count, light.shadow.cascadeDistribution);
    cascades.fadeEnd = distance;
    cascades.fadeStart = distance * (1.0f - kFadeShare);
    cascades.firstView = static_cast<uint32_t>(plan.views.size());
    float previousStart = nearPlane;
    for (uint32_t cascade = 0; cascade < count; ++cascade) {
        float start = nearPlane;
        if (cascade > 0) {
            const float previousEnd = cascades.ends[cascade - 1];
            start = previousEnd - (previousEnd - previousStart) * kCascadeBlend;
            previousStart = previousEnd;
        }
        ShadowView shadow =
            cascadeView(view, direction, start, cascades.ends[cascade], tiles.origins[cascade], tiles.sizes[cascade]);
        shadow.light = lightIndex;
        plan.views.push_back(shadow);
    }
    return true;
}

/// Plans the shadow map of a spot light, or the six of a point or a rect light. Returns
/// false when the view has no room for them.
bool planLocalLight(const SceneLight& light, uint32_t lightIndex, const ShadowSettings& settings,
                    TileAllocator& allocator, ShadowPlan& plan) {
    const bool isSpot = light.type == GpuLightType::Spot;
    const uint32_t count = isSpot ? 1 : 6;
    if (plan.views.size() + count > kMaxShadowViews) {
        return false;
    }
    // All the maps of a light are of one size, and stay so when they are made smaller.
    std::array<uint32_t, kMaxTilesPerLight> sizes{};
    sizes.fill(tileSizeFor(settings.localSize, light.shadow.resolutionScale, settings.atlasSize));
    const LightTiles tiles = allocateTiles(allocator, std::span(sizes).first(count));
    if (!tiles.found) {
        return false;
    }
    const uint32_t size = tiles.sizes[0];

    if (isSpot) {
        const float halfAngle = std::clamp(light.outerConeAngle, 0.01f, kMaxSpotShadowAngle);
        ShadowView shadow = perspectiveView(light.position, normalizeOr(light.direction, -kWorldUp), halfAngle,
                                            light.range, tiles.origins[0], size);
        shadow.light = lightIndex;
        plan.views.push_back(shadow);
        return true;
    }

    // Each map sees a little more than its side of the cube, so that the filter finds
    // texels of the same map around a point at the very edge of a side.
    const float guard = static_cast<float>(settings.filterRadius) + 1.5f;
    const float halfAngle = std::atan(1.0f + 2.0f * guard / static_cast<float>(size));
    for (uint32_t face = 0; face < count; ++face) {
        ShadowView shadow =
            perspectiveView(light.position, kFaceDirections[face], halfAngle, light.range, tiles.origins[face], size);
        shadow.light = lightIndex;
        plan.views.push_back(shadow);
    }
    return true;
}

} // namespace

std::string_view nameOf(ShadowQuality quality) {
    switch (quality) {
        case ShadowQuality::Off:
            return "off";
        case ShadowQuality::Low:
            return "low";
        case ShadowQuality::Medium:
            return "medium";
        case ShadowQuality::High:
            return "high";
    }
    return "unknown";
}

std::string_view displayNameOf(ShadowQuality quality) {
    switch (quality) {
        case ShadowQuality::Off:
            return "Off";
        case ShadowQuality::Low:
            return "Low";
        case ShadowQuality::Medium:
            return "Medium";
        case ShadowQuality::High:
            return "High";
    }
    return "Unknown";
}

std::optional<ShadowQuality> shadowQualityFromName(std::string_view name) {
    for (const ShadowQuality quality : kShadowQualities) {
        if (equalsIgnoreCase(name, nameOf(quality))) {
            return quality;
        }
    }
    return std::nullopt;
}

ShadowSettings shadowSettingsFor(ShadowQuality quality) {
    switch (quality) {
        case ShadowQuality::Off:
            return {.atlasSize = 0, .cascadeSize = 0, .localSize = 0, .filterRadius = 0};
        case ShadowQuality::Low:
            return {.atlasSize = 2048, .cascadeSize = 512, .localSize = 256, .filterRadius = 1};
        case ShadowQuality::Medium:
            return {.atlasSize = 4096, .cascadeSize = 1024, .localSize = 512, .filterRadius = 1};
        case ShadowQuality::High:
            return {.atlasSize = 8192, .cascadeSize = 2048, .localSize = 1024, .filterRadius = 2};
    }
    return {};
}

std::array<float, kMaxShadowCascades> cascadeEnds(float nearPlane, float distance, uint32_t count, float distribution) {
    std::array<float, kMaxShadowCascades> ends{};
    count = std::clamp(count, 1u, kMaxShadowCascades);
    nearPlane = std::max(nearPlane, 1.0e-3f);
    distance = std::max(distance, nearPlane);
    const float blend = std::clamp(numberOr(distribution, SceneLightShadow().cascadeDistribution), 0.0f, 1.0f);
    for (uint32_t cascade = 0; cascade + 1 < count; ++cascade) {
        const float share = static_cast<float>(cascade + 1) / static_cast<float>(count);
        const float even = nearPlane + (distance - nearPlane) * share;
        const float byFactor = nearPlane * std::pow(distance / nearPlane, share);
        ends[cascade] = even + (byFactor - even) * blend;
    }
    // Exactly, not as the arithmetic above would round it.
    ends[count - 1] = distance;
    return ends;
}

uint32_t pointShadowFace(const Vec3& offset) {
    const Vec3 size = glm::abs(offset);
    if (size.x >= size.y && size.x >= size.z) {
        return offset.x >= 0.0f ? 0 : 1;
    }
    if (size.y >= size.z) {
        return offset.y >= 0.0f ? 2 : 3;
    }
    return offset.z >= 0.0f ? 4 : 5;
}

void planShadows(const CameraView& view, std::span<const SceneLight> lights, LightClusters& clusters,
                 const ShadowSettings& settings, ShadowPlan& plan) {
    plan.views.clear();
    plan.cascades = {};
    plan.lightsWithoutRoom = 0;
    for (GpuLight& light : clusters.lights) {
        light.shadowView = -1;
    }
    const bool matches = clusters.sources.size() == clusters.lights.size();
    CORDITE_ASSERT(matches,
                   "the clusters were not built by a LightClusterBuilder: they do not say where lights come from");
    if (!matches || settings.atlasSize < kMinShadowTileSize) {
        // Without an atlas shadows are switched off; nobody goes without what nobody gets.
        return;
    }

    // The lights that ask for shadows. A view has no more lights than these.
    std::array<uint32_t, kMaxDirectionalLights + kMaxLocalLights> wanted{};
    size_t wantedCount = 0;
    for (uint32_t index = 0; index < clusters.lights.size() && wantedCount < wanted.size(); ++index) {
        const uint32_t source = clusters.sources[index];
        if (source < lights.size() && lights[source].shadow.enabled) {
            wanted[wantedCount++] = index;
        }
    }

    // The directional ones from the brightest on - the first of them is the sun - then
    // the others from the nearest to the camera to the farthest: when not all of them
    // find room, it is the far ones that go without. Lights that are as bright or as
    // near keep the order they have.
    const auto nearness = [&](uint32_t index) {
        const GpuLight& light = clusters.lights[index];
        const float value = glm::distance(light.position, view.position) - light.range;
        return std::isfinite(value) ? value : 0.0f;
    };
    const auto brightness = [&](uint32_t index) {
        const Vec3& color = clusters.lights[index].color;
        const float value = std::max({color.r, color.g, color.b});
        return std::isfinite(value) ? value : 0.0f;
    };
    std::sort(wanted.begin(), wanted.begin() + static_cast<std::ptrdiff_t>(wantedCount), [&](uint32_t a, uint32_t b) {
        const bool aIsDirectional = a < clusters.directionalCount;
        const bool bIsDirectional = b < clusters.directionalCount;
        if (aIsDirectional != bIsDirectional) {
            return aIsDirectional;
        }
        const float keyA = aIsDirectional ? -brightness(a) : nearness(a);
        const float keyB = aIsDirectional ? -brightness(b) : nearness(b);
        return keyA != keyB ? keyA < keyB : a < b;
    });

    TileAllocator allocator(settings.atlasSize);
    bool hasSun = false;
    for (size_t entry = 0; entry < wantedCount; ++entry) {
        const uint32_t index = wanted[entry];
        const SceneLight& light = lights[clusters.sources[index]];
        const auto firstView = static_cast<int32_t>(plan.views.size());
        bool planned = false;
        if (light.type == GpuLightType::Directional) {
            // One sun: the brightest directional light that casts shadows, and no other
            // in its place when it finds no room.
            planned = !hasSun && planCascades(view, light, index, settings, allocator, plan);
            hasSun = true;
        } else {
            planned = planLocalLight(light, index, settings, allocator, plan);
        }
        if (!planned) {
            ++plan.lightsWithoutRoom;
            continue;
        }
        GpuLight& gpuLight = clusters.lights[index];
        gpuLight.shadowView = firstView;
        gpuLight.shadowBias = std::clamp(numberOr(light.shadow.bias, 1.0f), 0.0f, kMaxShadowOffset);
        gpuLight.shadowNormalOffset = std::clamp(numberOr(light.shadow.normalOffset, 1.0f), 0.0f, kMaxShadowOffset);
    }
}

} // namespace cordite
