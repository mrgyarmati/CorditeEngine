#include "cordite/renderer/RectLights.h"

#include "cordite/core/Assert.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

// Written at build time by cmake/CorditeEmbedNumbers.cmake, from the tables of ltc_code
// that the CMakeLists.txt of this module names. They are defined in those sources, which
// is what misc-use-internal-linkage cannot see.
// NOLINTBEGIN(misc-use-internal-linkage)
namespace cordite::embedded {
std::span<const double> ltcMatrices();
std::span<const double> ltcMagnitudes();
} // namespace cordite::embedded

// NOLINTEND(misc-use-internal-linkage)

namespace cordite {
namespace {

constexpr size_t kTexelCount = size_t{kLtcTableSize} * kLtcTableSize;

using Table = std::array<Vec4, kTexelCount>;

Table tableOf(std::span<const double> numbers) {
    CORDITE_ASSERT(numbers.size() == 4 * kTexelCount);
    Table texels{};
    for (size_t index = 0; index < kTexelCount && 4 * index + 3 < numbers.size(); ++index) {
        texels[index] = Vec4(static_cast<float>(numbers[4 * index]), static_cast<float>(numbers[4 * index + 1]),
                             static_cast<float>(numbers[4 * index + 2]), static_cast<float>(numbers[4 * index + 3]));
    }
    return texels;
}

const Table& matrixTable() {
    static const Table kTable = tableOf(embedded::ltcMatrices());
    return kTable;
}

const Table& magnitudeTable() {
    static const Table kTable = tableOf(embedded::ltcMagnitudes());
    return kTable;
}

/// A number brought into 0..1 as a float, before anything is made of it: what is no number
/// counts as nothing.
float unitInterval(float value) {
    return value > 0.0f ? std::min(value, 1.0f) : 0.0f;
}

/// A table read between its texels, at texel coordinates from 0 to kLtcTableSize - 1 whose
/// whole numbers are the texels' middles: as a GPU's linear sampler reads it.
Vec4 filtered(const Table& table, float x, float y) {
    const auto column = std::min(static_cast<size_t>(x), size_t{kLtcTableSize - 2});
    const auto row = std::min(static_cast<size_t>(y), size_t{kLtcTableSize - 2});
    const float across = x - static_cast<float>(column);
    const float down = y - static_cast<float>(row);
    const size_t first = row * kLtcTableSize + column;
    const Vec4 top = glm::mix(table[first], table[first + 1], across);
    const Vec4 bottom = glm::mix(table[first + kLtcTableSize], table[first + kLtcTableSize + 1], across);
    return glm::mix(top, bottom, down);
}

/// A polygon as a point sees it: the corners as offsets from the point, in order. A
/// rectangle has four; cut by a plane, a convex polygon gains one corner at the most, and
/// the rectangle is cut twice.
struct Polygon {
    std::array<Vec3, 6> corners{};
    size_t count = 0;
};

/// What of a convex polygon lies above z = 0, in the same order.
Polygon aboveHorizon(const Polygon& polygon) {
    CORDITE_ASSERT(polygon.count < polygon.corners.size());
    Polygon above;
    const auto add = [&above](const Vec3& corner) {
        if (above.count < above.corners.size()) {
            above.corners[above.count++] = corner;
        }
    };
    for (size_t index = 0; index < polygon.count; ++index) {
        const Vec3& from = polygon.corners[index];
        const Vec3& to = polygon.corners[(index + 1) % polygon.count];
        const bool fromAbove = from.z > 0.0f;
        if (fromAbove) {
            add(from);
        }
        // Where an edge crosses the horizon: only between corners on either side of it,
        // so the division is by more than nothing.
        if (fromAbove != (to.z > 0.0f)) {
            add(glm::mix(from, to, from.z / (from.z - to.z)));
        }
    }
    return above;
}

/// The vector form factor of a polygon (Arvo, 1995): over every edge, the angle it spans
/// as the point sees it, along the unit normal of the plane through it and the point,
/// summed and divided by two pi. Its z is the integral of the cosine about +Z over the
/// polygon, over pi - more than nothing for corners that turn clockwise as the point sees
/// them - and it points the way the light of the polygon arrives on average.
Vec3 vectorFormFactor(const Polygon& polygon) {
    Vec3 sum(0.0f);
    for (size_t index = 0; index < polygon.count; ++index) {
        const Vec3 from = normalizeOr(polygon.corners[index], Vec3(0.0f));
        const Vec3 to = normalizeOr(polygon.corners[(index + 1) % polygon.count], Vec3(0.0f));
        const Vec3 normal = glm::cross(from, to);
        const float sine = glm::length(normal);
        // An edge that the point sees as a point spans no angle.
        if (sine > kEpsilon) {
            sum += normal * (std::atan2(sine, glm::dot(from, to)) / sine);
        }
    }
    return sum / (2.0f * kPi);
}

/// Whether `position` is in front of the face of `light` that shines. Behind it the corners
/// turn the other way, and what the integrals come to is less than nothing, which counts as
/// nothing: asking first only saves the work.
bool isInFront(const RectLightShape& light, const Vec3& position) {
    return glm::dot(position - light.center, glm::cross(light.up, light.right)) > 0.0f;
}

/// A frame about the normal, as rows: x along the surface towards the viewer, z the normal.
/// Seen along the normal any way along the surface will do: there the lobe is round.
struct Frame {
    Vec3 x;
    Vec3 y;
    Vec3 z;
};

Frame frameAbout(const Vec3& normal, const Vec3& toViewer) {
    const Vec3 anyTangent = std::abs(normal.x) < 0.9f ? Vec3(1.0f, 0.0f, 0.0f) : Vec3(0.0f, 1.0f, 0.0f);
    const Vec3 fallback = glm::normalize(anyTangent - normal * glm::dot(anyTangent, normal));
    const Vec3 x = normalizeOr(toViewer - normal * glm::dot(toViewer, normal), fallback);
    return {.x = x, .y = glm::cross(normal, x), .z = normal};
}

/// The corners of the rectangle as offsets from `position` in `frame`: counter-clockwise
/// seen from behind the light, and so clockwise as a point in front of it sees them.
Polygon cornersIn(const Frame& frame, const RectLightShape& light, const Vec3& position) {
    const std::array<Vec3, 4> world = {light.center - light.right - light.up, light.center + light.right - light.up,
                                       light.center + light.right + light.up, light.center - light.right + light.up};
    Polygon polygon;
    for (const Vec3& corner : world) {
        const Vec3 offset = corner - position;
        polygon.corners[polygon.count++] =
            Vec3(glm::dot(frame.x, offset), glm::dot(frame.y, offset), glm::dot(frame.z, offset));
    }
    return polygon;
}

} // namespace

std::span<const Vec4> ltcMatrixTexels() {
    return matrixTable();
}

std::span<const Vec4> ltcMagnitudeTexels() {
    return magnitudeTable();
}

LtcFit ltcFit(float roughness, float nDotV) {
    const auto last = static_cast<float>(kLtcTableSize - 1);
    const float x = unitInterval(roughness) * last;
    const float y = std::sqrt(1.0f - unitInterval(nDotV)) * last;
    return {.matrix = filtered(matrixTable(), x, y), .magnitude = filtered(magnitudeTable(), x, y)};
}

RectLightIrradiance rectLightIrradiance(const RectLightShape& light, const Vec3& position, const Vec3& normal) {
    if (!isInFront(light, position)) {
        return {};
    }
    const Frame frame = frameAbout(normal, normal);
    const Vec3 factor = vectorFormFactor(aboveHorizon(cornersIn(frame, light, position)));
    if (!(factor.z > 0.0f)) {
        return {};
    }
    const Vec3 direction = frame.x * factor.x + frame.y * factor.y + frame.z * factor.z;
    return {.formFactor = factor.z, .direction = normalizeOr(direction, normal)};
}

RectLightSpecular rectLightSpecular(const RectLightShape& light, const Vec3& position, const Vec3& normal,
                                    const Vec3& toViewer, float roughness) {
    if (!isInFront(light, position)) {
        return {};
    }
    // What the surface cannot see adds nothing, wherever the fitted lobe reaches: the lobe
    // of GGX ends at the horizon, and the cosine that stands in for it does not quite.
    const Frame frame = frameAbout(normal, toViewer);
    const Polygon visible = aboveHorizon(cornersIn(frame, light, position));
    if (visible.count < 3) {
        return {};
    }
    const LtcFit fit =
        ltcFit(roughness > kMinimumRoughness ? roughness : kMinimumRoughness, glm::dot(normal, toViewer));
    // The inverse matrix takes the lobe back to the cosine it was made of, and the polygon
    // along with it; there the cosine is cut at its own horizon.
    Polygon cosine = visible;
    for (size_t index = 0; index < cosine.count; ++index) {
        const Vec3& corner = visible.corners[index];
        cosine.corners[index] = Vec3(fit.matrix.x * corner.x + fit.matrix.z * corner.z, corner.y,
                                     fit.matrix.y * corner.x + fit.matrix.w * corner.z);
    }
    const float integral = vectorFormFactor(aboveHorizon(cosine)).z;
    if (!(integral > 0.0f)) {
        return {};
    }
    return {.lobe = integral * fit.magnitude.x, .grazing = integral * fit.magnitude.y};
}

} // namespace cordite
