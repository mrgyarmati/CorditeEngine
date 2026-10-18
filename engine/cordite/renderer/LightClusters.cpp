#include "cordite/renderer/LightClusters.h"

#include "cordite/core/Assert.h"
#include "cordite/renderer/LightCookies.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace cordite {
namespace {

/// A light with nothing of this left does not light anything worth the work.
constexpr float kMinimumBrightness = 1.0e-6f;

/// Depths are kept away from zero, where the logarithm has no value.
constexpr float kMinimumDepth = 1.0e-4f;

/// The smallest difference between the cosines of a spot light's two cone angles: a
/// light whose cone has no soft edge at all still fades over this much.
constexpr float kMinimumConeFade = 1.0e-4f;

/// The most a light lights a fog against how it lights a surface: the most the Light
/// component allows.
constexpr float kMostVolumetricIntensity = 100.0f;

bool isFiniteLight(const SceneLight& light) {
    return isFinite(light.position) && isFinite(light.direction) && isFinite(light.color) &&
           std::isfinite(light.range) && std::isfinite(light.innerConeAngle) && std::isfinite(light.outerConeAngle);
}

bool hasBrightness(const SceneLight& light) {
    return std::max({light.color.r, light.color.g, light.color.b}) > kMinimumBrightness;
}

GpuLight toGpuLight(const SceneLight& light) {
    GpuLight gpu;
    gpu.position = light.position;
    gpu.range = light.range;
    gpu.color = glm::max(light.color, Vec3(0.0f));
    gpu.typeAndChannels = static_cast<uint32_t>(light.type) | ((light.channels & 0xFFu) << 8);
    gpu.direction = normalizeOr(light.direction, -kWorldUp);
    if (light.type == GpuLightType::Spot) {
        // Full brightness inside the inner cone, none outside the outer one.
        const float outer = std::clamp(light.outerConeAngle, 0.0f, kHalfPi);
        const float inner = std::clamp(light.innerConeAngle, 0.0f, outer);
        const float cosOuter = std::cos(outer);
        gpu.spotScale = 1.0f / std::max(std::cos(inner) - cosOuter, kMinimumConeFade);
        gpu.spotOffset = -cosOuter * gpu.spotScale;
    }
    gpu.cookieRight = cookieAxisOf(light);
    // A light whose share of the fog is no number still lights surfaces: the fog gets none.
    const float volumetric = std::isfinite(light.volumetricIntensity)
                                 ? std::clamp(light.volumetricIntensity, 0.0f, kMostVolumetricIntensity)
                                 : 0.0f;
    gpu.fogColor = gpu.color * volumetric;
    return gpu;
}

/// A sphere that encloses everything a point or spot light reaches, in world space.
Sphere boundsOf(const SceneLight& light) {
    if (light.type != GpuLightType::Spot) {
        return {light.position, light.range};
    }
    // The smallest sphere around a cone ("Cull that cone", B. Wronski, 2017).
    const float angle = std::clamp(light.outerConeAngle, 0.0f, kHalfPi);
    const Vec3 axis = normalizeOr(light.direction, -kWorldUp);
    if (angle > kPi * 0.25f) {
        return {light.position + axis * (std::cos(angle) * light.range), std::sin(angle) * light.range};
    }
    const float radius = light.range / (2.0f * std::cos(angle));
    return {light.position + axis * radius, radius};
}

} // namespace

uint32_t ClusterDepthMapping::sliceOf(float depth) const {
    if (!(depth > kMinimumDepth)) {
        return 0;
    }
    // Compared as floats before it becomes an integer: an infinite depth has a slice,
    // the last one, but no integer value.
    const float slice = orthographic ? depth * scale + bias : std::log2(depth) * scale + bias;
    if (!(slice > 0.0f)) {
        return 0;
    }
    if (!(slice < static_cast<float>(kClusterSlices))) {
        return kClusterSlices - 1;
    }
    return static_cast<uint32_t>(slice);
}

float ClusterDepthMapping::sliceStart(uint32_t slice) const {
    const float position = (static_cast<float>(slice) - bias) / scale;
    return orthographic ? position : std::exp2(position);
}

ClusterDepthMapping clusterDepthMapping(const CameraView& view) {
    const float nearPlane = std::max(view.nearPlane, kMinimumDepth);
    // Beyond kMaxClusterDepth everything shares the last slice. The far end is never
    // closer than twice the near one, so that the slices have a depth to share out.
    const float farPlane = std::max(std::min(view.farPlane, kMaxClusterDepth), nearPlane * 2.0f);

    ClusterDepthMapping mapping;
    mapping.orthographic = view.orthographic;
    if (view.orthographic) {
        mapping.scale = static_cast<float>(kClusterSlices) / (farPlane - nearPlane);
        mapping.bias = -nearPlane * mapping.scale;
    } else {
        mapping.scale = static_cast<float>(kClusterSlices) / std::log2(farPlane / nearPlane);
        mapping.bias = -std::log2(nearPlane) * mapping.scale;
    }
    return mapping;
}

uint32_t clusterOf(Vec2 uv, float depth, const ClusterDepthMapping& mapping) {
    const auto cell = [](float coordinate, uint32_t count) {
        const float position = coordinate * static_cast<float>(count);
        if (!(position > 0.0f)) {
            return 0u;
        }
        if (!(position < static_cast<float>(count))) {
            return count - 1;
        }
        return static_cast<uint32_t>(position);
    };
    return clusterIndex(cell(uv.x, kClusterColumns), cell(uv.y, kClusterRows), mapping.sliceOf(depth));
}

std::span<const uint32_t> LightClusters::lightsOf(uint32_t cluster) const {
    CORDITE_ASSERT(cluster < grid.size(), "cluster {} of {}", cluster, grid.size());
    if (cluster >= grid.size()) {
        return {};
    }
    const ClusterRange& range = grid[cluster];
    return std::span(indices).subspan(range.offset, range.count);
}

void LightClusterBuilder::updateBounds(const CameraView& view, const ClusterDepthMapping& mapping) {
    if (m_hasBounds && m_boundsProjection == view.projection && m_boundsMapping == mapping) {
        return;
    }

    const Mat4 inverseProjection = glm::inverse(view.projection);
    // Where the planes between the slices are, as the projection writes depth.
    std::array<float, kClusterSlices + 1> planes{};
    for (uint32_t slice = 0; slice <= kClusterSlices; ++slice) {
        const Vec4 clip = view.projection * Vec4(0.0f, 0.0f, -mapping.sliceStart(slice), 1.0f);
        planes[slice] = clip.z / clip.w;
    }

    // The corners the clusters share, each worked out once: where the lines between the
    // columns and the rows cross the planes between the slices.
    constexpr uint32_t kLinesAcross = kClusterColumns + 1;
    constexpr uint32_t kLinesDown = kClusterRows + 1;
    std::vector<Vec3> corners(static_cast<size_t>(kLinesAcross) * kLinesDown * (kClusterSlices + 1));
    const auto cornerAt = [&corners](uint32_t column, uint32_t row, uint32_t plane) -> Vec3& {
        return corners[(static_cast<size_t>(plane) * kLinesDown + row) * kLinesAcross + column];
    };
    for (uint32_t plane = 0; plane <= kClusterSlices; ++plane) {
        for (uint32_t row = 0; row <= kClusterRows; ++row) {
            // Row 0 is the top of the picture, where clip space has y = +1.
            const float y = 1.0f - 2.0f * static_cast<float>(row) / static_cast<float>(kClusterRows);
            for (uint32_t column = 0; column <= kClusterColumns; ++column) {
                const float x = 2.0f * static_cast<float>(column) / static_cast<float>(kClusterColumns) - 1.0f;
                const Vec4 point = inverseProjection * Vec4(x, y, planes[plane], 1.0f);
                cornerAt(column, row, plane) = Vec3(point) / point.w;
            }
        }
    }

    m_bounds.assign(kClusterCount, Aabb());
    m_rowBounds.assign(static_cast<size_t>(kClusterSlices) * kClusterRows, Aabb());
    m_columnBounds.assign(static_cast<size_t>(kClusterSlices) * kClusterColumns, Aabb());
    for (uint32_t slice = 0; slice < kClusterSlices; ++slice) {
        for (uint32_t row = 0; row < kClusterRows; ++row) {
            for (uint32_t column = 0; column < kClusterColumns; ++column) {
                Aabb& box = m_bounds[clusterIndex(column, row, slice)];
                for (uint32_t corner = 0; corner < 8; ++corner) {
                    box.expand(
                        cornerAt(column + (corner & 1u), row + ((corner >> 1u) & 1u), slice + ((corner >> 2u) & 1u)));
                }
                m_rowBounds[static_cast<size_t>(slice) * kClusterRows + row].expand(box);
                m_columnBounds[static_cast<size_t>(slice) * kClusterColumns + column].expand(box);
            }
        }
    }

    m_boundsProjection = view.projection;
    m_boundsMapping = mapping;
    m_hasBounds = true;
}

void LightClusterBuilder::build(const CameraView& view, std::span<const SceneLight> lights, LightClusters& clusters) {
    clusters.lights.clear();
    clusters.sources.clear();
    clusters.indices.clear();
    clusters.grid.assign(kClusterCount, ClusterRange());
    clusters.directionalCount = 0;
    clusters.droppedLights = 0;
    clusters.depthMapping = clusterDepthMapping(view);

    // Directional lights come first in the list, whatever order the lights are given
    // in: they reach everywhere and are in no cluster's list.
    m_local.clear();
    for (const SceneLight& light : lights) {
        if (!isFiniteLight(light) || !hasBrightness(light)) {
            continue;
        }
        if (light.type != GpuLightType::Directional) {
            if (light.range > 0.0f) {
                m_local.push_back(&light);
            }
        } else if (clusters.directionalCount < kMaxDirectionalLights) {
            clusters.lights.push_back(toGpuLight(light));
            clusters.sources.push_back(static_cast<uint32_t>(&light - lights.data()));
            ++clusters.directionalCount;
        } else {
            ++clusters.droppedLights;
        }
    }
    if (m_local.empty()) {
        return;
    }

    updateBounds(view, clusters.depthMapping);

    // Every (cluster, light) pair, light after light; the lists are cut from these.
    m_pairs.clear();
    uint32_t localCount = 0;
    for (const SceneLight* light : m_local) {
        const Sphere world = boundsOf(*light);
        const Sphere reach{Vec3(view.view * Vec4(world.center, 1.0f)), world.radius};
        const auto lightIndex = static_cast<uint32_t>(clusters.lights.size());
        const size_t pairsBefore = m_pairs.size();
        addPairs(reach, lightIndex, clusters.depthMapping);

        // A light that reaches nothing the camera sees is not sent to the GPU at all,
        // and only lights that are sent count towards the limit.
        if (m_pairs.size() == pairsBefore) {
            continue;
        }
        if (localCount == kMaxLocalLights) {
            m_pairs.resize(pairsBefore);
            ++clusters.droppedLights;
            continue;
        }
        clusters.lights.push_back(toGpuLight(*light));
        clusters.sources.push_back(static_cast<uint32_t>(light - lights.data()));
        ++localCount;
    }
    writeLists(clusters);
}

void LightClusterBuilder::addPairs(const Sphere& reach, uint32_t light, const ClusterDepthMapping& mapping) {
    // The camera looks along -Z: how far in front of it the sphere begins and ends.
    const float nearest = -reach.center.z - reach.radius;
    const float farthest = -reach.center.z + reach.radius;
    if (farthest <= 0.0f) {
        return;
    }

    const uint32_t firstSlice = mapping.sliceOf(nearest);
    const uint32_t lastSlice = mapping.sliceOf(farthest);
    for (uint32_t slice = firstSlice; slice <= lastSlice; ++slice) {
        // The rows of the slice the light touches at all: most lights touch a few,
        // and the clusters of the others need not be asked.
        uint32_t rows = 0;
        for (uint32_t row = 0; row < kClusterRows; ++row) {
            if (reach.intersects(m_rowBounds[static_cast<size_t>(slice) * kClusterRows + row])) {
                rows |= 1u << row;
            }
        }
        if (rows == 0) {
            continue;
        }
        for (uint32_t column = 0; column < kClusterColumns; ++column) {
            if (!reach.intersects(m_columnBounds[static_cast<size_t>(slice) * kClusterColumns + column])) {
                continue;
            }
            for (uint32_t row = 0; row < kClusterRows; ++row) {
                const uint32_t cluster = clusterIndex(column, row, slice);
                if ((rows & (1u << row)) != 0 && reach.intersects(m_bounds[cluster])) {
                    m_pairs.push_back({cluster, light});
                }
            }
        }
    }
}

void LightClusterBuilder::writeLists(LightClusters& clusters) const {
    // The lists follow each other in the order of the clusters; within a list the
    // lights keep the order they were found in, which is the order of their indices.
    for (const Pair& pair : m_pairs) {
        ++clusters.grid[pair.cluster].count;
    }
    uint32_t offset = 0;
    for (ClusterRange& range : clusters.grid) {
        range.offset = offset;
        offset += range.count;
        range.count = 0;
    }
    clusters.indices.resize(m_pairs.size());
    for (const Pair& pair : m_pairs) {
        ClusterRange& range = clusters.grid[pair.cluster];
        clusters.indices[range.offset + range.count] = pair.light;
        ++range.count;
    }
}

void buildLightClusters(const CameraView& view, std::span<const SceneLight> lights, LightClusters& clusters) {
    LightClusterBuilder().build(view, lights, clusters);
}

} // namespace cordite
